idf_component_register(SRCS "main.c"
                            "audio.c"
                            "wifi.c"
                            "fonts/font_ua_16.c"
                            "fonts/font_ua_20.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_wifi esp_http_client
                                     esp-tls esp_driver_i2s esp_driver_gpio
                                     esp_driver_ledc esp_driver_spi esp_lcd
                                     esp_timer)
