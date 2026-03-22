#ifndef __SNTP_TIME_H
#define __SNTP_TIME_H

#include <stdbool.h>
#include <time.h>

// 初始化 SNTP 并等待同步完成（需在 WiFi 连接后调用）
// 超时 timeout_ms 毫秒，返回 true 表示同步成功
bool sntp_time_sync(uint32_t timeout_ms);

// 获取当前本地时间，填充 tm 结构体
// 返回 true 表示时间已同步过
bool sntp_time_get(struct tm *out);

// 是否已完成过一次同步
bool sntp_time_is_synced(void);

#endif
