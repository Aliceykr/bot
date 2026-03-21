# 生成 main.c，自动将汉字转为 GB2312 字节序列
import codecs

def to_gb2312_literal(s):
    b = s.encode('gb2312')
    return ''.join('\\x{:02x}'.format(c) for c in b)

lines = [
    (4,   4, '静夜思',   'BLUE'),
    (4,  24, '床前明月光', 'RED'),
    (4,  44, '疑是地上霜', 'RED'),
    (4,  64, '举头望明月', 'BLUE'),
    (4,  84, '低头思故乡', 'BLUE'),
]

content = '''#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lcd_init.h"
#include "lcd.h"

void app_main(void)
{
    LCD_Init();
    LCD_Fill(0, 0, LCD_W, LCD_H, BLACK);
    vTaskDelay(pdMS_TO_TICKS(500));

    // 白色背景
    LCD_Fill(0, 0, LCD_W, LCD_H, WHITE);

    // 李白《静夜思》
'''

for x, y, text, color in lines:
    gb = to_gb2312_literal(text)
    content += f'    Display_GB2312_String({x}, {y:2d}, 2, (uint8_t *)"{gb}", {color}, WHITE);  // {text}\n'

content += '''    LCD_DrawLine(0, 104, LCD_W, 104, DARKBLUE);
    Display_Asc_String(4, 108, 4, (uint8_t *)"- Li Bai", GRAY, WHITE);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
'''

with open('C:/Users/Aliceykr/Desktop/bot/main/main.c', 'w', encoding='utf-8') as f:
    f.write(content)
print('main.c generated')
