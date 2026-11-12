idf_component_register(SRCS "main.c" "ble_gateway.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES bt nvs_flash nimble_central_utils)
