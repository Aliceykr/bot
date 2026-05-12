#include "speaker.h"
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#define TAG             "SPEAKER"

/* ================================================================
 * 参考 esp32_1_hybrid 工程的音频配置：
 *   - 硬件 MONO 槽位（不再软件 stereo 展开）
 *   - DMA 自动清零：驱动喂不到数据时 DMA 描述符自动填 0
 *   - 播放前做字节尾对齐 + 轻度高通（DC block）
 *
 * 老版本杂音来源：
 *   1. 按字节送入 ring 时，HTTP chunked 给出奇数字节就会导致 16bit 样本
 *      错位 1 字节，从此整段 PCM 全部变成雪花白噪声
 *   2. 每 20ms 主动灌 32 字节静音 + 新数据到来时做 32 样本淡入 → 突发
 *      数据下反复出现 "静音—淡入" 循环，听感像周期性咔哒/颤抖
 *   3. 软件 stereo 展开放大一倍数据量，ring 饿得更快，上一条更明显
 *   4. MAX98357A 自带直流偏置，没有 HPF 时低信号底噪会被放大
 * ================================================================ */

/* 播放 RingBuffer：64KB ≈ 2 秒缓冲（16kHz 16bit mono）*/
#define SPK_RINGBUF_SIZE    (64 * 1024)

/* tx 任务每次取出并写入 I2S 的最大字节数 */
#define SPK_TX_CHUNK        1024

#define SPK_TASK_STACK      4096
#define SPK_TASK_PRIO       3

/* DMA：与参考工程对齐，8 x 256 帧 = ~128ms 缓冲 @16kHz */
#define DMA_DESC_NUM        8
#define DMA_FRAME_NUM       256

/* DC-block 高通：y[n] = x[n] - x[n-1] + a * y[n-1]，a=0.995
 * 截止 ≈ (1-a)*fs/(2π) ≈ 12.7Hz @ 16kHz，可听段零衰减，只砍直流 */
#define HP_COEF             0.995f

/* 首次播放 / flush 后再次开播 时的线性淡入长度（样本）
 * 64 samples @16kHz = 4ms，足够消爆破，人耳感知不到 */
#define FADE_SAMPLES        64

/* speaker_play 内部处理缓冲 */
#define PROC_BATCH          64

/* ================================================================
 * 模块内部状态
 * ================================================================ */
static i2s_chan_handle_t  s_tx_chan   = NULL;
static RingbufHandle_t    s_ringbuf   = NULL;
static SemaphoreHandle_t  s_play_mtx  = NULL;   /* 保护 HPF / tail / fade 状态 */

/* flush 协调：
 *   speaker_flush 是从别的任务（例如 LVGL）发出的"立即静音"请求。
 *   spk_tx_task 是 ring 的唯一合法 consumer，让它自己来清空 ring，
 *   避免两个 consumer 同时 xRingbufferReceive 造成漏消费 / 死锁。
 *
 * 协议：
 *   flush 调用方 → 置 s_flush_request，take s_flush_done（清为 0）→ 阻塞等
 *   tx 任务每轮循环 → 若 s_flush_request：先把自己刚拿的 item return，
 *                     再循环 xRingbufferReceive(timeout=0) 把 ring 清干净，
 *                     清完 → 清 s_flush_request → give s_flush_done
 *   flush 调用方被唤醒 → 继续复位 HPF / tail / fade → 返回
 *
 * 这样 flush 保证了 tx 任务看到的 ring 一定为空，且不会和 tx 竞争 item 所有权。 */
static volatile bool          s_flush_request = false;
static SemaphoreHandle_t      s_flush_done    = NULL;  /* binary semaphore */

/* 字节尾：上一次 speaker_play 输入字节数是奇数时，保留的最后一个字节，
 * 下次拼上首字节组成完整 16bit 采样，防止跨包字节错位 */
static uint8_t  s_pcm_tail       = 0;
static bool     s_pcm_tail_valid = false;

/* DC-block HPF 状态 */
static float    s_hp_x1          = 0.0f;
static float    s_hp_y1          = 0.0f;

/* 线性淡入：init / flush 后归零，前 FADE_SAMPLES 个样本按 (i+1)/N 递增 */
static uint16_t s_fade_count     = 0;

/* ================================================================
 * 单样本处理：HPF + 淡入 + 饱和
 * 调用方需持有 s_play_mtx
 * ================================================================ */
static inline int16_t process_sample(int16_t x)
{
    float xf = (float)x;
    float y  = xf - s_hp_x1 + HP_COEF * s_hp_y1;
    s_hp_x1 = xf;
    s_hp_y1 = y;

    int32_t yi = (int32_t)y;

    if (s_fade_count < FADE_SAMPLES) {
        yi = yi * (int32_t)(s_fade_count + 1) / (int32_t)FADE_SAMPLES;
        s_fade_count++;
    }

    if (yi >  32767) yi =  32767;
    if (yi < -32768) yi = -32768;
    return (int16_t)yi;
}

