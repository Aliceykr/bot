idf_component_register(SRCS "main.c"
                          "../mylvgl/lv_port_disp.c"
                          "../mylvgl/my_demo.c"
                          "../user/lcd.c"
                    INCLUDE_DIRS "."
                                 "../user"
                                 ".."
                                 "../mylvgl"
                    REQUIRES lvgl driver)