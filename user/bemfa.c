#include "bemfa.h"
#include "bemfa_config.h"
#include "wifi.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#define TAG "BEMFA"

/* ================================================================
 * HTTP 响应缓冲（PSRAM 动态扩容）
 *
 * 所有请求共用同一块缓冲，由 s_mutex 串行化；使用完在 API 出口释放，
 * 避免常驻占内存。
 * ================================================================ */
#define RESP_INIT_CAP   4096
#define RESP_MAX_CAP    32768      /* allTopic 在设备多时可能到 10KB+ */

static SemaphoreHandle_t s_mutex   = NULL;
static char   *s_resp_buf = NULL;
static size_t  s_resp_cap = 0;
static size_t  s_resp_len = 0;
static bool    s_resp_overflow = false;

/* 开机初始化：在 app_main 里单线程调用一次，消除 lock_init_once 的 TOCTOU 窗口。
 * 如果忘了调也无伤大雅——lock_init_once 仍会兜底，但并发首次调用可能泄漏一个
 * mutex 或死锁（极窄窗口）。*/
void bemfa_init(void)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

static inline void lock_init_once(void)
{
    /* 兜底路径。正常流程 main 已经调 bemfa_init()。 */
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

static void resp_reset(void)
{
    s_resp_len = 0;
    s_resp_overflow = false;
}

static void resp_free(void)
{
    if (s_resp_buf) {
        heap_caps_free(s_resp_buf);
        s_resp_buf = NULL;
    }
    s_resp_cap = 0;
    s_resp_len = 0;
    s_resp_overflow = false;
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_resp_overflow || !evt->data || evt->data_len <= 0) break;

        if (!s_resp_buf) {
            s_resp_buf = heap_caps_malloc(RESP_INIT_CAP, MALLOC_CAP_SPIRAM);
            if (!s_resp_buf) { s_resp_overflow = true; break; }
            s_resp_cap = RESP_INIT_CAP;
            s_resp_len = 0;
        }

        size_t need = s_resp_len + (size_t)evt->data_len + 1;
        if (need > s_resp_cap) {
            size_t new_cap = s_resp_cap;
            while (new_cap < need && new_cap < RESP_MAX_CAP) new_cap *= 2;
            if (new_cap > RESP_MAX_CAP) new_cap = RESP_MAX_CAP;
            if (need > new_cap) { s_resp_overflow = true; break; }

            char *np = heap_caps_realloc(s_resp_buf, new_cap, MALLOC_CAP_SPIRAM);
            if (!np) { s_resp_overflow = true; break; }
            s_resp_buf = np;
            s_resp_cap = new_cap;
        }

        memcpy(s_resp_buf + s_resp_len, evt->data, evt->data_len);
        s_resp_len += evt->data_len;
        break;

    case HTTP_EVENT_ON_FINISH:
        if (s_resp_buf && !s_resp_overflow) s_resp_buf[s_resp_len] = '\0';
        break;

    default:
        break;
    }
    return ESP_OK;
}

/* ================================================================
 * 公共 API
 * ================================================================ */

