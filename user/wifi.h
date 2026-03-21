#ifndef __WIFI_H
#define __WIFI_H

#include <stdint.h>
#include <stdbool.h>

// WiFi 连接状态
typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED,
} wifi_status_t;

// WiFi 配置，修改为你的路由器信息
#define WIFI_SSID      "11111111"
#define WIFI_PASSWORD  "22222222"
#define WIFI_MAX_RETRY 10

// 初始化并连接 WiFi，阻塞直到连接成功或失败
// 返回 true 表示连接成功
bool wifi_connect(void);

// 断开 WiFi
void wifi_disconnect(void);

// 获取当前连接状态
wifi_status_t wifi_get_status(void);

// 获取分配到的 IP 地址字符串，如 "192.168.1.100"
const char *wifi_get_ip(void);

#endif
