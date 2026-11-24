/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 *
 * Adapted from ESP-IDF v6.1 examples/bluetooth/nimble/blecent:
 * keep GAP discovery and notification subscription; send PING once per second.
 * Service discovery is provided by the SDK's unmodified nimble_central_utils.
 */

/**
 * @file ble_gateway.c
 * @brief S3 中央设备：连接 C3，处理触屏角度命令和定时 PING/PONG。
 * @note GAP（Generic Access Profile，通用访问规范）管理发现与连接；
 *       GATT（Generic Attribute Profile，通用属性规范）管理服务、特征和数据访问。
 */

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "ble_gateway.h"
#include "ble_protocol.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "esp_central.h"
#include "nimble/nimble_port.h"
#include "ota_update.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

/** 日志标签，用于区分网关模块的输出。 */
static const char *TAG = "ble_gateway";
static const char DEVICE_NAME[] = "BLE-S3-Gateway";
/** 扫描及服务发现的目标标识。 */
static const ble_uuid128_t SERVICE_UUID = BLE_LINK_SERVICE_UUID;
/** 收发特征标识。 */
static const ble_uuid128_t CHARACTERISTIC_UUID = BLE_LINK_CHARACTERISTIC_UUID;
/** 请求常量；数组含 '\0'，发送时只取前四字节。 */
static const char PING[] = BLE_LINK_PING;
static const uint32_t ANGLE_TIMEOUT_MS = 1500;

typedef enum
{
    GATT_WRITE_NONE,
    GATT_WRITE_PING,
    GATT_WRITE_ANGLE,
} gatt_write_t;

typedef struct
{
    uint32_t connection_id;
    uint16_t angle;
} angle_request_t;

/* 任务之间只传递命令及状态副本，蓝牙状态由 NimBLE 主机单独维护。 */
static QueueHandle_t s_angle_requests;
static QueueHandle_t s_status_updates;
static struct ble_npl_event s_angle_request_event;
static struct ble_npl_event s_ota_toggle_event;

/* 以下状态及定时回调均由 NimBLE 主机事件队列管理，不由额外应用任务直接读写。 */
/** 本机蓝牙地址类型，主机同步后确定。 */
static uint8_t s_own_address_type;
/** 当前连接标识，BLE_HS_CONN_HANDLE_NONE 表示未连接。 */
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_ota_phone = BLE_HS_CONN_HANDLE_NONE;
/** 对端通信特征的值句柄，发现前或断连后为 0。 */
static uint16_t s_characteristic_handle;
static gatt_write_t s_active_write;
static ble_gateway_status_t s_status;
static uint16_t s_requested_angle;
static bool s_angle_sent;
static ble_npl_time_t s_angle_deadline;
static struct ble_npl_callout s_angle_timer;
/** PING 单次定时器，发送回调中按一秒间隔重新调度。 */
static struct ble_npl_callout s_ping_timer;
/** 当前蓝牙模式启动失败时，延迟一秒再试。 */
static struct ble_npl_callout s_radio_retry;

static int gap_event(struct ble_gap_event *event, void *argument);
static int ota_gap_event(struct ble_gap_event *event, void *argument);
static void resume_radio(struct ble_npl_event *event);
static void scan(struct ble_npl_event *event);
static void try_write_angle(void);

static void publish_status(void)
{
    xQueueOverwrite(s_status_updates, &s_status);
}

bool ble_gateway_request_angle(uint32_t connection_id, uint16_t angle)
{
    const angle_request_t request = {.connection_id = connection_id, .angle = angle};
    if (angle > BLE_LINK_SERVO_MAX_ANGLE || xQueueSend(s_angle_requests, &request, 0) != pdTRUE)
    {
        return false;
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_angle_request_event);
    return true;
}

bool ble_gateway_take_status(ble_gateway_status_t *status)
{
    return xQueueReceive(s_status_updates, status, 0) == pdTRUE;
}

void ble_gateway_toggle_ota(void)
{
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_ota_toggle_event);
}

static void clear_link_status(void)
{
    ble_npl_callout_stop(&s_angle_timer);
    s_status.link = BLE_GATEWAY_SCANNING;
    s_status.angle_state = BLE_GATEWAY_ANGLE_IDLE;
    s_status.has_ack_angle = false;
    s_angle_sent = false;
    publish_status();
}

static void finish_angle_request(ble_gateway_angle_state_t state)
{
    ble_npl_callout_stop(&s_angle_timer);
    s_status.angle_state = state;
    s_angle_sent = false;
    publish_status();
}

