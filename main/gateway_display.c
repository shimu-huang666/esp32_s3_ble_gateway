/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Board pinout and ST7789T timings follow Waveshare's V1/V2 ESP-IDF examples.
 */

#include "gateway_display.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_touch_cst328.h"
#include "esp_lcd_touch_cst3530.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LCD_WIDTH 240
#define LCD_HEIGHT 320
#define LCD_DRAW_ROWS 24
#define LCD_HOST SPI3_HOST

static const char *TAG = "gateway_display";
static const gpio_num_t LCD_BACKLIGHT = GPIO_NUM_5;
static const gpio_num_t TOUCH_RESET = GPIO_NUM_2;

static esp_err_t configure_panel(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "LCD reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "LCD init");

    /* V1/V2 面板的 RGB565 字节顺序、供电及灰阶参数，来自微雪 Vernon_ST7789T 例程。 */
    static const struct
    {
        uint8_t command;
        uint8_t size;
        uint8_t data[14];
    } settings[] = {
        {0xB0, 2, {0x00, 0xE8}},
        {0xB2, 5, {0x0C, 0x0C, 0x00, 0x33, 0x33}},
        {0xB7, 1, {0x75}},
        {0xBB, 1, {0x1A}},
        {0xC0, 1, {0x80}},
        {0xC2, 2, {0x01, 0xFF}},
        {0xC3, 1, {0x13}},
        {0xC4, 1, {0x20}},
        {0xC6, 1, {0x0F}},
        {0xD0, 2, {0xA4, 0xA1}},
        {0xE0,
         14,
         {0xD0, 0x0D, 0x14, 0x0D, 0x0D, 0x09, 0x38, 0x44, 0x4E, 0x3A, 0x17, 0x18, 0x2F, 0x30}},
        {0xE1,
         14,
         {0xD0, 0x09, 0x0F, 0x08, 0x07, 0x14, 0x37, 0x44, 0x4D, 0x38, 0x15, 0x16, 0x2C, 0x2E}},
    };
    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); ++i)
    {
        ESP_RETURN_ON_ERROR(
            esp_lcd_panel_io_tx_param(io, settings[i].command, settings[i].data, settings[i].size),
            TAG, "LCD setup command");
    }
    return esp_lcd_panel_invert_color(panel, true);
}

static esp_err_t create_touch(i2c_master_bus_handle_t bus, esp_lcd_panel_io_handle_t *io,
                              esp_lcd_touch_handle_t *touch)
{
    /* 先释放触控复位再探测 V1。V2 驱动会按其时序复位并读取芯片 ID。 */
    ESP_RETURN_ON_ERROR(gpio_set_level(TOUCH_RESET, 1), TAG, "Touch reset level");
    ESP_RETURN_ON_ERROR(gpio_set_direction(TOUCH_RESET, GPIO_MODE_OUTPUT), TAG, "Touch reset pin");
    vTaskDelay(pdMS_TO_TICKS(50));

    const bool use_cst328 =
        i2c_master_probe(bus, ESP_LCD_TOUCH_IO_I2C_CST328_ADDRESS, 30) == ESP_OK;
    const esp_lcd_panel_io_i2c_config_t io_config =
        use_cst328 ? (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_CST328_CONFIG()
                   : (esp_lcd_panel_io_i2c_config_t)ESP_LCD_TOUCH_IO_I2C_CST3530_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_config, io), TAG, "Touch I2C IO");

    /* CST328 驱动最后输出 levels.reset，低有效的 RSTn 需要保持高电平。 */
    const esp_lcd_touch_config_t config = {
        .x_max = LCD_WIDTH,
        .y_max = LCD_HEIGHT,
        .rst_gpio_num = TOUCH_RESET,
        .int_gpio_num = GPIO_NUM_4,
        .levels = {.reset = use_cst328 ? 1 : 0, .interrupt = 0},
        .driver_data = use_cst328 ? NULL : bus,
    };
    const esp_err_t result = use_cst328 ? esp_lcd_touch_new_i2c_cst328(*io, &config, touch)
                                        : esp_lcd_touch_new_i2c_cst3530(*io, &config, touch);
    if (result == ESP_OK)
    {
        ESP_LOGI(TAG, "Touch controller: %s", use_cst328 ? "CST328" : "CST3530");
    }
    return result;
}

