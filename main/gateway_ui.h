#ifndef GATEWAY_UI_H
#define GATEWAY_UI_H

#include "esp_err.h"

/** 启动网关的触控界面；调用前须完成 ble_gateway_init()。 */
esp_err_t gateway_ui_init(void);

#endif
