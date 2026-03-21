#include "lv_port_disp.h"
#include "lcd.h"
#include "esp_heap_caps.h"

#define DISP_BUF_LINES 40

static lv_display_t *disp;
static lv_color_t *buf1;
static lv_color_t *buf2;

static void disp_flush(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    uint32_t size = w * h * 2;

    LCD_Address_Set(area->x1, area->y1, area->x2, area->y2);
    LCD_Send_Buf(px_map, size);
    lv_display_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    buf1 = heap_caps_malloc(LCD_W * DISP_BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    buf2 = heap_caps_malloc(LCD_W * DISP_BUF_LINES * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);

    disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, disp_flush);
    lv_display_set_buffers(disp, buf1, buf2,
                           LCD_W * DISP_BUF_LINES * sizeof(lv_color_t),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
}
