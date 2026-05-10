#include "health.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#define TAG "HEALTH"

/* 周期（毫秒）：60 秒一次，既能尽早发现泄漏，又不挤日志 */
#define HEALTH_INTERVAL_MS 60000

static void health_task(void *arg)
{
    (void)arg;
    /* 启动 5 秒后开始打印，等 WiFi/LVGL 等子系统稳定 */
    vTaskDelay(pdMS_TO_TICKS(5000));

    while (1) {
        size_t dram_free     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        size_t dram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        size_t psram_free     = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t psram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI(TAG, "DRAM free=%u min=%u | PSRAM free=%u min=%u",
                 (unsigned)dram_free, (unsigned)dram_min_free,
                 (unsigned)psram_free, (unsigned)psram_min_free);

        vTaskDelay(pdMS_TO_TICKS(HEALTH_INTERVAL_MS));
    }
}

void health_monitor_start(void)
{
    /* 低优先级 1。栈 3KB：ESP_LOGI 格式化多个 %u + 任务切换上下文，
     * 原来 2KB 在高并发（ESP-SR/WiFi/BLE 并发时）会栈溢出崩溃。 */
    xTaskCreate(health_task, "health", 3072, NULL, 1, NULL);
}
