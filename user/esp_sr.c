#include "esp_sr.h"
#include "asr.h"
#include "asr_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include <string.h>

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

/* 任务退出同步锁：保护 s_read_task / s_feed_task / s_detect_task 句柄。
 * 任务自删退出序列必须是"锁内清句柄 → 锁外 vTaskDelete(NULL)"，
 * stop 路径硬杀也必须在锁内读 + 立刻 vTaskDelete(handle) + 清 NULL，
 * 避免两者交叉时 stop 对已释放的 TCB 二次 delete 导致崩溃。 */
static SemaphoreHandle_t s_task_mutex = NULL;

/* ================================================================
 * I2S → AFE 环形缓冲（PSRAM）
 *
 * 架构：三个角色解耦
 *   读取任务（sr_read_task, Core 0）：持续从 I2S 读 32bit stereo，
 *       转 16bit mono 写入 ringbuf。I2S 短暂错误可自恢复不退出。
 *   feed 任务（sr_feed_task, Core 0）：从 ringbuf 取一块喂给 AFE。
 *   detect 任务（sr_detect_task, Core 1）：AFE fetch + MultiNet detect。
 *
 * Ring buffer 作用：
 *   - I2S 读速率偶发抖动时不会让 AFE 饿死
 *   - 识别 ISR 里不能等 I2S 读，分离出来更平稳
 *   - 大小够 ~500ms 音频（16kHz×16bit = 32KB/s × 0.5s = 16KB）。
 *     ESP-SR 推理一帧 32ms，偶尔被游戏/WiFi 抢占 200ms 不会丢音
 *
 * 长时间运行的关键：
 *   - 任何一个任务遇到错误必须自恢复，不能直接退出
 *   - detect 任务永远循环，TIMEOUT/DETECTED 后都 clean 继续
 * ================================================================ */
#define RING_BUF_BYTES  (16 * 1024)
static RingbufHandle_t s_pcm_ring = NULL;
static TaskHandle_t    s_read_task = NULL;
/* AFE 需要的每帧样本数；在 start 时填充以便所有任务共享 */
static int s_afe_chunk_samples = 0;
static int s_afe_nch = 1;

static inline void sr_task_lock(void)   { if (s_task_mutex) xSemaphoreTake(s_task_mutex, portMAX_DELAY); }
static inline void sr_task_unlock(void) { if (s_task_mutex) xSemaphoreGive(s_task_mutex); }

/* ================================================================
 * I2S 管理（ESP-SR 专属 16bit mono，区别于 asr.c 的 32bit stereo）
 * ================================================================ */
static esp_err_t sr_i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_sr_rx_chan));

    /* INMP441 输出 24bit 数据，左对齐到 32bit 槽位中。
     * 与 asr.c 保持一致：32bit stereo Philips 模式，软件层取左声道并右移 16 位
     * 转 16bit mono 给 AFE。如果改成 16bit MONO 槽位，硬件层直接丢弃低位会
     * 导致小信号失真严重，且和 read 任务里 (raw >> 16) * 3 的转换逻辑不匹配。 */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
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
    ESP_LOGI(TAG, "I2S NUM 0 初始化 (32bit stereo INMP441 → 软件转 16bit mono)");
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
 * read 任务（Core 0）：I2S → 转 16bit mono → 写入 ring buffer
 *
 * 设计准则（长时间运行关键）：
 *   - 任何 I2S read 错误都不退出，只记日志继续循环
 *   - ring buffer 满时丢弃最早数据（xRingbufferSend 失败就跳过）
 *   - 只在 s_listening=false 时才 break 退出
 * ================================================================ */
