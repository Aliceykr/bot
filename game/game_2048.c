#include "game_2048.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "lcd.h"
#include "keypad.h"
#include "mpu6050.h"

/* ================================================================
 * 契约校验：mpu_dir_t 的整数值必须与 parse_direction() 返回的方向 int 值
 * 保持一致，否则 do_move((int)tilt) 会跑错方向。
 * 修改任一方都会在编译期报错。
 * ================================================================ */
_Static_assert(MPU_DIR_NONE  == 0, "MPU_DIR_NONE must be 0");
_Static_assert(MPU_DIR_UP    == 1, "MPU_DIR_UP must match parse_direction UP=1");
_Static_assert(MPU_DIR_DOWN  == 2, "MPU_DIR_DOWN must match parse_direction DOWN=2");
_Static_assert(MPU_DIR_LEFT  == 3, "MPU_DIR_LEFT must match parse_direction LEFT=3");
_Static_assert(MPU_DIR_RIGHT == 4, "MPU_DIR_RIGHT must match parse_direction RIGHT=4");

/* 字体位图：定义在 lcdfont.h 里（被 lcd.c 编译时一起实例化）。
 * 这里不 include 头文件（会二次定义），用 extern 引用即可。 */
extern const unsigned char ascii_1206[][12];
extern const unsigned char ascii_1608[][16];
extern const unsigned char ascii_2412[][48];
extern const unsigned char ascii_3216[][64];

#define TAG "G2048"

/* ================================================================
 * 2048 游戏 —— 完全重构版（PSRAM framebuffer + 一次性 flush）
 *
 * 原来的问题：每个 LCD_Fill / LCD_ShowString 直接打屏幕，动画期间
 * 脏区擦除必然撕裂/残留。
 *
 * 新设计：
 *   - 分配 PSRAM 上的 240×240×2 = 115KB framebuffer（RGB565，大端）
 *   - 所有绘制函数只写 framebuffer 内存（纯 memcpy/memset 级别，快且原子）
 *   - 每帧调用一次 fb_flush() 把整块 framebuffer DMA 推到 LCD
 *   - 因为 framebuffer 是每帧完整重绘，"脏区管理"完全不需要了，
 *     直接整块渲染即可——CPU 画内存是 O(1M) 级别的，瓶颈是 SPI 输出
 *     （115KB @ 80MHz SPI ≈ 12ms）
 *
 * 整个游戏区 240×240：
 *   -（可选）framebuffer 跟棋盘同大小（240×240），不覆盖 header/footer
 *   - header 和 footer 还是直接调 LCD_*（它们不需要动画）
 *
 * 字体：直接从 lcdfont.h 读 ascii_1206/1608/2412/3216 位图，
 * 按 LCD_ShowChar 同样的规则：
 *   sizey=12 → sizex=6，每字符 12 字节
 *   sizey=16 → sizex=8，每字符 16 字节
 *   sizey=24 → sizex=12，每字符 48 字节
 *   sizey=32 → sizex=16，每字符 64 字节
 * 字节顺序：ascii[idx][i*row_bytes + col_byte]，
 * bit 0（LSB）是左边像素，bit 7 是右边（和 LCD_ShowChar 循环一致）。
 * ================================================================ */

/* 棋盘几何 */
#define GRID_N       4
#define CELL_SIZE    55
#define CELL_GAP     4
#define BOARD_SIZE   (GRID_N * CELL_SIZE + (GRID_N + 1) * CELL_GAP)  /* = 240 */

/* framebuffer 尺寸 */
#define FB_W         240
#define FB_H         240
#define FB_PIX       (FB_W * FB_H)
#define FB_BYTES     (FB_PIX * 2)

/* 屏幕上 framebuffer 的起始位置 —— 棋盘区左上角 */
#define BOARD_SCREEN_X 0
#define BOARD_SCREEN_Y 70   /* 留 70px 给 header */

/* 颜色（RGB565 大端，和 LCD 驱动一致）*/
#define BG_COLOR     0xBDB7
#define EMPTY_COLOR  0xCE59
#define BLACK        0x0000

