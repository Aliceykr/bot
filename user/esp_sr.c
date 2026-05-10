#include "esp_sr.h"
#include "asr.h"
#include "asr_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"

#define TAG "ESP_SR"

/* ================================================================
 * 静态状态
 * ================================================================ */
static const esp_afe_sr_iface_t *s_afe_handle = NULL;
static esp_afe_sr_data_t       *s_afe_data    = NULL;
static const esp_mn_iface_t    *s_multinet    = NULL;
static model_iface_data_t      *s_mn_data     = NULL;
static srmodel_list_t          *s_models      = NULL;

static volatile bool s_listening = false;
static esp_sr_result_cb_t s_on_result = NULL;

static i2s_chan_handle_t s_sr_rx_chan = NULL;
static TaskHandle_t s_feed_task   = NULL;
static TaskHandle_t s_detect_task = NULL;

/* ================================================================
 * I2S 管理（ESP-SR 专属 16bit mono，区别于 asr.c 的 32bit stereo）
 * ================================================================ */
static esp_err_t sr_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_sr_rx_chan));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_SCK_PIN,
            .ws   = MIC_WS_PIN,
            .din  = MIC_SD_PIN,
            .dout = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_sr_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_sr_rx_chan));
    ESP_LOGI(TAG, "I2S NUM 0 初始化 (16bit mono for ESP-SR)");
    return ESP_OK;
}

static void sr_i2s_deinit(void)
{
    if (s_sr_rx_chan) {
        i2s_channel_disable(s_sr_rx_chan);
        i2s_del_channel(s_sr_rx_chan);
        s_sr_rx_chan = NULL;
    }
}

/* ================================================================
 * feed 任务：读 I2S → 转 16bit mono（INMP441 仍出 32bit stereo）
 *           → 喂给 AFE
 * 绑 Core 0，优先级 6
 * ================================================================ */
static void sr_feed_task(void *arg)
{
    int afe_chunk = s_afe_handle->get_feed_chunksize(s_afe_data);
    int afe_nch   = s_afe_handle->get_feed_channel_num(s_afe_data);

    /* I2S 读缓冲：INMP441 需要 32bit stereo 读取，每批取 afe_chunk 个 mono 样本
     * → 需要 afe_chunk 个 stereo 对 = afe_chunk * 2 个 int32_t */
    int32_t *i2s_buf = heap_caps_malloc(afe_chunk * 2 * sizeof(int32_t), MALLOC_CAP_SPIRAM);
    /* AFE feed 缓冲：afe_nch 通道 × afe_chunk 样本 × 2 字节 */
    int16_t *afe_buf = malloc(afe_chunk * afe_nch * sizeof(int16_t));
    if (!i2s_buf || !afe_buf) {
        ESP_LOGE(TAG, "feed 任务缓冲分配失败");
        free(i2s_buf); free(afe_buf);
        s_listening = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "feed 任务启动, chunk=%d, nch=%d", afe_chunk, afe_nch);

    while (s_listening) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_sr_rx_chan, i2s_buf,
                                          afe_chunk * 2 * sizeof(int32_t),
                                          &bytes_read, pdMS_TO_TICKS(100));
        if (err != ESP_OK || bytes_read == 0) continue;
        if (!s_listening) break;

        int stereo_pairs = bytes_read / (2 * sizeof(int32_t));
        int mono_samples = stereo_pairs < afe_chunk ? stereo_pairs : afe_chunk;

        /* 32bit stereo → 16bit mono（左声道，INMP441 L/R=GND） */
        for (int i = 0; i < mono_samples; i++) {
            int32_t raw = i2s_buf[i * 2];       /* 左声道 */
            int32_t val = (raw >> 16) * 3;      /* 取高 16bit，放大 3x */
            if (val >  32767) val =  32767;
            if (val < -32768) val = -32768;
            afe_buf[i] = (int16_t)val;
        }

        /* 如果 AFE 需要多通道（如 "MR" 格式），ref 通道填 0 */
        if (afe_nch > 1) {
            for (int i = 0; i < mono_samples; i++) {
                for (int ch = 1; ch < afe_nch; ch++) {
                    afe_buf[i * afe_nch + ch] = 0;
                }
            }
        }

        s_afe_handle->feed(s_afe_data, afe_buf);
    }

    free(i2s_buf);
    free(afe_buf);
    ESP_LOGI(TAG, "feed 任务退出");
    vTaskDelete(NULL);
}

