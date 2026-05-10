#include "asr.h"
#include "asr_config.h"
#include "baidu_token.h"
#include "wifi.h"
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
#include "freertos/semphr.h"

#define TAG "ASR"

// ================================================================
// 录音缓冲区（PSRAM）
// ================================================================
static int16_t *s_rec_buf = NULL;
static volatile uint32_t s_rec_pos = 0;
static volatile bool s_recording = false;
static i2s_chan_handle_t s_rx_chan = NULL;

/* 录音任务：从 I2S 读取数据，转换为单声道 16bit PCM 写入 s_rec_buf
 * 常驻，通过 s_rec_active 标志控制启停，不录音时阻塞等通知 */
static TaskHandle_t   s_rec_task   = NULL;
static volatile bool  s_rec_active = false;
/* 调用 asr_record_stop 的任务句柄，录音任务停止后通知它 */
static TaskHandle_t   s_stop_waiter = NULL;

// ================================================================
// HTTP 响应缓冲（PSRAM 动态扩容）
// 初始 4KB，最大 32KB。token 响应可能 ~2KB，长句识别结果可能更大。
// ================================================================
#define RESP_INIT_CAP  4096
#define RESP_MAX_CAP   32768

static char   *s_http_buf = NULL;
static size_t  s_http_cap = 0;
static size_t  s_http_len = 0;
static bool    s_http_overflow = false;

/* 互斥锁：保护 s_http_buf（识别路径），录音任务独立不受影响。
 * 在 asr_mic_init 中提前创建，消除首次并发窗口。 */
static SemaphoreHandle_t s_recog_mutex = NULL;

/* 前向声明：asr_mic_init 需要创建该任务 */
static void asr_rec_task(void *arg);

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA: {
        if (s_http_overflow || !evt->data || evt->data_len <= 0) break;

        if (!s_http_buf) {
            s_http_buf = heap_caps_malloc(RESP_INIT_CAP, MALLOC_CAP_SPIRAM);
            if (!s_http_buf) { s_http_overflow = true; break; }
            s_http_cap = RESP_INIT_CAP;
            s_http_len = 0;
        }

        size_t need = s_http_len + (size_t)evt->data_len + 1;
        if (need > s_http_cap) {
            size_t new_cap = s_http_cap;
            while (new_cap < need && new_cap < RESP_MAX_CAP) new_cap *= 2;
            if (new_cap > RESP_MAX_CAP) new_cap = RESP_MAX_CAP;
            if (need > new_cap) { s_http_overflow = true; break; }

            char *np = heap_caps_realloc(s_http_buf, new_cap, MALLOC_CAP_SPIRAM);
            if (!np) { s_http_overflow = true; break; }
            s_http_buf = np;
            s_http_cap = new_cap;
        }

        memcpy(s_http_buf + s_http_len, evt->data, evt->data_len);
        s_http_len += evt->data_len;
        break;
    }
    case HTTP_EVENT_ON_FINISH:
        if (s_http_buf && !s_http_overflow) s_http_buf[s_http_len] = '\0';
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ================================================================
// access_token：使用共享 baidu_token 模块管理，带过期自动刷新
// ================================================================
bool asr_get_token(void)
{
    return baidu_token_get() != NULL;
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

    /* 提前创建识别互斥锁，消除 lazy-init 竞态窗口 */
    if (!s_recog_mutex) s_recog_mutex = xSemaphoreCreateMutex();

    /* 启动常驻录音任务。优先级 5：略高于 LVGL 任务(4)保证 I2S 及时读取，
     * 低于 encoder_task(6) 避免挤掉高频输入 */
    if (!s_rec_task) {
        xTaskCreate(asr_rec_task, "asr_rec", 2048, NULL, 5, &s_rec_task);
    }
}

// ================================================================
// 录音任务 & 控制
// ================================================================

/* 录音任务主体：阻塞式读 I2S，处理 INMP441 32bit stereo → 16bit mono。
 * 常驻，不录音时等通知；录音中缓冲满则自动停止。 */
