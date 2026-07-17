idf_component_register(SRCS "main.c"
                            "netcfg.c"
                            "settings.c"
                            "battery.c"
                            "led.c"
                            "ble_core.c"
                            "audio.c"
                            "app_wifi.c"
                            "app_weather.c"
                            "app_radio.c"
                            "app_news.c"
                            "app_settings.c"
                            "app_gemini.c"
                            "app_ble.c"
                            "app_wifitools.c"
                            "app_sdprobe.c"
                            "app_pet.c"
                            "app_pipboy.c"
                            "app_calendar.c"
                            "app_calc.c"
                            "app_modules.c"
                            "app_light.c"
                            "fonts/font_ua_16.c"
                            "fonts/font_ua_20.c"
                            "fonts/font_num_34.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES nvs_flash esp_wifi esp_http_client
                                     esp-tls json bt esp_adc esp_driver_i2c
                                     esp_driver_gpio
                                     esp_driver_ledc esp_driver_spi esp_lcd
                                     esp_driver_i2s esp_timer
                                     spi_flash app_update)

# Дозволяємо перевизначити ieee80211_raw_frame_sanity_check (сильний символ у
# libnet80211) власною версією у netcfg.c — потрібно для інʼєкції deauth-кадрів.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--allow-multiple-definition")
