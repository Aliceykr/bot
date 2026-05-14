#include "wifi.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "lwip/ip4_addr.h"

#define TAG "WIFI"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

/* 守护任务通知位 */
#define GUARDIAN_NOTIFY_DISCONNECT 0x01
#define GUARDIAN_NOTIFY_SHUTDOWN   0x02

/* ================================================================
 * 模块状态
 * ================================================================ */
static EventGroupHandle_t s_wifi_event_group = NULL;  /* 仅在 wifi_connect() 内部使用 */
/* volatile 让编译器总是从内存读取，避免 wifi_get_status() 高频无锁读时
 * 被优化成寄存器缓存；32 位对齐的 enum 读写在 Xtensa 上是原子的，因此
 * 这种最终一致的快照是可接受的，换成 mutex 会拖慢 HTTP/UI 路径。*/
static volatile wifi_status_t s_status = WIFI_STATUS_DISCONNECTED;
static char s_ip_str[16] = "0.0.0.0";
static int  s_retry_count = 0;         /* 首次连接期内的快速重试计数 */
static bool s_initialized = false;     /* WiFi 子系统一次性初始化标志 */
static bool s_user_stopped = false;    /* 用户主动 wifi_disconnect() */
static volatile bool s_has_connected_once = false;  /* 至少连接成功过一次（事件回调里读写，volatile 保守 */

/* 动态凭据（蓝牙配网写入，wifi_connect 使用） */
static char s_ssid[33]     = WIFI_SSID;
static char s_password[65] = WIFI_PASSWORD;

/* 状态互斥锁：保护 s_status / s_user_stopped / s_ip_str 等在事件回调、
 * 守护任务、公开 API 之间的并发读写。
 * 在 wifi_connect（首次调用）中创建，消除 lazy-init 竞态窗口。 */
static SemaphoreHandle_t s_wifi_mutex = NULL;

#define WIFI_LOCK()   do { if (s_wifi_mutex) xSemaphoreTake(s_wifi_mutex, portMAX_DELAY); } while(0)
#define WIFI_UNLOCK() do { if (s_wifi_mutex) xSemaphoreGive(s_wifi_mutex); } while(0)

/* 守护任务：运行期断线时指数退避重连，永不放弃 */
static TaskHandle_t s_guardian_handle = NULL;
static uint32_t     s_backoff_idx = 0;

/* 退避时间表（ms）：5s, 10s, 30s, 60s, 2min, 5min，满档后保持 5min */
static const uint32_t s_backoff_ms[] = {
    5000, 10000, 30000, 60000, 120000, 300000
};
#define BACKOFF_TABLE_SIZE (sizeof(s_backoff_ms) / sizeof(s_backoff_ms[0]))

/* 帮助函数：锁内快照读 user_stopped / status，避免守护任务读到撕裂状态 */
static inline bool snapshot_user_stopped(void)
{
    bool v;
    WIFI_LOCK();
    v = s_user_stopped;
    WIFI_UNLOCK();
    return v;
}

static inline wifi_status_t snapshot_status(void)
{
    wifi_status_t v;
    WIFI_LOCK();
    v = s_status;
    WIFI_UNLOCK();
    return v;
}

/* ================================================================
 * 守护任务：运行期断线自动重连
 * 只在首次连接成功后启动，此后常驻，负责所有后续重连
 *
 * 共享状态读写：
 *   s_user_stopped / s_status / s_backoff_idx 全部用 mutex 快照读取，
 *   避免与 event_handler / wifi_disconnect 的并发写入撕裂。
 * ================================================================ */