static void on_angle_timeout(struct ble_npl_event *event)
{
    (void)event;
    /* 已投递的旧定时器事件可能迟到，按当前请求的截止时间判断。 */
    if (s_status.angle_state == BLE_GATEWAY_ANGLE_WAITING &&
        (int32_t)(ble_npl_time_get() - s_angle_deadline) >= 0)
    {
        ESP_LOGW(TAG, "Angle %u acknowledgement timed out", (unsigned int)s_requested_angle);
        finish_angle_request(BLE_GATEWAY_ANGLE_TIMEOUT);
    }
}

static void on_angle_requested(struct ble_npl_event *event)
{
    (void)event;
    angle_request_t request;
    if (xQueueReceive(s_angle_requests, &request, 0) != pdTRUE)
    {
        return;
    }
    if (s_status.link != BLE_GATEWAY_READY || request.connection_id != s_status.connection_id ||
        s_status.angle_state == BLE_GATEWAY_ANGLE_WAITING)
    {
        publish_status();
        return;
    }

    s_requested_angle = request.angle;
    s_angle_sent = false;
    const ble_npl_time_t timeout = ble_npl_time_ms_to_ticks32(ANGLE_TIMEOUT_MS);
    s_angle_deadline = ble_npl_time_get() + timeout;
    if (ble_npl_callout_reset(&s_angle_timer, timeout) != BLE_NPL_OK)
    {
        finish_angle_request(BLE_GATEWAY_ANGLE_FAILED);
        return;
    }
    s_status.angle_state = BLE_GATEWAY_ANGLE_WAITING;
    publish_status();
    try_write_angle();
}

/**
 * @brief 停止发送并请求断开当前连接；连接句柄由断连事件统一清理。
 */
