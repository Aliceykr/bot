# Bot - ESP32-S3 LCD 驱动工程

基于 ESP-IDF 的 ESP32-S3 项目，驱动 1.8 寸 TFT LCD（ST7735S），支持外挂字库芯片（GT 系列）显示中文。

## 硬件

| 组件 | 型号 |
|------|------|
| 主控 | ESP32-S3-DevKitC-1 N16R8 |
| 屏幕 | 1.8 寸 TFT LCD，分辨率 128x160，驱动芯片 ST7735S |
| 字库 | 外挂 GT 系列字库芯片（SPI 接口） |

## 引脚接线

| 功能 | ESP32-S3 引脚 |
|------|---------------|
| MOSI (SDI) | GPIO11 |
| SCLK (CLK) | GPIO12 |
| CS         | GPIO10 |
| DC (RS)    | GPIO9  |
| BLK        | GPIO46 |
| MISO (字库 SDO) | GPIO13 |

> 引脚定义在 `user/lcd.h` 顶部，可按需修改。

## 目录结构

```
bot/
├── main/
│   ├── main.c           # 应用入口
│   └── CMakeLists.txt
├── user/
│   ├── lcd.h            # LCD 驱动头文件（引脚/颜色/函数声明）
│   ├── lcd.c            # LCD 驱动实现（GPIO、SPI、绘图、字符、GB2312）
│   ├── lcdfont.h        # ASCII 点阵字库
│   └── zk.c             # 外挂字库芯片驱动
├── CMakeLists.txt
└── README.md
```

## 主要功能

- ST7735S 软件 SPI 驱动
- 基本绘图：填充、画点、画线、矩形、圆
- ASCII 字符显示（12/16/24/32px）
- 外挂字库芯片读取（GB2312 汉字、多种字体）
- UTF-8 源码直接写汉字，自动转换为 GB2312

## 使用方式

```c
#include "lcd.h"

void app_main(void)
{
    LCD_Init();
    LCD_Fill(0, 0, LCD_W, LCD_H, WHITE);

    // 显示 ASCII
    LCD_ShowString(0, 0, (uint8_t *)"Hello!", RED, WHITE, 16, 0);

    // 显示中文（源码直接写汉字）
    SHOW_CN(0, 20, 2, "你好世界", BLUE, WHITE);

    // 如需显示未在转换表中的汉字，在 lcd.c 的 tbl[] 中添加对应条目
    // 用以下命令查询 GB2312 字节：
    // python -c "s='汉字'; b=s.encode('gb2312'); print([hex(x) for x in b])"
}
```

## 编译烧录

```bash
idf.py build
idf.py flash monitor
```

## 注意事项

- PSRAM 模式需设置为 **Octal Mode**（menuconfig → Component config → ESP PSRAM）
- FreeRTOS Tick Rate 建议设为 **1000 Hz**（menuconfig → Component config → FreeRTOS → configTICK_RATE_HZ）
- `SHOW_CN` 宏的汉字转换表在 `user/lcd.c` 的 `unicode_to_gb2312()` 函数中，显示新汉字需手动添加表项
