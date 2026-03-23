#include "asr.h"
#include "asr_config.h"
#include "usb_audio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "ASR"

// ================================================================
// 录音缓冲区（PSRAM）
// ================================================================
static int16_t *s_rec_buf = NULL;
static uint32_t s_rec_pos = 0;
static bool s_recording = false;
static i2s_chan_handle_t s_rx_chan = NULL;

// ================================================================
// HTTP 响应缓冲
// ================================================================
#define HTTP_BUF_SIZE 4096
static char s_http_buf[HTTP_BUF_SIZE];
static int  s_http_len = 0;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_http_len + evt->data_len < HTTP_BUF_SIZE - 1) {
            memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
            s_http_len += evt->data_len;
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        s_http_buf[s_http_len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ================================================================
// access_token 缓存
// ================================================================
static char s_access_token[256] = "";

bool asr_get_token(void)
{
    if (strlen(s_access_token) > 0) return true;  // 已有缓存

    char url[256];
    snprintf(url, sizeof(url),
        "%s?grant_type=client_credentials&client_id=%s&client_secret=%s",
        BAIDU_TOKEN_URL, BAIDU_API_KEY, BAIDU_SECRET_KEY);

    s_http_len = 0;
    memset(s_http_buf, 0, sizeof(s_http_buf));

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
        ESP_LOGE(TAG, "获取 token 失败: %d", status);
        return false;
    }
    ESP_LOGI(TAG, "token 响应: %s", s_http_buf);

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) return false;
    cJSON *token = cJSON_GetObjectItem(root, "access_token");
    if (token && token->valuestring) {
        strncpy(s_access_token, token->valuestring, sizeof(s_access_token) - 1);
        ESP_LOGI(TAG, "access_token 获取成功");
    }
    cJSON_Delete(root);
    return strlen(s_access_token) > 0;
}

// ================================================================
// I2S 初始化
// ================================================================
void asr_mic_init(void)
{
    if (s_rec_buf == NULL) {
        s_rec_buf = heap_caps_malloc(MIC_BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_rec_buf) { ESP_LOGE(TAG, "录音缓冲区分配失败"); return; }
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, NULL, &s_rx_chan);
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_PIN,
            .ws   = MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = MIC_SD_PIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    ESP_LOGI(TAG, "I2S 初始化完成 SCK=%d WS=%d SD=%d", MIC_SCK_PIN, MIC_WS_PIN, MIC_SD_PIN);
}

// ================================================================
// 录音控制
// ================================================================
void asr_record_start(void)
{
    if (!s_rx_chan || !s_rec_buf) return;
    s_rec_pos = 0;
    s_recording = true;
    i2s_channel_enable(s_rx_chan);
    ESP_LOGI(TAG, "开始录音");
}

uint32_t asr_record_stop(void)
{
    if (!s_recording) return 0;
    s_recording = false;
    i2s_channel_disable(s_rx_chan);
    uint32_t bytes = s_rec_pos * sizeof(int16_t);
    // 检查音量：取前100个样本的最大绝对值
    int32_t max_val = 0;
    uint32_t check = s_rec_pos > 100 ? 100 : s_rec_pos;
    for (uint32_t i = 0; i < check; i++) {
        int32_t v = s_rec_buf[i] < 0 ? -s_rec_buf[i] : s_rec_buf[i];
        if (v > max_val) max_val = v;
    }
    ESP_LOGI(TAG, "录音结束: %lu bytes (%.1f s), 峰值: %ld",
             (unsigned long)bytes, (float)bytes / (MIC_SAMPLE_RATE * 2.0f), (long)max_val);
    return bytes;
}

bool asr_is_recording(void) { return s_recording; }

void asr_record_read(void)
{
    if (!s_recording || !s_rx_chan) return;
    uint32_t max_samples = MIC_BUF_SIZE / sizeof(int16_t);
    if (s_rec_pos >= max_samples) { asr_record_stop(); return; }
    static int32_t tmp[1024];  // STEREO: 512对样本
    size_t bytes_read = 0;
    i2s_channel_read(s_rx_chan, tmp, sizeof(tmp), &bytes_read, pdMS_TO_TICKS(10));
    int stereo_samples = bytes_read / sizeof(int32_t);
    // 每50次打印一次原始值用于调试
    static int dbg_cnt = 0;
    if (++dbg_cnt >= 50) {
        dbg_cnt = 0;
        ESP_LOGI(TAG, "raw32 L=%ld R=%ld", (long)tmp[0], (long)tmp[1]);
    }
    // STEREO模式：偶数=左声道(INMP441 L/R=GND)，右移8位取24bit有效位的高16bit
    static int16_t tmp16[512];
    int out_samples = 0;
    for (int i = 0; i < stereo_samples - 1 && s_rec_pos < max_samples; i += 2) {
        int32_t raw = tmp[i];  // 左声道
        // INMP441: 数据在高24位，右移16位取高16bit，再放大2倍
        int32_t val = (raw >> 16) * 3;
        if (val >  32767) val =  32767;
        if (val < -32768) val = -32768;
        tmp16[out_samples++] = (int16_t)val;
        s_rec_buf[s_rec_pos++] = (int16_t)val;
    }
    // 发送 16bit PCM 到电脑
    if (out_samples > 0) usb_audio_send(tmp16, out_samples * sizeof(int16_t));
}

// ================================================================
// 识别
// ================================================================
bool asr_recognize(uint32_t audio_len_bytes, asr_result_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_rec_buf || audio_len_bytes == 0) {
        snprintf(out->error_msg, sizeof(out->error_msg), "No audio");
        return false;
    }
    if (!asr_get_token()) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Token failed");
        return false;
    }

    ESP_LOGI(TAG, "发送识别请求, 音频 %lu bytes", (unsigned long)audio_len_bytes);

    // 直接发送 PCM 数据，URL 带参数
    char asr_url[384];
    snprintf(asr_url, sizeof(asr_url),
        "%s?dev_pid=80001&cuid=esp32s3_bot&token=%s",
        BAIDU_ASR_URL, s_access_token);

    s_http_len = 0;
    memset(s_http_buf, 0, sizeof(s_http_buf));
    esp_http_client_config_t cfg = {
        .url = asr_url,
        .event_handler = http_event_cb,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "audio/pcm;rate=16000");
    esp_http_client_set_post_field(client, (const char *)s_rec_buf, audio_len_bytes);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "识别请求失败: %d", status);
        snprintf(out->error_msg, sizeof(out->error_msg), "HTTP %d", status);
        return false;
    }
    ESP_LOGI(TAG, "识别响应: %s", s_http_buf);

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Parse error");
        return false;
    }
    cJSON *err_no = cJSON_GetObjectItem(root, "err_no");
    if (!err_no || err_no->valueint != 0) {
        cJSON *err_msg = cJSON_GetObjectItem(root, "err_msg");
        snprintf(out->error_msg, sizeof(out->error_msg), "ASR err %d: %s",
                 err_no ? err_no->valueint : -1,
                 (err_msg && err_msg->valuestring) ? err_msg->valuestring : "");
        cJSON_Delete(root);
        return false;
    }
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
        cJSON *first = cJSON_GetArrayItem(result, 0);
        if (first && first->valuestring) {
            strncpy(out->result, first->valuestring, ASR_MAX_RESULT - 1);
        }
    }
    cJSON_Delete(root);
    out->success = true;
    ESP_LOGI(TAG, "识别结果: %s", out->result);
    return true;
}
