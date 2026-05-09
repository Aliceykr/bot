#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "lcd.h"
#include "lvgl.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "my_demo.h"
#include "speaker.h"
#include "health.h"
#include "rom_loader.h"

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
    /* NVS 和 SPIFFS 尽早初始化，不依赖任何业务路径。
     * 这样进游戏菜单无需先联 WiFi 或触发其他功能 */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    rom_loader_init();  /* 挂载 /spiffs，ROM 列表可用 */

    speaker_init();
    LCD_Init();

    lv_init();
    lv_port_disp_init();
    lv_port_indev_init();

    /* 小栈任务和 LVGL 主任务都用内部 DRAM 栈：
     * SPIFFS / NVS 等 flash IO 要求调用线程栈不能在 PSRAM（关 cache 后
     * PSRAM 访问会 panic）。LVGL 任务需要访问 SPIFFS 扫描 ROM 列表。
     * 16KB DRAM 栈对 LVGL 菜单逻辑足够（原来 32KB PSRAM 里实际用不到 6KB）。*/
    xTaskCreate(lvgl_tick_task, "lv_tick", 2048, NULL, 5, NULL);
    xTaskCreate(lvgl_task, "lv_task", 16384, NULL, 4, NULL);

    /* 启动健康监控：周期打印堆水位，便于发现长期运行中的内存泄漏 */
    health_monitor_start();
}
