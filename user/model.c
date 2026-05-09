#include "model.h"
#include "model_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#define TAG "MODEL"

/* 响应缓冲动态扩容：初始 4KB，最大 32KB。大模型长回复 + 完整 JSON 封装可能超 4KB */
#define RESP_INIT_CAP  4096
#define RESP_MAX_CAP   32768

static char   *s_resp_buf = NULL;
static size_t  s_resp_cap = 0;
static size_t  s_resp_len = 0;
static bool    s_resp_overflow = false;

/* 互斥锁：保护响应缓冲，防并发调用 */
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

bool model_chat(const char *user_msg, model_result_t *out)
{
    ensure_mutex();
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool ret = false;
    memset(out, 0, sizeof(*out));
    strncpy(out->input, user_msg, MODEL_MAX_INPUT - 1);

    // 构造请求 JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", MODEL_NAME);
    cJSON_AddBoolToObject(root, "stream", false);
    cJSON_AddNumberToObject(root, "max_tokens", 128);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", "Reply in 1-2 short sentences. Be concise. No emoji or emoticons. Chinese replies are acceptable.");
    cJSON_AddItemToArray(messages, sys_msg);
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(msg, "content", user_msg);
    cJSON_AddItemToArray(messages, msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        snprintf(out->error_msg, sizeof(out->error_msg), "JSON build error");
        goto out;
    }

    // 构造 Authorization header
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", MODEL_API_KEY);

    s_resp_len = 0;
    s_resp_overflow = false;

    esp_http_client_config_t config = {
        .url            = MODEL_API_URL,
        .event_handler  = http_event_handler,
        .timeout_ms     = 30000,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK || status != 200) {
        snprintf(out->error_msg, sizeof(out->error_msg), "HTTP error %d", status);
        goto out;
    }
    if (s_resp_overflow) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Response too large");
        goto out;
    }

    ESP_LOGI(TAG, "Response: %s", s_resp_buf);

    // 解析响应
    cJSON *resp = cJSON_Parse(s_resp_buf);
    if (!resp) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Parse error");
        goto out;
    }

    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        snprintf(out->error_msg, sizeof(out->error_msg), "No choices");
        cJSON_Delete(resp);
        goto out;
    }

    cJSON *choice  = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (!cJSON_IsString(content)) {
        snprintf(out->error_msg, sizeof(out->error_msg), "No content");
        cJSON_Delete(resp);
        goto out;
    }

    strncpy(out->output, content->valuestring, MODEL_MAX_OUTPUT - 1);
    cJSON_Delete(resp);
    out->success = true;
    ret = true;

out:
    if (s_mutex) xSemaphoreGive(s_mutex);
    return ret;
}
