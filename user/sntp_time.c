#include "sntp_time.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "SNTP"

/* 时区：中国标准时间 UTC+8 */
#define TIMEZONE "CST-8"

/* 后台自动同步间隔：1 小时（毫秒）*/
#define SNTP_RESYNC_INTERVAL_MS (60 * 60 * 1000)

static bool s_initialized = false;  /* SNTP 客户端是否已初始化（常驻）*/
static bool s_synced = false;       /* 至少同步成功过一次 */

/* SNTP 同步回调：每次成功对时触发 */
static void sntp_sync_cb(struct timeval *tv)
{
    s_synced = true;
    struct tm t;
    localtime_r(&tv->tv_sec, &t);
    ESP_LOGI(TAG, "时间已同步: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
}

bool sntp_time_sync(uint32_t timeout_ms)
{
    /* 已初始化：后台同步常驻，直接返回当前状态 */
    if (s_initialized) return s_synced;

    setenv("TZ", TIMEZONE, 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.sync_cb = sntp_sync_cb;
    /* start=true 默认，初始化后立即启动同步 */
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init 失败: %s", esp_err_to_name(err));
        return false;
    }
    s_initialized = true;
    /* 设置周期性同步间隔（默认由 CONFIG_LWIP_SNTP_UPDATE_DELAY 决定，显式指定更健壮）*/
    sntp_set_sync_interval(SNTP_RESYNC_INTERVAL_MS);

    ESP_LOGI(TAG, "等待首次 NTP 同步...");
    esp_err_t wait_err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (wait_err != ESP_OK) {
        /* 首次同步超时：SNTP 客户端保留在运行状态，WiFi 恢复后会自动重试 */
        ESP_LOGW(TAG, "首次同步超时: %s（后台将继续尝试）", esp_err_to_name(wait_err));
        return false;
    }

    return s_synced;
}

bool sntp_time_get(struct tm *out)
{
    time_t now;
    time(&now);
    localtime_r(&now, out);
    return s_synced;
}

bool sntp_time_is_synced(void)
{
    return s_synced;
}
