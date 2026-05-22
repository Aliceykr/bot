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
/* LCD_GPIO_Init：初始化 ILI9341 相关 GPIO 和 SPI2 设备。
 *
 * 由 LCD_Init 内部调用；普通业务代码不要重复调用。 */
void LCD_GPIO_Init(void);

/* LCD_Writ_Bus：向 LCD SPI 总线写 1 字节原始数据。 */
void LCD_Writ_Bus(uint8_t dat);

/* LCD_WR_DATA8：以数据模式写 8bit 参数。 */
void LCD_WR_DATA8(uint8_t dat);

/* LCD_WR_DATA：以数据模式写 16bit 参数（高字节在前）。 */
void LCD_WR_DATA(uint16_t dat);

/* LCD_WR_REG：以命令模式写 8bit LCD 寄存器命令。 */
void LCD_WR_REG(uint8_t dat);

/* LCD_Address_Set：设置后续像素写入窗口。
 *
 * x1/y1 为左上角，x2/y2 为右下角，均包含端点。LVGL flush 和游戏渲染
 * 都会先设置窗口再连续写 RGB565 像素。 */
void LCD_Address_Set(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/* LCD_Init：初始化 SPI LCD、发送 ILI9341 配置序列并点亮背光。
 *
 * app_main 中调用一次，必须早于 LVGL display 初始化和游戏渲染。 */
void LCD_Init(void);

/* LCD_Send_Buf：向当前窗口连续发送一段像素/数据缓冲。
 *
 * buf 内容由调用方保证格式正确；常用于全屏填充和图片输出。 */
void LCD_Send_Buf(const uint8_t *buf, uint32_t len);

/* LCD_Backlight：控制背光 GPIO。
 *
 * on 非 0 点亮，0 熄灭。 */
void LCD_Backlight(uint8_t on);

/* 获取 SPI 设备句柄（供 disp_flush 直接操作 DMA）*/
extern spi_device_handle_t s_spi;

// ---------- 绘图函数 ----------
/* LCD_Fill：用 RGB565 颜色填充矩形区域。 */
void LCD_Fill(uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color);

/* LCD_DrawPoint：在指定坐标绘制单个像素。 */
void LCD_DrawPoint(uint16_t x, uint16_t y, uint16_t color);

/* LCD_DrawLine：绘制一条直线。 */
void LCD_DrawLine(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);

/* LCD_DrawRectangle：绘制矩形边框。 */
void LCD_DrawRectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color);

/* Draw_Circle：绘制圆形边框。 */
void Draw_Circle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color);

// ---------- 字符显示 ----------
/* LCD_ShowChar：用内置 ASCII 点阵字库绘制单个字符。
 *
 * fc/bc 为前景/背景色，sizey 为字体高度，mode 控制是否叠加背景。 */
void LCD_ShowChar(uint16_t x, uint16_t y, uint8_t num, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);

/* LCD_ShowString：从指定坐标开始绘制 '\0' 结尾 ASCII 字符串。 */
void LCD_ShowString(uint16_t x, uint16_t y, const uint8_t *p, uint16_t fc, uint16_t bc, uint8_t sizey, uint8_t mode);

/* mypow：小型整数幂工具，供数字显示函数计算位权。 */
uint32_t mypow(uint8_t m, uint8_t n);

/* LCD_ShowIntNum：按固定长度显示无符号整数，不足位补空/零由实现决定。 */
void LCD_ShowIntNum(uint16_t x, uint16_t y, uint16_t num, uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey);

/* LCD_ShowFloatNum1：显示带 1 位小数的浮点数。 */
void LCD_ShowFloatNum1(uint16_t x, uint16_t y, float num, uint8_t len, uint16_t fc, uint16_t bc, uint8_t sizey);

/* LCD_ShowPicture：在指定区域显示 RGB565 图片数组。 */
void LCD_ShowPicture(uint16_t x, uint16_t y, uint16_t length, uint16_t width, const uint8_t pic[]);

#endif