static void wifi_guardian_task(void *arg)
{
    (void)arg;
    while (1) {
        /* 等待断线通知 */
        uint32_t notify = 0;
        xTaskNotifyWait(0, ULONG_MAX, &notify, portMAX_DELAY);

        /* 收到 shutdown 通知：自行退出，避免外部 vTaskDelete 导致死锁 */
        if (notify & GUARDIAN_NOTIFY_SHUTDOWN) {
            ESP_LOGI(TAG, "守护：收到 shutdown 通知，退出");
            break;
        }

        if (!(notify & GUARDIAN_NOTIFY_DISCONNECT)) continue;

        /* 指数退避循环：一直重试直到连上或用户主动停止 */
        while (1) {
            if (snapshot_user_stopped()) {
                ESP_LOGI(TAG, "守护：用户已停止，退出重连");
                WIFI_LOCK();
                s_backoff_idx = 0;
                WIFI_UNLOCK();
                break;
            }
            if (snapshot_status() == WIFI_STATUS_CONNECTED) {
                /* 已经连上（可能是 STA_START 自动连接或其他路径），停止退避 */
                WIFI_LOCK();
                s_backoff_idx = 0;
                WIFI_UNLOCK();
                break;
            }

            uint32_t idx;
            WIFI_LOCK();
            idx = s_backoff_idx;
            WIFI_UNLOCK();
            uint32_t wait_ms = s_backoff_ms[idx];
            ESP_LOGW(TAG, "守护：%u ms 后重连（第 %u 次退避）",
                     (unsigned)wait_ms, (unsigned)(idx + 1));

            /* 分段等待：每 100ms 检查一次 shutdown 通知，避免长等期间无法退出 */
            uint32_t waited = 0;
            while (waited < wait_ms) {
                uint32_t step = (wait_ms - waited) > 100 ? 100 : (wait_ms - waited);
                vTaskDelay(pdMS_TO_TICKS(step));
                waited += step;
                if (snapshot_user_stopped()) break;
            }

            if (snapshot_user_stopped()) continue;  /* 等待期间被取消 */
            if (snapshot_status() == WIFI_STATUS_CONNECTED) {
                WIFI_LOCK();
                s_backoff_idx = 0;
                WIFI_UNLOCK();
                break;
            }

            WIFI_LOCK();
            s_status = WIFI_STATUS_RECONNECTING;
            /* 退避档位递增，饱和在最大值。提前递增：即使 connect 内部立即触发
             * DISCONNECT 事件，下一轮也不会卡在同一档 */
            if (s_backoff_idx + 1 < BACKOFF_TABLE_SIZE) s_backoff_idx++;
            WIFI_UNLOCK();

            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "守护：esp_wifi_connect() 返回 %d", err);
            }

            /* 等待 15s 看 GOT_IP 是否到来；超时则继续下一轮退避 */
            for (int i = 0; i < 15; i++) {
                if (snapshot_user_stopped()) break;
                if (snapshot_status() == WIFI_STATUS_CONNECTED) break;
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (snapshot_status() == WIFI_STATUS_CONNECTED) {
                WIFI_LOCK();
                s_backoff_idx = 0;
                WIFI_UNLOCK();
                break;
            }
            /* 超时未连上：loop 继续下一档退避 */
        }
    }

    /* 自行退出前清句柄 */
    WIFI_LOCK();
    s_guardian_handle = NULL;
    WIFI_UNLOCK();
    vTaskDelete(NULL);
}

