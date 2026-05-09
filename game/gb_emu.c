#include "gb_emu.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "lcd.h"

/* =================================================================
 * Walnut-CGB 集成（DMG-only 配置，后续再开 CGB）
 * =================================================================
 * 性能开关：
 *   - WALNUT_GB_32BIT_DMA（默认 1）：用 32 位 DMA 路径，ESP32-S3 原生支持
 *   - WALNUT_GB_12_COLOUR 0 + WALNUT_FULL_GBC_SUPPORT 0：只跑 DMG，内存少一半
 *   - WALNUT_GB_HIGH_LCD_ACCURACY 0：关闭精确 sprite 排序省 CPU
 *
 * 性能策略：gb_run_frame_dualfetch() 代替 gb_run_frame()：双指令取
 */
#define ENABLE_SOUND                    1
#define ENABLE_LCD                      1
/* DMG-only 模式：保持 60fps 稳定。
 * CGB 彩色开过（WALNUT_GB_12_COLOUR=1 + WALNUT_FULL_GBC_SUPPORT=1），
 * 但塞尔达 DX 场景下帧率掉到 30fps，CPU 不够。回退 DMG 灰白。 */
#define WALNUT_GB_12_COLOUR             0
#define WALNUT_FULL_GBC_SUPPORT         0
#define WALNUT_GB_HIGH_LCD_ACCURACY     0

/* 先 include audio 头，让 walnut_cgb.h 里的 audio_read/audio_write
 * 调用看到正确的函数原型（否则会警告 implicit declaration 并与后续定义冲突）*/
#include "gb_audio.h"
#include "walnut_cgb.h"
#include "keypad.h"

#define TAG "GB_EMU"

/* GB 原生分辨率 */
#define GB_W 160
#define GB_H 144

/* 1.5× 缩放 → 240×216，居中显示 */
#define SCALED_W 240
#define SCALED_H 216
#define OFFSET_X ((LCD_W - SCALED_W) / 2)
#define OFFSET_Y ((LCD_H - SCALED_H) / 2)

/* DMG 4 色灰度调色板（RGB565 SWAPPED 格式）*/
static const uint16_t s_palette_dmg_rgb565[4] = {
    0xFFFF, /* 白 */
    0xB596, /* 浅灰 */
    0x52AA, /* 深灰 */
    0x0000, /* 黑 */
};

/* 运行期上下文 */
typedef struct {
    const uint8_t *rom;
    uint8_t       *rom_dram_copy;  /* 若 ROM 小到能放 DRAM 加速 */
    size_t         rom_size;
    uint8_t       *cart_ram;
    size_t         cart_ram_size;
} gb_ctx_t;

static gb_ctx_t s_ctx;
static volatile bool s_exit_requested = false;
static struct gb_s s_gb;

/* ================================================================
 * Peanut/Walnut 回调：ROM/Cart RAM 读写
 * Walnut 新增 read16/read32：批量取多字节，加速指令 fetch
 * ================================================================ */
static IRAM_ATTR uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    gb_ctx_t *c = (gb_ctx_t *)gb->direct.priv;
    if (addr < c->rom_size) return c->rom[addr];
    return 0xFF;
}

static IRAM_ATTR uint16_t gb_rom_read16(struct gb_s *gb, const uint_fast32_t addr)
{
    gb_ctx_t *c = (gb_ctx_t *)gb->direct.priv;
    if (addr + 1 < c->rom_size) {
        /* 小端访问；ESP32-S3 支持非对齐读 */
        return *(const uint16_t *)(c->rom + addr);
    }
    return 0xFFFF;
}

static IRAM_ATTR uint32_t gb_rom_read32(struct gb_s *gb, const uint_fast32_t addr)
{
    gb_ctx_t *c = (gb_ctx_t *)gb->direct.priv;
    if (addr + 3 < c->rom_size) {
        return *(const uint32_t *)(c->rom + addr);
    }
    return 0xFFFFFFFF;
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
    s_exit_requested = true;
}

/* ================================================================
 * 1.5× 横向缩放：160 px → 240 px
 * DMG 模式：直接用灰度表查色
 * ================================================================ */
