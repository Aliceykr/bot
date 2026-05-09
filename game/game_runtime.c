#include "game_runtime.h"
#include "rom_loader.h"
#include "gb_emu.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "lcd.h"

#define TAG "GAME_RT"

/* 编码器 SW 键作为临时退出按键（与 lv_port_indev 定义一致）*/
#define EXIT_KEY_GPIO  GPIO_NUM_6
/* 长按时长（ms）：避免误触 */
#define EXIT_HOLD_MS   800

static volatile bool s_exit_requested = false;
static TaskHandle_t  s_watcher_task   = NULL;

void game_runtime_request_exit(void)
{
    s_exit_requested = true;
    gb_emu_request_exit();
}

/* 退出按键监视任务：模拟器主循环在 gb_run_frame 内，无法顺带轮询 GPIO，
 * 这里起独立任务专门盯着 SW 键长按 */
static void exit_watcher_task(void *arg)
{
    int hold_ms = 0;
    while (!s_exit_requested) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (gpio_get_level(EXIT_KEY_GPIO) == 0) {
            hold_ms += 50;
            if (hold_ms >= EXIT_HOLD_MS) {
                ESP_LOGI(TAG, "长按退出键，通知模拟器退出");
                gb_emu_request_exit();
                s_exit_requested = true;
                break;
            }
        } else {
            hold_ms = 0;
        }
    }
    s_watcher_task = NULL;
    vTaskDelete(NULL);
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

    ESP_LOGI(TAG, "ROM 加载 %u 字节，启动 Peanut-GB", (unsigned)rom_size);

    /* 启动退出按键监视任务 */
    xTaskCreate(exit_watcher_task, "exit_watch", 2048, NULL, 4, &s_watcher_task);

    /* 运行模拟器（阻塞） */
    bool ok = gb_emu_run((const uint8_t *)rom, rom_size);

    /* 停止退出监视 */
    s_exit_requested = true;
    int wait = 0;
    while (s_watcher_task && wait < 20) {
        vTaskDelay(pdMS_TO_TICKS(50));
        wait++;
    }

    rom_loader_free(rom);

    if (!ok) {
        LCD_Fill(0, 0, LCD_W, LCD_H, 0x0000);
        LCD_ShowString(4, 20, (const uint8_t *)"Emulator init failed",
                       0xF800, 0x0000, 16, 0);
        LCD_ShowString(4, 40, (const uint8_t *)"ROM may be invalid.",
                       0xFFFF, 0x0000, 16, 0);
        LCD_ShowString(4, 60, (const uint8_t *)"Hold SW to exit.",
                       0x07FF, 0x0000, 16, 0);
        /* 等用户长按退出 */
        int hold_ms = 0;
        while (hold_ms < EXIT_HOLD_MS) {
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(EXIT_KEY_GPIO) == 0) hold_ms += 50;
            else hold_ms = 0;
        }
    }

    ESP_LOGI(TAG, "game_runtime_run 退出");
}