static void close_connection(void)
{
    ble_npl_callout_stop(&s_ping_timer);
    clear_link_status();
    if (s_connection_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        /* 这里只表示断开请求是否提交成功，实际断开由 GAP 事件确认。 */
        int result = ble_gap_terminate(s_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (result != 0)
        {
            ESP_LOGW(TAG, "Could not terminate connection; status=%d", result);
        }
    }
}

/**
 * @brief 释放 GATT 写入位，若有触屏命令正在排队，则用空出的写入位提交。
 * @param connection_handle 此次写操作所属连接，用于排除非当前连接的回调。
 * @param error 写操作结果；status 为 0 不代表已收到角度回执或 PONG。
 * @param attribute 写操作对应的属性，本回调未使用。
 * @param argument 发起写操作时传入的用户参数，本回调未使用。
 * @return 始终返回 0；PING 错误触发重连，角度写入错误反馈到界面。
 */
static int on_command_written(uint16_t connection_handle, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attribute, void *argument)
{
    (void)attribute;
    (void)argument;
    if (s_status.ota_mode || connection_handle != s_connection_handle)
    {
        return 0;
    }

    const gatt_write_t completed_write = s_active_write;
    s_active_write = GATT_WRITE_NONE;
    if (error->status != 0)
    {
        ESP_LOGE(TAG, "Command write failed; status=%d", error->status);
        if (completed_write == GATT_WRITE_PING)
        {
            close_connection();
            return 0;
        }
        if (completed_write == GATT_WRITE_ANGLE && s_angle_sent)
        {
            finish_angle_request(BLE_GATEWAY_ANGLE_FAILED);
        }
    }
    try_write_angle();
    return 0;
}

static void try_write_angle(void)
{
    if (s_status.angle_state != BLE_GATEWAY_ANGLE_WAITING || s_angle_sent ||
        s_active_write != GATT_WRITE_NONE)
    {
        return;
    }
    if ((int32_t)(ble_npl_time_get() - s_angle_deadline) >= 0)
    {
        /* 等待 PING 写入也占用同一超时预算，超时后不再迟发旧命令。 */
        on_angle_timeout(NULL);
        return;
    }

    char request[BLE_LINK_MESSAGE_LENGTH];
    if (!ble_link_encode_angle(BLE_LINK_SERVO_SET, s_requested_angle, request))
    {
        finish_angle_request(BLE_GATEWAY_ANGLE_FAILED);
        return;
    }
    int result = ble_gattc_write_flat(s_connection_handle, s_characteristic_handle, request,
                                      sizeof(request), on_command_written, NULL);
    if (result != 0)
    {
        ESP_LOGE(TAG, "Could not send servo target; status=%d", result);
        finish_angle_request(BLE_GATEWAY_ANGLE_FAILED);
        return;
    }
    s_active_write = GATT_WRITE_ANGLE;
    s_angle_sent = true;
    ESP_LOGI(TAG, "TX: S%03u", (unsigned int)s_requested_angle);
}

/**
 * @brief 定时提交四字节 PING 写请求，并安排下一次发送。
 * @param event 定时器投递的事件，本回调未使用事件内容。
 * @note 前一次写请求未完成时跳过本轮；写入启动失败则停止发送并断开连接。
 */
static void send_ping(struct ble_npl_event *event)
{
    (void)event;
    if (s_status.link != BLE_GATEWAY_READY)
    {
        return;
    }

    /* ATT（Attribute Protocol，属性协议）写响应未返回前，不叠加提交下一次写请求。 */
    if (s_active_write == GATT_WRITE_NONE)
    {
        /* result 为请求提交结果，异步完成结果由 on_command_written() 接收。 */
        int result = ble_gattc_write_flat(s_connection_handle, s_characteristic_handle, PING,
                                          BLE_LINK_MESSAGE_LENGTH, on_command_written, NULL);
        if (result != 0)
        {
            ESP_LOGE(TAG, "Could not send PING; status=%d", result);
            close_connection();
            return;
        }
        s_active_write = GATT_WRITE_PING;
        ESP_LOGI(TAG, "TX: PING");
    }

    ble_npl_callout_reset(&s_ping_timer, ble_npl_time_ms_to_ticks32(1000));
}

/**
 * @brief 订阅成功后启用触屏下发，并启动 PING 定时器。
 * @param connection_handle 此次订阅操作所属连接。
 * @param error 订阅配置的写入结果，status 为 0 表示对端已确认。
 * @param attribute 写入的配置属性，本回调未使用。
 * @param argument 发起订阅时传入的用户参数，本回调未使用。
 * @return 始终返回 0；订阅失败时请求断开连接。
 */
static int on_subscribed(uint16_t connection_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attribute, void *argument)
{
    (void)attribute;
    (void)argument;
    if (s_status.ota_mode || connection_handle != s_connection_handle)
    {
        return 0;
    }
    if (error->status != 0)
    {
        ESP_LOGE(TAG, "Notification subscription failed; status=%d", error->status);
        close_connection();
        return 0;
    }

    /* 必须先确认通知开关已写入，才能接收角度设置回执和 PONG。 */
    ESP_LOGI(TAG, "BLE link ready; PONG notifications enabled");
    ++s_status.connection_id;
    s_status.link = BLE_GATEWAY_READY;
    publish_status();
    ble_npl_callout_reset(&s_ping_timer, ble_npl_time_ms_to_ticks32(1000));
    return 0;
}

/**
 * @brief 从发现缓存中取得通信特征及其通知开关，随后发起通知订阅。
 * @param peer 官方发现组件维护的对端记录，包含连接句柄及服务发现结果。
 * @param status 服务发现结果，0 表示成功。
 * @param argument 发起发现时传入的用户参数，本回调未使用。
 * @note 任一必要属性缺失时断开连接，不能仅凭连接成功就开始发送。
 */
static void on_discovery_complete(const struct peer *peer, int status, void *argument)
{
    (void)argument;
    if (s_status.ota_mode || peer->conn_handle != s_connection_handle)
    {
        return;
    }
    if (status != 0)
    {
        ESP_LOGE(TAG, "Service discovery failed; status=%d", status);
        close_connection();
        return;
    }

    /* characteristic 提供数据读写句柄，descriptor 提供该特征的通知配置句柄。 */
    const struct peer_chr *characteristic =
        peer_chr_find_uuid(peer, &SERVICE_UUID.u, &CHARACTERISTIC_UUID.u);
    const struct peer_dsc *descriptor =
        peer_dsc_find_uuid(peer, &SERVICE_UUID.u, &CHARACTERISTIC_UUID.u,
                           BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16));
    if (characteristic == NULL || descriptor == NULL)
    {
        ESP_LOGE(TAG, "Peer lacks the PING/PONG characteristic or notification descriptor");
        close_connection();
        return;
    }

    s_characteristic_handle = characteristic->chr.val_handle;
    /* CCCD（Client Characteristic Configuration Descriptor，客户端特征配置描述符）
     * 控制本连接的通知订阅；0x0001 按小端字节序写入，表示开启通知。
     */
    const uint8_t enable_notifications[] = {0x01, 0x00};
    int result =
        ble_gattc_write_flat(peer->conn_handle, descriptor->dsc.handle, enable_notifications,
                             sizeof(enable_notifications), on_subscribed, NULL);
    if (result != 0)
    {
        ESP_LOGE(TAG, "Could not subscribe to PONG; status=%d", result);
        close_connection();
    }
}

