#include "lv_port_disp.h"
#include "lcd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "inttypes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

/* 全帧缓冲：PSRAM 双缓冲，LVGL FULL 模式 */
#define DISP_BUF_SIZE  (LCD_W * LCD_H * sizeof(lv_color_t))
/* SPI DMA 中转缓冲：内部 DMA 内存，每次拷贝 BOUNCE_SIZE 字节后发送 */
#define BOUNCE_SIZE    8192

static lv_display_t    *disp;
static lv_color_t      *buf1;        /* PSRAM 渲染缓冲 */
static lv_color_t      *buf2;        /* PSRAM 渲染缓冲 */
static uint8_t         *bounce_buf;  /* 内部 DMA 发送中转 */

static void disp_flush(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w    = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h    = (uint32_t)(area->y2 - area->y1 + 1);
    uint32_t size = w * h * 2U;

    LCD_Address_Set(area->x1, area->y1, area->x2, area->y2);

    /* 从 PSRAM 拷贝到内部 DMA 缓冲，分块发送，避免 SPI DMA 直接访问 PSRAM */
    const uint8_t *src = px_map;
    uint32_t remain = size;
    while (remain > 0) {
        uint32_t chunk = (remain > BOUNCE_SIZE) ? BOUNCE_SIZE : remain;
        memcpy(bounce_buf, src, chunk);
        LCD_Send_Buf(bounce_buf, chunk);
        src += chunk;
        remain -= chunk;
    }

    lv_display_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    /* PSRAM 全帧双缓冲 */
    buf1 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    buf2 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf1) {
        ESP_LOGE("DISP", "PSRAM alloc failed (%u bytes)", (unsigned)DISP_BUF_SIZE);
        return;
    }
    if (!buf2) {
        ESP_LOGW("DISP", "No PSRAM for buf2, single buffer mode");
    }

    /* 内部 DMA 中转缓冲，保证 SPI DMA 可直接访问 */
    bounce_buf = heap_caps_malloc(BOUNCE_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!bounce_buf) {
        ESP_LOGE("DISP", "bounce buffer alloc failed (%u bytes)", (unsigned)BOUNCE_SIZE);
        return;
    }

    disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, disp_flush);

    lv_display_render_mode_t mode = buf2 ? LV_DISPLAY_RENDER_MODE_FULL
                                         : LV_DISPLAY_RENDER_MODE_PARTIAL;
    lv_display_set_buffers(disp, buf1, buf2, DISP_BUF_SIZE, mode);

    ESP_LOGI("DISP", "PSRAM double buf %u+D%u, bounce %u, %s mode",
             (unsigned)DISP_BUF_SIZE, (unsigned)DISP_BUF_SIZE,
             (unsigned)BOUNCE_SIZE, buf2 ? "FULL" : "PARTIAL");
}