static const uint16_t s_tile_bg[16] = {
    EMPTY_COLOR,
    0xEF5B, 0xEF3A, 0xF4E3, 0xF408,
    0xF346, 0xF244, 0xEE6C, 0xEE4B,
    0xEE28, 0xEE06, 0xEDE5, 0x3CEF,
    0x3B6F, 0x3A8F, 0x39AF,
};
static const uint16_t s_tile_fg[16] = {
    0x0000, 0x7BAF, 0x7BAF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
};

/* ================================================================
 * 游戏状态
 * ================================================================ */
static uint8_t  s_board[GRID_N][GRID_N];
static uint8_t  s_prev [GRID_N][GRID_N];
static uint32_t s_score = 0;
static uint32_t s_best  = 0;
static uint32_t s_moves = 0;
static bool     s_game_over = false;
/* volatile：跨任务读写。game_2048_request_exit() 从 LVGL 任务设置，
 * 主循环（独立任务）轮询读取。volatile 防止编译器把循环中的读优化成寄存器缓存。 */
static volatile bool s_exit_requested = false;

typedef struct {
    int8_t  sr, sc;   /* 源格 */
    int8_t  tr, tc;   /* 目标格 */
    uint8_t val;      /* 飞行中显示的幂值 */
    bool    merged;   /* 此块会合并进目标（终态目标是 val+1）*/
} tile_move_t;

#define MAX_TRACKS (GRID_N * GRID_N * 2)

/* ================================================================
 * Framebuffer
 * ================================================================ */
/* framebuffer 存成"行连续"的 RGB565 大端：
 * 像素 (x, y) 的字节偏移 = (y * FB_W + x) * 2
 * 字节[0] = color >> 8, 字节[1] = color & 0xFF（SPI 先发高字节）*/
static uint8_t *s_fb = NULL;

static inline void fb_set_pixel_raw(int x, int y, uint16_t color)
{
    /* 调用方保证 x/y 在 [0, FB_W) / [0, FB_H) 内 */
    size_t off = ((size_t)y * FB_W + (size_t)x) * 2;
    s_fb[off    ] = (uint8_t)(color >> 8);
    s_fb[off + 1] = (uint8_t)(color & 0xFF);
}

/* 裁剪后填矩形。left/top/right/bottom 都是 inclusive。 */
static void fb_fill_rect(int x0, int y0, int x1, int y1, uint16_t color)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > FB_W - 1) x1 = FB_W - 1;
    if (y1 > FB_H - 1) y1 = FB_H - 1;
    if (x1 < x0 || y1 < y0) return;

    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFF);

    /* 先构造一行（x0..x1），然后逐行 memcpy 加速 */
    int w = x1 - x0 + 1;
    uint8_t *first_row_start = s_fb + ((size_t)y0 * FB_W + x0) * 2;
    for (int i = 0; i < w; i++) {
        first_row_start[i * 2]     = hi;
        first_row_start[i * 2 + 1] = lo;
    }
    for (int y = y0 + 1; y <= y1; y++) {
        uint8_t *dst = s_fb + ((size_t)y * FB_W + x0) * 2;
        memcpy(dst, first_row_start, (size_t)w * 2);
    }
}

/* 整屏清到某色（BG_COLOR 常用）*/
static void fb_clear(uint16_t color)
{
    fb_fill_rect(0, 0, FB_W - 1, FB_H - 1, color);
}

/* 画单个字符到 framebuffer。
 * 参数和 LCD_ShowChar 一致：sizey ∈ {12,16,24,32}，sizex = sizey/2。
 * num 是 ASCII 字符（会 -' ' 索引到字体数组）。
 * mode=0：画 fc/bc 两色；mode=1：只画前景像素（透明背景）。 */
