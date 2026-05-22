#include "tts.h"
#include "asr_config.h"
#include "speaker.h"
#include "baidu_token.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#define TAG "TTS"

/* 百度短文本在线合成基础音库 per 参数。
 * BLE 命令使用 /v1.. /v4 的本地编号，真正请求百度时映射到 per。 */
static const tts_voice_t s_voices[] = {
    { .per = 0, .name = "度小美-标准女主播" },
    { .per = 1, .name = "度小宇-亲切男声" },
    { .per = 3, .name = "度逍遥-情感男声" },
    { .per = 4, .name = "度丫丫-童声" },
};

static volatile int s_voice_index = 0;   /* 0-based, default: 度小美 */

/* ================================================================
 * 模块内部状态（全部 static）
 * access_token 由 baidu_token 模块统一管理，带过期刷新
 * ================================================================ */
static bool s_is_pcm = false;        /* 当前响应是否为音频流 */
static int  s_wav_skip = 0;          /* 已跳过的 WAV 头字节数（固定44字节）*/

/* 错误响应缓冲（非音频流时收集 JSON 错误信息，固定小容量足够）*/
#define ERR_BUF_SIZE   256
static char  s_err_buf[ERR_BUF_SIZE];
static int   s_err_len = 0;

/* 互斥锁：保护 tts_speak 全过程（encoded / body / s_err_buf 等 static 共享资源）*/
static SemaphoreHandle_t s_mutex = NULL;
/* 懒创建 TTS 互斥锁，保护一次合成请求中的静态缓冲和音色状态读取。 */
static void ensure_mutex(void) {
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
}

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
 * 公开接口
 * ================================================================ */
bool tts_speak(const char *text)
{
    if (!text || strlen(text) == 0) return false;

    ensure_mutex();
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool ret = false;

    char token_buf[256];
    if (!baidu_token_copy(token_buf, sizeof(token_buf))) {
        ESP_LOGE(TAG, "获取 token 失败");
        goto out;
    }

    int per = s_voices[s_voice_index].per;

    /* percent-encode 文本（中文每字3字节，编码后9字节，512字节文本最坏约4608字节）
     * 使用 static 避免在任务栈上分配大数组；mutex 保护并发 */
    static char encoded[4096];
    tts_url_encode(text, encoded, sizeof(encoded));

    /* 构建 POST body */
    static char body[5120];
    int body_len = snprintf(body, sizeof(body),
        "tex=%s&tok=%s&cuid=esp32s3_bot&ctp=1&lan=zh&spd=5&pit=5&vol=9&per=%d&aue=6",
        encoded, token_buf, per);
    if (body_len <= 0 || body_len >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "body 构建失败或过长");
        goto out;
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
        /* HTTP 401：token 失效，下次重取 */
        if (status == 401) baidu_token_invalidate();
        goto out;
    }
    if (!s_is_pcm) {
        ESP_LOGE(TAG, "TTS 未返回 PCM 数据");
        /* 百度 TTS 失败时返回 JSON 错误，包含 err_no。常见 token 错误：
         * 501/502/503 token 相关。保守起见，失败且非 PCM 响应就失效 token */
        baidu_token_invalidate();
        goto out;
    }

    ESP_LOGI(TAG, "合成完成");
    ret = true;

out:
    if (s_mutex) xSemaphoreGive(s_mutex);
    return ret;
}

/* 返回当前固件内置的百度 TTS 音色数量。 */
int tts_voice_count(void)
{
    return (int)(sizeof(s_voices) / sizeof(s_voices[0]));
}

/* 按 1-based 编号取得音色配置；BLE 命令直接使用这个编号体系。 */
const tts_voice_t *tts_voice_get(int index)
{
    if (index < 1 || index > tts_voice_count()) return NULL;
    return &s_voices[index - 1];
}

/* 返回当前音色的 1-based 编号，便于 UI/BLE 显示。 */
int tts_voice_current_index(void)
{
    return s_voice_index + 1;
}

/* 返回当前音色结构体，失败时由 tts_voice_get 统一处理。 */
const tts_voice_t *tts_voice_current(void)
{
    return tts_voice_get(tts_voice_current_index());
}

/* 切换百度 TTS per 参数。
 * 只修改索引，不中断正在播放的音频；下一次 tts_speak 生效。 */
bool tts_voice_set(int index)
{
    if (index < 1 || index > tts_voice_count()) return false;

    s_voice_index = index - 1;

    ESP_LOGI(TAG, "切换音色: v%d per=%d %s",
             index, s_voices[index - 1].per, s_voices[index - 1].name);
    return true;
}
