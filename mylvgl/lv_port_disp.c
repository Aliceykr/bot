#include "lv_port_disp.h"
#include "lcd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

/* 全帧缓冲：PSRAM 双缓冲，LVGL FULL 模式 */
#define DISP_BUF_SIZE  (LCD_W * LCD_H * sizeof(lv_color_t))

/* 双 bounce 缓冲：内部 DMA 内存，ping-pong 并行拷贝+传输 */
#define BOUNCE_SIZE    4092   /* 匹配 ESP32-S3 SPI DMA 单次事务上限 */

static lv_display_t    *disp;
static lv_color_t      *buf1;
static lv_color_t      *buf2;
static uint8_t         *bounce_a;   /* 内部 DMA bounce 缓冲 A */
static uint8_t         *bounce_b;   /* 内部 DMA bounce 缓冲 B */
static spi_transaction_t trans_a;   /* DMA 事务 A */
static spi_transaction_t trans_b;   /* DMA 事务 B */
static bool dma_pending = false;    /* 是否有未完成的异步 DMA */

static void wait_prev_dma(void)
{
    if (!dma_pending) return;
    spi_transaction_t *ret_trans;
    spi_device_get_trans_result(s_spi, &ret_trans, portMAX_DELAY);
    dma_pending = false;
}

static void disp_flush(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w    = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h    = (uint32_t)(area->y2 - area->y1 + 1);
    uint32_t size = w * h * 2U;

    LCD_Address_Set(area->x1, area->y1, area->x2, area->y2);
    gpio_set_level(LCD_DC_PIN, 1);

    /* Ping-pong 传输：拷贝到 A 时 B 在传，拷贝到 B 时 A 在传 */
    const uint8_t *src = px_map;
    uint32_t remain = size;
    bool use_a = true;

    while (remain > 0) {
        uint32_t chunk = (remain > BOUNCE_SIZE) ? BOUNCE_SIZE : remain;
        uint8_t *dst = use_a ? bounce_a : bounce_b;
        memcpy(dst, src, chunk);

        /* 等上一个 DMA 完成，提交当前块 */
        wait_prev_dma();
        spi_transaction_t *t = use_a ? &trans_a : &trans_b;
        memset(t, 0, sizeof(*t));
        t->length    = chunk * 8;
        t->tx_buffer = dst;
        spi_device_queue_trans(s_spi, t, portMAX_DELAY);
        dma_pending = true;

        src    += chunk;
        remain -= chunk;
        use_a   = !use_a;
    }

    /* 等最后一个 DMA 块完成 */
    wait_prev_dma();
    lv_display_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    buf1 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    buf2 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf1) {
        ESP_LOGE("DISP", "PSRAM alloc failed (%u bytes)", (unsigned)DISP_BUF_SIZE);
        return;
    }
    if (!buf2) ESP_LOGW("DISP", "No PSRAM for buf2, single buffer");

    bounce_a = heap_caps_malloc(BOUNCE_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    bounce_b = heap_caps_malloc(BOUNCE_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!bounce_a || !bounce_b) {
        ESP_LOGE("DISP", "bounce buffer alloc failed");
        return;
    }

    disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, disp_flush);

    lv_display_render_mode_t mode = buf2 ? LV_DISPLAY_RENDER_MODE_FULL
                                         : LV_DISPLAY_RENDER_MODE_PARTIAL;
    lv_display_set_buffers(disp, buf1, buf2, DISP_BUF_SIZE, mode);

    ESP_LOGI("DISP", "PSRAM %u+D%u, bounce %u×2, SPI 80MHz, %s mode",
             (unsigned)DISP_BUF_SIZE, (unsigned)DISP_BUF_SIZE,
             (unsigned)BOUNCE_SIZE, buf2 ? "FULL" : "PARTIAL");
}
