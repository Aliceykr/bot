#include "sntp_time.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "SNTP"

// 时区：中国标准时间 UTC+8
#define TIMEZONE "CST-8"

static bool s_synced = false;

bool sntp_time_sync(uint32_t timeout_ms)
{
    setenv("TZ", TIMEZONE, 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_netif_sntp_init(&config);

    ESP_LOGI(TAG, "等待 NTP 同步...");
    esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NTP 同步超时或失败: %s", esp_err_to_name(err));
        esp_netif_sntp_deinit();
        return false;
    }

    s_synced = true;
    time_t now;
    struct tm t;
    time(&now);
    localtime_r(&now, &t);
    ESP_LOGI(TAG, "NTP 同步成功: %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
             t.tm_hour, t.tm_min, t.tm_sec);
    esp_netif_sntp_deinit();
    return true;
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
