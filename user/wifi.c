#include "wifi.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

#define TAG "WIFI"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static char s_ip_str[16] = "0.0.0.0";
static int s_retry_count = 0;
static bool s_give_up = false;  // 超过最大重试次数后置true，停止重连
static bool s_initialized = false;  // 一次性初始化标志

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_retry_count = 0;
        s_give_up = false;
        esp_wifi_connect();
        s_status = WIFI_STATUS_CONNECTING;
        ESP_LOGI(TAG, "正在连接 WiFi...");

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_give_up) {
            // 已放弃重连，不再处理
            return;
        }
        s_status = WIFI_STATUS_DISCONNECTED;
        memcpy(s_ip_str, "0.0.0.0", 8);

        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            s_status = WIFI_STATUS_CONNECTING;
            ESP_LOGW(TAG, "WiFi 断开，第 %d/%d 次重连...", s_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            s_give_up = true;
            s_status = WIFI_STATUS_FAILED;
            ESP_LOGE(TAG, "WiFi 连接错误：已重试 %d 次，停止连接", WIFI_MAX_RETRY);
            if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_give_up = false;
        s_status = WIFI_STATUS_CONNECTED;
        ESP_LOGI(TAG, "WiFi 已连接，IP: %s", s_ip_str);
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

bool wifi_connect(void)
{
    // 初始化 NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (!s_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());
        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&cfg));

        esp_event_handler_instance_t instance_any_id;
        esp_event_handler_instance_t instance_got_ip;
        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                            &event_handler, NULL, &instance_any_id));
        ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                            &event_handler, NULL, &instance_got_ip));

        wifi_config_t wifi_config = {
            .sta = {
                .ssid     = WIFI_SSID,
                .password = WIFI_PASSWORD,
                .threshold.authmode = WIFI_AUTH_WPA_PSK,
            },
        };
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        s_initialized = true;
    }

    s_give_up = false;
    s_retry_count = 0;
    s_status = WIFI_STATUS_DISCONNECTED;
    s_wifi_event_group = xEventGroupCreate();

    // 如果已经 started，先 stop 再 start，确保 STA_START 事件重新触发
    esp_wifi_disconnect();
    esp_wifi_stop();
    ESP_ERROR_CHECK(esp_wifi_start());

    // 等待连接结果（最多等待60秒，含所有重试时间）
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(60000));

    vEventGroupDelete(s_wifi_event_group);
    s_wifi_event_group = NULL;

    if (bits & WIFI_CONNECTED_BIT) {
        return true;
    } else {
        ESP_LOGE(TAG, "WiFi 连接错误：无法连接到 %s", WIFI_SSID);
        return false;
    }
}

void wifi_disconnect(void)
{
    s_give_up = true;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_status = WIFI_STATUS_DISCONNECTED;
    memcpy(s_ip_str, "0.0.0.0", 8);
}

wifi_status_t wifi_get_status(void)
{
    return s_status;
}

const char *wifi_get_ip(void)
{
    return s_ip_str;
}
