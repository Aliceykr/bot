#include "model.h"
#include "model_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"

#define TAG "MODEL"
#define BUF_SIZE 2048

static char s_resp_buf[BUF_SIZE];
static int  s_resp_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_resp_len + evt->data_len < BUF_SIZE - 1) {
            memcpy(s_resp_buf + s_resp_len, evt->data, evt->data_len);
            s_resp_len += evt->data_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        s_resp_buf[s_resp_len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

bool model_chat(const char *user_msg, model_result_t *out)
{
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
        return false;
    }

    // 构造 Authorization header
    char auth_header[128];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", MODEL_API_KEY);

    s_resp_len = 0;
    memset(s_resp_buf, 0, sizeof(s_resp_buf));

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
        return false;
    }

    ESP_LOGI(TAG, "Response: %s", s_resp_buf);

    // 解析响应
    cJSON *resp = cJSON_Parse(s_resp_buf);
    if (!resp) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Parse error");
        return false;
    }

    cJSON *choices = cJSON_GetObjectItem(resp, "choices");
    if (!cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
        snprintf(out->error_msg, sizeof(out->error_msg), "No choices");
        cJSON_Delete(resp);
        return false;
    }

    cJSON *choice  = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice, "message");
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (!cJSON_IsString(content)) {
        snprintf(out->error_msg, sizeof(out->error_msg), "No content");
        cJSON_Delete(resp);
        return false;
    }

    strncpy(out->output, content->valuestring, MODEL_MAX_OUTPUT - 1);
    cJSON_Delete(resp);
    out->success = true;
    return true;
}
