idf_component_register(SRCS "main.c"
                          "../mylvgl/lv_port_disp.c"
                          "../mylvgl/lv_port_indev.c"
                          "../mylvgl/my_demo.c"
                          "../user/lcd.c"
                          "../user/wifi.c"
                          "../user/weather.c"
                          "../user/sntp_time.c"
                          "../user/model.c"
                          "../user/asr.c"
                          "../user/speaker.c"
                          "../user/tts.c"
                          "../user/lv_font_simhei_16.c"
                    INCLUDE_DIRS "."
                                 "../user"
                                 ".."
                                 "../mylvgl"
                    REQUIRES lvgl driver esp_wifi nvs_flash esp_http_client json mbedtls esp_netif esp_mm
                             )

target_compile_definitions(__idf_main PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1 LV_CONF_INCLUDE_SIMPLE=1)