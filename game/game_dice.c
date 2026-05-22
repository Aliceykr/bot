#include "game_dice.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lcd.h"
#include "keypad.h"
#include "mpu6050.h"

#define TAG "DICE"

#define SCREEN_BG        0x0841
#define PANEL_BG         0x18C3
#define TEXT_MAIN        0xFFFF
#define TEXT_MUTED       0xA514
#define TEXT_ACCENT      0xFF6A
#define DICE_FACE        0xFFFF
#define DICE_EDGE        0xD6BA
#define DICE_SHADOW      0x4208
#define PIP_COLOR        0x0000

#define DICE_CENTER_X    (LCD_W / 2)
#define DICE_CENTER_Y    166
#define DICE_BASE_SIZE   126
#define SHAKE_THRESHOLD  0.35f
#define SHAKE_QUIET_G    0.10f
#define SHAKE_CONFIRM    2
#define SHAKE_COOLDOWN_MS 900

static volatile bool s_exit_requested = false;

/* 用 LCD 字库粗略估算文本宽度并居中绘制。
 * 这里服务于游戏提示文案，不追求复杂排版，只保证小屏上位置稳定。 */
static void draw_centered_text(int y, const char *text, uint16_t fc,
                               uint16_t bc, uint8_t sizey)
{
    int w = (int)strlen(text) * (sizey / 2);
    int x = (LCD_W - w) / 2;
    if (x < 0) x = 0;
    LCD_ShowString((uint16_t)x, (uint16_t)y, (const uint8_t *)text, fc, bc,
                   sizey, 0);
}

/* 带边界裁剪的矩形填充，避免动画抖动时坐标越界传给 LCD 驱动。 */
static void fill_rect_clip(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > LCD_W) x1 = LCD_W;
    if (y1 > LCD_H) y1 = LCD_H;
    if (x1 <= x0 || y1 <= y0) return;
    LCD_Fill((uint16_t)x0, (uint16_t)y0, (uint16_t)x1, (uint16_t)y1, color);
}

/* 通过逐行填充短矩形画实心圆点，比逐像素打点少很多 LCD 调用。 */
static void draw_disc(int cx, int cy, int r, uint16_t color)
{
    if (r <= 0) return;
    int rr = r * r;
    for (int y = -r; y <= r; y++) {
        int yy = y * y;
        int dx = 0;
        while ((dx + 1) * (dx + 1) + yy <= rr) {
            dx++;
        }
        fill_rect_clip(cx - dx, cy + y, cx + dx + 1, cy + y + 1, color);
    }
}

/* 根据点数画骰子本体和圆点。
 * 所有尺寸按 size 推导，方便滚动动画里做轻微缩放。 */
static void draw_die(int cx, int cy, int size, int face)
{
    if (size < 36) size = 36;
    int x0 = cx - size / 2;
    int y0 = cy - size / 2;
    int x1 = x0 + size;
    int y1 = y0 + size;
    int border = size / 18;
    if (border < 3) border = 3;
    int pip_r = size / 11;
    if (pip_r < 4) pip_r = 4;

    fill_rect_clip(x0 + 7, y0 + 9, x1 + 7, y1 + 9, DICE_SHADOW);
    fill_rect_clip(x0, y0, x1, y1, DICE_EDGE);
    fill_rect_clip(x0 + border, y0 + border, x1 - border, y1 - border,
                   DICE_FACE);

    int left = x0 + size / 4;
    int mid_x = x0 + size / 2;
    int right = x0 + size * 3 / 4;
    int top = y0 + size / 4;
    int mid_y = y0 + size / 2;
    int bottom = y0 + size * 3 / 4;

    switch (face) {
    case 1:
        draw_disc(mid_x, mid_y, pip_r, PIP_COLOR);
        break;
    case 2:
        draw_disc(left, top, pip_r, PIP_COLOR);
        draw_disc(right, bottom, pip_r, PIP_COLOR);
        break;
    case 3:
        draw_disc(left, top, pip_r, PIP_COLOR);
        draw_disc(mid_x, mid_y, pip_r, PIP_COLOR);
        draw_disc(right, bottom, pip_r, PIP_COLOR);
        break;
    case 4:
        draw_disc(left, top, pip_r, PIP_COLOR);
        draw_disc(right, top, pip_r, PIP_COLOR);
        draw_disc(left, bottom, pip_r, PIP_COLOR);
        draw_disc(right, bottom, pip_r, PIP_COLOR);
        break;
    case 5:
        draw_disc(left, top, pip_r, PIP_COLOR);
        draw_disc(right, top, pip_r, PIP_COLOR);
        draw_disc(mid_x, mid_y, pip_r, PIP_COLOR);
        draw_disc(left, bottom, pip_r, PIP_COLOR);
        draw_disc(right, bottom, pip_r, PIP_COLOR);
        break;
    default:
        draw_disc(left, top, pip_r, PIP_COLOR);
        draw_disc(right, top, pip_r, PIP_COLOR);
        draw_disc(left, mid_y, pip_r, PIP_COLOR);
        draw_disc(right, mid_y, pip_r, PIP_COLOR);
        draw_disc(left, bottom, pip_r, PIP_COLOR);
        draw_disc(right, bottom, pip_r, PIP_COLOR);
        break;
    }
}

