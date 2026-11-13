#ifndef BLE_GATEWAY_H
#define BLE_GATEWAY_H

/**
 * @brief 配置 S3 网关的蓝牙回调、对端发现缓存和收发定时器。
 * @note 在 nimble_port_init() 成功后、启动主机任务前调用一次。
 *       扫描由主机与控制器同步完成后的回调启动，本函数不直接连接设备。
 */
void ble_gateway_init(void);

#endif
