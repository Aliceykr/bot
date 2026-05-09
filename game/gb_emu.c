#include "gb_emu.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "lcd.h"

/* Peanut-GB 配置：
 *   ENABLE_SOUND 关掉（下一步做）
 *   ENABLE_LCD 开
 *   PEANUT_GB_HIGH_LCD_ACCURACY 关掉：关精确 sprite 排序，CPU 省 15-25%
 *     换来代价：复杂场景可能有 sprite 优先级错乱，塞尔达/Tobu 不明显 */
#define ENABLE_SOUND        0
#define ENABLE_LCD          1
#define PEANUT_GB_HIGH_LCD_ACCURACY 0

#include "peanut_gb.h"

#define TAG "GB_EMU"

/* GB 原生分辨率 */
#define GB_W 160
#define GB_H 144

/* 显示到 LCD 的缩放因子：1.5× → 240×216，竖屏 240×320 正好有上下留白 */
/* 240 / 160 = 1.5 → 整数乘表：x_dst = x_src * 3 / 2 */
#define SCALED_W 240
#define SCALED_H 216
#define OFFSET_X ((LCD_W - SCALED_W) / 2)   /* = 0 */
#define OFFSET_Y ((LCD_H - SCALED_H) / 2)   /* = 52 */

/* GB 4 色调色板（DMG 经典绿灰色调，BGR 顺序因 LV_COLOR_FORMAT_RGB565_SWAPPED）。
 * 顺序：0 白 → 3 黑 */
static const uint16_t s_palette_rgb565[4] = {
    0xFFFF, /* 白 */
    0xB596, /* 浅灰 */
    0x52AA, /* 深灰 */
    0x0000, /* 黑 */
};

/* 运行期上下文：放在 ctx 以便 Peanut-GB 回调能取到 */
typedef struct {
    const uint8_t *rom;            /* ROM 数据源（可能指向 PSRAM 或 DRAM 副本）*/
    uint8_t       *rom_dram_copy;  /* ROM 在 DRAM 的副本；无副本时为 NULL */
    size_t         rom_size;
    uint8_t       *cart_ram;
    size_t         cart_ram_size;
    /* 每行 scaled 缓冲（DRAM，DMA 可直接读）。
     * 一行 240 像素 RGB565 = 480 字节，为 1.5× 纵向一次算 3 行：1440 字节 */
    uint16_t      *line_buf;
} gb_ctx_t;

static gb_ctx_t s_ctx;
static volatile bool s_exit_requested = false;
static struct gb_s s_gb;

/* ================================================================
 * Peanut-GB 回调
 * ================================================================ */
static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    gb_ctx_t *c = (gb_ctx_t *)gb->direct.priv;
    if (addr < c->rom_size) return c->rom[addr];
    return 0xFF;
}

static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    gb_ctx_t *c = (gb_ctx_t *)gb->direct.priv;
    if (c->cart_ram && addr < c->cart_ram_size) return c->cart_ram[addr];
    return 0xFF;
}

static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val)
{
    gb_ctx_t *c = (gb_ctx_t *)gb->direct.priv;
    if (c->cart_ram && addr < c->cart_ram_size) c->cart_ram[addr] = val;
}

static void gb_error_cb(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t val)
{
    ESP_LOGE(TAG, "gb_error %d val=0x%04x", gb_err, val);
    s_exit_requested = true;  /* 致命错误直接退出 */
}

/* 把一行 GB 像素（160 宽，每像素 2bit 色 + 其他 bit）扩展到 scaled 缓冲。
 * 横向 1.5×：每 2 个源像素 → 3 个目标像素（复制中间像素）。
 * 输出：dst_line 240 个 RGB565 */
static inline void scale_line_1p5x(const uint8_t *src, uint16_t *dst_line)
{
    /* 160 个源像素按每 2 个一组扫描，共 80 组；每组输出 3 个目标像素 */
    const uint8_t *s = src;
    uint16_t *d = dst_line;
    for (int i = 0; i < 80; i++) {
        uint16_t a = s_palette_rgb565[s[0] & LCD_COLOUR];
        uint16_t b = s_palette_rgb565[s[1] & LCD_COLOUR];
        /* 3 个像素：a, a (近 a), b —— 简单策略，视觉上接近原图 */
        d[0] = a;
        d[1] = a;
        d[2] = b;
        s += 2;
        d += 3;
    }
}

