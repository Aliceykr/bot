#ifndef __LCD_INIT_H
#define __LCD_INIT_H

#include <stdint.h>
#include "driver/gpio.h"

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
#define LCD_MISO_PIN   GPIO_NUM_13  // 字库芯片 SDO
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

// 字库芯片片选与LCD共用CS脚，逻辑相反
#define ZK_CS_Set()     gpio_set_level(LCD_CS_PIN, 0)
#define ZK_CS_Clr()     gpio_set_level(LCD_CS_PIN, 1)

void LCD_GPIO_Init(void);
void LCD_Writ_Bus(uint8_t dat);
void LCD_WR_DATA8(uint8_t dat);
void LCD_WR_DATA(uint16_t dat);
void LCD_WR_REG(uint8_t dat);
void LCD_Address_Set(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);
void LCD_Init(void);

#endif
