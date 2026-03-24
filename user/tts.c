#include "tts.h"
#include "asr_config.h"
#include "speaker.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#define TAG "TTS"

/* ================================================================
 * 模块内部状态（全部 static）
 * ================================================================ */
static char s_tts_token[256] = "";  /* access_token 缓存 */
static bool s_is_pcm = false;        /* 当前响应是否为音频流 */
static int  s_wav_skip = 0;          /* 已跳过的 WAV 头字节数（固定44字节）*/

/* HTTP 响应缓冲（用于 token 响应及错误 JSON 收集） */
#define TOKEN_BUF_SIZE 2048
#define ERR_BUF_SIZE   256
static char  s_token_buf[TOKEN_BUF_SIZE];
static int   s_token_len = 0;
static char  s_err_buf[ERR_BUF_SIZE];
static int   s_err_len = 0;

/* ================================================================
 * percent-encode：对非字母数字及 -_.~ 以外的字节编码为 %XX
 * ================================================================ */
static void tts_url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 4 < dst_size; si++) {
        unsigned char c = (unsigned char)src[si];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[di++] = (char)c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0x0F];
        }
    }
    dst[di] = '\0';
}

/* ================================================================
 * Token 请求专用 HTTP 回调（响应较大，使用独立 s_token_buf）
 * ================================================================ */
static esp_err_t token_http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data && evt->data_len > 0) {
        int copy = evt->data_len;
        if (s_token_len + copy >= TOKEN_BUF_SIZE - 1)
            copy = TOKEN_BUF_SIZE - 1 - s_token_len;
        if (copy > 0) {
            memcpy(s_token_buf + s_token_len, evt->data, copy);
            s_token_len += copy;
        }
    } else if (evt->event_id == HTTP_EVENT_ON_FINISH) {
        s_token_buf[s_token_len] = '\0';
    }
    return ESP_OK;
}

/* ================================================================
 * TTS 请求专用 HTTP 回调
 * - ON_HEADER：检测 Content-Type 决定是否为 PCM 流
 * - ON_DATA  ：PCM 数据流式喂给 speaker_play；错误 JSON 收集到 s_err_buf
 * ================================================================ */
static esp_err_t tts_http_event_cb(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_HEADER:
        /* 兼容 audio/pcm 和 audio/wav（aue=6 实际返回 WAV 封装的 PCM）*/
        if (strcasecmp(evt->header_key, "Content-Type") == 0) {
            s_is_pcm = (strstr(evt->header_value, "audio/") != NULL);
        }
        break;

    case HTTP_EVENT_ON_DATA: {
        if (!evt->data || evt->data_len <= 0) break;

        if (s_is_pcm) {
            /* 首块：解析 WAV 头，打印格式信息 */
            const uint8_t *ptr = (const uint8_t *)evt->data;
            int remaining = evt->data_len;
            if (s_wav_skip == 0 && remaining >= 44) {
                uint32_t sample_rate = ptr[24] | (ptr[25]<<8) | (ptr[26]<<16) | (ptr[27]<<24);
                uint16_t channels    = ptr[22] | (ptr[23]<<8);
                uint16_t bits        = ptr[34] | (ptr[35]<<8);
                ESP_LOGI(TAG, "WAV: %luHz %dbit %dch",
                         (unsigned long)sample_rate, bits, channels);
            }
            /* 跳过 WAV 文件头（RIFF 头固定44字节），之后为裸 PCM */
            if (s_wav_skip < 44) {
                int skip = 44 - s_wav_skip;
                if (skip > remaining) skip = remaining;
                ptr          += skip;
                remaining    -= skip;
                s_wav_skip   += skip;
            }
            /* 流式推送 PCM：缓冲满时短暂让出 CPU，避免丢弃 */
            while (remaining > 0) {
                int written = speaker_play((const int16_t *)ptr, (size_t)remaining);
                if (written > 0) {
                    ptr       += written;
                    remaining -= written;
                } else {
                    /* RingBuffer 暂满，让出 CPU 等待 spk_tx_task 消耗 */
                    vTaskDelay(pdMS_TO_TICKS(1));
                }
            }
        } else {
            /* 收集错误 JSON（有限长度） */
            int copy = evt->data_len;
            if (s_err_len + copy >= ERR_BUF_SIZE - 1)
                copy = ERR_BUF_SIZE - 1 - s_err_len;
            if (copy > 0) {
                memcpy(s_err_buf + s_err_len, evt->data, copy);
                s_err_len += copy;
            }
        }
        break;
    }

    case HTTP_EVENT_ON_FINISH:
        if (!s_is_pcm) {
            s_err_buf[s_err_len] = '\0';
            ESP_LOGE(TAG, "TTS 错误响应: %s", s_err_buf);
        }
        break;

    default:
        break;
    }
    return ESP_OK;
}

