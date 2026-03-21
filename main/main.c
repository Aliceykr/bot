#include <stdio.h>
#include "lcd.h"
#include "wifi.h"

void app_main(void)
{
    LCD_Init();
    LCD_Fill(0, 0, LCD_W, LCD_H, BLACK);
    vTaskDelay(pdMS_TO_TICKS(500));
    LCD_Fill(0, 0, LCD_W, LCD_H, WHITE);

    // 连接 WiFi
    LCD_ShowString(4, 4, (uint8_t *)"Connecting WiFi...", BLACK, WHITE, 16, 0);
    if (wifi_connect()) {
        LCD_Fill(0, 0, LCD_W, 24, WHITE);
        LCD_ShowString(4, 4, (uint8_t *)"WiFi OK", GREEN, WHITE, 16, 0);
        LCD_ShowString(4, 24, (uint8_t *)wifi_get_ip(), BLUE, WHITE, 16, 0);
    } else {
        LCD_Fill(0, 0, LCD_W, 24, WHITE);
        LCD_ShowString(4, 4, (uint8_t *)"WiFi FAIL", RED, WHITE, 16, 0);
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
    LCD_Fill(0, 0, LCD_W, LCD_H, WHITE);

    // 李白《静夜思》
    SHOW_CN(4,  4, 2, "静夜思",   BLUE, WHITE);
    SHOW_CN(4, 24, 2, "床前明月光", RED,  WHITE);
    SHOW_CN(4, 44, 2, "疑是地上霜", RED,  WHITE);
    SHOW_CN(4, 64, 2, "举头望明月", BLUE, WHITE);
    SHOW_CN(4, 84, 2, "低头思故乡", BLUE, WHITE);
    LCD_DrawLine(0, 104, LCD_W, 104, DARKBLUE);
    Display_Asc_String(4, 108, 4, (uint8_t *)"- Li Bai", GRAY, WHITE);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
