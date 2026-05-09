#include "game_runtime.h"
#include "rom_loader.h"
#include "gb_emu.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lcd.h"

#define TAG "GAME_RT"

static volatile bool s_exit_requested = false;

void game_runtime_request_exit(void)
{
    s_exit_requested = true;
    gb_emu_request_exit();
}

void game_runtime_run(const char *rom_name)
{
    s_exit_requested = false;

    /* 先显示一个加载画面 */
    LCD_Fill(0, 0, LCD_W, LCD_H, 0x0000);
    LCD_ShowString(4, 20, (const uint8_t *)"Loading ROM...", 0xFFFF, 0x0000, 16, 0);
    if (rom_name) {
        LCD_ShowString(4, 40, (const uint8_t *)rom_name, 0x07FF, 0x0000, 16, 0);
    }

    /* 加载 ROM 到 PSRAM */
    size_t rom_size = 0;
    void *rom = rom_loader_read(rom_name, &rom_size);
    if (!rom) {
        LCD_ShowString(4, 80, (const uint8_t *)"ROM load FAILED",
                       0xF800, 0x0000, 16, 0);
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    ESP_LOGI(TAG, "ROM 加载 %u 字节，启动 Walnut-CGB", (unsigned)rom_size);

    /* 退出键监控由矩阵键盘模块内部完成（中间键长按 800ms）。
     * gb_emu 主循环每帧会调 keypad_consume_exit_request 检测。
     * 不再需要独立 watcher_task。*/

    bool ok = gb_emu_run((const uint8_t *)rom, rom_size);

    rom_loader_free(rom);

    if (!ok) {
        LCD_Fill(0, 0, LCD_W, LCD_H, 0x0000);
        LCD_ShowString(4, 20, (const uint8_t *)"Emulator init failed",
                       0xF800, 0x0000, 16, 0);
        LCD_ShowString(4, 40, (const uint8_t *)"ROM may be invalid.",
                       0xFFFF, 0x0000, 16, 0);
        LCD_ShowString(4, 60, (const uint8_t *)"Hold center key to exit.",
                       0x07FF, 0x0000, 16, 0);
        /* 失败场景下等用户按中间键退出（短按即退，2 秒超时兜底）*/
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    ESP_LOGI(TAG, "game_runtime_run 退出");
}