static void fb_draw_char(int x, int y, uint8_t ch, uint16_t fc, uint16_t bc,
                         uint8_t sizey, bool transparent)
{
    const uint8_t *font;
    uint8_t sizex = sizey / 2;
    uint16_t TypefaceNum;
    if (sizey == 12)      { font = ascii_1206[ch - ' ']; TypefaceNum = 12; }
    else if (sizey == 16) { font = ascii_1608[ch - ' ']; TypefaceNum = 16; }
    else if (sizey == 24) { font = ascii_2412[ch - ' ']; TypefaceNum = 48; }
    else if (sizey == 32) { font = ascii_3216[ch - ' ']; TypefaceNum = 64; }
    else return;

    int m = 0;
    int cur_x = x;
    int cur_y = y;
    for (int i = 0; i < TypefaceNum; i++) {
        uint8_t temp = font[i];
        for (int t = 0; t < 8; t++) {
            bool on = (temp & (1 << t)) != 0;
            if (cur_x >= 0 && cur_x < FB_W && cur_y >= 0 && cur_y < FB_H) {
                if (on)         fb_set_pixel_raw(cur_x, cur_y, fc);
                else if (!transparent) fb_set_pixel_raw(cur_x, cur_y, bc);
            }
            cur_x++;
            m++;
            if (m % sizex == 0) {
                m = 0;
                cur_x = x;
                cur_y++;
                break;
            }
        }
    }
}

static void fb_draw_string(int x, int y, const char *p, uint16_t fc, uint16_t bc, uint8_t sizey)
{
    uint8_t sizex = sizey / 2;
    while (*p) {
        fb_draw_char(x, y, (uint8_t)*p, fc, bc, sizey, false);
        x += sizex;
        p++;
    }
}

/* ================================================================
 * framebuffer → LCD 一次性 DMA 推送
 *
 * LCD_Send_Buf 用 32KB 一段发，但当前 SPI bus 配置 max_transfer_sz=4096，
 * 直接用会被拒（"txdata transfer > host maximum"）。这里按 4092 字节分段
 * 自己发，和 lv_port_disp.c 的 DMA_MAX_CHUNK 一致。
 * 性能开销可忽略：SPI 驱动分段内部是连续 DMA，不会引入空闲间隙。
 * ================================================================ */
#define FB_SPI_CHUNK  4092U

static void fb_flush(void)
{
    LCD_Address_Set(BOARD_SCREEN_X, BOARD_SCREEN_Y,
                    BOARD_SCREEN_X + FB_W - 1,
                    BOARD_SCREEN_Y + FB_H - 1);
    gpio_set_level(LCD_DC_PIN, 1);
    uint32_t remain = FB_BYTES;
    const uint8_t *p = s_fb;
    while (remain > 0) {
        uint32_t chunk = remain > FB_SPI_CHUNK ? FB_SPI_CHUNK : remain;
        spi_transaction_t t = { .length = chunk * 8, .tx_buffer = p };
        spi_device_polling_transmit(s_spi, &t);
        p += chunk;
        remain -= chunk;
    }
}

/* ================================================================
 * 棋盘坐标 → framebuffer 坐标（注意 fb 的 0,0 对应棋盘区左上角）
 * ================================================================ */
static inline void grid_to_fb(int r, int c, int *px, int *py)
{
    *px = CELL_GAP + c * (CELL_SIZE + CELL_GAP);
    *py = CELL_GAP + r * (CELL_SIZE + CELL_GAP);
}

/* 画一个方块到 framebuffer 指定位置（像素坐标在 FB 内）。
 * size = 方块边长（支持 pop-in / bounce 缩放）。 */
static void fb_draw_tile(int px, int py, int size, uint8_t power)
{
    uint16_t bg = (power < 16) ? s_tile_bg[power] : s_tile_bg[15];
    fb_fill_rect(px, py, px + size - 1, py + size - 1, bg);

    if (power == 0 || size < 20) return;

    char buf[12];
    uint32_t val = 1u << power;
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)val);
    int n = (int)strlen(buf);

    uint8_t sizey, char_w;
    if (size >= 48) {
        if (n <= 3)      { sizey = 24; char_w = 12; }
        else if (n == 4) { sizey = 16; char_w = 8;  }
        else             { sizey = 12; char_w = 6;  }
    } else if (size >= 32) {
        if (n <= 3)      { sizey = 16; char_w = 8;  }
        else             { sizey = 12; char_w = 6;  }
    } else {
        sizey = 12; char_w = 6;
    }

    int tx = px + (size - n * char_w) / 2;
    int ty = py + (size - sizey) / 2;
    uint16_t fg = (power < 16) ? s_tile_fg[power] : s_tile_fg[15];
    fb_draw_string(tx, ty, buf, fg, bg, sizey);
}