static void sr_read_task(void *arg)
{
    (void)arg;
    /* I2S 一次读 320ms 音频：INMP441 32bit stereo = 1 对 8 字节。
     * 16kHz × 0.02s(一帧) × 8B = ~2.5KB 每 20ms 读一次。
     * 缓冲开 4KB，读 512 对 stereo（= 16kHz × 32ms）。*/
    const int I2S_PAIRS_PER_READ = 512;
    int32_t *i2s_buf = heap_caps_malloc(I2S_PAIRS_PER_READ * 2 * sizeof(int32_t),
                                         MALLOC_CAP_SPIRAM);
    int16_t *mono_buf = heap_caps_malloc(I2S_PAIRS_PER_READ * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM);
    if (!i2s_buf || !mono_buf) {
        ESP_LOGE(TAG, "read 任务缓冲分配失败");
        goto read_exit;
    }

    ESP_LOGI(TAG, "read 任务启动, pairs_per_read=%d", I2S_PAIRS_PER_READ);

    int err_count = 0;
    while (s_listening) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_sr_rx_chan, i2s_buf,
                                          I2S_PAIRS_PER_READ * 2 * sizeof(int32_t),
                                          &bytes_read, pdMS_TO_TICKS(100));
        if (!s_listening) break;

        if (err != ESP_OK) {
            if (++err_count < 5 || (err_count % 50) == 0) {
                ESP_LOGW(TAG, "I2S read 失败: err=%d count=%d", err, err_count);
            }
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (bytes_read == 0) continue;
        err_count = 0;

        int pairs = bytes_read / (2 * sizeof(int32_t));
        /* 32bit stereo → 16bit mono（左声道，INMP441 L/R=GND）*/
        for (int i = 0; i < pairs; i++) {
            int32_t raw = i2s_buf[i * 2];
            int32_t val = (raw >> 16) * 3;
            if (val >  32767) val =  32767;
            if (val < -32768) val = -32768;
            mono_buf[i] = (int16_t)val;
        }

        /* 写入 ring buffer。满了等 10ms，仍满就丢这块（NoSplit）*/
        BaseType_t sent = xRingbufferSend(s_pcm_ring, mono_buf,
                                           pairs * sizeof(int16_t),
                                           pdMS_TO_TICKS(10));
        if (sent != pdTRUE) {
            /* ring buffer 满：feed 任务处理不过来。通常是 detect 卡死或被抢占。
             * 不阻塞，直接丢帧，保证 I2S 不堆积 */
            static uint32_t drop_count = 0;
            if ((++drop_count % 50) == 0) {
                ESP_LOGW(TAG, "ring buffer 满，丢帧 #%u", (unsigned)drop_count);
            }
        }
    }

read_exit:
    if (i2s_buf)  heap_caps_free(i2s_buf);
    if (mono_buf) heap_caps_free(mono_buf);
    ESP_LOGI(TAG, "read 任务退出");
    sr_task_lock();
    s_read_task = NULL;  /* 锁内清：stop 路径硬杀看不到悬空句柄 */
    sr_task_unlock();
    vTaskDelete(NULL);
}

/* ================================================================
 * feed 任务（Core 0）：从 ring buffer 取一帧 AFE 大小的数据喂给 AFE
 *
 * AFE 需要固定 chunk 大小（来自 get_feed_chunksize）。ring buffer 里
 * 数据是可变长度 write，这里凑齐 chunk 再 feed。
 *
 * 长时间运行：
 *   - AFE feed 本身是非阻塞的，不会失败
 *   - 如果 ring buffer 空太久说明 read 任务挂了，持续重试
 * ================================================================ */
