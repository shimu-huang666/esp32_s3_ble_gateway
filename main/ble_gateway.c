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

#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "ble_gateway.h"
#include "ble_protocol.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "esp_central.h"
#include "nimble/nimble_port.h"

static const char *TAG = "ble_gateway";
static const ble_uuid128_t SERVICE_UUID = BLE_LINK_SERVICE_UUID;
static const ble_uuid128_t CHARACTERISTIC_UUID = BLE_LINK_CHARACTERISTIC_UUID;
static const char PING[] = BLE_LINK_PING;

/* All state below is owned by the NimBLE host event queue, including timers. */
static uint8_t s_own_address_type;
static uint16_t s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_characteristic_handle;
static bool s_write_pending;
static struct ble_npl_callout s_ping_timer;
static struct ble_npl_callout s_scan_retry;

static int gap_event(struct ble_gap_event *event, void *argument);
static void scan(struct ble_npl_event *event);

static void close_connection(void)
{
    ble_npl_callout_stop(&s_ping_timer);
    if (s_connection_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        int result = ble_gap_terminate(s_connection_handle, BLE_ERR_REM_USER_CONN_TERM);
        if (result != 0)
        {
            ESP_LOGW(TAG, "Could not terminate connection; status=%d", result);
        }
    }
}

static int on_ping_written(uint16_t connection_handle, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attribute, void *argument)
{
    (void)attribute;
    (void)argument;
    if (connection_handle != s_connection_handle)
    {
        return 0;
    }

    s_write_pending = false;
    if (error->status != 0)
    {
        ESP_LOGE(TAG, "PING write failed; status=%d", error->status);
        close_connection();
    }
    return 0;
}

static void send_ping(struct ble_npl_event *event)
{
    (void)event;
    if (s_connection_handle == BLE_HS_CONN_HANDLE_NONE || s_characteristic_handle == 0)
    {
        return;
    }

    /* Do not queue a second write while the previous ATT request is pending. */
    if (!s_write_pending)
    {
        int result = ble_gattc_write_flat(s_connection_handle, s_characteristic_handle, PING,
                                          BLE_LINK_MESSAGE_LENGTH, on_ping_written, NULL);
        if (result != 0)
        {
            ESP_LOGE(TAG, "Could not send PING; status=%d", result);
            close_connection();
            return;
        }
        s_write_pending = true;
        ESP_LOGI(TAG, "TX: PING");
    }

    ble_npl_callout_reset(&s_ping_timer, ble_npl_time_ms_to_ticks32(1000));
}

static int on_subscribed(uint16_t connection_handle, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attribute, void *argument)
{
    (void)attribute;
    (void)argument;
    if (connection_handle != s_connection_handle)
    {
        return 0;
    }
    if (error->status != 0)
    {
        ESP_LOGE(TAG, "Notification subscription failed; status=%d", error->status);
        close_connection();
        return 0;
    }

    /* Start traffic only after the server acknowledges its CCCD write. */
    ESP_LOGI(TAG, "BLE link ready; PONG notifications enabled");
    ble_npl_callout_reset(&s_ping_timer, ble_npl_time_ms_to_ticks32(1000));
    return 0;
}