/**
 * @brief 在空闲状态启动持续扫描，失败时安排一秒后的重试。
 * @param event 重试定时器的事件；直接调用时可传 NULL，函数不读取此参数。
 * @note 已连接、正在连接或正在扫描时直接返回，避免重复启动 GAP 操作。
 */
static void scan(struct ble_npl_event *event)
{
    (void)event;
    if (s_status.ota_mode || s_connection_handle != BLE_HS_CONN_HANDLE_NONE ||
        ble_gap_conn_active() ||
        ble_gap_disc_active())
    {
        return;
    }

    /* 被动扫描只接收广播，不发送扫描请求；匹配所需的服务 UUID 已在广播包中。 */
    struct ble_gap_disc_params parameters = {0};
    parameters.passive = 1;
    parameters.filter_duplicates = 1; /* 过滤重复广播报告，减少主机处理量。 */
    // 注册 GAP 回调，扫描结果由 gap_event() 处理。
    int result = ble_gap_disc(s_own_address_type, BLE_HS_FOREVER, &parameters, gap_event, NULL);
    if (result != 0)
    {
        ESP_LOGW(TAG, "Scan failed; status=%d, retrying in 1 s", result);
        ble_npl_callout_reset(&s_radio_retry, ble_npl_time_ms_to_ticks32(1000));
        return;
    }
    ESP_LOGI(TAG, "Scanning for C3 PING/PONG service");
    ota_update_ble_ready();
}

static int advertise_ota(void)
{
    ble_uuid16_t ota_uuid = BLE_UUID16_INIT(OTA_SERVICE_UUID16);
    const struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .name = (const uint8_t *)DEVICE_NAME,
        .name_len = sizeof(DEVICE_NAME) - 1,
        .name_is_complete = 1,
        .uuids16 = &ota_uuid,
        .num_uuids16 = 1,
        .uuids16_is_complete = 1,
    };
    int result = ble_gap_adv_set_fields(&fields);
    if (result == 0)
    {
        const struct ble_gap_adv_params parameters = {
            .conn_mode = BLE_GAP_CONN_MODE_UND,
            .disc_mode = BLE_GAP_DISC_MODE_GEN,
        };
        result = ble_gap_adv_start(s_own_address_type, NULL, BLE_HS_FOREVER, &parameters,
                                   ota_gap_event, NULL);
    }
    if (result == 0)
    {
        ESP_LOGI(TAG, "OTA advertising as %s", DEVICE_NAME);
        ota_update_ble_ready();
    }
    return result;
}

/* 先等旧连接的 GAP 断开事件，再启动当前模式，确保单连接可用。 */
static void resume_radio(struct ble_npl_event *event)
{
    (void)event;
    if (!ble_hs_synced())
    {
        return;
    }

    int result = 0;
    if (s_status.ota_mode)
    {
        if (s_connection_handle != BLE_HS_CONN_HANDLE_NONE)
        {
            result = ble_gap_terminate(s_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        else if (ble_gap_conn_active())
        {
            result = ble_gap_conn_cancel();
        }
        else if (s_ota_phone == BLE_HS_CONN_HANDLE_NONE && !ble_gap_adv_active())
        {
            if (ble_gap_disc_active())
            {
                result = ble_gap_disc_cancel();
            }
            if (result == 0)
            {
                result = advertise_ota();
            }
        }
    }
    else if (s_ota_phone != BLE_HS_CONN_HANDLE_NONE)
    {
        result = ble_gap_terminate(s_ota_phone, BLE_ERR_REM_USER_CONN_TERM);
    }
    else
    {
        if (ble_gap_adv_active())
        {
            result = ble_gap_adv_stop();
        }
        if (result == 0)
        {
            scan(NULL);
        }
    }

    if (result != 0 && result != BLE_HS_EALREADY)
    {
        ESP_LOGW(TAG, "Could not switch BLE mode; status=%d, retrying in 1 s", result);
        ble_npl_callout_reset(&s_radio_retry, ble_npl_time_ms_to_ticks32(1000));
    }
}

static void toggle_ota_mode(struct ble_npl_event *event)
{
    (void)event;
    s_status.ota_mode = !s_status.ota_mode;
    if (!s_status.ota_mode)
    {
        ota_update_link_reset();
    }
    ble_npl_callout_stop(&s_radio_retry);
    ble_npl_callout_stop(&s_ping_timer);
    clear_link_status();
    resume_radio(NULL);
}

static int ota_gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    if (event->type != BLE_GAP_EVENT_CONNECT || s_status.ota_mode)
    {
        ota_update_gap_event(event);
    }

    switch (event->type)
    {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0)
            {
                s_ota_phone = event->connect.conn_handle;
            }
            resume_radio(NULL);
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            s_ota_phone = BLE_HS_CONN_HANDLE_NONE;
            resume_radio(NULL);
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            resume_radio(NULL);
            break;
        default:
            break;
    }
    return 0;
}