/* 按当前 s_board 状态把整个棋盘渲染到 framebuffer（不 flush）*/
static void fb_draw_board(uint8_t board[GRID_N][GRID_N])
{
    /* 背景（gap 颜色）*/
    fb_clear(BG_COLOR);
    /* 每格 */
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            int px, py;
            grid_to_fb(r, c, &px, &py);
            fb_draw_tile(px, py, CELL_SIZE, board[r][c]);
        }
    }
}

/* ================================================================
 * 棋盘 header / footer（在 framebuffer 区域外，直接打屏）
 * 它们只在分数/步数变化时重画，不参与动画，和动画不冲突。
 * ================================================================ */
static void render_header(void)
{
    LCD_Fill(0, 0, LCD_W, BOARD_SCREEN_Y, BLACK);

    LCD_ShowString(8, 4, (const uint8_t *)"2048", 0xEDE5, BLACK, 32, 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "Score: %lu", (unsigned long)s_score);
    LCD_ShowString(8, 42, (const uint8_t *)buf, 0xFFFF, BLACK, 16, 0);

    snprintf(buf, sizeof(buf), "Best: %lu", (unsigned long)s_best);
    int w = (int)strlen(buf) * 8;
    int x = LCD_W - w - 8;
    if (x < 120) x = 120;
    LCD_ShowString(x, 12, (const uint8_t *)buf, 0xFFFF, BLACK, 16, 0);

    snprintf(buf, sizeof(buf), "Moves: %lu", (unsigned long)s_moves);
    w = (int)strlen(buf) * 8;
    x = LCD_W - w - 8;
    if (x < 120) x = 120;
    LCD_ShowString(x, 42, (const uint8_t *)buf, 0xAAAA, BLACK, 16, 0);
}

static void render_footer(void)
{
    LCD_Fill(0, BOARD_SCREEN_Y + BOARD_SIZE, LCD_W, LCD_H, BLACK);
    LCD_ShowString(4, BOARD_SCREEN_Y + BOARD_SIZE + 2,
                   (const uint8_t *)"Arrow=Move HoldMid=Exit",
                   0x888C, BLACK, 12, 0);
}

/* 游戏结束遮罩（直接打屏，简单居中文字 + 框）*/
static void render_game_over(void)
{
    int y0 = BOARD_SCREEN_Y + BOARD_SIZE / 2 - 40;
    int y1 = BOARD_SCREEN_Y + BOARD_SIZE / 2 + 40;
    int x0 = 8;
    int x1 = BOARD_SIZE - 8;
    LCD_Fill(x0, y0, x1, y0 + 80, 0x18C3);
    vTaskDelay(pdMS_TO_TICKS(80));
    LCD_Fill(x0, y0, x1, y1, 0x3186);
    LCD_ShowString(BOARD_SIZE / 2 - 64, y0 + 8,
                   (const uint8_t *)"GAME OVER",
                   0xF800, 0x3186, 24, 0);
    vTaskDelay(pdMS_TO_TICKS(80));
    LCD_ShowString(BOARD_SIZE / 2 - 80, y0 + 44,
                   (const uint8_t *)"Hold Mid to exit",
                   0xFFFF, 0x3186, 16, 0);
}

/* ================================================================
 * 核心算法（不变）
 * ================================================================ */
typedef struct {
    int8_t  src_idx;
    int8_t  dst_idx;
    uint8_t val;
    bool    merged;
} line_move_t;

