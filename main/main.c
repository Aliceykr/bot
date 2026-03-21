#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "my_demo.h"

static void lvgl_tick_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5));
        lv_tick_inc(5);
    }
}

static void lvgl_task(void *arg)
{
    my_demo();

    // 绑定编码器到列表的 group
    lv_group_t *group = my_demo_get_group();
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            lv_indev_set_group(indev, group);
            break;
        }
        indev = lv_indev_get_next(indev);
    }

    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void)
{
    LCD_Init();

    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();

    xTaskCreate(lvgl_tick_task, "lv_tick", 2048,  NULL, 5, NULL);
    xTaskCreate(lvgl_task,      "lv_task", 32768, NULL, 4, NULL);
}