/**
 * @brief 检查可连接广播中的服务 UUID，匹配后停止扫描并发起连接。
 * @param advertisement 当前收到的广播报告，包含广播类型、数据及对端地址。
 * @note 按服务标识筛选，不按设备名称或固定地址识别设备。
 */
static void connect_if_target(const struct ble_gap_disc_desc *advertisement)
{
    if (s_status.ota_mode || advertisement->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND)
    {
        return;
    }

    /* 将原始广播数据解析成字段视图，供下方遍历其中的 128 位服务 UUID。 */
    struct ble_hs_adv_fields fields;
    if (ble_hs_adv_parse_fields(&fields, advertisement->data, advertisement->length_data) != 0)
    {
        return;
    }

    for (int index = 0; index < fields.num_uuids128; ++index)
    {
        if (ble_uuid_cmp(&fields.uuids128[index].u, &SERVICE_UUID.u) != 0)
        {
            continue;
        }

        /* 沿用官方 blecent 流程：先停止扫描，再启动连接，避免 GAP 操作冲突。 */
        int result = ble_gap_disc_cancel();
        if (result != 0)
        {
            return;
        }
        /* 连接过程的超时为 30000 毫秒，完成结果由 gap_event() 接收。 */
        result =
            ble_gap_connect(s_own_address_type, &advertisement->addr, 30000, NULL, gap_event, NULL);
        if (result != 0)
        {
            ESP_LOGW(TAG, "Could not start connection; status=%d", result);
            ble_npl_callout_reset(&s_radio_retry, ble_npl_time_ms_to_ticks32(1000));
        }
        else
        {
            s_status.link = BLE_GATEWAY_CONNECTING;
            publish_status();
        }
        return;
    }
}

/**
 * @brief 统一处理扫描报告、连接、断连和收到通知等 GAP 事件。
 * @param event 事件类型及其对应数据，只读取与 type 匹配的成员。
 * @param argument 注册 GAP 回调时传入的用户参数，本回调未使用。
 * @return 始终返回 0，表示本回调已处理或忽略该事件。
 * @note 回调运行在主机事件队列中，不阻塞等待连接或收发完成。
 */
static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    int result; /* 对端缓存添加、服务发现启动等操作的即时返回码，0 表示成功。 */

    switch (event->type)
    {
        case BLE_GAP_EVENT_DISC:
            connect_if_target(&event->disc);
            return 0;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0)
            {
                ESP_LOGW(TAG, "Connection failed; status=%d", event->connect.status);
                clear_link_status();
                ble_npl_callout_reset(&s_radio_retry, ble_npl_time_ms_to_ticks32(1000));
                return 0;
            }

            s_connection_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected; handle=%u", s_connection_handle);
            if (s_status.ota_mode)
            {
                resume_radio(NULL);
                return 0;
            }
            /* 先登记对端，再异步发现目标服务；完成后才订阅通知。 */
            result = peer_add(s_connection_handle);
            if (result == 0)
            {
                result = peer_disc_svc_by_uuid(s_connection_handle, &SERVICE_UUID.u,
                                               on_discovery_complete, NULL);
            }
            if (result != 0)
            {
                ESP_LOGE(TAG, "Could not discover peer; status=%d", result);
                close_connection();
            }
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "Disconnected; reason=%d", event->disconnect.reason);
            /* 清理上一条连接的缓存、句柄及写入状态，再允许重新扫描。 */
            ble_npl_callout_stop(&s_ping_timer);
            peer_delete(event->disconnect.conn.conn_handle);
            s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
            s_characteristic_handle = 0;
            s_active_write = GATT_WRITE_NONE;
            clear_link_status();
            resume_radio(NULL);
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (!s_status.ota_mode && event->notify_rx.conn_handle == s_connection_handle &&
                event->notify_rx.attr_handle == s_characteristic_handle)
            {
                /* 响应是四字节原始数据，不含 '\0'，因此按长度比较而非使用 strcmp。 */
                char response[BLE_LINK_MESSAGE_LENGTH];
                uint16_t angle;
                if (OS_MBUF_PKTLEN(event->notify_rx.om) != sizeof(response) ||
                    ble_hs_mbuf_to_flat(event->notify_rx.om, response, sizeof(response), NULL) != 0)
                {
                    ESP_LOGW(TAG, "Malformed notification");
                }
                else if (memcmp(response, BLE_LINK_PONG, sizeof(response)) == 0)
                {
                    ESP_LOGI(TAG, "RX: PONG");
                }
                else if (response[0] == BLE_LINK_SERVO_ACK &&
                         ble_link_decode_angle(response, sizeof(response), &angle))
                {
                    ESP_LOGI(TAG, "RX: A%03u; C3 PWM configured (position not measured)",
                             (unsigned int)angle);
                    if (s_angle_sent && angle == s_requested_angle)
                    {
                        s_status.last_ack_angle = angle;
                        s_status.has_ack_angle = true;
                        finish_angle_request(BLE_GATEWAY_ANGLE_IDLE);
                    }
                }
                else
                {
                    ESP_LOGW(TAG, "Unexpected notification; expected PONG or Addd");
                }
            }
            return 0;

        default:
            return 0;
    }
}