static uint32_t merge_line_left_track(uint8_t line[GRID_N],
                                       line_move_t moves[GRID_N],
                                       int *n_moves)
{
    uint32_t gained = 0;
    uint8_t tmp[GRID_N] = { 0 };
    int8_t tmp_src[GRID_N];
    int w = 0;
    int m_count = 0;

    for (int i = 0; i < GRID_N; i++) {
        if (line[i] != 0) { tmp[w] = line[i]; tmp_src[w] = (int8_t)i; w++; }
    }
    uint8_t out[GRID_N] = { 0 };
    int8_t  out_src1[GRID_N];
    int8_t  out_src2[GRID_N];
    int ow = 0;
    for (int i = 0; i < w; ) {
        if (i + 1 < w && tmp[i] == tmp[i + 1]) {
            out[ow] = tmp[i] + 1;
            out_src1[ow] = tmp_src[i];
            out_src2[ow] = tmp_src[i + 1];
            gained += (1u << out[ow]);
            ow++;
            i += 2;
        } else {
            out[ow] = tmp[i];
            out_src1[ow] = tmp_src[i];
            out_src2[ow] = -1;
            ow++;
            i++;
        }
    }

    memset(line, 0, GRID_N);
    for (int k = 0; k < ow; k++) {
        line[k] = out[k];
        moves[m_count].src_idx = out_src1[k];
        moves[m_count].dst_idx = (int8_t)k;
        moves[m_count].val = (uint8_t)(out_src2[k] >= 0 ? out[k] - 1 : out[k]);
        moves[m_count].merged = false;
        m_count++;

        if (out_src2[k] >= 0) {
            moves[m_count].src_idx = out_src2[k];
            moves[m_count].dst_idx = (int8_t)k;
            moves[m_count].val = (uint8_t)(out[k] - 1);
            moves[m_count].merged = true;
            m_count++;
        }
    }
    *n_moves = m_count;
    return gained;
}

static void reverse_line(uint8_t line[GRID_N])
{
    for (int i = 0; i < GRID_N / 2; i++) {
        uint8_t t = line[i];
        line[i] = line[GRID_N - 1 - i];
        line[GRID_N - 1 - i] = t;
    }
}

static uint32_t move_dir_track(int dir,
                                tile_move_t tracks[MAX_TRACKS],
                                int *n_tracks)
{
    uint32_t total = 0;
    int t = 0;
    for (int fixed = 0; fixed < GRID_N; fixed++) {
        uint8_t line[GRID_N];
        if (dir == 1 || dir == 2) {
            for (int i = 0; i < GRID_N; i++) line[i] = s_board[i][fixed];
        } else {
            for (int i = 0; i < GRID_N; i++) line[i] = s_board[fixed][i];
        }
        bool reverse = (dir == 2 || dir == 4);
        if (reverse) reverse_line(line);

        line_move_t lmoves[GRID_N];
        int nm = 0;
        total += merge_line_left_track(line, lmoves, &nm);

        if (reverse) reverse_line(line);
        if (dir == 1 || dir == 2) {
            for (int i = 0; i < GRID_N; i++) s_board[i][fixed] = line[i];
        } else {
            for (int i = 0; i < GRID_N; i++) s_board[fixed][i] = line[i];
        }

        for (int k = 0; k < nm; k++) {
            int8_t sidx = lmoves[k].src_idx;
            int8_t didx = lmoves[k].dst_idx;
            if (reverse) {
                sidx = (int8_t)(GRID_N - 1 - sidx);
                didx = (int8_t)(GRID_N - 1 - didx);
            }
            int8_t sr, sc, tr, tc;
            if (dir == 1 || dir == 2) { sr = sidx; sc = (int8_t)fixed; tr = didx; tc = (int8_t)fixed; }
            else                      { sr = (int8_t)fixed; sc = sidx; tr = (int8_t)fixed; tc = didx; }
            if (sr == tr && sc == tc && !lmoves[k].merged) continue;

            tracks[t].sr = sr; tracks[t].sc = sc;
            tracks[t].tr = tr; tracks[t].tc = tc;
            tracks[t].val = lmoves[k].val;
            tracks[t].merged = lmoves[k].merged;
            t++;
        }
    }
    *n_tracks = t;
    return total;
}