bool bemfa_list_devices(bemfa_device_t *out_list, int *out_count)
{
    if (!out_list || !out_count) return false;
    *out_count = 0;

    if (wifi_get_status() != WIFI_STATUS_CONNECTED) {
        ESP_LOGE(TAG, "WiFi 未连接，无法拉取设备列表");
        return false;
    }

    lock_init_once();
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    char url[192];
    snprintf(url, sizeof(url),
        "https://apis.bemfa.com/vb/api/v2/allTopic?openID=%s&type=%d",
        BEMFA_UID, BEMFA_TYPE);

    resp_reset();
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = http_event_cb,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init 失败");
        xSemaphoreGive(s_mutex);
        return false;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool ret = false;
    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "list GET 失败: err=%d status=%d", err, status);
        goto out;
    }
    if (s_resp_overflow || !s_resp_buf) {
        ESP_LOGE(TAG, "响应缓冲溢出或为空");
        goto out;
    }

    ESP_LOGI(TAG, "list resp: %s", s_resp_buf);

    cJSON *root = cJSON_Parse(s_resp_buf);
    if (!root) { ESP_LOGE(TAG, "JSON 解析失败"); goto out; }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!code || code->valueint != 0) {
        ESP_LOGE(TAG, "API 返回错误 code=%d",
                 code ? code->valueint : -1);
        cJSON_Delete(root);
        goto out;
    }

    cJSON *data = cJSON_GetObjectItem(root, "data");
    if (!data) {
        ESP_LOGE(TAG, "缺少 data 字段");
        cJSON_Delete(root);
        goto out;
    }

    /* 兼容两种响应结构：
     *   扁平：{"code":0,"data":[...]}
     *   嵌套：{"code":0,"msg":"success","data":{"data":[...]}}
     * 实测 allTopic 返回嵌套形式，但文档示例写的是扁平；两种都处理。 */
    cJSON *arr = data;
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_GetObjectItem(data, "data");
    }
    if (!cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "data 字段不是数组");
        cJSON_Delete(root);
        goto out;
    }

    int n = cJSON_GetArraySize(arr);
    if (n > BEMFA_MAX_DEVICES) n = BEMFA_MAX_DEVICES;

    for (int i = 0; i < n; ++i) {
        cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) continue;
        bemfa_device_t *d = &out_list[*out_count];
        memset(d, 0, sizeof(*d));

        cJSON *topic = cJSON_GetObjectItem(item, "topic");
        cJSON *name  = cJSON_GetObjectItem(item, "name");
        cJSON *msg   = cJSON_GetObjectItem(item, "msg");
        cJSON *online = cJSON_GetObjectItem(item, "online");

        if (topic && topic->valuestring) {
            strncpy(d->topic, topic->valuestring, sizeof(d->topic) - 1);
        } else {
            continue;   /* 无 topic 的条目跳过 */
        }
        if (name && name->valuestring) {
            strncpy(d->name, name->valuestring, sizeof(d->name) - 1);
        } else {
            /* 没昵称就用 topic 当显示 */
            strncpy(d->name, d->topic, sizeof(d->name) - 1);
        }
        if (msg && msg->valuestring) {
            strncpy(d->msg, msg->valuestring, sizeof(d->msg) - 1);
        }
        d->online = (online && cJSON_IsTrue(online));
        (*out_count)++;
    }

    cJSON_Delete(root);
    ret = true;
    ESP_LOGI(TAG, "list 成功，%d 个设备", *out_count);

out:
    resp_free();
    xSemaphoreGive(s_mutex);
    return ret;
}

bool bemfa_push_msg(const char *topic, const char *msg)
{
    if (!topic || !msg) return false;
    if (wifi_get_status() != WIFI_STATUS_CONNECTED) {
        ESP_LOGE(TAG, "WiFi 未连接，无法推送");
        return false;
    }

    lock_init_once();
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    /* 构造 JSON body。
     * 这里没有做 JSON 转义：当前只有我们控制的字段（uid 常量，topic 来自服务端，
     * msg 固定是 "on"/"off"），都不含双引号或反斜杠。如果未来要让调用方传自定义
     * msg（比如场景消息），必须加转义函数再拼 body。 */
    char body[256];
    int body_len = snprintf(body, sizeof(body),
        "{\"uid\":\"%s\",\"topic\":\"%s\",\"type\":%d,\"msg\":\"%s\"}",
        BEMFA_UID, topic, BEMFA_TYPE, msg);
    if (body_len <= 0 || body_len >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "body 构造溢出");
        xSemaphoreGive(s_mutex);
        return false;
    }

    resp_reset();
    esp_http_client_config_t cfg = {
        .url = "https://apis.bemfa.com/va/postJsonMsg",
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_cb,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ESP_LOGE(TAG, "http_client_init 失败");
        xSemaphoreGive(s_mutex);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf-8");
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool ret = false;
    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "push 失败: err=%d status=%d", err, status);
        goto out;
    }
    if (s_resp_overflow || !s_resp_buf) {
        ESP_LOGE(TAG, "push 响应缓冲溢出或为空");
        goto out;
    }

    ESP_LOGI(TAG, "push resp: %s", s_resp_buf);

    cJSON *root = cJSON_Parse(s_resp_buf);
    if (!root) { ESP_LOGE(TAG, "push JSON 解析失败"); goto out; }
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && code->valueint == 0) {
        ret = true;
    } else {
        ESP_LOGE(TAG, "push API 返回错误 code=%d",
                 code ? code->valueint : -1);
    }
    cJSON_Delete(root);

out:
    resp_free();
    xSemaphoreGive(s_mutex);
    return ret;
}

bool bemfa_toggle(const char *topic, const char *current_msg,
                  char *new_msg_out, size_t cap)
{
    if (!topic || !new_msg_out || cap < 4) return false;

    /* 判断当前是 on 还是 off：
     * - 如果 current_msg 就是 "on"，下一个发 "off"
     * - 其余情况（包括空串、"off"、未知值）都发 "on"
     * 这种单向偏置对用户最友好：偶尔 msg 字段不同步时仍能开灯。 */
    const char *next = (current_msg && strcmp(current_msg, "on") == 0) ? "off" : "on";

    if (!bemfa_push_msg(topic, next)) return false;

    snprintf(new_msg_out, cap, "%s", next);
    return true;
}
