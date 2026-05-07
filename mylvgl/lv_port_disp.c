#include "lv_port_disp.h"
#include "lcd.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "inttypes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* PARTIAL 模式：每次刷新 20 行，缓冲区分配于内部 DMA 内存 */
#define BUF_LINES  20
#define DISP_BUF_SIZE  (LCD_W * BUF_LINES * sizeof(lv_color_t))

static lv_display_t    *disp;
static lv_color_t      *buf1;
static lv_color_t      *buf2;

static void disp_flush(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w    = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h    = (uint32_t)(area->y2 - area->y1 + 1);
    uint32_t size = w * h * 2U;

    ESP_LOGI("DISP", "flush %" PRId32 ",%" PRId32 " -> %" PRId32 ",%" PRId32 " (%lu bytes)",
             area->x1, area->y1, area->x2, area->y2, size);

    LCD_Address_Set(area->x1, area->y1, area->x2, area->y2);
    LCD_Send_Buf(px_map, size);
    lv_display_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    /* 内部 DMA 内存，确保 SPI DMA 可直接访问 */
    buf1 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    buf2 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!buf1) {
        ESP_LOGE("DISP", "DMA buffer alloc failed (%u bytes)", (unsigned)DISP_BUF_SIZE);
        return;
    }
    if (!buf2) {
        ESP_LOGW("DISP", "Only single buffer available");
    }

    disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, disp_flush);
    lv_display_set_buffers(disp, buf1, buf2, DISP_BUF_SIZE, LV_DISPLAY_RENDER_MODE_PARTIAL);

    ESP_LOGI("DISP", "init ok, buf=%u bytes, partial %d lines", (unsigned)DISP_BUF_SIZE, BUF_LINES);
}