static bool board_changed(void)
{
    return memcmp(s_board, s_prev, sizeof(s_board)) != 0;
}

static bool is_game_over(void)
{
    for (int r = 0; r < GRID_N; r++)
        for (int c = 0; c < GRID_N; c++)
            if (s_board[r][c] == 0) return false;
    for (int r = 0; r < GRID_N; r++)
        for (int c = 0; c + 1 < GRID_N; c++)
            if (s_board[r][c] == s_board[r][c + 1]) return false;
    for (int c = 0; c < GRID_N; c++)
        for (int r = 0; r + 1 < GRID_N; r++)
            if (s_board[r][c] == s_board[r + 1][c]) return false;
    return true;
}

static void spawn_random_tile(int *out_r, int *out_c)
{
    int empty[GRID_N * GRID_N]; int n = 0;
    for (int r = 0; r < GRID_N; r++)
        for (int c = 0; c < GRID_N; c++)
            if (s_board[r][c] == 0) empty[n++] = r * GRID_N + c;
    if (n == 0) {
        if (out_r) *out_r = -1;
        if (out_c) *out_c = -1;
        return;
    }
    int pick = empty[esp_random() % n];
    int r = pick / GRID_N, c = pick % GRID_N;
    s_board[r][c] = (esp_random() % 10 == 0) ? 2 : 1;
    if (out_r) *out_r = r;
    if (out_c) *out_c = c;
}

static void new_game(void)
{
    memset(s_board, 0, sizeof(s_board));
    s_score = 0; s_moves = 0; s_game_over = false;
    spawn_random_tile(NULL, NULL);
    spawn_random_tile(NULL, NULL);
}

/* ================================================================
 * 动画：滑动、pop-in、bounce —— 全部走 framebuffer
 * ================================================================ */
#define SLIDE_FRAMES   8
#define POPIN_FRAMES   3
#define FRAME_DELAY_MS 18

/* 画"动画中"的一帧：背景 + s_prev 的"未参与本次移动"方块 + 动态方块的插值位置 */
static void render_slide_frame(tile_move_t tracks[], int n_tracks,
                                int num, int den)
{
    /* 1. 背景（gap 色）+ 所有格子位置画 EMPTY 底（覆盖 gap 露出的格内部分）*/
    fb_clear(BG_COLOR);
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            int px, py;
            grid_to_fb(r, c, &px, &py);
            fb_fill_rect(px, py, px + CELL_SIZE - 1, py + CELL_SIZE - 1, EMPTY_COLOR);
        }
    }
    /* 2. 静态格（s_prev 里的值，但排除掉所有轨迹的源格和目标格 —— 它们在动画期间
     *    算作"动态"，动态方块会画到插值位置）*/
    bool busy[GRID_N][GRID_N] = { { false } };
    for (int i = 0; i < n_tracks; i++) {
        busy[tracks[i].sr][tracks[i].sc] = true;
        busy[tracks[i].tr][tracks[i].tc] = true;
    }
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            if (busy[r][c] || s_prev[r][c] == 0) continue;
            int px, py;
            grid_to_fb(r, c, &px, &py);
            fb_draw_tile(px, py, CELL_SIZE, s_prev[r][c]);
        }
    }
    /* 3. 动态方块：在插值位置画（cell 坐标系 → fb 像素坐标）*/
    for (int i = 0; i < n_tracks; i++) {
        int sx, sy, tx, ty;
        grid_to_fb(tracks[i].sr, tracks[i].sc, &sx, &sy);
        grid_to_fb(tracks[i].tr, tracks[i].tc, &tx, &ty);
        int px = sx + (tx - sx) * num / den;
        int py = sy + (ty - sy) * num / den;
        fb_draw_tile(px, py, CELL_SIZE, tracks[i].val);
    }
    /* 4. flush 一次 */
    fb_flush();
}

