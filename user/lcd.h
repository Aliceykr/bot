#ifndef __LCD_H
#define __LCD_H

#include <stdint.h>
#include <string.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// ---------- 方向配置 ----------
#define USE_HORIZONTAL 1  // 0,1为竖屏 2,3为横屏

#if USE_HORIZONTAL == 0 || USE_HORIZONTAL == 1
#define LCD_W 128
#define LCD_H 160
#else
#define LCD_W 160
#define LCD_H 128
#endif

// ---------- 引脚定义，按需修改 ----------
#define LCD_MOSI_PIN   GPIO_NUM_11
#define LCD_SCLK_PIN   GPIO_NUM_12
#define LCD_CS_PIN     GPIO_NUM_10
#define LCD_DC_PIN     GPIO_NUM_9
#define LCD_BLK_PIN    GPIO_NUM_46
#define LCD_MISO_PIN   GPIO_NUM_13
// ----------------------------------------

#define LCD_MOSI_Clr()  gpio_set_level(LCD_MOSI_PIN, 0)
#define LCD_MOSI_Set()  gpio_set_level(LCD_MOSI_PIN, 1)
#define LCD_SCLK_Clr()  gpio_set_level(LCD_SCLK_PIN, 0)
#define LCD_SCLK_Set()  gpio_set_level(LCD_SCLK_PIN, 1)
#define LCD_CS_Clr()    gpio_set_level(LCD_CS_PIN, 0)
#define LCD_CS_Set()    gpio_set_level(LCD_CS_PIN, 1)
#define LCD_DC_Clr()    gpio_set_level(LCD_DC_PIN, 0)
#define LCD_DC_Set()    gpio_set_level(LCD_DC_PIN, 1)
#define LCD_BLK_Clr()   gpio_set_level(LCD_BLK_PIN, 0)
#define LCD_BLK_Set()   gpio_set_level(LCD_BLK_PIN, 1)
#define ZK_MISO         gpio_get_level(LCD_MISO_PIN)
#define ZK_CS_Set()     gpio_set_level(LCD_CS_PIN, 0)
#define ZK_CS_Clr()     gpio_set_level(LCD_CS_PIN, 1)

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

// ---------- LCD 底层 ----------
void LCD_GPIO_Init(void);
void LCD_Writ_Bus(uint8_t dat);
void LCD_WR_DATA8(uint8_t dat);
void LCD_WR_DATA(uint16_t dat);
void LCD_WR_REG(uint8_t dat);
void LCD_Address_Set(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void LCD_Init(void);

// ---------- 绘图函数 ----------
void LCD_Fill(uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color);
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);
void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void LCD_DrawRectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);
void Draw_Circle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color);

// ---------- 字符显示 ----------
void LCD_ShowChinese(uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void LCD_ShowChinese12x12(uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void LCD_ShowChinese16x16(uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void LCD_ShowChinese24x24(uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void LCD_ShowChinese32x32(uint16_t x, uint16_t y, uint8_t *s, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void LCD_ShowChar(uint16_t x, uint16_t y, uint8_t num, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
void LCD_ShowString(uint16_t x, uint16_t y, const uint8_t *p, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);
uint32_t mypow(uint8_t m, uint8_t n);
void LCD_ShowIntNum(uint16_t x, uint16_t y, uint16_t num, uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey);
void LCD_ShowFloatNum1(uint16_t x, uint16_t y, float num, uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey);
void LCD_ShowPicture(uint16_t x, uint16_t y, uint16_t length, uint16_t width, const uint8_t pic[]);

// ---------- 字库芯片函数 ----------
void ZK_command(uint8_t dat);
uint8_t get_data_from_ROM(void);
void get_n_bytes_data_from_ROM(uint8_t AddrHigh, uint8_t AddrMid, uint8_t AddrLow, uint8_t *pBuff, uint8_t DataLen);
void Display_GB2312(uint16_t x, uint16_t y, uint8_t zk_num, uint16_t fc, uint16_t bc);
void Display_GB2312_String(uint16_t x, uint16_t y, uint8_t zk_num, uint8_t text[], uint16_t fc, uint16_t bc);
void Display_Asc(uint16_t x, uint16_t y, uint8_t zk_num, uint16_t fc, uint16_t bc);
void Display_Asc_String(uint16_t x, uint16_t y, uint16_t zk_num, uint8_t text[], uint16_t fc, uint16_t bc);
void Display_Arial_TimesNewRoman(uint16_t x, uint16_t y, uint8_t zk_num, uint16_t fc, uint16_t bc);
void Display_Arial_String(uint16_t x, uint16_t y, uint16_t zk_num, uint8_t text[], uint16_t fc, uint16_t bc);
void Display_TimesNewRoman_String(uint16_t x, uint16_t y, uint16_t zk_num, uint8_t text[], uint16_t fc, uint16_t bc);

// ---------- UTF-8 转 GB2312 ----------
int utf8_to_gb2312(const char *utf8, uint8_t *buf, int buf_size);

// 源码直接写汉字显示宏
#define SHOW_CN(x, y, size, str, fc, bc) do { \
    uint8_t _buf[64]; \
    utf8_to_gb2312(str, _buf, sizeof(_buf)); \
    Display_GB2312_String(x, y, size, _buf, fc, bc); \
} while(0)

#endif
