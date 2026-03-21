#include "lcd_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"

static void lcd_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void LCD_GPIO_Init(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << LCD_MOSI_PIN) | (1ULL << LCD_SCLK_PIN) |
                           (1ULL << LCD_CS_PIN)   | (1ULL << LCD_DC_PIN)   |
                           (1ULL << LCD_BLK_PIN);
    io_conf.pull_down_en = 0;
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    // MISO 配置为输入（字库芯片读数据）
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << LCD_MISO_PIN);
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    // 默认高电平
    gpio_set_level(LCD_CS_PIN,   1);
    gpio_set_level(LCD_DC_PIN,   1);
    gpio_set_level(LCD_BLK_PIN,  1);
    gpio_set_level(LCD_SCLK_PIN, 1);
    gpio_set_level(LCD_MOSI_PIN, 1);
}

void LCD_Writ_Bus(uint8_t dat)
{
    uint8_t i;
    LCD_CS_Clr();
    for (i = 0; i < 8; i++) {
        LCD_SCLK_Clr();
        if (dat & 0x80)
            LCD_MOSI_Set();
        else
            LCD_MOSI_Clr();
        LCD_SCLK_Set();
        dat <<= 1;
    }
    LCD_CS_Set();
}

void LCD_WR_DATA8(uint8_t dat)
{
    LCD_Writ_Bus(dat);
}

void LCD_WR_DATA(uint16_t dat)
{
    LCD_Writ_Bus(dat >> 8);
    LCD_Writ_Bus(dat);
}

void LCD_WR_REG(uint8_t dat)
{
    LCD_DC_Clr();
    LCD_Writ_Bus(dat);
    LCD_DC_Set();
}

void LCD_Address_Set(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    LCD_WR_REG(0x2a);
    LCD_WR_DATA(x1);
    LCD_WR_DATA(x2);
    LCD_WR_REG(0x2b);
    LCD_WR_DATA(y1);
    LCD_WR_DATA(y2);
    LCD_WR_REG(0x2c);
}

void LCD_Init(void)
{
    LCD_GPIO_Init();

    LCD_BLK_Set();
    lcd_delay_ms(100);

    LCD_WR_REG(0x11); // Sleep out
    lcd_delay_ms(120);

    // Frame Rate
    LCD_WR_REG(0xB1);
    LCD_WR_DATA8(0x05); LCD_WR_DATA8(0x3C); LCD_WR_DATA8(0x3C);
    LCD_WR_REG(0xB2);
    LCD_WR_DATA8(0x05); LCD_WR_DATA8(0x3C); LCD_WR_DATA8(0x3C);
    LCD_WR_REG(0xB3);
    LCD_WR_DATA8(0x05); LCD_WR_DATA8(0x3C); LCD_WR_DATA8(0x3C);
    LCD_WR_DATA8(0x05); LCD_WR_DATA8(0x3C); LCD_WR_DATA8(0x3C);

    LCD_WR_REG(0xB4); // Dot inversion
    LCD_WR_DATA8(0x03);

    // Power Sequence
    LCD_WR_REG(0xC0);
    LCD_WR_DATA8(0x28); LCD_WR_DATA8(0x08); LCD_WR_DATA8(0x04);
    LCD_WR_REG(0xC1); LCD_WR_DATA8(0xC0);
    LCD_WR_REG(0xC2); LCD_WR_DATA8(0x0D); LCD_WR_DATA8(0x00);
    LCD_WR_REG(0xC3); LCD_WR_DATA8(0x8D); LCD_WR_DATA8(0x2A);
    LCD_WR_REG(0xC4); LCD_WR_DATA8(0x8D); LCD_WR_DATA8(0xEE);

    LCD_WR_REG(0xC5); // VCOM
    LCD_WR_DATA8(0x1A);

    LCD_WR_REG(0x36); // MX, MY, RGB mode
    if      (USE_HORIZONTAL == 0) LCD_WR_DATA8(0x00);
    else if (USE_HORIZONTAL == 1) LCD_WR_DATA8(0xC0);
    else if (USE_HORIZONTAL == 2) LCD_WR_DATA8(0x70);
    else                          LCD_WR_DATA8(0xA0);

    // Gamma
    LCD_WR_REG(0xE0);
    LCD_WR_DATA8(0x04); LCD_WR_DATA8(0x22); LCD_WR_DATA8(0x07); LCD_WR_DATA8(0x0A);
    LCD_WR_DATA8(0x2E); LCD_WR_DATA8(0x30); LCD_WR_DATA8(0x25); LCD_WR_DATA8(0x2A);
    LCD_WR_DATA8(0x28); LCD_WR_DATA8(0x26); LCD_WR_DATA8(0x2E); LCD_WR_DATA8(0x3A);
    LCD_WR_DATA8(0x00); LCD_WR_DATA8(0x01); LCD_WR_DATA8(0x03); LCD_WR_DATA8(0x13);
    LCD_WR_REG(0xE1);
    LCD_WR_DATA8(0x04); LCD_WR_DATA8(0x16); LCD_WR_DATA8(0x06); LCD_WR_DATA8(0x0D);
    LCD_WR_DATA8(0x2D); LCD_WR_DATA8(0x26); LCD_WR_DATA8(0x23); LCD_WR_DATA8(0x27);
    LCD_WR_DATA8(0x27); LCD_WR_DATA8(0x25); LCD_WR_DATA8(0x2D); LCD_WR_DATA8(0x3B);
    LCD_WR_DATA8(0x00); LCD_WR_DATA8(0x01); LCD_WR_DATA8(0x04); LCD_WR_DATA8(0x13);

    LCD_WR_REG(0x3A); // 65k mode
    LCD_WR_DATA8(0x05);
    LCD_WR_REG(0x29); // Display on
}