/* ================================================================
 * 播放任务：从 ring 取出数据，直接用硬件 MONO 写 I2S
 * ring 空时不做任何动作，DMA 由驱动的 auto_clear 自动输出零电平
 *
 * flush 协作：每轮入口检查 s_flush_request，若置位则自己清空 ring 再
 * 通过 s_flush_done 通知请求方。确保 ring 的 consumer 唯一性。
 * ================================================================ */
static void spk_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 优先处理 flush 请求：此时可能没有 item，但也要清空 ring 中的任何
         * 残留数据，然后 signal done */
        if (s_flush_request) {
            size_t n = 0;
            while (1) {
                void *item = xRingbufferReceive(s_ringbuf, &n, 0);
                if (!item) break;
                vRingbufferReturnItem(s_ringbuf, item);
            }
            s_flush_request = false;
            if (s_flush_done) xSemaphoreGive(s_flush_done);
            /* flush 完立刻回头去检查 ring，避免错过新塞进来的数据 */
            continue;
        }

        size_t recv_size = 0;
        void *item = xRingbufferReceiveUpTo(
            s_ringbuf, &recv_size, pdMS_TO_TICKS(50), SPK_TX_CHUNK);

        if (item == NULL) {
            /* ring 空：什么都不做，DMA 由 auto_clear 负责输出零电平 */
            continue;
        }

        /* 在 i2s_write 之前再查一次 flush：若刚收到 item 就被请求 flush，
         * 不要把这块数据播出去，直接丢弃。这是 flush 语义必须的。 */
        if (s_flush_request) {
            vRingbufferReturnItem(s_ringbuf, item);
            /* 下一轮循环会处理 flush_request 并清空 ring */
            continue;
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(
            s_tx_chan, item, recv_size, &bytes_written, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_write 失败: %d", err);
        }

        vRingbufferReturnItem(s_ringbuf, item);
    }
}

/* ================================================================
 * 初始化
 * ================================================================ */
void speaker_init(void)
{
    s_play_mtx = xSemaphoreCreateMutex();
    if (!s_play_mtx) {
        ESP_LOGE(TAG, "mutex 创建失败");
        return;
    }

    /* flush 协作用的 binary semaphore：初始为空，tx 任务完成清空后 give，
     * flush 调用方 take 后清空，再走下一次 flush */
    s_flush_done = xSemaphoreCreateBinary();
    if (!s_flush_done) {
        ESP_LOGE(TAG, "flush_done sem 创建失败");
        return;
    }

    s_ringbuf = xRingbufferCreate(SPK_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "RingBuffer 创建失败");
        return;
    }

    /* I2S channel 配置：开启 auto_clear，DMA 饿死时自动喂 0 */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = DMA_DESC_NUM;
    chan_cfg.dma_frame_num = DMA_FRAME_NUM;
    chan_cfg.auto_clear    = true;    /* 关键：DMA 未喂到数据时硬件自动填 0 */

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %d", err);
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
        return;
    }

    /* 标准 I2S (Philips) + 硬件 MONO
     * 默认 slot_mask = I2S_STD_SLOT_LEFT，配合 MAX98357A SD 接 3.3V
     * （>1.4V = Left only mode），每个 mono 样本只写一次即可。 */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPK_BCLK_PIN,
            .ws   = SPK_LRCK_PIN,
            .dout = SPK_DOUT_PIN,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode 失败: %d", err);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
        return;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable 失败: %d", err);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
        return;
    }

    /* 启动播放任务 */
    BaseType_t ret = xTaskCreate(
        spk_tx_task, "spk_tx", SPK_TASK_STACK, NULL, SPK_TASK_PRIO, NULL);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "播放任务创建失败");
        i2s_channel_disable(s_tx_chan);
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
        return;
    }

    ESP_LOGI(TAG, "I2S TX 初始化完成 (MONO, auto_clear) BCLK=%d LRCK=%d DOUT=%d",
             SPK_BCLK_PIN, SPK_LRCK_PIN, SPK_DOUT_PIN);
}

/* ================================================================
 * 推入 PCM：按字节入口，内部按 16bit 样本对齐，跨包保留尾字节
 *
 * 返回值：本次"消费掉"的输入字节数（≤ len_bytes）
 *   - 成功写入 ring 的部分
 *   - 加上 1 字节被保留到下一次的 tail（从调用方视角也算消费了）
 * 若 ring 空间不够，返回 0，调用方重试（当前数据原样保留）
 * ================================================================ */