static inline IRAM_ATTR void scale_line_1p5x(const uint8_t *src, uint16_t *dst_line)
{
    const uint8_t *s = src;
    uint16_t *d = dst_line;
    for (int i = 0; i < 80; i++) {
        uint16_t a = s_palette_dmg_rgb565[s[0] & LCD_COLOUR];
        uint16_t b = s_palette_dmg_rgb565[s[1] & LCD_COLOUR];
        d[0] = a;
        d[1] = a;
        d[2] = b;
        s += 2;
        d += 3;
    }
}

/* ================================================================
 * LCD 行绘制：每 2 个 GB 行批量成 3 屏幕行 × 1440 字节 DMA
 * 双缓冲异步传输，DMA 与 CPU 重叠
 * ================================================================ */
static DRAM_ATTR uint16_t s_group_buf[2][SCALED_W * 3];
static int s_buf_idx = 0;
static bool s_dma_busy = false;
static spi_transaction_t s_dma_trans;

static IRAM_ATTR void lcd_draw_line_cb(struct gb_s *gb,
                                       const uint8_t *line,
                                       const uint_fast8_t y)
{
    if (s_exit_requested) return;

    uint16_t *buf = s_group_buf[s_buf_idx];

    if ((y & 1) == 0) {
        /* 偶数行：先 scale 到 buf 第 1 段，等下一行 */
        scale_line_1p5x(line, buf);
        return;
    }

    /* 奇数行：完成 3 屏幕行 */
    scale_line_1p5x(line, buf + SCALED_W);
    memcpy(buf + SCALED_W * 2, buf + SCALED_W, SCALED_W * 2);

    /* 等上一组 DMA 完成（若在跑）*/
    if (s_dma_busy) {
        spi_transaction_t *ret;
        spi_device_get_trans_result(s_spi, &ret, portMAX_DELAY);
        s_dma_busy = false;
    }

    /* 设置窗口 */
    int screen_y0 = OFFSET_Y + ((y - 1) * 3) / 2;
    LCD_Address_Set(OFFSET_X, screen_y0,
                    OFFSET_X + SCALED_W - 1, screen_y0 + 2);
    gpio_set_level(LCD_DC_PIN, 1);

    /* 异步 DMA */
    memset(&s_dma_trans, 0, sizeof(s_dma_trans));
    s_dma_trans.length    = SCALED_W * 3 * 2 * 8;
    s_dma_trans.tx_buffer = buf;
    spi_device_queue_trans(s_spi, &s_dma_trans, portMAX_DELAY);
    s_dma_busy = true;

    s_buf_idx ^= 1;
}