static void asr_rec_task(void *arg)
{
    (void)arg;
    static int32_t tmp[1024];  /* STEREO: 512 对样本。static 避免栈膨胀 */
    while (1) {
        if (!s_rec_active) {
            /* 通知 stop 调用者（如果有人在等） */
            if (s_stop_waiter) {
                xTaskNotifyGive(s_stop_waiter);
                s_stop_waiter = NULL;
            }
            /* 空闲：阻塞等 start 唤醒 */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }

        if (!s_rx_chan || !s_rec_buf) {
            s_rec_active = false;
            s_recording  = false;
            continue;
        }

        uint32_t max_samples = MIC_BUF_SIZE / sizeof(int16_t);
        if (s_rec_pos >= max_samples) {
            /* 缓冲满：自动停止 */
            s_rec_active = false;
            s_recording  = false;
            i2s_channel_disable(s_rx_chan);
            ESP_LOGI(TAG, "录音缓冲满，自动停止");
            continue;
        }

        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_rx_chan, tmp, sizeof(tmp),
                                          &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) continue;
        if (!s_rec_active) continue;  /* 停止信号在读取途中到达 */

        int stereo_samples = bytes_read / sizeof(int32_t);
        /* 调试：每 50 批打印一次 */
        static int dbg_cnt = 0;
        if (++dbg_cnt >= 50) {
            dbg_cnt = 0;
            ESP_LOGI(TAG, "raw32 L=%ld R=%ld", (long)tmp[0], (long)tmp[1]);
        }
        /* STEREO 模式：偶数为左声道（INMP441 L/R=GND）
         * 数据在高 24 位，右移 16 位取高 16bit，放大 3 倍 */
        for (int i = 0; i < stereo_samples - 1 && s_rec_pos < max_samples; i += 2) {
            int32_t raw = tmp[i];
            int32_t val = (raw >> 16) * 3;
            if (val >  32767) val =  32767;
            if (val < -32768) val = -32768;
            s_rec_buf[s_rec_pos++] = (int16_t)val;
        }
    }
}

void asr_record_start(void)
{
    if (!s_rx_chan || !s_rec_buf || !s_rec_task) return;
    s_rec_pos    = 0;
    s_recording  = true;
    s_rec_active = true;
    i2s_channel_enable(s_rx_chan);
    xTaskNotifyGive(s_rec_task);  /* 唤醒录音任务 */
    ESP_LOGI(TAG, "开始录音");
}

uint32_t asr_record_stop(void)
{
    if (!s_recording) return 0;
    s_recording  = false;
    s_rec_active = false;
    /* 设置当前任务为 stop 等待者，录音任务退出循环后会 notify */
    s_stop_waiter = xTaskGetCurrentTaskHandle();
    /* 等录音任务确认停止：任务检测到 s_rec_active=false 后会发通知。
     * 最多等 200ms（i2s_channel_read 超时 100ms + 余量），
     * 超时也继续 disable，不会死锁。 */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    if (s_rx_chan) i2s_channel_disable(s_rx_chan);

    uint32_t pos   = s_rec_pos;
    uint32_t bytes = pos * sizeof(int16_t);
    ESP_LOGI(TAG, "stop: s_rec_pos=%lu", (unsigned long)pos);
    /* 检查音量：取前 100 个样本的最大绝对值 */
    int32_t max_val = 0;
    uint32_t check = pos > 100 ? 100 : pos;
    for (uint32_t i = 0; i < check; i++) {
        int32_t v = s_rec_buf[i] < 0 ? -s_rec_buf[i] : s_rec_buf[i];
        if (v > max_val) max_val = v;
    }
    ESP_LOGI(TAG, "录音结束: %lu bytes (%.1f s), 峰值: %ld",
             (unsigned long)bytes,
             (float)bytes / (MIC_SAMPLE_RATE * 2.0f),
             (long)max_val);
    return bytes;
}

bool asr_is_recording(void) { return s_recording; }