/**
 * @brief 主机异常复位时停止定时器，并清理失效的连接状态和发现缓存。
 * @param reason NimBLE 上报的复位原因码，用于日志诊断。
 * @note 此处不启动扫描，等待主机重新同步后由 on_sync() 恢复。
 */
static void on_reset(int reason)
{
    ble_npl_callout_stop(&s_ping_timer);
    ble_npl_callout_stop(&s_radio_retry);
    if (s_connection_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        peer_delete(s_connection_handle);
    }
    s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
    s_ota_phone = BLE_HS_CONN_HANDLE_NONE;
    ota_update_link_reset();
    s_characteristic_handle = 0;
    s_active_write = GATT_WRITE_NONE;
    clear_link_status();
    ESP_LOGE(TAG, "NimBLE reset; reason=%d", reason);
}

/**
 * @brief 主机与控制器同步后准备本机地址，并开始扫描 C3。
 * @note 地址准备成功后才能发起 GAP 操作；地址类型推断时不启用地址隐私。
 */
static void on_sync(void)
{
    /* 地址准备和地址类型推断的结果，0 表示成功。 */
    int result = ble_hs_util_ensure_addr(0);
    assert(result == 0);
    result = ble_hs_id_infer_auto(0, &s_own_address_type);
    assert(result == 0);
    resume_radio(NULL);
}

/** @brief 初始化网关资源，具体调用时机见 ble_gateway.h。 */
esp_err_t ble_gateway_init(void)
{
    s_angle_requests = xQueueCreate(1, sizeof(angle_request_t));
    s_status_updates = xQueueCreate(1, sizeof(ble_gateway_status_t));
    if (s_angle_requests == NULL || s_status_updates == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_svc_gap_device_name_set(DEVICE_NAME) != 0)
    {
        return ESP_FAIL;
    }

    /* 发现缓存最多容纳 1 个对端、4 个服务、8 个特征和 8 个描述符。
     * 启用包含服务发现时，还需配置最多 4 个包含服务条目；按 SDK 配置选择接口签名。
     */
#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    int result = peer_init(1, 4, 4, 8, 8);
#else
    int result = peer_init(1, 4, 8, 8);
#endif
    assert(result == 0);
    /* 角度命令和定时器统一投递到主机队列，与 GATT 回调串行执行。 */
    struct ble_npl_eventq *host_queue = nimble_port_get_dflt_eventq();
    ble_npl_event_init(&s_angle_request_event, on_angle_requested, NULL);
    ble_npl_event_init(&s_ota_toggle_event, toggle_ota_mode, NULL);
    ble_npl_callout_init(&s_angle_timer, host_queue, on_angle_timeout, NULL);
    ble_npl_callout_init(&s_radio_retry, host_queue, resume_radio, NULL);
    ble_npl_callout_init(&s_ping_timer, host_queue, send_ping, NULL);
    publish_status();
    ESP_LOGI(TAG, "Gateway initialized; PING interval is 1 s");
    return ESP_OK;
}
