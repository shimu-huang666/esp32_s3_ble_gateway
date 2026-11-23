#ifndef GATEWAY_DISPLAY_H
#define GATEWAY_DISPLAY_H

#include "esp_err.h"
#include "lvgl.h"

/** 初始化微雪 ESP32-S3-Touch-LCD-2.8 的竖屏和触控，返回已注册的 LVGL 显示器。 */
esp_err_t gateway_display_init(lv_display_t **display);

#endif