/* ================================================================
 * detect 任务：AFE fetch → MultiNet detect → 回调结果
 * 绑 Core 1，优先级 5
 * ================================================================ */
static void sr_detect_task(void *arg)
{
    int afe_chunk = s_afe_handle->get_fetch_chunksize(s_afe_data);
    int mn_chunk  = s_multinet->get_samp_chunksize(s_mn_data);
    ESP_LOGI(TAG, "detect 任务启动, afe_chunk=%d, mn_chunk=%d", afe_chunk, mn_chunk);

    while (s_listening) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            ESP_LOGW(TAG, "AFE fetch 失败");
            break;
        }
        if (!s_listening) break;

        esp_mn_state_t state = s_multinet->detect(s_mn_data, res->data);

        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_res = s_multinet->get_results(s_mn_data);
            if (s_on_result && mn_res->num > 0) {
                ESP_LOGI(TAG, "识别到命令: id=%d, str=%s, prob=%.2f",
                         mn_res->command_id[0], mn_res->string, mn_res->prob[0]);
                s_on_result(mn_res->command_id[0], mn_res->string, mn_res->prob[0]);
            }
            s_multinet->clean(s_mn_data);
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGI(TAG, "命令识别超时");
            if (s_on_result) {
                s_on_result(-1, NULL, 0);
            }
            break;
        }
    }

    ESP_LOGI(TAG, "detect 任务退出");
    vTaskDelete(NULL);
}

/* ================================================================
 * 公共 API
 * ================================================================ */

static bool s_sr_initialized = false;