int speaker_play(const int16_t *pcm, size_t len_bytes)
{
    if (!s_ringbuf || !pcm || len_bytes == 0) return 0;
    if (!s_play_mtx) return 0;

    xSemaphoreTake(s_play_mtx, portMAX_DELAY);

    const uint8_t *bytes = (const uint8_t *)pcm;
    size_t bytes_left = len_bytes;

    /* 计算要写入 ring 的字节数，提前判断空间
     * - 若有 tail_valid：合并出 1 个样本 → 2 字节
     * - 剩余 (bytes_left - 1) 字节中 ((bytes_left-1) & ~1) 字节是完整样本，
     *   按原样处理后写入 ring（每 2 字节 → 1 个 int16 → 2 字节输出）
     * - 最后可能有 1 个奇数字节保留到下次 */
    bool will_consume_tail = (s_pcm_tail_valid && bytes_left >= 1);
    size_t input_after_tail = bytes_left - (will_consume_tail ? 1 : 0);
    size_t full_samples_in  = input_after_tail / 2;  /* 完整样本数（来自输入）*/
    size_t ring_bytes_need  = (will_consume_tail ? 2 : 0) + full_samples_in * 2;

    size_t free_size = xRingbufferGetCurFreeSize(s_ringbuf);
    if (free_size < ring_bytes_need) {
        /* 空间不足：调用方重试，不改状态 */
        xSemaphoreGive(s_play_mtx);
        return 0;
    }

    size_t consumed = 0;
    int16_t proc[PROC_BATCH];
    size_t  proc_n = 0;

    /* 刷 proc 到 ring；空间已提前预留，portMAX_DELAY 会立即成功 */
    #define FLUSH_PROC() do {                                                  \
        if (proc_n > 0) {                                                      \
            xRingbufferSend(s_ringbuf, proc,                                   \
                            proc_n * sizeof(int16_t), portMAX_DELAY);          \
            proc_n = 0;                                                        \
        }                                                                      \
    } while (0)

    /* 1) 合并上次遗留的 tail 字节 */
    if (will_consume_tail) {
        int16_t s = (int16_t)((uint16_t)s_pcm_tail |
                              ((uint16_t)bytes[0] << 8));
        proc[proc_n++] = process_sample(s);
        s_pcm_tail_valid = false;
        bytes      += 1;
        bytes_left -= 1;
        consumed   += 1;
    }

    /* 2) 按 2 字节一个样本处理剩余字节 */
    while (bytes_left >= 2) {
        int16_t s = (int16_t)((uint16_t)bytes[0] |
                              ((uint16_t)bytes[1] << 8));
        proc[proc_n++] = process_sample(s);
        bytes      += 2;
        bytes_left -= 2;
        consumed   += 2;

        if (proc_n >= PROC_BATCH) {
            FLUSH_PROC();
        }
    }

    FLUSH_PROC();

    /* 3) 末尾若剩 1 个奇数字节，保留到下一次 */
    if (bytes_left == 1) {
        s_pcm_tail       = bytes[0];
        s_pcm_tail_valid = true;
        consumed += 1;
    }

    #undef FLUSH_PROC

    xSemaphoreGive(s_play_mtx);
    return (int)consumed;
}

/* ================================================================
 * 丢弃所有待播数据，并复位播放状态
 *
 * 通过 s_flush_request 通知 spk_tx_task 自己清空 ring（唯一 consumer），
 * 等它 signal done 后再复位 HPF/tail/fade。这样严格避免双消费者竞争。
 *
 * 如果 tx 任务出问题没响应，给 500ms 超时兜底，即使没清干净也至少把本地
 * 状态复位，下次 speaker_play 仍是可工作的（ring 里残留数据会继续播，
 * 但那种情况已经是 I2S 严重故障，flush 也救不了）。
 * ================================================================ */
void speaker_flush(void)
{
    if (!s_ringbuf) return;

    /* 第一步：请 tx 任务清空 ring */
    if (s_flush_done) {
        /* 清掉上次可能遗留的未被 take 的 signal（比如多次 flush 串行） */
        xSemaphoreTake(s_flush_done, 0);
        s_flush_request = true;

        /* 等 tx 任务确认已清空 ring。tx 任务每 50ms 轮询一次 receive，
         * 最长 50ms + 一次 i2s_write（<=200ms）就会看到 flush_request。
         * 给 500ms 超时，足够任何情况下响应。 */
        xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(500));
    }

    /* 第二步：复位播放本地状态（HPF/tail/fade）。
     * 此时 ring 已清空，tx 不会再从 ring 取数据，所以这些状态的写入安全。
     * 仍然持 play_mtx，避免和 speaker_play 并发写 HPF 状态。 */
    if (s_play_mtx) xSemaphoreTake(s_play_mtx, portMAX_DELAY);
    s_pcm_tail       = 0;
    s_pcm_tail_valid = false;
    s_hp_x1          = 0.0f;
    s_hp_y1          = 0.0f;
    s_fade_count     = 0;
    if (s_play_mtx) xSemaphoreGive(s_play_mtx);
}
