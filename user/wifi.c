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

/* 守护任务通知位 */
#define GUARDIAN_NOTIFY_DISCONNECT 0x01

/* ================================================================
 * 模块状态
 * ================================================================ */
static EventGroupHandle_t s_wifi_event_group = NULL;  /* 仅在 wifi_connect() 内部使用 */
static wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static char s_ip_str[16] = "0.0.0.0";
static int  s_retry_count = 0;         /* 首次连接期内的快速重试计数 */
static bool s_initialized = false;     /* WiFi 子系统一次性初始化标志 */
static bool s_user_stopped = false;    /* 用户主动 wifi_disconnect() */
static bool s_has_connected_once = false;  /* 至少连接成功过一次（用于区分首次连接 vs 运行期断线）*/

/* 动态凭据（蓝牙配网写入，wifi_connect 使用） */
static char s_ssid[33]     = WIFI_SSID;
static char s_password[65] = WIFI_PASSWORD;

/* 守护任务：运行期断线时指数退避重连，永不放弃 */
static TaskHandle_t s_guardian_handle = NULL;
static uint32_t     s_backoff_idx = 0;

/* 退避时间表（ms）：5s, 10s, 30s, 60s, 2min, 5min，满档后保持 5min */
static const uint32_t s_backoff_ms[] = {
    5000, 10000, 30000, 60000, 120000, 300000
};
#define BACKOFF_TABLE_SIZE (sizeof(s_backoff_ms) / sizeof(s_backoff_ms[0]))

/* ================================================================
 * 守护任务：运行期断线自动重连
 * 只在首次连接成功后启动，此后常驻，负责所有后续重连
 * ================================================================ */
static void wifi_guardian_task(void *arg)
{
    (void)arg;
    while (1) {
        /* 等待断线通知 */
        uint32_t notify = 0;
        xTaskNotifyWait(0, ULONG_MAX, &notify, portMAX_DELAY);

        if (!(notify & GUARDIAN_NOTIFY_DISCONNECT)) continue;

        /* 指数退避循环：一直重试直到连上或用户主动停止 */
        while (1) {
            if (s_user_stopped) {
                ESP_LOGI(TAG, "守护：用户已停止，退出重连");
                s_backoff_idx = 0;
                break;
            }
            if (s_status == WIFI_STATUS_CONNECTED) {
                /* 已经连上（可能是 STA_START 自动连接或其他路径），停止退避 */
                s_backoff_idx = 0;
                break;
            }

            uint32_t wait_ms = s_backoff_ms[s_backoff_idx];
            ESP_LOGW(TAG, "守护：%u ms 后重连（第 %u 次退避）",
                     (unsigned)wait_ms, (unsigned)(s_backoff_idx + 1));
            vTaskDelay(pdMS_TO_TICKS(wait_ms));

            if (s_user_stopped) continue;  /* 等待期间被取消，回到外层判断 */
            if (s_status == WIFI_STATUS_CONNECTED) {
                s_backoff_idx = 0;
                break;
            }

            s_status = WIFI_STATUS_RECONNECTING;
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "守护：esp_wifi_connect() 返回 %d", err);
            }

            /* 退避档位递增，饱和在最大值 */
            if (s_backoff_idx + 1 < BACKOFF_TABLE_SIZE) s_backoff_idx++;

            /* 等待 15s 看 GOT_IP 是否到来；超时则继续下一轮退避 */
            for (int i = 0; i < 15 && s_status != WIFI_STATUS_CONNECTED; i++) {
                if (s_user_stopped) break;
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (s_status == WIFI_STATUS_CONNECTED) {
                s_backoff_idx = 0;
                break;
            }
            /* 超时未连上：loop 继续下一档退避 */
        }
    }
}