static void on_discovery_complete(const struct peer *peer, int status, void *argument)
{
    (void)argument;
    if (peer->conn_handle != s_connection_handle)
    {
        return;
    }
    if (status != 0)
    {
        ESP_LOGE(TAG, "Service discovery failed; status=%d", status);
        close_connection();
        return;
    }

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
    /* CCCD is the client-side notification switch: 0x0001, little-endian. */
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

static void scan(struct ble_npl_event *event)
{
    (void)event;
    if (s_connection_handle != BLE_HS_CONN_HANDLE_NONE || ble_gap_conn_active() ||
        ble_gap_disc_active())
    {
        return;
    }

    struct ble_gap_disc_params parameters = {0};
    parameters.passive = 1;
    parameters.filter_duplicates = 1;
    int result = ble_gap_disc(s_own_address_type, BLE_HS_FOREVER, &parameters, gap_event, NULL);
    if (result != 0)
    {
        ESP_LOGW(TAG, "Scan failed; status=%d, retrying in 1 s", result);
        ble_npl_callout_reset(&s_scan_retry, ble_npl_time_ms_to_ticks32(1000));
        return;
    }
    ESP_LOGI(TAG, "Scanning for C3 PING/PONG service");
}

static void connect_if_target(const struct ble_gap_disc_desc *advertisement)
{
    if (advertisement->event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND)
    {
        return;
    }

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

        /* Follow blecent: cancel scanning before starting a connection. */
        int result = ble_gap_disc_cancel();
        if (result != 0)
        {
            return;
        }
        result =
            ble_gap_connect(s_own_address_type, &advertisement->addr, 30000, NULL, gap_event, NULL);
        if (result != 0)
        {
            ESP_LOGW(TAG, "Could not start connection; status=%d", result);
            ble_npl_callout_reset(&s_scan_retry, ble_npl_time_ms_to_ticks32(1000));
        }
        return;
    }
}

static int gap_event(struct ble_gap_event *event, void *argument)
{
    (void)argument;
    int result;

    switch (event->type)
    {
        case BLE_GAP_EVENT_DISC:
            connect_if_target(&event->disc);
            return 0;

        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0)
            {
                ESP_LOGW(TAG, "Connection failed; status=%d", event->connect.status);
                ble_npl_callout_reset(&s_scan_retry, ble_npl_time_ms_to_ticks32(1000));
                return 0;
            }

            s_connection_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected; handle=%u", s_connection_handle);
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
            ble_npl_callout_stop(&s_ping_timer);
            peer_delete(event->disconnect.conn.conn_handle);
            s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
            s_characteristic_handle = 0;
            s_write_pending = false;
            scan(NULL);
            return 0;

        case BLE_GAP_EVENT_NOTIFY_RX:
            if (event->notify_rx.conn_handle == s_connection_handle &&
                event->notify_rx.attr_handle == s_characteristic_handle)
            {
                char response[BLE_LINK_MESSAGE_LENGTH];
                if (OS_MBUF_PKTLEN(event->notify_rx.om) == sizeof(response) &&
                    ble_hs_mbuf_to_flat(event->notify_rx.om, response, sizeof(response), NULL) ==
                        0 &&
                    memcmp(response, BLE_LINK_PONG, sizeof(response)) == 0)
                {
                    ESP_LOGI(TAG, "RX: PONG");
                }
                else
                {
                    ESP_LOGW(TAG, "Unexpected notification; expected four-byte PONG");
                }
            }
            return 0;

        default:
            return 0;
    }
}

static void on_reset(int reason)
{
    ble_npl_callout_stop(&s_ping_timer);
    ble_npl_callout_stop(&s_scan_retry);
    if (s_connection_handle != BLE_HS_CONN_HANDLE_NONE)
    {
        peer_delete(s_connection_handle);
    }
    s_connection_handle = BLE_HS_CONN_HANDLE_NONE;
    s_characteristic_handle = 0;
    s_write_pending = false;
    ESP_LOGE(TAG, "NimBLE reset; reason=%d", reason);
}

static void on_sync(void)
{
    int result = ble_hs_util_ensure_addr(0);
    assert(result == 0);
    result = ble_hs_id_infer_auto(0, &s_own_address_type);
    assert(result == 0);
    scan(NULL);
}

void ble_gateway_init(void)
{
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

#if MYNEWT_VAL(BLE_INCL_SVC_DISCOVERY) || MYNEWT_VAL(BLE_GATT_CACHING_INCLUDE_SERVICES)
    int result = peer_init(1, 4, 4, 8, 8);
#else
    int result = peer_init(1, 4, 8, 8);
#endif
    assert(result == 0);
    ble_npl_callout_init(&s_ping_timer, nimble_port_get_dflt_eventq(), send_ping, NULL);
    ble_npl_callout_init(&s_scan_retry, nimble_port_get_dflt_eventq(), scan, NULL);
    ESP_LOGI(TAG, "Gateway initialized; PING interval is 1 s");
}
