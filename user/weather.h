#ifndef __WEATHER_H
#define __WEATHER_H

#include <stdbool.h>

typedef struct {
    char city[32];
    char province[32];
    char weather[32];    // 天气描述，如"雾""晴"
    char weather_icon[8]; // 图标代码，如"500"
    char temperature[16];
    char humidity[16];
    char wind_direction[32];
    char wind_power[16];
    char date[32];
    char time_str[32];  // 初始时间字符串 HH:MM:SS
    int  hour;          // 解析后的时分秒，用于本地计时
    int  minute;
    int  second;
    char weekday[16];
    int  error_code;
    char error_msg[32];
} weather_data_t;

// 查询天气+时间，结果存入 out
bool weather_fetch(weather_data_t *out);

// 必须在 app_main 启动阶段单线程调用一次，确保 mutex 在任何并发调用前创建
void weather_init(void);

#endif