/* ================================================================
 * WiFi / IP 事件处理
 * ================================================================ */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        WIFI_LOCK();
        s_retry_count = 0;
        s_status = WIFI_STATUS_CONNECTING;
        WIFI_UNLOCK();
        esp_wifi_connect();
        ESP_LOGI(TAG, "正在连接 WiFi...");

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        WIFI_LOCK();
        if (s_user_stopped) {
            /* 用户主动断开，不重连 */
            s_status = WIFI_STATUS_DISCONNECTED;
            memcpy(s_ip_str, "0.0.0.0", 8);
            WIFI_UNLOCK();
            return;
        }

        memcpy(s_ip_str, "0.0.0.0", 8);

        if (s_wifi_event_group != NULL && !s_has_connected_once) {
            /* 首次连接期：在 wifi_connect() 内等结果 */
            if (s_retry_count < WIFI_MAX_RETRY) {
                s_retry_count++;
                s_status = WIFI_STATUS_CONNECTING;
                WIFI_UNLOCK();
                ESP_LOGW(TAG, "首次连接失败，第 %d/%d 次重连...",
                         s_retry_count, WIFI_MAX_RETRY);
                esp_wifi_connect();
            } else {
                s_status = WIFI_STATUS_FAILED;
                WIFI_UNLOCK();
                ESP_LOGE(TAG, "首次连接失败：已重试 %d 次", WIFI_MAX_RETRY);
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        } else {
            /* 运行期断线：交给守护任务指数退避重连 */
            s_status = WIFI_STATUS_RECONNECTING;
            WIFI_UNLOCK();
            ESP_LOGW(TAG, "运行期断线，通知守护任务重连");
            if (s_guardian_handle) {
                xTaskNotify(s_guardian_handle, GUARDIAN_NOTIFY_DISCONNECT, eSetBits);
            }
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        WIFI_LOCK();
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        s_backoff_idx = 0;
        s_status = WIFI_STATUS_CONNECTED;
        s_has_connected_once = true;
        WIFI_UNLOCK();
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
/* netif/event 子系统只能 init 一次（即便 wifi 驱动栈被 deinit 又 init）*/
static bool s_netif_inited = false;
static esp_event_handler_instance_t s_inst_any_id = NULL;
static esp_event_handler_instance_t s_inst_got_ip = NULL;

bool wifi_connect(void)
{
    /* 首次调用时创建 mutex，消除 lazy-init 竞态 */
    if (!s_wifi_mutex) s_wifi_mutex = xSemaphoreCreateMutex();
    /* 初始化 NVS（已在 app_main 做过一次，此处幂等重复调用是安全的：
     * nvs_flash_init 第二次起会直接返回 ESP_OK 不做动作）*/
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    if (!s_initialized) {
        /* netif + event loop 只 init 一次（即使 WiFi 驱动栈被 deinit 又 init） */
        if (!s_netif_inited) {
            ESP_ERROR_CHECK(esp_netif_init());
            ESP_ERROR_CHECK(esp_event_loop_create_default());
            esp_netif_create_default_wifi_sta();
            s_netif_inited = true;
        }

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        /* 不用 ESP_ERROR_CHECK：内部 DRAM 不足时 esp_wifi_init 会返回 ESP_ERR_NO_MEM，
         * 原本 abort 导致整机重启。改成返回 false 让上层 UI 提示"WiFi 启动失败"。 */
        esp_err_t wifi_init_err = esp_wifi_init(&cfg);
        if (wifi_init_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_init 失败: %s (DRAM free=%u)",
                     esp_err_to_name(wifi_init_err),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            s_status = WIFI_STATUS_FAILED;
            return false;
        }

        /* 事件 handler 只注册一次（重新 init 时 handler 已经在 default event loop） */
        if (!s_inst_any_id) {
            ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                                &event_handler, NULL, &s_inst_any_id));
            ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                                &event_handler, NULL, &s_inst_got_ip));
        }

        wifi_config_t wifi_config = {
            .sta = {
                .threshold.authmode = WIFI_AUTH_WPA_PSK,
            },
        };
        WIFI_LOCK();
        memcpy(wifi_config.sta.ssid, s_ssid, sizeof(wifi_config.sta.ssid));
        wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
        memcpy(wifi_config.sta.password, s_password, sizeof(wifi_config.sta.password));
        wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';
        WIFI_UNLOCK();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        s_initialized = true;
    }

    WIFI_LOCK();
    s_user_stopped = false;
    s_retry_count = 0;
    s_backoff_idx = 0;
    s_status = WIFI_STATUS_DISCONNECTED;
    WIFI_UNLOCK();
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
        WIFI_LOCK();
        s_status = WIFI_STATUS_FAILED;
        WIFI_UNLOCK();
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
    WIFI_LOCK();
    s_user_stopped = true;
    WIFI_UNLOCK();
    esp_wifi_disconnect();
    esp_wifi_stop();
    WIFI_LOCK();
    s_status = WIFI_STATUS_DISCONNECTED;
    memcpy(s_ip_str, "0.0.0.0", 8);
    WIFI_UNLOCK();
}

wifi_status_t wifi_get_status(void)
{
    /* 单字段读取，volatile 语义足够，不加锁避免高频调用开销 */
    return s_status;
}

void wifi_copy_ip(char *out, size_t cap)
{
    if (!out || cap == 0) return;
    WIFI_LOCK();
    /* 带锁快照：事件回调里 snprintf 到 s_ip_str 的同时我们做一次性 memcpy，
     * mutex 保证读到的字节全来自同一个版本。*/
    size_t n = strnlen(s_ip_str, sizeof(s_ip_str));
    if (n >= cap) n = cap - 1;
    memcpy(out, s_ip_str, n);
    out[n] = '\0';
    WIFI_UNLOCK();
}


/* ================================================================
 * 游戏模式下暂停 / 恢复 WiFi
 * ================================================================ */