bool esp_sr_init(void)
{
    /* 幂等：已初始化直接返回 */
    if (s_sr_initialized) return true;

    ESP_LOGI(TAG, "初始化 ESP-SR...");

    /* 1. 加载模型列表 */
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "模型分区加载失败，请检查 partitions.csv 和 srmodels.bin");
        return false;
    }

    /* 2. 找到 MultiNet7 CN 模型 */
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        ESP_LOGE(TAG, "未找到中文 MultiNet 模型");
        return false;
    }
    ESP_LOGI(TAG, "MultiNet 模型: %s", mn_name);

    /* 3. 创建 AFE（WakeNet 禁用） */
    afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_cfg) {
        ESP_LOGE(TAG, "AFE 配置创建失败");
        return false;
    }
    afe_cfg->wakenet_init = false;      /* 不需要唤醒词 */
    afe_cfg->vad_init     = true;
    afe_cfg->ns_init      = true;       /* 降噪 */
    afe_cfg->agc_mode     = 3;          /* 自动增益 */
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_cfg->afe_linear_gain = 2.0;

    s_afe_handle = esp_afe_handle_from_config(afe_cfg);
    if (!s_afe_handle) {
        ESP_LOGE(TAG, "AFE handle 创建失败");
        afe_config_free(afe_cfg);
        return false;
    }
    s_afe_data = s_afe_handle->create_from_config(afe_cfg);
    afe_config_free(afe_cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE 实例创建失败");
        return false;
    }

    /* 4. 创建 MultiNet */
    s_multinet = esp_mn_handle_from_name(mn_name);
    if (!s_multinet) {
        ESP_LOGE(TAG, "MultiNet handle 创建失败");
        return false;
    }
    s_mn_data = s_multinet->create(mn_name, 6000);   /* 6 秒超时 */
    if (!s_mn_data) {
        ESP_LOGE(TAG, "MultiNet 实例创建失败");
        return false;
    }

    /* 5. 注册命令词（MultiNet7 要求拼音格式，空格分隔音节） */
    esp_mn_commands_alloc(s_multinet, s_mn_data);

    esp_mn_commands_add(1,  "fan hui");
    esp_mn_commands_add(2,  "que ren");
    esp_mn_commands_add(3,  "qu xiao");
    esp_mn_commands_add(10, "lian jie wang luo");
    esp_mn_commands_add(11, "cha kan tian qi");
    esp_mn_commands_add(12, "da kai you xi");
    esp_mn_commands_add(13, "da kai liao tian");
    esp_mn_commands_add(14, "yu yin zhu shou");
    esp_mn_commands_add(20, "tui chu you xi");
    esp_mn_commands_add(30, "tiao da yin liang");
    esp_mn_commands_add(31, "tiao xiao yin liang");

    esp_mn_commands_update();
    s_multinet->print_active_speech_commands(s_mn_data);

    ESP_LOGI(TAG, "ESP-SR 初始化完成");
    ESP_LOGI(TAG, "内部 RAM 剩余: %lu KB", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ESP_LOGI(TAG, "PSRAM 剩余: %lu KB", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    s_sr_initialized = true;
    return true;
}

bool esp_sr_start_listening(esp_sr_result_cb_t on_result)
{
    if (s_listening) {
        ESP_LOGW(TAG, "已在监听中");
        return false;
    }
    if (!s_afe_handle || !s_mn_data) {
        ESP_LOGE(TAG, "ESP-SR 未初始化");
        return false;
    }

    s_on_result = on_result;

    /* 释放 asr.c 的 I2S_NUM_0 */
    asr_mic_deinit();

    /* 创建 ESP-SR 专属 I2S */
    if (sr_i2s_init() != ESP_OK) {
        ESP_LOGE(TAG, "I2S 初始化失败");
        asr_mic_reinit();
        return false;
    }

    /* 重置 MultiNet 状态 */
    s_multinet->clean(s_mn_data);

    s_listening = true;

    /* 创建 feed 和 detect 任务 */
    BaseType_t r1 = xTaskCreatePinnedToCore(sr_feed_task, "sr_feed", 4096, NULL, 6, &s_feed_task, 0);
    BaseType_t r2 = xTaskCreatePinnedToCore(sr_detect_task, "sr_detect", 6144, NULL, 5, &s_detect_task, 1);

    if (r1 != pdPASS || r2 != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败 feed=%ld detect=%ld", (long)r1, (long)r2);
        s_listening = false;
        sr_i2s_deinit();
        asr_mic_reinit();
        return false;
    }

    ESP_LOGI(TAG, "开始监听命令词...");
    return true;
}

void esp_sr_stop_listening(void)
{
    if (!s_listening) return;
    s_listening = false;
    s_on_result = NULL;

    /* 先等 feed 任务退出（I2S read 最多阻塞 100ms，循环检查确保它真正结束） */
    for (int i = 0; i < 20 && s_feed_task != NULL; i++) {
        eTaskState st = eTaskGetState(s_feed_task);
        if (st == eDeleted || st == eInvalid) { s_feed_task = NULL; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_feed_task = NULL;

    /* detect 任务依赖 feed 产出的数据。feed 停了后 fetch 会很快返回失败。
     * 最多等 2 秒（detect 可能在做 MultiNet 推理） */
    for (int i = 0; i < 40 && s_detect_task != NULL; i++) {
        eTaskState st = eTaskGetState(s_detect_task);
        if (st == eDeleted || st == eInvalid) { s_detect_task = NULL; break; }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    s_detect_task = NULL;

    /* 任务已退出，安全释放 I2S */
    sr_i2s_deinit();
    asr_mic_reinit();

    ESP_LOGI(TAG, "停止监听，I2S 已归还 asr 模块");
}

bool esp_sr_is_listening(void)
{
    return s_listening;
}

void esp_sr_deinit(void)
{
    if (!s_sr_initialized) return;

    /* 如果正在监听，先停止 */
    if (s_listening) {
        esp_sr_stop_listening();
    }

    ESP_LOGI(TAG, "释放 ESP-SR 资源...");

    /* 释放 MultiNet 实例 */
    if (s_multinet && s_mn_data) {
        s_multinet->destroy(s_mn_data);
        s_mn_data = NULL;
    }
    s_multinet = NULL;

    /* 释放 AFE 实例 */
    if (s_afe_handle && s_afe_data) {
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    s_afe_handle = NULL;

    /* 释放模型列表 */
    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }

    s_sr_initialized = false;
    ESP_LOGI(TAG, "ESP-SR 资源已释放, 内部 RAM 剩余: %lu KB",
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}