static void sr_feed_task(void *arg)
{
    (void)arg;
    int chunk_samples = s_afe_chunk_samples;
    int nch = s_afe_nch;

    /* AFE 输入缓冲：nch 通道 × chunk 样本 × 2 字节 */
    int16_t *afe_buf = heap_caps_malloc(chunk_samples * nch * sizeof(int16_t),
                                         MALLOC_CAP_INTERNAL);
    /* 累积缓冲：从 ringbuf 可能一次拿到不足 chunk，累积到够再 feed */
    int16_t *accum = heap_caps_malloc(chunk_samples * sizeof(int16_t),
                                       MALLOC_CAP_INTERNAL);
    if (!afe_buf || !accum) {
        ESP_LOGE(TAG, "feed 任务缓冲分配失败");
        goto feed_exit;
    }

    ESP_LOGI(TAG, "feed 任务启动, chunk=%d, nch=%d", chunk_samples, nch);

    int accum_pos = 0;  /* 当前累积的 mono 样本数（0..chunk_samples）*/
    while (s_listening) {
        /* 从 ring 取数据，最多等 100ms；取不到不退出，继续轮询 */
        size_t got = 0;
        int16_t *item = (int16_t *)xRingbufferReceive(s_pcm_ring, &got,
                                                        pdMS_TO_TICKS(100));
        if (!s_listening) {
            if (item) vRingbufferReturnItem(s_pcm_ring, item);
            break;
        }
        if (!item || got == 0) continue;

        int items_samples = got / sizeof(int16_t);
        int src_pos = 0;
        /* 拷进 accum，满一个 chunk 就 feed 一次，循环直到 item 消费完 */
        while (src_pos < items_samples) {
            int to_copy = chunk_samples - accum_pos;
            int available = items_samples - src_pos;
            int n = to_copy < available ? to_copy : available;
            memcpy(accum + accum_pos, item + src_pos, n * sizeof(int16_t));
            accum_pos += n;
            src_pos += n;

            if (accum_pos == chunk_samples) {
                /* 填充到 AFE 多通道布局（当前单麦，nch=1 时直接复制）*/
                if (nch == 1) {
                    memcpy(afe_buf, accum, chunk_samples * sizeof(int16_t));
                } else {
                    for (int i = 0; i < chunk_samples; i++) {
                        afe_buf[i * nch] = accum[i];
                        for (int ch = 1; ch < nch; ch++) {
                            afe_buf[i * nch + ch] = 0;
                        }
                    }
                }
                s_afe_handle->feed(s_afe_data, afe_buf);
                accum_pos = 0;
            }
        }
        vRingbufferReturnItem(s_pcm_ring, item);
    }

feed_exit:
    if (afe_buf) heap_caps_free(afe_buf);
    if (accum)   heap_caps_free(accum);
    ESP_LOGI(TAG, "feed 任务退出");
    sr_task_lock();
    s_feed_task = NULL;
    sr_task_unlock();
    vTaskDelete(NULL);
}

/* ================================================================
 * detect 任务（Core 1）：AFE fetch → MultiNet detect → 回调结果
 *
 * 长时间运行：
 *   - DETECTED：回调通知 UI → clean → 继续监听下一个命令
 *   - TIMEOUT：clean 后继续（不再退出任务，ESP-SR 会不断给 TIMEOUT）
 *   - AFE fetch 失败：短暂等待后重试（可能是 feed 暂时断了）
 * ================================================================ */
static void sr_detect_task(void *arg)
{
    (void)arg;
    int mn_chunk = s_multinet->get_samp_chunksize(s_mn_data);
    ESP_LOGI(TAG, "detect 任务启动, mn_chunk=%d", mn_chunk);

    int fetch_err_count = 0;
    while (s_listening) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!s_listening) break;

        if (!res || res->ret_value == ESP_FAIL) {
            if (++fetch_err_count < 5 || (fetch_err_count % 100) == 0) {
                ESP_LOGW(TAG, "AFE fetch 失败 count=%d", fetch_err_count);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        fetch_err_count = 0;

        esp_mn_state_t state = s_multinet->detect(s_mn_data, res->data);

        if (state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *mn_res = s_multinet->get_results(s_mn_data);
            if (s_on_result && mn_res && mn_res->num > 0) {
                ESP_LOGI(TAG, "识别到命令: id=%d, str=%s, prob=%.2f",
                         mn_res->command_id[0], mn_res->string, mn_res->prob[0]);
                s_on_result(mn_res->command_id[0], mn_res->string, mn_res->prob[0]);
            }
            /* 重置 MultiNet 状态，继续监听下一个命令 */
            s_multinet->clean(s_mn_data);
        } else if (state == ESP_MN_STATE_TIMEOUT) {
            /* 持续监听场景下 TIMEOUT 是常态（VAD 判断一句话结束）
             * 静默 clean 继续，不通知 UI，不退出任务 */
            s_multinet->clean(s_mn_data);
        }
        /* 其他状态（ESP_MN_STATE_DETECTING / IDLE）不做处理，继续 fetch */
    }

    ESP_LOGI(TAG, "detect 任务退出");
    sr_task_lock();
    s_detect_task = NULL;
    sr_task_unlock();
    vTaskDelete(NULL);
}

