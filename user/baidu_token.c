#include "baidu_token.h"
#include "asr_config.h"   /* BAIDU_API_KEY / BAIDU_SECRET_KEY / BAIDU_TOKEN_URL */
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "cJSON.h"

#define TAG "BAIDU_TOK"

/* 到期前 1 小时主动刷新（余量避免时钟误差和请求延迟）*/
#define REFRESH_MARGIN_US (3600LL * 1000 * 1000)

/* 响应缓冲动态扩容：百度 token 响应含 scope 等字段，可能超 2KB */
#define RESP_INIT_CAP  4096
#define RESP_MAX_CAP   32768

static char   *s_resp_buf = NULL;
static size_t  s_resp_cap = 0;
static size_t  s_resp_len = 0;
static bool    s_resp_overflow = false;

/* 释放 HTTP 响应缓冲区，在 API 函数返回前调用（必须在 mutex 持有期间） */
static inline void resp_buf_release(void)
{
    if (s_resp_buf) {
        heap_caps_free(s_resp_buf);
        s_resp_buf = NULL;
    }
    s_resp_cap      = 0;
    s_resp_len      = 0;
    s_resp_overflow = false;
}

/* Token 缓存 */
static char s_token[256] = "";
static int64_t s_expire_us = 0;   /* esp_timer_get_time() 单调时间戳，到期点 */

static SemaphoreHandle_t s_mutex = NULL;

/* ================================================================
 * HTTP 事件回调：动态扩容累积响应
 * ================================================================ */
static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA: {
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
    }
    case HTTP_EVENT_ON_FINISH:
        if (s_resp_buf && !s_resp_overflow) s_resp_buf[s_resp_len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

/* ================================================================
 * 向百度请求 token，解析 access_token + expires_in，写入缓存
 * 调用方必须已持有 s_mutex
 * ================================================================ */
static bool fetch_token_locked(void)
{
    char url[384];
    snprintf(url, sizeof(url),
        "%s?grant_type=client_credentials&client_id=%s&client_secret=%s",
        BAIDU_TOKEN_URL, BAIDU_API_KEY, BAIDU_SECRET_KEY);

    s_resp_len = 0;
    s_resp_overflow = false;

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_event_cb,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, "", 0);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "token 请求失败: err=%d status=%d", err, status);
        resp_buf_release();
        return false;
    }
    if (s_resp_overflow || !s_resp_buf) {
        ESP_LOGE(TAG, "token 响应超大或分配失败");
        resp_buf_release();
        return false;
    }

    cJSON *root = cJSON_Parse(s_resp_buf);
    if (!root) {
        ESP_LOGE(TAG, "token JSON 解析失败");
        resp_buf_release();
        return false;
    }
    cJSON *tok = cJSON_GetObjectItem(root, "access_token");
    cJSON *exp = cJSON_GetObjectItem(root, "expires_in");

    bool ok = false;
    if (tok && tok->valuestring && strlen(tok->valuestring) > 0) {
        strncpy(s_token, tok->valuestring, sizeof(s_token) - 1);
        s_token[sizeof(s_token) - 1] = '\0';

        /* expires_in 单位秒，典型 2592000 (30 天) */
        int64_t expires_sec = (exp && cJSON_IsNumber(exp)) ? (int64_t)exp->valueint : 30LL * 24 * 3600;
        s_expire_us = esp_timer_get_time() + expires_sec * 1000LL * 1000LL;
        ESP_LOGI(TAG, "token 已获取, 有效期 %lld 秒", (long long)expires_sec);
        ok = true;
    } else {
        ESP_LOGE(TAG, "响应无 access_token: %.200s", s_resp_buf);
    }
    cJSON_Delete(root);
    resp_buf_release();
    return ok;
}

/* 必须在 app_main 启动阶段单线程调用一次，确保 mutex 在任何并发调用前创建 */
void baidu_token_init(void)
{
    /* 幂等：仅在 mutex 尚未创建时创建，app_main 单线程阶段调用 */
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        configASSERT(s_mutex);
    }
}

static void ensure_mutex(void)
{
    if (!s_mutex) {
        /* 兜底：理论上 baidu_token_init 已在 app_main 调用过 */
        ESP_LOGW(TAG, "baidu_token_init 未在 app_main 阶段调用，回退 lazy-create");
        s_mutex = xSemaphoreCreateMutex();
    }
}

/* ================================================================
 * 公开接口
 * ================================================================ */
const char *baidu_token_get(void)
{
    ensure_mutex();
    if (!s_mutex) return NULL;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int64_t now = esp_timer_get_time();
    bool need_fetch = (strlen(s_token) == 0) || (now + REFRESH_MARGIN_US >= s_expire_us);

    if (need_fetch) {
        if (!fetch_token_locked()) {
            xSemaphoreGive(s_mutex);
            return NULL;
        }
    }

    const char *ret = s_token;
    xSemaphoreGive(s_mutex);
    return ret;
}

void baidu_token_invalidate(void)
{
    ensure_mutex();
    if (!s_mutex) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_token[0] = '\0';
    s_expire_us = 0;
    xSemaphoreGive(s_mutex);
    ESP_LOGW(TAG, "token 已失效，下次请求将重新获取");
}
