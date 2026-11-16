idf_component_register(SRCS "tesis_SAPC.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_adc esp_wifi nvs_flash esp_event esp_netif)