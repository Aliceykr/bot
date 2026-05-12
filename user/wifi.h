#ifndef __WIFI_H
#define __WIFI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// WiFi 连接状态
typedef enum {
    WIFI_STATUS_DISCONNECTED = 0,
    WIFI_STATUS_CONNECTING,
    WIFI_STATUS_CONNECTED,
    WIFI_STATUS_FAILED,
    WIFI_STATUS_RECONNECTING,  /* 运行期断线，后台守护任务正在退避重连 */
} wifi_status_t;

// WiFi 配置，修改为你的路由器信息
#define WIFI_SSID      "11111111"
#define WIFI_PASSWORD  "22222222"
#define WIFI_MAX_RETRY 10

// 动态设置 WiFi 凭据（蓝牙配网用），下次 wifi_connect 时生效
void wifi_set_credentials(const char *ssid, const char *password);

// 初始化并连接 WiFi，阻塞直到连接成功或失败
// 返回 true 表示连接成功
bool wifi_connect(void);

// 断开 WiFi
void wifi_disconnect(void);

// 获取当前连接状态
wifi_status_t wifi_get_status(void);

// 将当前 IP 地址以稳定快照方式 copy 进 out（\0 结尾），最多 cap-1 字符。
// 内部持锁读取，避免事件回调并发改写时读到撕裂字符串。
// cap 必须 >= 1；cap=0 时什么都不做。
void wifi_copy_ip(char *out, size_t cap);

/* 游戏模式：完整停止 WiFi 并阻止守护任务重连，腾出所有 WiFi 内部 DRAM。
 * 不释放 WiFi 子系统配置（下次 resume 直接用），仅 stop（发断开 + 释放 buffer）。
 * 比 esp_wifi_stop 多做了：挡住守护任务，避免它立即重连回来。 */
void wifi_suspend_for_game(void);

/* 游戏结束后恢复 WiFi（异步，立即返回）。 */
void wifi_resume_after_game(void);

#endif