/* LCD 行绘制回调
 * line 长度 160 字节（每字节 2bit 色 + 2bit 其他），对应 GB 一行
 * y 是 GB 行号 (0..143)
 *
 * 性能优化：
 *   - 每对 GB 行（偶+奇）批量：3 屏幕行 = 1440 字节一次 DMA
 *   - 双缓冲 + 异步 DMA：CPU 计算下一组 scale 时，上一组正在 DMA 传输
 *   - 每帧 Address_Set 72 次，数据 DMA 72 次 */
static DRAM_ATTR uint16_t s_group_buf[2][SCALED_W * 3];
static int s_buf_idx = 0;
static bool s_dma_busy = false;
static spi_transaction_t s_dma_trans;

/* ---- 性能诊断开关 ----
 * 置 1 可跳过 LCD 输出，只看 Peanut-GB 核心 FPS。
 * 用于定位瓶颈：
 *   - 跳过后 FPS 大幅提升 → 瓶颈在 SPI/显示
 *   - 跳过后 FPS 基本不变 → 瓶颈在 CPU（模拟器核心或 ROM 读取）
 * 正式使用时保持 0 */
#define GB_SKIP_LCD  0

static void lcd_draw_line_cb(struct gb_s *gb,
                             const uint8_t *line,
                             const uint_fast8_t y)
{
    if (s_exit_requested) return;
#if GB_SKIP_LCD
    (void)line; (void)y;
    return;
#endif

    uint16_t *buf = s_group_buf[s_buf_idx];

    if ((y & 1) == 0) {
        /* 偶数行：scale 到当前 buf 的第 1 段，等奇数行 */
        scale_line_1p5x(line, buf);
        return;
    }

    /* 奇数行：完成 3 屏幕行 */
    scale_line_1p5x(line, buf + SCALED_W);
    memcpy(buf + SCALED_W * 2, buf + SCALED_W, SCALED_W * 2);

    /* 等上一组 DMA 完成（如果在跑），才能修改窗口地址和复用 trans 对象 */
    if (s_dma_busy) {
        spi_transaction_t *ret;
        spi_device_get_trans_result(s_spi, &ret, portMAX_DELAY);
        s_dma_busy = false;
    }

    /* 设置窗口（这部分必须同步等完成，否则与后续 DMA 事务抢 SPI 总线）*/
    int screen_y0 = OFFSET_Y + ((y - 1) * 3) / 2;
    LCD_Address_Set(OFFSET_X, screen_y0,
                    OFFSET_X + SCALED_W - 1, screen_y0 + 2);
    gpio_set_level(LCD_DC_PIN, 1);

    /* 异步提交 DMA：函数返回后 DMA 在后台跑，CPU 可继续算下一组 scale */
    memset(&s_dma_trans, 0, sizeof(s_dma_trans));
    s_dma_trans.length    = SCALED_W * 3 * 2 * 8;  /* 1440 字节 */
    s_dma_trans.tx_buffer = buf;
    spi_device_queue_trans(s_spi, &s_dma_trans, portMAX_DELAY);
    s_dma_busy = true;

    /* 切换到另一 buffer，下一组用 */
    s_buf_idx ^= 1;
}

/* 帧末尾调用：等最后一组 DMA 完成，确保画面完整 */
static inline void flush_pending_dma(void)
{
    if (s_dma_busy) {
        spi_transaction_t *ret;
        spi_device_get_trans_result(s_spi, &ret, portMAX_DELAY);
        s_dma_busy = false;
    }
}

/* ================================================================
 * 公开接口
 * ================================================================ */
void gb_emu_request_exit(void)
{
    s_exit_requested = true;
}

