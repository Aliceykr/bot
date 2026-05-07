#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lcd.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "my_demo.h"
#include "speaker.h"

/* 在 PSRAM 上创建任务：栈分配于 SPIRAM，TCB 必须在内部 DRAM */
static TaskHandle_t xTaskCreatePSRAM(TaskFunction_t pxTaskCode,
                                     const char *pcName,
                                     uint32_t ulStackDepth,
                                     void *pvParameters,
                                     UBaseType_t uxPriority)
{
    StackType_t *stack = heap_caps_malloc(ulStackDepth * sizeof(StackType_t),
                                          MALLOC_CAP_SPIRAM);
    if (!stack) {
        ESP_LOGE("PSRAM_TASK", "stack alloc failed %u", (unsigned)ulStackDepth);
        return NULL;
    }
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t),
                                          MALLOC_CAP_INTERNAL);
    if (!tcb) {
        ESP_LOGE("PSRAM_TASK", "TCB alloc failed");
        free(stack);
        return NULL;
    }
    TaskHandle_t handle = xTaskCreateStatic(pxTaskCode, pcName,
                                            ulStackDepth, pvParameters,
                                            uxPriority, stack, tcb);
    return handle;
}

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
    speaker_init();
    LCD_Init();

    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();

    /* 小栈任务留内部 DRAM，大栈任务用 PSRAM */
    xTaskCreate(lvgl_tick_task, "lv_tick", 2048, NULL, 5, NULL);
    xTaskCreatePSRAM(lvgl_task, "lv_task", 32768, NULL, 4);
}
