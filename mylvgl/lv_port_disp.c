#include "lv_port_disp.h"
#include "lcd.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* 全帧缓冲大小：LCD_W × LCD_H × 2字节（RGB565），分配于 PSRAM */
#define DISP_BUF_SIZE  (LCD_W * LCD_H * sizeof(lv_color_t))

static lv_display_t    *disp;
static lv_color_t      *buf1;
static lv_color_t      *buf2;
static SemaphoreHandle_t s_flush_mutex;  /* 保护 LCD SPI 总线的互斥锁 */

static void disp_flush(lv_display_t *disp_drv, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t w    = (uint32_t)(area->x2 - area->x1 + 1);
    uint32_t h    = (uint32_t)(area->y2 - area->y1 + 1);
    uint32_t size = w * h * 2U;  /* RGB565：每像素 2 字节 */

    /* 获取互斥锁，防止多任务并发访问 SPI 总线 */
    xSemaphoreTake(s_flush_mutex, portMAX_DELAY);

    /* 等待上一帧 DMA 传输完成，确保 SPI 总线空闲、缓冲区可安全切换 */
    LCD_Send_Buf_Wait();

    /* 设置 LCD 写入窗口坐标（同步命令，耗时极短）*/
    LCD_Address_Set(area->x1, area->y1, area->x2, area->y2);

    /* 以 DMA 非阻塞方式提交像素数据，CPU 立即返回，DMA 在后台传输 */
    LCD_Send_Buf_Async(px_map, size);

    /* 释放互斥锁（DMA 传输仍在进行，但 SPI 总线由 DMA 独占，无需持锁）*/
    xSemaphoreGive(s_flush_mutex);

    /* 通知 LVGL 本次 flush 已提交，可开始渲染下一帧到另一缓冲区。
     * FULL 模式下 LVGL 切换缓冲区前会再次调用 disp_flush，
     * 届时 LCD_Send_Buf_Wait() 确保上一帧 DMA 已完成。 */
    lv_display_flush_ready(disp_drv);
}

void lv_port_disp_init(void)
{
    /* 创建 SPI 访问互斥锁，初始化后立即可用 */
    s_flush_mutex = xSemaphoreCreateMutex();

    /* 双缓冲各一整帧，全部分配于 PSRAM，避免占用紧张的内部 SRAM */
    buf1 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    buf2 = heap_caps_malloc(DISP_BUF_SIZE, MALLOC_CAP_SPIRAM);

    /* 以物理分辨率创建 LVGL 显示器实例 */
    disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_flush_cb(disp, disp_flush);

    /* FULL 模式：每帧渲染完整画面后一次性提交 DMA flush，
     * 消除 PARTIAL 模式多次 flush 造成的画面撕裂和 FreeRTOS 调度竞态 */
    lv_display_set_buffers(disp, buf1, buf2,
                           DISP_BUF_SIZE,
                           LV_DISPLAY_RENDER_MODE_FULL);
}