/* 等帧末尾的 DMA 完成 */
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

    /* ROM 保留在 PSRAM。之前做过"小 ROM 拷到 DRAM 加速"的优化，
     * 但 Walnut-CGB + dualfetch + 32bit DMA 已能稳 60fps，
     * 不再需要 DRAM 副本。腾出 DRAM 给 WiFi/mbedtls 和其他业务。*/

    /* Walnut-CGB 的 gb_init 多两个 16/32 位回调 */
    enum gb_init_error_e rc = gb_init(&s_gb,
        gb_rom_read, gb_rom_read16, gb_rom_read32,
        gb_cart_ram_read, gb_cart_ram_write,
        gb_error_cb, &s_ctx);
    if (rc != GB_INIT_NO_ERROR) {
        ESP_LOGE(TAG, "gb_init 失败: %d", rc);
        return false;
    }

    /* Cart RAM（存档）*/
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

    gb_init_lcd(&s_gb, lcd_draw_line_cb);
    /* frame_skip=1：渲染隔帧，仿真仍按 60fps；画面 30fps 但游戏速度正常 */
    s_gb.direct.frame_skip = 1;
    s_gb.direct.joypad = 0xFF;

    /* 矩阵键盘：app_main 已初始化，此处直接使用即可 */

    /* 初始化 APU */
    gb_audio_init();

    ESP_LOGI(TAG, "进入 Walnut-CGB 主循环（dualfetch + 32bit DMA）");
    int64_t last_log = esp_timer_get_time();
    int frames = 0;
    int displayed_fps = 0;
    /* 下一帧的 deadline（单调时间戳），累加式避免 tick 抖动导致慢半拍 */
    int64_t next_deadline_us = esp_timer_get_time();
    const int64_t period_us = 16743;  /* 59.7Hz GB 原生帧时长 */

    while (!s_exit_requested) {
        /* 读矩阵键盘位图 → 映射成 GB direct.joypad（0=按下，位布局见 walnut_cgb.h）
         * 布局：R0C0=B  R0C1=UP  R0C2=A
         *       R1C0=LEFT   R1C1=EXIT（长按退出，不映射按键）  R1C2=RIGHT
         *       R2C0=SELECT R2C1=DOWN R2C2=START */
        uint16_t kp = keypad_get_bits();
        uint8_t pad = 0xFF;
        if (kp & KEYPAD_BIT_R0C0) pad &= (uint8_t)~JOYPAD_B;
        if (kp & KEYPAD_BIT_R0C1) pad &= (uint8_t)~JOYPAD_UP;
        if (kp & KEYPAD_BIT_R0C2) pad &= (uint8_t)~JOYPAD_A;
        if (kp & KEYPAD_BIT_R1C0) pad &= (uint8_t)~JOYPAD_LEFT;
        if (kp & KEYPAD_BIT_R1C2) pad &= (uint8_t)~JOYPAD_RIGHT;
        if (kp & KEYPAD_BIT_R2C0) pad &= (uint8_t)~JOYPAD_SELECT;
        if (kp & KEYPAD_BIT_R2C1) pad &= (uint8_t)~JOYPAD_DOWN;
        if (kp & KEYPAD_BIT_R2C2) pad &= (uint8_t)~JOYPAD_START;
        s_gb.direct.joypad = pad;

        /* 长按中间键 800ms → keypad 模块置位，这里消费 */
        if (keypad_consume_exit_request()) {
            s_exit_requested = true;
        }

        /* Walnut 的高性能入口：双指令取 + 链式执行 */
        gb_run_frame_dualfetch(&s_gb);
        flush_pending_dma();
        /* 每帧末尾生成音频并推给 speaker */
        gb_audio_emit_frame();
        frames++;

        int64_t now = esp_timer_get_time();
        if (now - last_log >= 1000000) {
            displayed_fps = frames;
            ESP_LOGI(TAG, "FPS=%d heap_free=%u",
                     displayed_fps, (unsigned)esp_get_free_heap_size());
            frames = 0;
            last_log = now;

            char fps_str[16];
            snprintf(fps_str, sizeof(fps_str), "FPS %2d", displayed_fps);
            LCD_ShowString(LCD_W - 56, LCD_H - 20,
                           (const uint8_t *)fps_str,
                           0xFFE0, 0x0000, 16, 0);
        }

        /* 累加式 60Hz 节拍：deadline 按 period_us 递增，睡到 deadline。
         * 即使某帧超时，下一帧 deadline 仍按时间线推进，不会累积误差。
         * 落后超过一帧时重新对齐，避免追不上 */
        next_deadline_us += period_us;
        int64_t now2 = esp_timer_get_time();
        if (now2 > next_deadline_us + period_us * 2) {
            /* 追不上：重置基线 */
            next_deadline_us = now2 + period_us;
            taskYIELD();
        } else {
            int64_t sleep_us = next_deadline_us - now2;
            if (sleep_us >= 2000) {
                /* 大于 2ms 用 vTaskDelay，精度足够且省 CPU */
                vTaskDelay(pdMS_TO_TICKS(sleep_us / 1000));
            } else if (sleep_us > 0) {
                /* 最后几百微秒用忙等对齐，保证精确到帧边界 */
                esp_rom_delay_us((uint32_t)sleep_us);
            }
        }
    }

    ESP_LOGI(TAG, "退出模拟器主循环");

    /* 销毁 APU 任务 + 信号量，释放 ram */
    gb_audio_deinit();

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