// ================================================================
// I2S 释放 / 回收（给 ESP-SR 等模块临时使用 I2S_NUM_0）
// ================================================================
void asr_mic_deinit(void)
{
    s_rec_active = false;
    s_recording  = false;
    if (s_rx_chan) {
        i2s_channel_disable(s_rx_chan);
        i2s_del_channel(s_rx_chan);
        s_rx_chan = NULL;
    }
    ESP_LOGI(TAG, "I2S NUM 0 已释放");
}

void asr_mic_reinit(void)
{
    if (s_rx_chan) {
        ESP_LOGW(TAG, "I2S NUM 0 已存在，跳过 reinit");
        return;
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
    ESP_LOGI(TAG, "I2S NUM 0 重新初始化完成");
}

// ================================================================
// 识别
// ================================================================
bool asr_recognize(uint32_t audio_len_bytes, asr_result_t *out)
{
    if (s_recog_mutex) xSemaphoreTake(s_recog_mutex, portMAX_DELAY);

    bool ret = false;
    memset(out, 0, sizeof(*out));

    /* WiFi 守卫：百度 API 必须联网。WiFi 未连接时 lwIP tcpip 队列无效，
     * 直接发请求会 panic（assert failed: Invalid mbox）。提前检查并给明确错误。*/
    wifi_status_t wst = wifi_get_status();
    if (wst != WIFI_STATUS_CONNECTED) {
        ESP_LOGE(TAG, "WiFi 未连接 (status=%d)，无法识别", (int)wst);
        snprintf(out->error_msg, sizeof(out->error_msg), "请先连接 WiFi");
        goto out;
    }

    if (!s_rec_buf || audio_len_bytes == 0) {
        snprintf(out->error_msg, sizeof(out->error_msg), "No audio");
        goto out;
    }
    const char *token = baidu_token_get();
    if (!token) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Token failed");
        goto out;
    }

    ESP_LOGI(TAG, "发送识别请求, 音频 %lu bytes", (unsigned long)audio_len_bytes);

    // 直接发送 PCM 数据，URL 带参数
    char asr_url[384];
    snprintf(asr_url, sizeof(asr_url),
        "%s?dev_pid=80001&cuid=esp32s3_bot&token=%s",
        BAIDU_ASR_URL, token);

    s_http_len = 0;
    s_http_overflow = false;
    esp_http_client_config_t cfg = {
        .url = asr_url,
        .event_handler = http_event_cb,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .buffer_size_tx = 8192,
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
        /* HTTP 401：token 失效，强制下次重取 */
        if (status == 401) baidu_token_invalidate();
        snprintf(out->error_msg, sizeof(out->error_msg), "HTTP %d", status);
        goto out;
    }
    if (s_http_overflow) {
        ESP_LOGE(TAG, "识别响应超过最大缓冲 %u 字节", RESP_MAX_CAP);
        snprintf(out->error_msg, sizeof(out->error_msg), "Response too large");
        goto out;
    }
    ESP_LOGI(TAG, "识别响应: %s", s_http_buf);

    cJSON *root = cJSON_Parse(s_http_buf);
    if (!root) {
        snprintf(out->error_msg, sizeof(out->error_msg), "Parse error");
        goto out;
    }
    cJSON *err_no = cJSON_GetObjectItem(root, "err_no");
    if (!err_no || err_no->valueint != 0) {
        int code = err_no ? err_no->valueint : -1;
        cJSON *err_msg = cJSON_GetObjectItem(root, "err_msg");
        /* 百度 ASR 常见 token 错误码：3300/3301/3302/3303 access_token 相关 */
        if (code == 3300 || code == 3301 || code == 3302 || code == 3303) {
            baidu_token_invalidate();
        }
        snprintf(out->error_msg, sizeof(out->error_msg), "ASR err %d: %s",
                 code,
                 (err_msg && err_msg->valuestring) ? err_msg->valuestring : "");
        cJSON_Delete(root);
        goto out;
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
    ret = true;

out:
    if (s_recog_mutex) xSemaphoreGive(s_recog_mutex);
    return ret;
}
