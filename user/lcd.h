#ifndef __LCD_H
#define __LCD_H

#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---------- 方向配置 ----------
#define USE_HORIZONTAL 0  // 0:竖屏 1:竖屏180 2:横屏 3:横屏180

#if USE_HORIZONTAL == 0 || USE_HORIZONTAL == 1
#define LCD_W 240
#define LCD_H 320
#else
#define LCD_W 320
#define LCD_H 240
#endif

// ---------- 引脚定义 ----------
#define LCD_MOSI_PIN   GPIO_NUM_11
#define LCD_SCLK_PIN   GPIO_NUM_12
#define LCD_RES_PIN    GPIO_NUM_10   // 复位
#define LCD_DC_PIN     GPIO_NUM_9    // 数据/命令
#define LCD_BLK_PIN    GPIO_NUM_46   // 背光
// ----------------------------

#define LCD_MOSI_Set()  gpio_set_level(LCD_MOSI_PIN, 1)
#define LCD_MOSI_Clr()  gpio_set_level(LCD_MOSI_PIN, 0)
#define LCD_SCLK_Set()  gpio_set_level(LCD_SCLK_PIN, 1)
#define LCD_SCLK_Clr()  gpio_set_level(LCD_SCLK_PIN, 0)
#define LCD_RES_Set()   gpio_set_level(LCD_RES_PIN,  1)
#define LCD_RES_Clr()   gpio_set_level(LCD_RES_PIN,  0)
#define LCD_DC_Set()    gpio_set_level(LCD_DC_PIN,   1)
#define LCD_DC_Clr()    gpio_set_level(LCD_DC_PIN,   0)
#define LCD_BLK_Set()   gpio_set_level(LCD_BLK_PIN,  1)
#define LCD_BLK_Clr()   gpio_set_level(LCD_BLK_PIN,  0)

// ---------- 颜色定义 ----------
#define WHITE         0xFFFF
#define BLACK         0x0000
#define BLUE          0x001F
#define BRED          0xF81F
#define GRED          0xFFE0
#define GBLUE         0x07FF
#define RED           0xF800
#define MAGENTA       0xF81F
#define GREEN         0x07E0
#define CYAN          0x7FFF
#define YELLOW        0xFFE0
#define BROWN         0xBC40
#define BRRED         0xFC07
#define GRAY          0x8430
#define DARKBLUE      0x01CF
#define LIGHTBLUE     0x7D7C
#define GRAYBLUE      0x5458
#define LIGHTGREEN    0x841F
#define LGRAY         0xC618
#define LGRAYBLUE     0xA651
#define LBBLUE        0x2B12
#define DARKBLUE2     0x0030
#define ORANGE        0xFD20

// ---------- 底层函数 ----------
void LCD_GPIO_Init(void);
void LCD_Writ_Bus(uint8_t dat);
void LCD_WR_DATA8(uint8_t dat);
void LCD_WR_DATA(uint16_t dat);
void LCD_WR_REG(uint8_t dat);
void LCD_Address_Set(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void LCD_Init(void);
void LCD_Send_Buf(const uint8_t *buf, uint32_t len);
void LCD_Backlight(uint8_t on);  // 背光控制

/* 获取 SPI 设备句柄（供 disp_flush 直接操作 DMA）*/
extern spi_device_handle_t s_spi;

/* 异步 DMA 发送：提交事务到 SPI 队列后立即返回，CPU 不等待传输完成 */
void LCD_Send_Buf_Async(const uint8_t *buf, uint32_t len);
/* 等待上次异步 DMA 传输完成（须在下次 LCD_Send_Buf_Async 前调用）*/
void LCD_Send_Buf_Wait(void);

// ---------- 绘图函数 ----------
void LCD_Fill(uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color);
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void LCD_DrawRectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void Draw_Circle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color);

// ---------- 字符显示 ----------
void LCD_ShowChar(uint16_t x, uint16_t y, uint8_t num, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void LCD_ShowString(uint16_t x, uint16_t y, const uint8_t *p, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
uint32_t mypow(uint8_t m, uint8_t n);
void LCD_ShowIntNum(uint16_t x, uint16_t y, uint16_t num, uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey);
void LCD_ShowFloatNum1(uint16_t x, uint16_t y, float num, uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey);
void LCD_ShowPicture(uint16_t x, uint16_t y, uint16_t length, uint16_t width, const uint8_t pic[]);

#endif
