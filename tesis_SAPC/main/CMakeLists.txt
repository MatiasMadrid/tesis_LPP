idf_component_register(SRCS "tesis_SAPC.c" "wifi_portal.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "portal.html"
                       PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash
                                     esp_http_server esp_adc esp_driver_gpio
                                     mqtt lwip)