void wifi_suspend_for_game(void)
{
    WIFI_LOCK();
    s_user_stopped = true;
    WIFI_UNLOCK();
    esp_wifi_disconnect();
    esp_wifi_stop();
    WIFI_LOCK();
    s_status = WIFI_STATUS_DISCONNECTED;
    memcpy(s_ip_str, "0.0.0.0", 8);
    WIFI_UNLOCK();
    ESP_LOGI(TAG, "WiFi 已为游戏暂停");
}

void wifi_resume_after_game(void)
{
    if (!s_initialized) return;
    WIFI_LOCK();
    s_user_stopped = false;
    s_backoff_idx = 0;
    WIFI_UNLOCK();
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi_resume: esp_wifi_start 返回 %d", err);
    } else {
        ESP_LOGI(TAG, "WiFi 恢复启动，等待自动重连");
    }
}

/* ================================================================
 * 蓝牙互斥模式：完整释放 WiFi 驱动栈，腾出 ~30KB 连续 DRAM 给 BLE controller
 * 调用后 wifi_connect 会重新走完整 init 流程
 * ================================================================ */

void wifi_full_shutdown_for_ble(void)
{
    if (!s_initialized) return;

    /* 通知守护任务自行退出，避免 vTaskDelete 导致 mutex 死锁 */
    WIFI_LOCK();
    TaskHandle_t guard = s_guardian_handle;
    s_user_stopped = true;
    s_status = WIFI_STATUS_DISCONNECTED;
    memcpy(s_ip_str, "0.0.0.0", 8);
    /* 重置 has_connected_once：下次重连失败要走"首次连接"快速失败路径，
     * 否则会卡 60s 等 EventGroup 超时（H1）*/
    s_has_connected_once = false;
    WIFI_UNLOCK();

    if (guard) {
        /* 发送 shutdown 通知让守护任务自行退出 */
        xTaskNotify(guard, GUARDIAN_NOTIFY_SHUTDOWN, eSetBits);
        /* 等守护任务自行清理退出（最多 2 秒）*/
        for (int i = 0; i < 40; i++) {
            WIFI_LOCK();
            bool gone = (s_guardian_handle == NULL);
            WIFI_UNLOCK();
            if (gone) break;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        /* 超时兜底：如果还没退出，记日志但不强杀 */
        WIFI_LOCK();
        if (s_guardian_handle != NULL) {
            ESP_LOGW(TAG, "守护任务退出超时，强制清理句柄");
            s_guardian_handle = NULL;
        }
        WIFI_UNLOCK();
    }

    esp_wifi_disconnect();
    esp_wifi_stop();
    /* 完整释放 WiFi 驱动占用的 DRAM（包括 RX buffer pool） */
    esp_wifi_deinit();
    s_initialized = false;
    ESP_LOGI(TAG, "WiFi 完全关闭，释放 DRAM。free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

void wifi_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || !password) return;

    /* 锁保护 s_ssid / s_password 与 wifi_connect 的读取并发。
     * 这里故意只在 s_ssid/s_password 写入期间持锁，不覆盖整个
     * esp_wifi_set_config，因为 WiFi 驱动自己需要较久的时间，
     * 持长锁会阻塞 event_handler 读状态。 */
    char new_ssid[33];
    char new_pass[65];
    snprintf(new_ssid, sizeof(new_ssid), "%s", ssid);
    snprintf(new_pass, sizeof(new_pass), "%s", password);

    WIFI_LOCK();
    memcpy(s_ssid,     new_ssid, sizeof(s_ssid));
    memcpy(s_password, new_pass, sizeof(s_password));
    bool inited = s_initialized;
    WIFI_UNLOCK();

    ESP_LOGI(TAG, "凭据已更新: SSID=%s", new_ssid);

    /* 如果 WiFi 已初始化，立即更新驱动配置。
     * 从持锁内拷出副本再设置，期间 s_ssid/s_password 可能被再次改写，
     * 但 wifi_config 是本地栈变量，一致性保证没问题。 */
    if (inited) {
        wifi_config_t cfg = { .sta = { .threshold.authmode = WIFI_AUTH_WPA_PSK } };
        WIFI_LOCK();
        memcpy(cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
        cfg.sta.ssid[sizeof(cfg.sta.ssid) - 1] = '\0';
        memcpy(cfg.sta.password, s_password, sizeof(cfg.sta.password));
        cfg.sta.password[sizeof(cfg.sta.password) - 1] = '\0';
        WIFI_UNLOCK();
        esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
}