/* 绘制固定背景、标题、传感器状态和退出提示。 */
static void render_static_screen(bool mpu_ok)
{
    LCD_Fill(0, 0, LCD_W, LCD_H, SCREEN_BG);
    LCD_Fill(0, 0, LCD_W, 52, PANEL_BG);
    draw_centered_text(8, "DICE", TEXT_ACCENT, PANEL_BG, 32);

    if (mpu_ok) {
        draw_centered_text(58, "Shake to roll", TEXT_MAIN, SCREEN_BG, 16);
    } else {
        draw_centered_text(58, "MPU6050 not found", 0xF800, SCREEN_BG, 16);
    }
    draw_centered_text(286, "Hold Mid to exit", TEXT_MUTED, SCREEN_BG, 16);
}

/* 更新底部结果区域，避免每次摇完重画整屏。 */
static void render_result(int face)
{
    char buf[24];
    snprintf(buf, sizeof(buf), "Result: %d", face);
    LCD_Fill(0, 248, LCD_W, 278, SCREEN_BG);
    draw_centered_text(252, buf, TEXT_ACCENT, SCREEN_BG, 24);
}

/* 动画专用等待：把长延时拆成 20ms 小片，期间持续响应退出请求。 */
static bool wait_with_exit(uint32_t ms)
{
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        if (s_exit_requested || keypad_consume_exit_request()) {
            s_exit_requested = true;
            return true;
        }
        uint32_t step = (ms - elapsed > 20) ? 20 : (ms - elapsed);
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }
    return false;
}

/* 播放一次摇骰子的随机跳动动画，最后落到指定点数。 */
static void play_roll_animation(int final_face)
{
    static const int offsets[][2] = {
        { -18, -8 }, { 14, -16 }, { -10, 12 }, { 18, 6 },
        { -7, -18 }, { 10, 14 }, { -14, 3 }, { 6, -10 },
    };
    int frames = (int)(sizeof(offsets) / sizeof(offsets[0]));

    LCD_Fill(0, 82, LCD_W, 246, SCREEN_BG);
    for (int i = 0; i < frames; i++) {
        int face = (int)(esp_random() % 6) + 1;
        int size = DICE_BASE_SIZE - 18 + (i % 4) * 8;
        int cx = DICE_CENTER_X + offsets[i][0];
        int cy = DICE_CENTER_Y + offsets[i][1];
        LCD_Fill(0, 82, LCD_W, 246, SCREEN_BG);
        draw_die(cx, cy, size, face);
        if (wait_with_exit(70)) return;
    }

    for (int size = DICE_BASE_SIZE + 16; size >= DICE_BASE_SIZE; size -= 8) {
        LCD_Fill(0, 82, LCD_W, 246, SCREEN_BG);
        draw_die(DICE_CENTER_X, DICE_CENTER_Y, size, final_face);
        if (wait_with_exit(55)) return;
    }
    render_result(final_face);
}

/* 摇动检测状态机。
 * diff 用相邻两帧加速度变化量判断“动了一下”；armed 要求设备先安静下来，
 * confirm 要求连续多次超过阈值，二者一起减少抖动和拿起设备时的误触发。 */
static bool shake_detected(float *prev_ax, float *prev_ay, float *prev_az,
                           bool *armed, uint8_t *confirm)
{
    float ax, ay, az;
    if (!mpu6050_read_accel(&ax, &ay, &az)) {
        *confirm = 0;
        return false;
    }

    float dx = ax - *prev_ax;
    float dy = ay - *prev_ay;
    float dz = az - *prev_az;
    float diff = sqrtf(dx * dx + dy * dy + dz * dz);
    *prev_ax = ax;
    *prev_ay = ay;
    *prev_az = az;

    if (diff < SHAKE_QUIET_G) {
        *armed = true;
        *confirm = 0;
        return false;
    }

    if (!*armed || diff < SHAKE_THRESHOLD) {
        return false;
    }

    if (*confirm < SHAKE_CONFIRM) {
        (*confirm)++;
    }
    if (*confirm < SHAKE_CONFIRM) {
        return false;
    }

    *armed = false;
    *confirm = 0;
    return true;
}

/* 外部任务请求退出骰子游戏，主循环在下一轮或动画等待片段里响应。 */
void game_dice_request_exit(void)
{
    s_exit_requested = true;
}

/* 摇骰子游戏入口：初始化 MPU6050，绘制界面，循环等待摇动或长按退出。 */
void game_dice_run(void)
{
    s_exit_requested = false;
    ESP_LOGI(TAG, "进入摇骰子");

    render_static_screen(false);
    bool mpu_ok = mpu6050_init();
    render_static_screen(mpu_ok);

    int face = (int)(esp_random() % 6) + 1;
    draw_die(DICE_CENTER_X, DICE_CENTER_Y, DICE_BASE_SIZE, face);
    render_result(face);

    float prev_ax = 0.0f;
    float prev_ay = 0.0f;
    float prev_az = 1.0f;
    mpu6050_read_accel(&prev_ax, &prev_ay, &prev_az);
    bool armed = true;
    uint8_t confirm = 0;
    TickType_t last_roll = 0;

    while (!s_exit_requested) {
        if (keypad_consume_exit_request()) {
            ESP_LOGI(TAG, "长按中键，退出摇骰子");
            break;
        }

        if (mpu_ok && shake_detected(&prev_ax, &prev_ay, &prev_az,
                                     &armed, &confirm)) {
            TickType_t now = xTaskGetTickCount();
            uint32_t dt = (uint32_t)((now - last_roll) * portTICK_PERIOD_MS);
            if (last_roll == 0 || dt >= SHAKE_COOLDOWN_MS) {
                face = (int)(esp_random() % 6) + 1;
                ESP_LOGI(TAG, "摇动触发，结果=%d", face);
                play_roll_animation(face);
                last_roll = xTaskGetTickCount();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(25));
    }

    if (mpu_ok) {
        mpu6050_deinit();
    }
    ESP_LOGI(TAG, "退出摇骰子");
}
