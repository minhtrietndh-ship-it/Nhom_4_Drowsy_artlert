idf_component_register(SRCS "backup.c" "i2c_lcd.c" "blink_example_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES 
                            freertos
                            nvs_flash
                            esp_wifi
                            esp_event
                            esp_netif
                            mqtt
                            driver
                            log
                            spiffs
                            )
