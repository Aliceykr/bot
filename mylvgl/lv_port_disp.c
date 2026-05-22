#include "lv_port_disp.h"
#include "lcd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include <string.h>

/* =====================================================================
 * PARTIAL 渲染模式 + DRAM 双缓冲 + 异步 DMA
 *
 * 关键修复：原 FULL 模式下 px_map 是整屏 buffer 起点，脏区数据不连续，
 *   disp_flush 按连续数据发送会导致错位闪烁。
 * PARTIAL 模式下 px_map 只含脏区像素且连续排列，可直接 DMA 发送。
 *
 * buffer 放在内部 DRAM（MALLOC_CAP_DMA），SPI DMA 可直接读取，
 * 无需 PSRAM→DRAM 的 bounce memcpy，速度提升 2-3 倍。
 *
 * 双缓冲 + 异步入队 DMA：flush 返回后 LVGL 立即渲染下一块到另一个 buffer，
 * SPI DMA 在后台把上一块送出。下次 flush 进来时等上一轮 DMA 完成再提交。
 * ===================================================================== */

/* 每个 buffer 行数。约 1/10 屏（LVGL 建议最小值）。
 * 34 行 × 240 × 2 字节 = 16320 字节 < 16KB，DRAM 能轻松容纳。
 * 调大可略提升大脏区刷新效率，但增加 DRAM 占用。 */
#define DISP_BUF_LINES  34
#define DISP_BUF_SIZE   (DISP_BUF_LINES * LCD_W * 2)

/* SPI DMA 单次事务硬件上限（ESP32-S3）*/
#define DMA_MAX_CHUNK   4092U

/* 单次 flush 的 DMA 事务数上限：16320/4092 = 4 块，预留 5 个 */
#define MAX_TRANS_PER_FLUSH  5

static lv_display_t *disp;
static lv_color_t   *buf1 = NULL;
static lv_color_t   *buf2 = NULL;

/* DMA 事务池：需在传输期间保持有效，故 static */
static spi_transaction_t s_trans[MAX_TRANS_PER_FLUSH];
static int s_pending = 0;  /* 当前已入队但未 get_result 的事务数 */

/* 暂停标志：置位时 disp_flush 立即 ready，不再发送 SPI。
 * 用于游戏模式接管 LCD 前释放 SPI 总线 */
static volatile bool s_flush_suspended = false;

/* 等待所有已入队的 DMA 事务完成 */
static void wait_all_dma(void)
{
    if (!s_spi) {
        s_pending = 0;
        return;
    }
    while (s_pending > 0) {
        spi_transaction_t *ret;
        spi_device_get_trans_result(s_spi, &ret, portMAX_DELAY);
        s_pending--;
    }
}

static void disp_flush(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *px_map)
{
    /* 暂停模式：不发送 SPI，直接告诉 LVGL 已完成。游戏模式下用这种方式
     * 让出 SPI 总线，避免 polling 和 queue 两种传输模式互相冲突 */
    if (s_flush_suspended) {
        lv_display_flush_ready(disp_drv);
        return;
    }
    if (!s_spi) {
        lv_display_flush_ready(disp_drv);
        return;
    }

    /* 等上一轮所有 DMA 完成，才能修改窗口地址和复用 trans 池 */
    wait_all_dma();

    /* 设置 LCD 写入窗口（polling 同步发送命令，耗时极短）*/
    LCD_Address_Set(area->x1, area->y1, area->x2, area->y2);
    gpio_set_level(LCD_DC_PIN, 1);  /* DC=1 表示接下来是像素数据 */

    uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
    uint32_t size = w * h * 2U;  /* RGB565 每像素 2 字节 */

    /* px_map 在 PARTIAL 模式下是脏区连续像素，可直接 DMA。
     * 按 4092 字节上限分块入队，全部异步。 */
    const uint8_t *p = px_map;
    uint32_t remain = size;
    int idx = 0;

    while (remain > 0 && idx < MAX_TRANS_PER_FLUSH) {
        uint32_t chunk = (remain > DMA_MAX_CHUNK) ? DMA_MAX_CHUNK : remain;

        memset(&s_trans[idx], 0, sizeof(s_trans[idx]));
        s_trans[idx].length    = (size_t)chunk * 8;
        s_trans[idx].tx_buffer = p;
        spi_device_queue_trans(s_spi, &s_trans[idx], portMAX_DELAY);
        s_pending++;

        p      += chunk;
        remain -= chunk;
        idx++;
    }

    /* 理论上 size 不应超过 DISP_BUF_SIZE，MAX_TRANS_PER_FLUSH 也设了余量。
     * 若真超过，剩余数据 fallback 同步发送（防止静默丢失）*/
    if (remain > 0) {
        wait_all_dma();  /* 先等上面的入队完成，避免混用 polling 和 queue */
        while (remain > 0) {
            uint32_t chunk = (remain > DMA_MAX_CHUNK) ? DMA_MAX_CHUNK : remain;
            spi_transaction_t t = { .length = chunk * 8, .tx_buffer = p };
            spi_device_polling_transmit(s_spi, &t);
            p += chunk;
            remain -= chunk;
        }
    }

    /* 立即告知 LVGL 可以渲染下一块：DMA 在后台跑，下次 flush 会等它们 */
    lv_display_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    if (!s_spi) {
        ESP_LOGE("DISP", "display init skipped: LCD SPI device not initialized");
        return;
    }
    /* 两个 buffer 都放在内部 DRAM（MALLOC_CAP_DMA）：
     * SPI DMA 可直接读取，避免 PSRAM 的 bounce 开销 */
    buf1 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    buf2 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!buf1) {
        ESP_LOGE("DISP", "DRAM 分配 buf1 失败 (%u bytes)", (unsigned)DISP_BUF_SIZE);
        return;
    }
    if (!buf2) {
        ESP_LOGW("DISP", "DRAM 分配 buf2 失败，退化为单缓冲");
    }

    disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, disp_flush);

    /* PARTIAL 模式：LVGL 按 buffer 大小分块渲染，px_map 传连续脏区数据 */
    lv_display_set_buffers(disp, buf1, buf2, DISP_BUF_SIZE,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI("DISP", "PARTIAL mode, DRAM 2×%u bytes, SPI 80MHz, %s",
             (unsigned)DISP_BUF_SIZE, buf2 ? "double-buffer" : "single-buffer");
}

/* 暂停 LVGL 的 SPI 输出，并等待所有 DMA 完成。调用后 SPI 总线空闲，
 * 其他模块（如游戏 runtime）可独占使用 LCD。 */
void lv_port_disp_suspend(void)
{
    s_flush_suspended = true;
    /* 等干净再返回，避免调用者刚开始用 SPI 就撞上尾随的 DMA */
    wait_all_dma();
}

/* 恢复 LVGL 输出。恢复后一般需要手动 invalidate 强制整屏重绘 */
void lv_port_disp_resume(void)
{
    s_flush_suspended = false;
}

/* 查询当前是否处于暂停状态（供 LVGL 主任务决定是否跳过 lv_timer_handler）*/
bool lv_port_disp_is_suspended(void)
{
    return s_flush_suspended;
}
