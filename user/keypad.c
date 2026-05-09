#include "keypad.h"
#include <stdio.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define TAG "KEYPAD"

/* 引脚定义：与 keypad.h 中的布局一致 */
static const gpio_num_t s_row_pins[3] = { GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_14 };
static const gpio_num_t s_col_pins[3] = { GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_47 };

/* 扫描任务维护：原始读数 + 上一次读数（用于去抖）+ 已稳定的状态 */
static volatile uint16_t s_stable_bits = 0;
static bool s_inited = false;

/* 扫描一轮：逐行驱动低电平，读 3 个列电平，拼成 9 位位图。
 * 其它行保持输入浮空（用 OD 模式 + 写 1 等效于高阻，避免短路）。
 * 简化起见用推挽输出，写 1 拉高非扫描行，按键按下不会造成多行短路电流
 * 因为 GPIO 驱动能力仅 ~40mA，另一端是按键对另一行的推挽输出 1。
 * 为了更稳妥，仍把其余行置为输入态（高阻），仅扫描行做推挽输出。*/
static inline uint16_t scan_once(void)
{
    uint16_t bits = 0;
    for (int r = 0; r < 3; r++) {
        /* 本行设为输出低 */
        gpio_set_direction(s_row_pins[r], GPIO_MODE_OUTPUT);
        gpio_set_level(s_row_pins[r], 0);
        /* 其它行置高阻（输入） */
        for (int r2 = 0; r2 < 3; r2++) {
            if (r2 != r) gpio_set_direction(s_row_pins[r2], GPIO_MODE_INPUT);
        }
        /* 等电平稳定：走线短、内部上拉 ~45kΩ + 列寄生电容，几微秒足够 */
        esp_rom_delay_us(5);

        for (int c = 0; c < 3; c++) {
            if (gpio_get_level(s_col_pins[c]) == 0) {
                bits |= (1U << (r * 3 + c));
            }
        }
    }

    /* 扫描完复位：所有行设回输入高阻，避免持续耗电 */
    for (int r = 0; r < 3; r++) {
        gpio_set_direction(s_row_pins[r], GPIO_MODE_INPUT);
    }
    return bits;
}

static void keypad_task(void *arg)
{
    (void)arg;
    uint16_t last = 0;
    uint16_t last_reported = 0;
    /* buf 设 static 避免每次循环在栈上开 128 字节；
     * 本任务单实例，静态缓冲安全。*/
    static char buf[128];
    while (1) {
        uint16_t cur = scan_once();
        /* 去抖：连续两次一致才更新稳态。
         * 单次扫描 3 行 × 5us ≈ 15us，算上 vTaskDelay 周期 2 ms，
         * 最坏确认延迟 4 ms，对游戏输入几乎无感。*/
        if (cur == last) {
            s_stable_bits = cur;
            /* 稳态变化时打印出来，便于硬件调试区分按键。
             * 没变化时不刷日志，避免按住时刷屏。*/
            if (cur != last_reported) {
                static const char *const names[9] = {
                    "R0C0", "R0C1", "R0C2",
                    "R1C0", "R1C1", "R1C2",
                    "R2C0", "R2C1", "R2C2",
                };
                int p = 0;
                for (int i = 0; i < 9; i++) {
                    if (cur & (1U << i)) {
                        p += snprintf(buf + p, sizeof(buf) - p, "%s ", names[i]);
                        if (p >= (int)sizeof(buf) - 8) break;
                    }
                }
                if (p == 0) {
                    ESP_LOGI(TAG, "释放");
                } else {
                    ESP_LOGI(TAG, "按下: %s(0x%03X)", buf, cur);
                }
                last_reported = cur;
            }
        }
        last = cur;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void keypad_init(void)
{
    if (s_inited) return;

    /* 行脚：先置为输入高阻（扫描时再切输出） */
    gpio_config_t row_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 0,
        .pull_down_en = 0,
        .pin_bit_mask = 0,
    };
    for (int i = 0; i < 3; i++) row_conf.pin_bit_mask |= (1ULL << s_row_pins[i]);
    gpio_config(&row_conf);

    /* 列脚：输入 + 内部上拉 */
    gpio_config_t col_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,
        .pull_down_en = 0,
        .pin_bit_mask = 0,
    };
    for (int i = 0; i < 3; i++) col_conf.pin_bit_mask |= (1ULL << s_col_pins[i]);
    gpio_config(&col_conf);

    /* 扫描任务：优先级 6 与编码器任务相同，Core 0 不固定。
     * 栈 4KB：ESP_LOGI 格式化 + snprintf + gpio API 调用合计需要 ~3KB，
     * 2KB 在打印按键日志时会栈溢出（实测触发）。*/
    xTaskCreate(keypad_task, "kpad_task", 4096, NULL, 6, NULL);

    s_inited = true;
    ESP_LOGI(TAG, "3x3 矩阵键盘初始化：行 %d %d %d 列 %d %d %d",
             s_row_pins[0], s_row_pins[1], s_row_pins[2],
             s_col_pins[0], s_col_pins[1], s_col_pins[2]);
}

uint16_t keypad_get_bits(void)
{
    return s_stable_bits;
}
