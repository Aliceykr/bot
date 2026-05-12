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
#include "keypad.h"
#include "rom_loader.h"
#include "psram_task.h"
#include "sdcard.h"
#include "bemfa.h"

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
        /* 游戏运行期间 LVGL 完全冻结：不处理 timer / indev，
         * 避免编码器旋转触发 LVGL 事件抢占 CPU 影响游戏帧率 */
        if (lv_port_disp_is_suspended()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void app_main(void)
{
    /* NVS 和 SD 卡尽早初始化，不依赖任何业务路径。
     * 游戏 ROM / 音乐文件都在 SD 卡上，不再打包 SPIFFS。*/
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    /* 原来 rom_loader_init 挂载 SPIFFS；现在在 sdcard_mount() 后再调，
     * 仅做 SD 挂载状态打印。*/

    /* PSRAM 任务管理：业务 HTTPS/cJSON 任务的栈从 PSRAM 分配，防泄漏 */
    psram_task_init();

    speaker_init();
    /* 巴法云模块一次性 init：创建内部 mutex，避免首次并发调用竞态（C1）*/
    bemfa_init();
    /* ESP-SR 不再常驻：进入"语音命令"界面时懒加载 esp_sr_init()，
     * 退出时 esp_sr_deinit() 释放 DRAM。节省 ~60-80KB 内部 RAM。 */
    /* BLE 也不再常驻：进入"蓝牙"菜单时才 ble_prov_init()，
     * 关闭时 ble_prov_deinit() 释放 Bluedroid 栈。节省 ~50-60KB DRAM。 */
    LCD_Init();

    /* SD 卡挂载：失败不影响启动（用户可能没插卡），
     * 应用层读 /sdcard 前自行检查 sdcard_is_mounted() */
    if (sdcard_mount()) {
        ESP_LOGI("MAIN", "SD 卡可用: %lu MB / %lu MB",
                 (unsigned long)sdcard_free_mb(),
                 (unsigned long)sdcard_total_mb());
    } else {
        ESP_LOGW("MAIN", "SD 卡未挂载（可能未插卡），游戏/音乐功能不可用");
    }

    /* rom_loader 现在只是包装 SD 卡读取，函数名保留兼容性；
     * 必须在 sdcard_mount() 之后调。*/
    rom_loader_init();

    /* 矩阵键盘：GB 模拟器运行时用作 8 键输入，菜单期间也会扫描但不拦截事件 */
    keypad_init();

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
