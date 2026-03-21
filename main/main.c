#include <stdio.h>
#include "lcd.h"
#include "wifi.h"

void app_main(void)
{
    LCD_Init();

    // 连接 WiFi
    LCD_ShowString(10, 10, (uint8_t *)"Connecting WiFi...", BLACK, WHITE, 16, 0);
    if (wifi_connect()) {
        LCD_Fill(0, 0, LCD_W, 40, WHITE);
        LCD_ShowString(10, 10, (uint8_t *)"WiFi OK", GREEN, WHITE, 16, 0);
        LCD_ShowString(10, 30, (uint8_t *)wifi_get_ip(), BLUE, WHITE, 16, 0);
    } else {
        LCD_Fill(0, 0, LCD_W, 30, WHITE);
        LCD_ShowString(10, 10, (uint8_t *)"WiFi FAIL", RED, WHITE, 16, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    LCD_Fill(0, 0, LCD_W, LCD_H, WHITE);

    // 测试显示
    LCD_ShowString(10,  10, (uint8_t *)"ST7789 240x320", RED,   WHITE, 16, 0);
    LCD_ShowString(10,  30, (uint8_t *)"ESP32-S3",       BLUE,  WHITE, 16, 0);
    LCD_ShowString(10,  50, (uint8_t *)"Hello World!",   GREEN, WHITE, 16, 0);
    LCD_DrawLine(0, 80, LCD_W, 80, DARKBLUE);
    LCD_DrawRectangle(10, 100, 230, 200, RED);
    Draw_Circle(120, 260, 50, BLUE);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