esp_err_t gateway_display_init(lv_display_t **display)
{
    esp_err_t ret;
    i2c_master_bus_handle_t touch_bus = NULL;
    esp_lcd_panel_io_handle_t touch_io = NULL;
    esp_lcd_touch_handle_t touch = NULL;
    esp_lcd_panel_io_handle_t lcd_io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    lv_display_t *lv_display = NULL;
    lv_indev_t *lv_touch = NULL;
    bool port_started = false;

    ESP_RETURN_ON_ERROR(gpio_set_direction(LCD_BACKLIGHT, GPIO_MODE_OUTPUT), TAG, "Backlight pin");
    ESP_RETURN_ON_ERROR(gpio_set_level(LCD_BACKLIGHT, 0), TAG, "Backlight off");

    const spi_bus_config_t spi_config = {
        .sclk_io_num = GPIO_NUM_40,
        .mosi_io_num = GPIO_NUM_45,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_DRAW_ROWS * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &spi_config, SPI_DMA_CH_AUTO), TAG, "LCD SPI");
    const esp_lcd_panel_io_spi_config_t lcd_io_config = {
        .cs_gpio_num = GPIO_NUM_42,
        .dc_gpio_num = GPIO_NUM_41,
        .pclk_hz = 40 * 1000 * 1000,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 4,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi(LCD_HOST, &lcd_io_config, &lcd_io), fail, TAG,
                      "LCD SPI IO");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_39,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io, &panel_config, &panel), fail, TAG, "ST7789");
    ESP_GOTO_ON_ERROR(configure_panel(lcd_io, panel), fail, TAG, "LCD setup");

    const i2c_master_bus_config_t touch_bus_config = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_NUM_1,
        .scl_io_num = GPIO_NUM_3,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_GOTO_ON_ERROR(i2c_new_master_bus(&touch_bus_config, &touch_bus), fail, TAG, "Touch I2C");
    ESP_GOTO_ON_ERROR(create_touch(touch_bus, &touch_io, &touch), fail, TAG, "Touch init");

    const lvgl_port_cfg_t port_config = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_GOTO_ON_ERROR(lvgl_port_init(&port_config), fail, TAG, "LVGL port");
    port_started = true;

    /* 仅分配 24 行的 DMA 绘制缓冲；ST7789 已配置小端 RGB565，无需再交换字节。 */
    const lvgl_port_display_cfg_t display_config = {
        .io_handle = lcd_io,
        .panel_handle = panel,
        .buffer_size = LCD_WIDTH * LCD_DRAW_ROWS,
        .hres = LCD_WIDTH,
        .vres = LCD_HEIGHT,
        .rotation.mirror_x = true,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags.buff_dma = true,
    };
    lv_display = lvgl_port_add_disp(&display_config);
    ESP_GOTO_ON_FALSE(lv_display, ESP_ERR_NO_MEM, fail, TAG, "LVGL display");
    const lvgl_port_touch_cfg_t touch_config = {.disp = lv_display, .handle = touch};
    lv_touch = lvgl_port_add_touch(&touch_config);
    ESP_GOTO_ON_FALSE(lv_touch, ESP_ERR_NO_MEM, fail, TAG, "LVGL touch");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), fail, TAG, "LCD on");
    ESP_GOTO_ON_ERROR(gpio_set_level(LCD_BACKLIGHT, 1), fail, TAG, "Backlight on");
    *display = lv_display;
    return ESP_OK;

fail:
    if (lv_touch)
    {
        lvgl_port_remove_touch(lv_touch);
    }
    if (lv_display)
    {
        lvgl_port_remove_disp(lv_display);
    }
    if (port_started)
    {
        lvgl_port_deinit();
    }
    if (touch)
    {
        esp_lcd_touch_del(touch);
    }
    if (touch_io)
    {
        esp_lcd_panel_io_del(touch_io);
    }
    if (touch_bus)
    {
        i2c_del_master_bus(touch_bus);
    }
    if (panel)
    {
        esp_lcd_panel_del(panel);
    }
    if (lcd_io)
    {
        esp_lcd_panel_io_del(lcd_io);
    }
    spi_bus_free(LCD_HOST);
    return ret;
}
