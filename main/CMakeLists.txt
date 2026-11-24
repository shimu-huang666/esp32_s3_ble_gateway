idf_component_register(SRCS "main.c" "ble_gateway.c" "gateway_display.c" "gateway_ui.c" "ota_update.c"
                            "fonts/gateway_font_16.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash nimble_central_utils esp_lcd esp_driver_gpio
                                     esp_driver_i2c esp_driver_spi app_update esp_app_format)