static void play_slide_animation(tile_move_t tracks[], int n_tracks)
{
    if (n_tracks == 0) return;
    for (int f = 1; f <= SLIDE_FRAMES; f++) {
        render_slide_frame(tracks, n_tracks, f, SLIDE_FRAMES);
        vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
    }

    /* 终帧：画最终 s_board 状态（合并后的值），再 flush */
    fb_draw_board(s_board);
    fb_flush();

    /* bounce：合并格放大一圈（一帧）*/
    bool bounce[GRID_N][GRID_N] = { { false } };
    bool any = false;
    for (int i = 0; i < n_tracks; i++) {
        if (tracks[i].merged) { bounce[tracks[i].tr][tracks[i].tc] = true; any = true; }
    }
    if (!any) return;

    /* 先在 FB 上画最终状态，然后把合并格放大一圈画上去，flush */
    fb_draw_board(s_board);
    int overshoot = 8;
    for (int r = 0; r < GRID_N; r++) {
        for (int c = 0; c < GRID_N; c++) {
            if (!bounce[r][c]) continue;
            int px, py;
            grid_to_fb(r, c, &px, &py);
            fb_draw_tile(px - overshoot / 2, py - overshoot / 2,
                         CELL_SIZE + overshoot, s_board[r][c]);
        }
    }
    fb_flush();
    vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS * 2));

    /* 恢复标准大小 */
    fb_draw_board(s_board);
    fb_flush();
}

static void play_popin_animation(int r, int c)
{
    if (s_board[r][c] == 0) return;
    int px, py;
    grid_to_fb(r, c, &px, &py);

    for (int f = 1; f <= POPIN_FRAMES; f++) {
        int pct = 40 + (100 - 40) * f / POPIN_FRAMES;
        int size = CELL_SIZE * pct / 100;
        int ox = (CELL_SIZE - size) / 2;

        /* 先画完整当前棋盘（包含其他格）到 FB，然后在 (r,c) 上用缩放尺寸覆盖 */
        fb_draw_board(s_board);
        /* 覆盖掉 (r,c) 原位置的方块（s_board 已经是新状态，含这个格）*/
        fb_fill_rect(px, py, px + CELL_SIZE - 1, py + CELL_SIZE - 1, EMPTY_COLOR);
        /* 画缩放版方块 */
        fb_draw_tile(px + ox, py + ox, size, s_board[r][c]);
        fb_flush();
        vTaskDelay(pdMS_TO_TICKS(FRAME_DELAY_MS));
    }
    /* 最终帧 */
    fb_draw_board(s_board);
    fb_flush();
}

/* ================================================================
 * 输入 + 主循环
 * ================================================================ */
static int parse_direction(uint16_t bits)
{
    uint16_t dir_mask = KEYPAD_BIT_R0C1 | KEYPAD_BIT_R2C1 |
                        KEYPAD_BIT_R1C0 | KEYPAD_BIT_R1C2;
    uint16_t dirs = bits & dir_mask;
    if (__builtin_popcount(dirs) != 1) return 0;
    if (dirs & KEYPAD_BIT_R0C1) return 1;
    if (dirs & KEYPAD_BIT_R2C1) return 2;
    if (dirs & KEYPAD_BIT_R1C0) return 3;
    if (dirs & KEYPAD_BIT_R1C2) return 4;
    return 0;
}

static void do_move(int dir)
{
    memcpy(s_prev, s_board, sizeof(s_prev));

    tile_move_t tracks[MAX_TRACKS];
    int n_tracks = 0;
    uint32_t gained = move_dir_track(dir, tracks, &n_tracks);

    if (!board_changed()) return;

    play_slide_animation(tracks, n_tracks);

    s_score += gained;
    if (s_score > s_best) s_best = s_score;
    s_moves++;

    int nr, nc;
    spawn_random_tile(&nr, &nc);
    if (nr >= 0) play_popin_animation(nr, nc);

    render_header();

    if (is_game_over()) {
        s_game_over = true;
        render_game_over();
    }
}

/* ================================================================
 * 公共 API
 * ================================================================ */
void game_2048_request_exit(void)
{
    s_exit_requested = true;
}