bool gb_emu_run(const uint8_t *rom_data, size_t rom_size)
{
    s_exit_requested = false;
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.rom      = rom_data;
    s_ctx.rom_size = rom_size;

    /* 如果 ROM 小到能塞进内部 DRAM，复制一份过去。
     * PSRAM 访问有 cache miss 开销，DRAM 副本能显著提升 CPU 仿真速度（+30-50%）。
     * 阈值 256KB：留出约 150KB 给 WiFi/LVGL/栈。
     * 注意 heap_caps_malloc 失败时继续用 PSRAM 原数据，不致命。 */
    if (rom_size <= 256 * 1024) {
        size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        if (dram_free >= rom_size + 64 * 1024) {
            uint8_t *copy = heap_caps_malloc(rom_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (copy) {
                memcpy(copy, rom_data, rom_size);
                s_ctx.rom_dram_copy = copy;
                s_ctx.rom = copy;
                ESP_LOGI(TAG, "ROM 已拷贝到 DRAM (%u bytes)", (unsigned)rom_size);
            }
        } else {
            ESP_LOGW(TAG, "DRAM 不足 (%u 字节可用)，ROM 留 PSRAM", (unsigned)dram_free);
        }
    } else {
        ESP_LOGI(TAG, "ROM 过大 (%u 字节)，仅能使用 PSRAM", (unsigned)rom_size);
    }

    /* 行扫描缓冲（可选优化：放 DRAM），本例用栈更简单，240 * 2 = 480 字节 */
    s_ctx.line_buf = NULL;

    /* 初始化 Peanut-GB */
    enum gb_init_error_e rc = gb_init(&s_gb,
        gb_rom_read, gb_cart_ram_read, gb_cart_ram_write,
        gb_error_cb, &s_ctx);
    if (rc != GB_INIT_NO_ERROR) {
        ESP_LOGE(TAG, "gb_init 失败: %d", rc);
        return false;
    }

    /* 分配 Cart RAM（存档用，存档持久化留给下一步）*/
    size_t save_size = 0;
    if (gb_get_save_size_s(&s_gb, &save_size) == 0 && save_size > 0) {
        s_ctx.cart_ram = heap_caps_malloc(save_size, MALLOC_CAP_SPIRAM);
        if (!s_ctx.cart_ram) {
            ESP_LOGE(TAG, "Cart RAM 分配失败 %u", (unsigned)save_size);
            return false;
        }
        memset(s_ctx.cart_ram, 0, save_size);
        s_ctx.cart_ram_size = save_size;
        ESP_LOGI(TAG, "Cart RAM: %u 字节", (unsigned)save_size);
    } else {
        ESP_LOGI(TAG, "ROM 无 Cart RAM");
    }

    /* 初始化 LCD 回调 */
    gb_init_lcd(&s_gb, lcd_draw_line_cb);
    /* 帧跳跃模式：CPU 每秒仿真 60 帧保证游戏速度正常，每隔一帧跳过渲染
     * 只显示 30fps。屏幕 OSD 的 FPS 是"仿真帧率"，稳定 60 说明游戏速度正常。 */
    s_gb.direct.frame_skip = 1;
    /* 默认 frame skip = 0 不跳帧。性能不足时可 s_gb.direct.frame_skip = 1 */

    /* 初始按键全松开（高电平有效：0xFF = 无按键）*/
    s_gb.direct.joypad = 0xFF;

    ESP_LOGI(TAG, "进入模拟器主循环");
    int64_t last_log = esp_timer_get_time();
    int frames = 0;
    int displayed_fps = 0;

    while (!s_exit_requested) {
        int64_t t0 = esp_timer_get_time();
        gb_run_frame(&s_gb);
        flush_pending_dma();
        frames++;

        /* 帧率统计：每秒更新一次，同时打印日志 + 叠加到屏幕 */
        int64_t now = esp_timer_get_time();
        if (now - last_log >= 1000000) {
            displayed_fps = frames;
            ESP_LOGI(TAG, "FPS=%d heap_free=%u",
                     displayed_fps, (unsigned)esp_get_free_heap_size());
            frames = 0;
            last_log = now;

            /* 右下角 OSD：画面底部留白区显示 FPS */
            char fps_str[16];
            snprintf(fps_str, sizeof(fps_str), "FPS %2d", displayed_fps);
            LCD_ShowString(LCD_W - 56, LCD_H - 20,
                           (const uint8_t *)fps_str,
                           0xFFE0,   /* 黄色 RGB565 */
                           0x0000,   /* 黑底 */
                           16, 0);
        }

        /* 60Hz 节拍锁：GB 原生 59.7Hz（16.74ms/frame）。
         * 仿真按 60Hz 跑，渲染 frame_skip=1 实际显示 30fps。
         * 游戏逻辑/音效/计时全部按原速运行。 */
        int64_t spent_us = now - t0;
        const int64_t target_us = 16743;
        if (spent_us < target_us) {
            int64_t sleep_us = target_us - spent_us;
            if (sleep_us >= 1000) {
                vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
            } else {
                taskYIELD();
            }
        } else {
            taskYIELD();
        }
    }

    ESP_LOGI(TAG, "退出模拟器主循环");

    if (s_ctx.cart_ram) {
        heap_caps_free(s_ctx.cart_ram);
        s_ctx.cart_ram = NULL;
    }
    if (s_ctx.rom_dram_copy) {
        heap_caps_free(s_ctx.rom_dram_copy);
        s_ctx.rom_dram_copy = NULL;
    }

    return true;
}