/* ================================================================
 * WiFi / IP 事件处理
 * ================================================================ */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_retry_count = 0;
        esp_wifi_connect();
        s_status = WIFI_STATUS_CONNECTING;
        ESP_LOGI(TAG, "正在连接 WiFi...");

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_user_stopped) {
            /* 用户主动断开，不重连 */
            s_status = WIFI_STATUS_DISCONNECTED;
            memcpy(s_ip_str, "0.0.0.0", 8);
            return;
        }

        memcpy(s_ip_str, "0.0.0.0", 8);

        if (s_wifi_event_group != NULL && !s_has_connected_once) {
            /* 首次连接期：在 wifi_connect() 内等结果 */
            if (s_retry_count < WIFI_MAX_RETRY) {
                s_retry_count++;
                s_status = WIFI_STATUS_CONNECTING;
                ESP_LOGW(TAG, "首次连接失败，第 %d/%d 次重连...",
                         s_retry_count, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                s_status = WIFI_STATUS_FAILED;
                ESP_LOGE(TAG, "首次连接失败：已重试 %d 次", WIFI_MAX_RETRY);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else {
            /* 运行期断线：交给守护任务指数退避重连 */
            s_status = WIFI_STATUS_RECONNECTING;
            ESP_LOGW(TAG, "运行期断线，通知守护任务重连");
            if (s_guardian_handle) {
                xTaskNotify(s_guardian_handle, GUARDIAN_NOTIFY_DISCONNECT, eSetBits);
            }
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_backoff_idx = 0;
        s_status = WIFI_STATUS_CONNECTED;
        s_has_connected_once = true;
        ESP_LOGI(TAG, "WiFi 已连接，IP: %s", s_ip_str);

        /* 首次连接成功后启动守护任务 */
        if (s_guardian_handle == NULL) {
            xTaskCreate(wifi_guardian_task, "wifi_guard", 3072, NULL, 4, &s_guardian_handle);
        }

        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

/* ================================================================
 * 公开接口
 * ================================================================ */
bool wifi_connect(void)
{
    /* 初始化 NVS（已在 app_main 做过一次，此处幂等重复调用是安全的：
     * nvs_flash_init 第二次起会直接返回 ESP_OK 不做动作）*/
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
                .threshold.authmode = WIFI_AUTH_WPA_PSK,
            },
        };
        strncpy((char *)wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid) - 1);
        strncpy((char *)wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        s_initialized = true;
    }

    s_user_stopped = false;
    s_retry_count = 0;
    s_backoff_idx = 0;
    s_status = WIFI_STATUS_DISCONNECTED;
    s_wifi_event_group = xEventGroupCreate();

    /* 已 started 时先 stop 再 start，确保 STA_START 事件重新触发 */
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_err_t start_err = esp_wifi_start();
    if (start_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start 失败: %s", esp_err_to_name(start_err));
        EventGroupHandle_t eg = s_wifi_event_group;
        s_wifi_event_group = NULL;
        vEventGroupDelete(eg);
        s_status = WIFI_STATUS_FAILED;
        return false;
    }

    /* 等待连接结果（最多等 60 秒，含快速重试时间）*/
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(60000));

    /* 清空 event group 指针：之后的断线事件走守护任务，不再走 event_group 路径 */
    EventGroupHandle_t eg = s_wifi_event_group;
    s_wifi_event_group = NULL;
    vEventGroupDelete(eg);

    if (bits & WIFI_CONNECTED_BIT) return true;

    ESP_LOGE(TAG, "WiFi 连接错误：无法连接到 %s", WIFI_SSID);
    return false;
}

void wifi_disconnect(void)
{
    s_user_stopped = true;
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


/* ================================================================
 * 游戏模式下暂停 / 恢复 WiFi
 * ================================================================ */

void wifi_suspend_for_game(void)
{
    /* 置 user_stopped 防止 DISCONNECTED 事件被守护任务当成异常重连 */
    s_user_stopped = true;
    esp_wifi_disconnect();
    esp_wifi_stop();
    s_status = WIFI_STATUS_DISCONNECTED;
    memcpy(s_ip_str, "0.0.0.0", 8);
    ESP_LOGI(TAG, "WiFi 已为游戏暂停");
}

void wifi_resume_after_game(void)
{
    if (!s_initialized) return;  /* 从未连过 WiFi，跳过 */
    s_user_stopped = false;
    s_backoff_idx = 0;
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_resume: esp_wifi_start 返回 %d", err);
    } else {
        ESP_LOGI(TAG, "WiFi 恢复启动，等待自动重连");
    }
}

void wifi_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password) return;
    strncpy(s_ssid, ssid, sizeof(s_ssid) - 1);
    s_ssid[sizeof(s_ssid) - 1] = '\0';
    strncpy(s_password, password, sizeof(s_password) - 1);
    s_password[sizeof(s_password) - 1] = '\0';
    ESP_LOGI(TAG, "凭据已更新: SSID=%s", s_ssid);

    /* 如果 WiFi 已初始化，立即更新驱动配置 */
    if (s_initialized) {
        wifi_config_t cfg = { .sta = { .threshold.authmode = WIFI_AUTH_WPA_PSK } };
        strncpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid) - 1);
        strncpy((char *)cfg.sta.password, s_password, sizeof(cfg.sta.password) - 1);
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
}