void game_2048_run(void)
{
    s_exit_requested = false;
    ESP_LOGI(TAG, "进入 2048");

    /* 分配 framebuffer（PSRAM，115KB）。只在运行期存在，退出时释放。 */
    if (!s_fb) {
        s_fb = heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_fb) {
            ESP_LOGE(TAG, "framebuffer 分配失败（PSRAM 不足）");
            return;
        }
    }
    memset(s_fb, 0, FB_BYTES);

    /* 清屏 + 初始化 */
    LCD_Fill(0, 0, LCD_W, LCD_H, BLACK);
    new_game();
    render_header();
    render_footer();

    /* 开场 pop-in：把起始两方块依次从 40% 长到 100% */
    fb_clear(BG_COLOR);
    for (int r = 0; r < GRID_N; r++)
        for (int c = 0; c < GRID_N; c++) {
            int px, py;
            grid_to_fb(r, c, &px, &py);
            fb_fill_rect(px, py, px + CELL_SIZE - 1, py + CELL_SIZE - 1, EMPTY_COLOR);
        }
    fb_flush();

    for (int r = 0; r < GRID_N; r++)
        for (int c = 0; c < GRID_N; c++)
            if (s_board[r][c] != 0) play_popin_animation(r, c);

    /* MPU6050 倾斜控制（可选）：进入游戏时初始化，失败则降级到纯按键 */
    bool tilt_available = mpu6050_init();
    if (tilt_available) {
        ESP_LOGI(TAG, "倾斜控制已启用");
    } else {
        ESP_LOGW(TAG, "MPU6050 不可用，仅按键控制");
    }

    /* 输入主循环 */
    TickType_t last_move_tick = 0;
    int last_dir = 0;
    int prev_parsed = 0;
    /* 倾斜方向需要"经过 NONE 才能再次触发"，避免持续倾斜连发 */
    int prev_tilt = 0;

    while (!s_exit_requested) {
        if (keypad_consume_exit_request()) {
            ESP_LOGI(TAG, "长按中键，退出 2048");
            break;
        }

        /* 1. 按键方向 */
        uint16_t bits = keypad_get_bits();
        int dir = parse_direction(bits);

        /* 2. 倾斜方向（仅在按键无方向输入时启用，避免冲突）*/
        int tilt = 0;
        if (tilt_available && dir == 0) {
            tilt = (int)mpu6050_get_direction();
        }

        /* 按键边沿触发 */
        if (dir != 0 && dir != prev_parsed) {
            TickType_t now = xTaskGetTickCount();
            TickType_t dt = (now - last_move_tick) * portTICK_PERIOD_MS;
            if (dir != last_dir || dt >= 150) {
                if (!s_game_over) {
                    do_move(dir);
                    last_dir = dir;
                    last_move_tick = now;
                }
            }
        }
        prev_parsed = dir;

        /* 倾斜边沿触发：必须从 NONE 切换到非 NONE 才动一次。
         * 这样持续倾斜不会狂刷方向，必须先回正再倾斜才再次触发。
         * （tilt != 0 由 prev_tilt == 0 && tilt != prev_tilt 隐含保证）*/
        if (prev_tilt == 0 && tilt != 0) {
            TickType_t now = xTaskGetTickCount();
            TickType_t dt = (now - last_move_tick) * portTICK_PERIOD_MS;
            if (dt >= 200) {   /* 倾斜冷却 200ms，避免抖动误触 */
                if (!s_game_over) {
                    do_move(tilt);
                    last_dir = tilt;
                    last_move_tick = now;
                }
            }
        }
        prev_tilt = tilt;

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGI(TAG, "退出 2048，最终分数 %lu，步数 %lu",
             (unsigned long)s_score, (unsigned long)s_moves);

    /* 释放 MPU6050（如果初始化过） */
    if (tilt_available) {
        mpu6050_deinit();
    }

    /* 释放 framebuffer */
    if (s_fb) {
        heap_caps_free(s_fb);
        s_fb = NULL;
    }
}