/* ================================================================
 * 公共 API
 * ================================================================ */

static bool s_sr_initialized = false;

/* 释放 ESP-SR 资源（内部通用）：用于 init 失败回滚和 deinit 主路径。
 * 单线程安全（外部调用者要保证 s_listening=false）。*/
static void sr_free_resources(void)
{
    if (s_multinet && s_mn_data) {
        s_multinet->destroy(s_mn_data);
        s_mn_data = NULL;
    }
    s_multinet = NULL;

    if (s_afe_handle && s_afe_data) {
        s_afe_handle->destroy(s_afe_data);
        s_afe_data = NULL;
    }
    s_afe_handle = NULL;

    if (s_models) {
        esp_srmodel_deinit(s_models);
        s_models = NULL;
    }
}

bool esp_sr_init(void)
{
    /* 幂等：已初始化直接返回 */
    if (s_sr_initialized) return true;

    /* 创建任务同步锁（跨 init/deinit 周期复用）*/
    if (!s_task_mutex) {
        s_task_mutex = xSemaphoreCreateMutex();
        if (!s_task_mutex) {
            ESP_LOGE(TAG, "task mutex 创建失败");
            return false;
        }
    }

    ESP_LOGI(TAG, "初始化 ESP-SR...");

    /* 1. 加载模型列表 */
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "模型分区加载失败，请检查 partitions.csv 和 srmodels.bin");
        goto init_fail;
    }

    /* 2. 找到 MultiNet7 CN 模型 */
    char *mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (!mn_name) {
        ESP_LOGE(TAG, "未找到中文 MultiNet 模型");
        goto init_fail;
    }
    ESP_LOGI(TAG, "MultiNet 模型: %s", mn_name);

    /* 3. 创建 AFE（WakeNet 禁用） */
    afe_config_t *afe_cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!afe_cfg) {
        ESP_LOGE(TAG, "AFE 配置创建失败");
        goto init_fail;
    }
    afe_cfg->wakenet_init = false;      /* 不需要唤醒词 */
    afe_cfg->vad_init     = true;
    afe_cfg->ns_init      = false;      /* 关闭降噪：NSNet2 会损害识别准确率（官方警告） */
    afe_cfg->agc_mode     = 3;          /* 自动增益 */
    afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_cfg->afe_linear_gain = 2.0;

    s_afe_handle = esp_afe_handle_from_config(afe_cfg);
    if (!s_afe_handle) {
        ESP_LOGE(TAG, "AFE handle 创建失败");
        afe_config_free(afe_cfg);
        goto init_fail;
    }
    s_afe_data = s_afe_handle->create_from_config(afe_cfg);
    afe_config_free(afe_cfg);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "AFE 实例创建失败");
        goto init_fail;
    }

    /* 4. 创建 MultiNet */
    s_multinet = esp_mn_handle_from_name(mn_name);
    if (!s_multinet) {
        ESP_LOGE(TAG, "MultiNet handle 创建失败");
        goto init_fail;
    }
    s_mn_data = s_multinet->create(mn_name, 60000);   /* 60 秒超时：持续监听场景下延长，减少 TIMEOUT 清理频率 */
    if (!s_mn_data) {
        ESP_LOGE(TAG, "MultiNet 实例创建失败");
        goto init_fail;
    }

    /* 5. 注册命令词（MultiNet7 要求拼音格式，空格分隔音节）
     * 注意：MultiNet7 对短命令（≤2 音节）识别率差，官方建议 3-6 音节。
     * 命令 ID 与 my_demo.c 中 sr_dispatch_cmd 的分发逻辑一一对应，
     * 修改时需同步更新两边。 */
    esp_mn_commands_alloc(s_multinet, s_mn_data);

    esp_mn_commands_add(1, "da kai huan jing jian ce");      /* 打开环境监测 */
    esp_mn_commands_add(2, "da kai tian qi he ri qi");       /* 打开天气和日期（需 WiFi）*/
    esp_mn_commands_add(3, "da kai you xi");                 /* 打开游戏 */
    esp_mn_commands_add(4, "da kai liao tian zhu shou");     /* 打开聊天助手（需 WiFi）*/
    esp_mn_commands_add(5, "da kai yu yin zhu shou");        /* 打开语音助手（需 WiFi）*/
    esp_mn_commands_add(6, "da kai lan ya");                 /* 打开蓝牙 */
    esp_mn_commands_add(7, "da kai yin yue");                /* 打开音乐 */
    esp_mn_commands_add(8, "da kai yin liang");              /* 打开音量 */
    esp_mn_commands_add(9, "da kai zhi neng she bei");       /* 打开智能设备（需 WiFi）*/

    esp_mn_commands_update();
    s_multinet->print_active_speech_commands(s_mn_data);

    /* 6. 抑制 AFE 库在 stop 时空 ringbuf 的警告。
     * stop 时 feed 任务先退出，detect 任务最后一次 fetch 时 AFE 内部
     * ring 已空，AFE 会打 W 警告。这是正常退出顺序，不是错误。
     * 把 AFE tag 的日志等级降到 ERROR，屏蔽这条无害警告。
     *
     * 注：这是全局副作用（esp_log_level_set 是进程范围的），deinit 后
     * 不会还原。考虑到目前只有 ESP-SR 用 AFE，且压制的只是 W 警告
     * （不影响 E 错误），副作用可接受。如果将来其他模块用 AFE，
     * 把这行移到 app_main 一次性设置。 */
    esp_log_level_set("AFE", ESP_LOG_ERROR);

    ESP_LOGI(TAG, "ESP-SR 初始化完成");
    ESP_LOGI(TAG, "内部 RAM 剩余: %lu KB", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    ESP_LOGI(TAG, "PSRAM 剩余: %lu KB", (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    s_sr_initialized = true;
    return true;

init_fail:
    /* 统一失败回滚：无论卡在哪一步，已分配的 s_models/s_afe_handle/s_afe_data/
     * s_multinet/s_mn_data 都会被 sr_free_resources 释放干净，不留泄漏。
     * 下次 esp_sr_init() 可以干净重试。 */
    sr_free_resources();
    return false;
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

    /* 创建环形缓冲（I2S ↔ AFE 解耦）。
     * v5.1.2 的 xRingbufferCreate 只能从 DRAM 分配，但 16KB 对目前
     * 剩余 DRAM（SR 启动后 ~77KB）可接受。等升级 IDF 5.2+ 可换成
     * xRingbufferCreateWithCaps 放 PSRAM 进一步省内存 */
    s_pcm_ring = xRingbufferCreate(RING_BUF_BYTES, RINGBUF_TYPE_NOSPLIT);
    if (!s_pcm_ring) {
        ESP_LOGE(TAG, "ring buffer 创建失败");
        sr_i2s_deinit();
        asr_mic_reinit();
        return false;
    }

    /* 预取 AFE chunk/nch 给 feed 任务共享 */
    s_afe_chunk_samples = s_afe_handle->get_feed_chunksize(s_afe_data);
    s_afe_nch           = s_afe_handle->get_feed_channel_num(s_afe_data);

    /* 重置 MultiNet 状态 */
    s_multinet->clean(s_mn_data);

    /* 先置 true 让任务起来能进 while 循环，但此前所有资源（pcm_ring / I2S /
     * AFE / MultiNet）已经分配完毕、可以被任务安全使用。任务创建若失败走清理
     * 分支：先把 s_listening 回滚到 false，等已起来的任务退出再释放资源。*/
    s_listening = true;

    /* 三个任务（按优先级从高到低）：
     *   read 任务 prio=6（I2S 读取最及时）
     *   feed 任务 prio=5（凑 AFE chunk）
     *   detect 任务 prio=5（推理，独占 Core 1）
     * 栈调整：
     *   - feed 要喂 AFE 内部分配，4KB 吃紧，提到 5KB
     *   - read 带 I2S log + PSRAM malloc，实际 ~2.8KB，提到 5KB 留余量
     *   - detect 推理栈保持 6KB */
    BaseType_t r0 = xTaskCreatePinnedToCore(sr_read_task, "sr_read", 5120,
                                             NULL, 6, &s_read_task, 0);
    BaseType_t r1 = xTaskCreatePinnedToCore(sr_feed_task, "sr_feed", 5120,
                                             NULL, 5, &s_feed_task, 0);
    BaseType_t r2 = xTaskCreatePinnedToCore(sr_detect_task, "sr_detect", 6144,
                                             NULL, 5, &s_detect_task, 1);

    if (r0 != pdPASS || r1 != pdPASS || r2 != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败 read=%ld feed=%ld detect=%ld",
                 (long)r0, (long)r1, (long)r2);
        s_listening = false;
        /* 等已起来的任务检测到 s_listening=false 后自然退出 */
        vTaskDelay(pdMS_TO_TICKS(200));
        if (s_pcm_ring) {
            vRingbufferDelete(s_pcm_ring);
            s_pcm_ring = NULL;
        }
        sr_i2s_deinit();
        asr_mic_reinit();
        return false;
    }

    ESP_LOGI(TAG, "开始监听命令词（持续模式）...");
    return true;
}

void esp_sr_stop_listening(void)
{
    if (!s_listening) return;
    s_listening = false;
    s_on_result = NULL;

    /* 等三个任务检测到 s_listening=false 后退出。
     * read 任务最多阻塞在 i2s_read 100ms；feed 任务阻塞在 ringbuf recv 100ms；
     * detect 任务可能在 MultiNet 推理中（<500ms）。给 2 秒总容差。*/
    const int total_wait_steps = 40;  /* 40 × 50ms = 2s */
    for (int i = 0; i < total_wait_steps; i++) {
        sr_task_lock();
        bool all_gone = (s_read_task == NULL) &&
                        (s_feed_task == NULL) &&
                        (s_detect_task == NULL);
        sr_task_unlock();
        if (all_gone) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* 如果还有任务没退出，硬杀（极少发生）。
     * 在锁内完成"读句柄 + delete + 清 NULL"三步，与任务自退出（锁内清 NULL）
     * 互斥，避免两者交叉时 stop 对已释放 TCB 二次 delete。*/
    sr_task_lock();
    TaskHandle_t to_kill[3] = { s_read_task, s_feed_task, s_detect_task };
    s_read_task = NULL;
    s_feed_task = NULL;
    s_detect_task = NULL;
    sr_task_unlock();
    bool any_hard_killed = false;
    for (int i = 0; i < 3; i++) {
        if (to_kill[i]) {
            ESP_LOGW(TAG, "硬杀未退出的任务 %p", to_kill[i]);
            vTaskDelete(to_kill[i]);
            any_hard_killed = true;
        }
    }

    /* 被硬杀的任务可能正持有 ringbuf 内部互斥锁或 item 引用；
     * 此时 vRingbufferDelete() 会走断言（持有者 ≠ 当前任务）→ 整机崩。
     * 选择故意泄漏这块 ringbuf（16KB PSRAM），等 start_listening 下次
     * 调用前手动置 NULL 让新 ringbuf 被建立。这比崩机可接受得多，
     * 且一个周期最多泄漏一次（硬杀本身是极稀有事件）。*/
    if (s_pcm_ring) {
        if (any_hard_killed) {
            ESP_LOGW(TAG, "任务被硬杀，放弃 ringbuf 删除以避免断言崩溃（16KB 泄漏）");
        } else {
            vRingbufferDelete(s_pcm_ring);
        }
        s_pcm_ring = NULL;
    }

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

    /* 统一走内部清理：和 init 失败回滚共用代码 */
    sr_free_resources();

    s_sr_initialized = false;
    ESP_LOGI(TAG, "ESP-SR 资源已释放, 内部 RAM 剩余: %lu KB",
             (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}
