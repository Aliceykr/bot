#include <stdio.h>
#include "lcd.h"

// 显示中文字符串的辅助宏
#define SHOW_CN(x, y, size, str, fc, bc) do { \
    uint8_t _buf[64]; \
    utf8_to_gb2312(str, _buf, sizeof(_buf)); \
    Display_GB2312_String(x, y, size, _buf, fc, bc); \
} while(0)

void app_main(void)
{
    LCD_Init();
    LCD_Fill(0, 0, LCD_W, LCD_H, BLACK);
    vTaskDelay(pdMS_TO_TICKS(500));

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
