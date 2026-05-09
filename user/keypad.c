#include "keypad.h"
#include <stdio.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#define TAG "KEYPAD"

/* 长按退出时长 */
#define EXIT_HOLD_MS  800

/* 引脚定义：与 keypad.h 中的布局一致 */
static const gpio_num_t s_row_pins[3] = { GPIO_NUM_1, GPIO_NUM_2, GPIO_NUM_14 };
static const gpio_num_t s_col_pins[3] = { GPIO_NUM_41, GPIO_NUM_42, GPIO_NUM_47 };

/* 扫描任务维护：稳态位图 */
static volatile uint16_t s_stable_bits = 0;
/* 游戏模式开关。默认 false（菜单期间不生效）*/
static volatile bool     s_game_mode   = false;
/* EXIT 请求标志：R1C1 长按 800ms 后置 1，被 consume 后清 0 */
static volatile bool     s_exit_request = false;
static bool s_inited = false;

/* 扫描一轮：逐行驱动低电平，读 3 个列电平，拼成 9 位位图。 */
static inline uint16_t scan_once(void)
{
    uint16_t bits = 0;
    for (int r = 0; r < 3; r++) {
        gpio_set_direction(s_row_pins[r], GPIO_MODE_OUTPUT);
        gpio_set_level(s_row_pins[r], 0);
        for (int r2 = 0; r2 < 3; r2++) {
            if (r2 != r) gpio_set_direction(s_row_pins[r2], GPIO_MODE_INPUT);
        }
        esp_rom_delay_us(5);
        for (int c = 0; c < 3; c++) {
            if (gpio_get_level(s_col_pins[c]) == 0) {
                bits |= (1U << (r * 3 + c));
            }
        }
    }
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
    uint32_t exit_hold_ms = 0;
    static char buf[128];
    while (1) {
        uint16_t cur = scan_once();
        if (cur == last) {
            /* 菜单模式下 bits 强制返回 0，确保游戏外按键不会影响业务 */
            s_stable_bits = s_game_mode ? cur : 0;

            /* 只有游戏模式才打日志 + 检测长按退出 */
            if (s_game_mode) {
                if (cur != last_reported) {
                    static const char *const names[9] = {
                        "R0C0(B)",     "R0C1(UP)",    "R0C2(A)",
                        "R1C0(LEFT)",  "R1C1(EXIT)",  "R1C2(RIGHT)",
                        "R2C0(SEL)",   "R2C1(DOWN)",  "R2C2(START)",
                    };
                    int p = 0;
                    for (int i = 0; i < 9; i++) {
                        if (cur & (1U << i)) {
                            p += snprintf(buf + p, sizeof(buf) - p, "%s ", names[i]);
                            if (p >= (int)sizeof(buf) - 16) break;
                        }
                    }
                    if (p == 0) {
                        ESP_LOGI(TAG, "释放");
                    } else {
                        ESP_LOGI(TAG, "按下: %s(0x%03X)", buf, cur);
                    }
                    last_reported = cur;
                }

                /* R1C1 长按 800ms → 退出标志
                 * 必须是"只按着中间键"，其它键同按不计入（防误触）*/
                if (cur == KEYPAD_BIT_R1C1) {
                    exit_hold_ms += 2;  /* 每轮 2ms */
                    if (exit_hold_ms == EXIT_HOLD_MS) {
                        ESP_LOGW(TAG, "中间键长按 %u ms，触发退出", (unsigned)EXIT_HOLD_MS);
                        s_exit_request = true;
                    }
                } else {
                    exit_hold_ms = 0;
                }
            } else {
                /* 菜单模式下重置计数 */
                exit_hold_ms = 0;
                last_reported = 0;
            }
        }
        last = cur;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void keypad_init(void)
{
    if (s_inited) return;

    gpio_config_t row_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 0,
        .pull_down_en = 0,
        .pin_bit_mask = 0,
    };
    for (int i = 0; i < 3; i++) row_conf.pin_bit_mask |= (1ULL << s_row_pins[i]);
    gpio_config(&row_conf);

    gpio_config_t col_conf = {
        .intr_type    = GPIO_INTR_DISABLE,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = 1,
        .pull_down_en = 0,
        .pin_bit_mask = 0,
    };
    for (int i = 0; i < 3; i++) col_conf.pin_bit_mask |= (1ULL << s_col_pins[i]);
    gpio_config(&col_conf);

    xTaskCreate(keypad_task, "kpad_task", 4096, NULL, 6, NULL);
    s_inited = true;
    ESP_LOGI(TAG, "3x3 矩阵键盘初始化：行 %d %d %d 列 %d %d %d（初始菜单模式）",
             s_row_pins[0], s_row_pins[1], s_row_pins[2],
             s_col_pins[0], s_col_pins[1], s_col_pins[2]);
}

void keypad_set_game_mode(bool enable)
{
    s_game_mode = enable;
    /* 切换模式时清一下临时状态，避免带残留 */
    s_exit_request = false;
    if (!enable) s_stable_bits = 0;
    ESP_LOGI(TAG, "game_mode=%d", (int)enable);
}

uint16_t keypad_get_bits(void)
{
    return s_stable_bits;
}

bool keypad_consume_exit_request(void)
{
    if (s_exit_request) {
        s_exit_request = false;
        return true;
    }
    return false;
}