/* ================================================================
 * 获取 Baidu access_token（首次请求，之后缓存）
 * ================================================================ */
static bool tts_get_token(void)
{
    if (strlen(s_tts_token) > 0) return true;

    char url[320];
    snprintf(url, sizeof(url),
        "%s?grant_type=client_credentials&client_id=%s&client_secret=%s",
        BAIDU_TOKEN_URL, BAIDU_API_KEY, BAIDU_SECRET_KEY);

    /* 使用独立 token 缓冲，避免被截断 */
    s_token_len = 0;
    memset(s_token_buf, 0, sizeof(s_token_buf));

    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = token_http_event_cb,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_post_field(client, "", 0);
    esp_err_t err = esp_http_client_perform(client);
    int status   = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "获取 token 失败: err=%d status=%d", err, status);
        return false;
    }

    /* 直接字符串搜索提取 access_token，不依赖完整 JSON（响应可能很长）*/
    const char *key = "\"access_token\":\"";
    char *pos = strstr(s_token_buf, key);
    if (pos) {
        pos += strlen(key);
        char *end = strchr(pos, '"');
        if (end && (size_t)(end - pos) < sizeof(s_tts_token)) {
            size_t len = (size_t)(end - pos);
            memcpy(s_tts_token, pos, len);
            s_tts_token[len] = '\0';
        }
    }
    if (strlen(s_tts_token) == 0) {
        ESP_LOGE(TAG, "access_token 提取失败，响应(%d bytes): %.200s", s_token_len, s_token_buf);
        return false;
    }
    ESP_LOGI(TAG, "access_token 获取成功");
    return true;
}

/* ================================================================
 * 公开接口
 * ================================================================ */
bool tts_speak(const char *text)
{
    if (!text || strlen(text) == 0) return false;

    if (!tts_get_token()) return false;

    /* percent-encode 文本（中文每字3字节，编码后9字节，512字节文本最坏约4608字节） */
    char encoded[4096];
    tts_url_encode(text, encoded, sizeof(encoded));

    /* 构建 POST body */
    char body[5120];
    int body_len = snprintf(body, sizeof(body),
        "tex=%s&tok=%s&cuid=esp32s3_bot&ctp=1&lan=zh&spd=5&pit=5&vol=9&per=0&aue=6",
        encoded, s_tts_token);
    if (body_len <= 0 || body_len >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "body 构建失败或过长");
        return false;
    }

    ESP_LOGI(TAG, "开始合成: %.60s%s", text, strlen(text) > 60 ? "..." : "");

    /* 重置状态 */
    s_is_pcm  = false;
    s_wav_skip = 0;
    s_err_len = 0;
    memset(s_err_buf, 0, sizeof(s_err_buf));

    esp_http_client_config_t cfg = {
        .url = "https://tsn.baidu.com/text2audio",
        .event_handler = tts_http_event_cb,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .buffer_size_tx = 6144,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type",
                               "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, body_len);

    esp_err_t err = esp_http_client_perform(client);
    int status   = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "TTS 请求失败: err=%d status=%d", err, status);
        return false;
    }
    if (!s_is_pcm) {
        ESP_LOGE(TAG, "TTS 未返回 PCM 数据");
        return false;
    }

    ESP_LOGI(TAG, "合成完成");
    return true;
}
