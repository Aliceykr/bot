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
/* 消抖：要求连续 DEBOUNCE_N 次扫描一致才接受状态变化
 * 每次 2ms，N=5 → 12ms 窗口，可过滤绝大多数机械抖动 */
#define DEBOUNCE_N    5

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

/* 扫描一轮：逐行驱动低电平，读 3 个列电平，拼成 9 位位图。
 *
 * 之前只 5us 稳定时间 + 单次采样，列会读到上一轮残留低电平，
 * 表现为按一个键报三个同列键。修复：
 *   1) 行切换后给 100us 让 GPIO 完全恢复（内部上拉 ~45kΩ × 列 PCB 电容）
 *   2) 列电平投票：连续 3 次读都是 0 才算按下，任何一次读到 1 就判未按
 *      这样能过滤瞬时串扰和慢沿带来的伪低电平 */
static inline uint16_t scan_once(void)
{
    uint16_t bits = 0;
    for (int r = 0; r < 3; r++) {
        /* 先把所有行设为输入（高阻 + 上拉），确保没有遗留的驱动低 */
        for (int r2 = 0; r2 < 3; r2++) {
            gpio_set_direction(s_row_pins[r2], GPIO_MODE_INPUT);
        }
        /* 只驱动当前行为低 */
        gpio_set_direction(s_row_pins[r], GPIO_MODE_OUTPUT);
        gpio_set_level(s_row_pins[r], 0);

        /* 100us 稳定时间：覆盖 GPIO 切换 + 列线上拉充电时间 */
        esp_rom_delay_us(100);

        for (int c = 0; c < 3; c++) {
            /* 3 次投票：全 0 才算按下 */
            int lo_votes = 0;
            for (int v = 0; v < 3; v++) {
                if (gpio_get_level(s_col_pins[c]) == 0) lo_votes++;
                esp_rom_delay_us(10);
            }
            if (lo_votes == 3) {
                bits |= (1U << (r * 3 + c));
            }
        }
    }
    /* 扫描结束后全部设为输入，避免下一轮进入前还有行在驱低 */
    for (int r = 0; r < 3; r++) {
        gpio_set_direction(s_row_pins[r], GPIO_MODE_INPUT);
    }
    return bits;
}

/* 矩阵键盘常驻扫描任务。
 * 负责消抖、游戏模式屏蔽、按键日志，以及中键长按退出事件的锁存。 */
static void keypad_task(void *arg)
{
    (void)arg;
    uint16_t last = 0;
    uint16_t last_reported = 0;
    int debounce_cnt = 0;
    uint32_t exit_hold_ms = 0;
    /* exit_latched：本次按下周期内已经触发过退出请求。
     * 只在用户松开中间键后清零，避免"消费者 consume 后用户还按着"时重复触发。 */
    bool exit_latched = false;
    static char buf[128];
    while (1) {
        uint16_t cur = scan_once();
        if (cur == last) {
            if (debounce_cnt < DEBOUNCE_N) debounce_cnt++;
        } else {
            debounce_cnt = 0;
        }
        last = cur;

        /* 连续 DEBOUNCE_N 次一致才接受新状态 */
        if (debounce_cnt >= DEBOUNCE_N) {
            s_stable_bits = s_game_mode ? cur : 0;

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

                /* R1C1 长按 800ms → 退出标志（只触发一次）
                 * 用 exit_latched 本地锁存：本次按住周期内只触发一次，
                 * 消费者 consume 后不会因为用户还按着导致重复触发。
                 * 松开中间键（else 分支）清零 exit_hold_ms 和 latched，下次长按重新生效。 */
                if (cur == KEYPAD_BIT_R1C1) {
                    exit_hold_ms += 2;
                    if (exit_hold_ms >= EXIT_HOLD_MS && !exit_latched) {
                        ESP_LOGW(TAG, "中间键长按 %u ms，触发退出", (unsigned)EXIT_HOLD_MS);
                        s_exit_request = true;
                        exit_latched = true;
                    }
                } else {
                    exit_hold_ms = 0;
                    exit_latched = false;
                }
            } else {
                exit_hold_ms = 0;
                exit_latched = false;
                last_reported = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/* 初始化 3x3 矩阵键盘 GPIO，并启动唯一的后台扫描任务。 */
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

/* 切换菜单/游戏输入模式。
 * 菜单模式下清空稳定键值，避免游戏键位影响 LVGL 菜单操作。 */
void keypad_set_game_mode(bool enable)
{
    s_game_mode = enable;
    /* 切换模式时清一下临时状态，避免带残留 */
    s_exit_request = false;
    if (!enable) s_stable_bits = 0;
    ESP_LOGI(TAG, "game_mode=%d", (int)enable);
}

/* 返回最近一次消抖后的 9 位按键状态，游戏主循环按需轮询。 */
uint16_t keypad_get_bits(void)
{
    return s_stable_bits;
}

/* 消费一次长按退出请求。
 * 使用 consume 语义是为了让多个游戏循环不会反复处理同一个长按事件。 */
bool keypad_consume_exit_request(void)
{
    if (s_exit_request) {
        s_exit_request = false;
        return true;
    }
    return false;
}
