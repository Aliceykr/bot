#include "weather.h"
#include <string.h>
#include <stdio.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#define TAG "WEATHER"

#define WEATHER_URL "https://uapis.cn/api/v1/misc/weather"

/* 响应缓冲动态扩容：初始 4KB，最大 32KB，超过视为异常 */
#define RESP_INIT_CAP  4096
#define RESP_MAX_CAP   32768

static char   *s_resp_buf = NULL;
static size_t  s_resp_cap = 0;
static size_t  s_resp_len = 0;
static bool    s_resp_overflow = false;

/* 互斥锁：保护 s_resp_buf 及后续 JSON 解析期间的模块状态，防并发调用 */
static SemaphoreHandle_t s_mutex = NULL;
static void ensure_mutex(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
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

        size_t need = s_resp_len + (size_t)evt->data_len + 1;  /* +1 for '\0' */
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

static bool http_get(const char *url)
{
    s_resp_len = 0;
    s_resp_overflow = false;
    /* 缓冲复用，不在此处释放 */

    esp_http_client_config_t config = {
        .url            = url,
        .event_handler  = http_event_handler,
        .timeout_ms     = 10000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return false;
    if (status != 200) return false;
    return true;
}

bool weather_fetch(weather_data_t *out)
{
    ensure_mutex();
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool ret = false;
    memset(out, 0, sizeof(*out));

    // 检查 WiFi 是否已连接
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        snprintf(out->error_msg, sizeof(out->error_msg), "No WiFi / Network Error");
        out->error_code = -1;
        goto out;
    }

    // 查询天气
    if (!http_get(WEATHER_URL)) {
        snprintf(out->error_msg, sizeof(out->error_msg), "No WiFi / Network Error");
        out->error_code = -1;
        goto out;
    }
    if (s_resp_overflow) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Response too large");
        out->error_code = -1;
        goto out;
    }
    ESP_LOGI(TAG, "Weather response: %s", s_resp_buf);

    cJSON *root = cJSON_Parse(s_resp_buf);
    if (!root) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Parse Error");
        out->error_code = -1;
        goto out;
    }

    // 检查错误码
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (code && code->valueint != 200) {
        out->error_code = code->valueint;
        if (code->valueint == 503 || code->valueint == 400) {
            snprintf(out->error_msg, sizeof(out->error_msg), "No WiFi / Network Error");
        } else {
            snprintf(out->error_msg, sizeof(out->error_msg), "Error %d", code->valueint);
        }
        cJSON_Delete(root);
        goto out;
    }

    /* uapis.cn 的响应是平铺的 JSON 对象，字段直接在顶层，无 data 包裹层 */
    cJSON *item;
    item = cJSON_GetObjectItem(root, "city");          if(item && item->valuestring) strncpy(out->city,          item->valuestring, sizeof(out->city)-1);
    item = cJSON_GetObjectItem(root, "province");      if(item && item->valuestring) strncpy(out->province,      item->valuestring, sizeof(out->province)-1);
    item = cJSON_GetObjectItem(root, "weather");       if(item && item->valuestring) strncpy(out->weather,       item->valuestring, sizeof(out->weather)-1);
    // temperature 是数字类型
    item = cJSON_GetObjectItem(root, "temperature");
    if(item) snprintf(out->temperature, sizeof(out->temperature), "%d摄氏度", item->valueint);
    item = cJSON_GetObjectItem(root, "humidity");
    if(item) snprintf(out->humidity, sizeof(out->humidity), "%d", item->valueint);
    item = cJSON_GetObjectItem(root, "wind_direction"); if(item && item->valuestring) strncpy(out->wind_direction, item->valuestring, sizeof(out->wind_direction)-1);
    item = cJSON_GetObjectItem(root, "wind_power");    if(item && item->valuestring) strncpy(out->wind_power,    item->valuestring, sizeof(out->wind_power)-1);
    item = cJSON_GetObjectItem(root, "report_time");
    if (item && item->valuestring) {
        // report_time 格式: "2026-03-21 19:26:37"
        strncpy(out->date,     item->valuestring, 10);
        out->date[10] = '\0';
        strncpy(out->time_str, item->valuestring + 11, sizeof(out->time_str)-1);
        // 解析时分秒
        sscanf(item->valuestring + 11, "%d:%d:%d", &out->hour, &out->minute, &out->second);
    }
    cJSON_Delete(root);
    ret = true;

out:
    if (s_mutex) xSemaphoreGive(s_mutex);
    return ret;
}

