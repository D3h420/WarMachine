idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_wifi esp_event nvs_flash fatfs sdmmc esp_driver_uart esp_driver_spi esp_driver_gpio esp_driver_sdspi)
