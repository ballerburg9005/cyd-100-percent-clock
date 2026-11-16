idf_component_register(
    SRCS "main.c" "screen.c" "media.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer
             esp_http_server esp_lcd driver lwip fatfs sdmmc esp_driver_spi esp_driver_sdspi
)
