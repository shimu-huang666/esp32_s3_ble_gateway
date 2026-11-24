#ifndef BLE_GATEWAY_H
#define BLE_GATEWAY_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum
{
    BLE_GATEWAY_SCANNING,
    BLE_GATEWAY_CONNECTING,
    BLE_GATEWAY_READY,
} ble_gateway_link_state_t;

typedef enum
{
    BLE_GATEWAY_ANGLE_IDLE,
    BLE_GATEWAY_ANGLE_WAITING,
    BLE_GATEWAY_ANGLE_TIMEOUT,
    BLE_GATEWAY_ANGLE_FAILED,
} ble_gateway_angle_state_t;

typedef struct
{
    uint32_t connection_id;
    ble_gateway_link_state_t link;
    ble_gateway_angle_state_t angle_state;
    uint16_t last_ack_angle;
    bool has_ack_angle;
    bool ota_mode;
} ble_gateway_status_t;

/**
 * @brief 配置 S3 网关的蓝牙回调、对端发现缓存和收发定时器。
 * @note 在 nimble_port_init() 成功后、启动主机任务前调用一次。
 *       扫描由主机与控制器同步完成后的回调启动，本函数不直接连接设备。
 */
esp_err_t ble_gateway_init(void);

/** 提交触屏选定的目标角度。连接编号来自最近的状态，避免在重连后重放旧命令。 */
bool ble_gateway_request_angle(uint32_t connection_id, uint16_t angle);

/** 取出最新状态。由 LVGL 任务读取，返回 false 时界面无需刷新。 */
bool ble_gateway_take_status(ble_gateway_status_t *status);

/** 切换手机升级模式。断开和恢复连接由 NimBLE 主机事件队列处理。 */
void ble_gateway_toggle_ota(void);

#endif
