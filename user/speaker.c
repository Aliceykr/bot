#include "speaker.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

#define TAG             "SPEAKER"

/* 播放 RingBuffer 大小：64 KB ≈ 2 秒缓冲（16kHz 16bit mono）*/
#define SPK_RINGBUF_SIZE    (64 * 1024)

/* 每次从 RingBuffer 取出并写入 I2S 的最大字节数 */
#define SPK_TX_CHUNK        512

/* 播放任务栈：4KB。之前 2KB 在高采样率 + stereo 展开时会溢出
 * （stereo_buf 512 × 2 字节 + I2S 写入调用栈 + ESP_LOG 栈）*/
#define SPK_TASK_STACK      4096
#define SPK_TASK_PRIO       3

/* ================================================================
 * 模块内部状态（全部 static，不暴露到外部）
 * ================================================================ */
static i2s_chan_handle_t  s_tx_chan   = NULL;
static RingbufHandle_t    s_ringbuf  = NULL;

/* ================================================================
 * 播放任务：从 RingBuffer 取数据，通过 I2S DMA 输出到 MAX98357A
 *
 * i2s_channel_write 底层使用 DMA 传输，CPU 仅需将数据复制到
 * DMA buffer，实际搬运由 DMA 控制器完成，不占用 CPU 忙等。
 *
 * 噪声消除策略：
 *   - RingBuffer 空 → 每 20ms 主动写一小段静音，保持 DMA 输出稳定零电平
 *     防止 MAX98357A 在长时间无数据时出现 DC 漂移/爆破
 *   - 从"静默 → 有音频"切换时，前 32 样本做线性淡入，避免幅度突变爆音
 * ================================================================ */
static void spk_tx_task(void *arg)
{
    (void)arg;
    /* 判断当前是否处于"刚从静音恢复"状态：
     * true = 上一轮没收到数据（在播静音），下次拿到数据要做淡入 */
    bool need_fade_in = true;
    /* 静音 fill buffer：16 样本 stereo = 64 字节，够维持 ring 不饿死 */
    static const int16_t silence_pad[32] = { 0 };  /* 16 L+R 对 */

    for (;;) {
        size_t recv_size = 0;
        void *item = xRingbufferReceiveUpTo(
            s_ringbuf, &recv_size, pdMS_TO_TICKS(20), SPK_TX_CHUNK);

        if (item == NULL) {
            /* RingBuffer 空：灌一小段静音保持 DMA 稳定。
             * 下次真正收到数据时会走 fade-in 分支。*/
            size_t w = 0;
            i2s_channel_write(s_tx_chan, silence_pad, sizeof(silence_pad),
                              &w, pdMS_TO_TICKS(50));
            need_fade_in = true;
            continue;
        }

        /* mono PCM → stereo：每个 int16 样本复制为左右各一份，
         * MAX98357A L/R 接 GND 取左声道。 */
        int16_t *src = (int16_t *)item;
        size_t n_samples = recv_size / sizeof(int16_t);
        /* stereo_buf 需容纳 n_samples * 2 个 int16 */
        static int16_t stereo_buf[SPK_TX_CHUNK];
        _Static_assert(sizeof(stereo_buf) >= (size_t)SPK_TX_CHUNK * 2,
                       "stereo_buf too small for SPK_TX_CHUNK worth of stereo samples");
        for (size_t i = 0; i < n_samples; i++) {
            stereo_buf[i * 2]     = src[i];  /* L */
            stereo_buf[i * 2 + 1] = src[i];  /* R */
        }

        /* 从静音 → 有音频的瞬间，前 32 样本做线性淡入，避免爆音。
         * 典型场景：语音助手回答 / 游戏 APU 开场 / TTS 播报首帧。
         * 32 样本 @16kHz = 2ms，人耳几乎察觉不到但足以消除爆破声。 */
        if (need_fade_in) {
            size_t fade_samples = n_samples < 32 ? n_samples : 32;
            for (size_t i = 0; i < fade_samples; i++) {
                int32_t gain = (int32_t)(i + 1);  /* 1..fade_samples */
                stereo_buf[i * 2]     = (int16_t)((int32_t)stereo_buf[i * 2]     * gain / (int32_t)fade_samples);
                stereo_buf[i * 2 + 1] = (int16_t)((int32_t)stereo_buf[i * 2 + 1] * gain / (int32_t)fade_samples);
            }
            need_fade_in = false;
        }

        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(
            s_tx_chan, stereo_buf, n_samples * 2 * sizeof(int16_t),
            &bytes_written, pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_write 失败: %d", err);
        }

        /* 无论写入是否成功，必须归还 item，防止 RingBuffer 泄漏 */
        vRingbufferReturnItem(s_ringbuf, item);
    }
}

/* ================================================================
 * 公开接口
 * ================================================================ */

void speaker_init(void)
{
    /* --- 创建播放 RingBuffer（字节模式，内部 SRAM）--- */
    s_ringbuf = xRingbufferCreate(SPK_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_ringbuf) {
        ESP_LOGE(TAG, "RingBuffer 创建失败");
        return;
    }

    /* --- 初始化 I2S_NUM_1 TX 通道 --- */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);  /* TX only，rx=NULL */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel 失败: %d", err);
        vRingbufferDelete(s_ringbuf);
        s_ringbuf = NULL;
        return;
    }

    /* --- 配置标准 I2S（Philips）模式 ---
     * STEREO 模式：左右声道填入相同的 mono 样本。
     * MAX98357A 的 L/R 引脚决定取哪个声道（接 GND 取左声道）。
     * 16bit 位宽匹配 PC 端发来的 int16_t PCM 格式。 */
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
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

    /* === 开机静音预热：消除首播噪音 ===
     * 问题根因：I2S enable 后 DMA buffer 里可能是未初始化的残留数据，
     * 首次有真实音频到达前这些残留会被播出去 → 前半秒爆破/噪声。
     * 修复：主动灌入 200ms 静音（0 值 PCM），强制 DMA 清零所有 ring 中的 buffer，
     * MAX98357A 在稳定的零信号上锁定 DC offset，后续播放就不会炸响。
     * 200ms 对应 16kHz stereo 16bit = 12800 字节，分批写入避免栈上大 buffer。 */
    static const int16_t silence[512] = { 0 };  /* 512 个 int16 = 1024 字节一次 */
    const int silence_rounds = 16000 / 512 / 5;  /* ~200ms @ 16kHz stereo */
    for (int i = 0; i < silence_rounds; i++) {
        size_t w = 0;
        i2s_channel_write(s_tx_chan, silence, sizeof(silence), &w, pdMS_TO_TICKS(100));
    }

    /* --- 启动播放任务 --- */
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

    ESP_LOGI(TAG, "I2S TX 初始化完成 BCLK=%d LRCK=%d DOUT=%d",
             SPK_BCLK_PIN, SPK_LRCK_PIN, SPK_DOUT_PIN);
}

int speaker_play(const int16_t *pcm, size_t len_bytes)
{
    if (!s_ringbuf || !pcm || len_bytes == 0) return 0;

    /* timeout=0：非阻塞，缓冲满时静默丢弃，保证调用方实时性 */
    BaseType_t ret = xRingbufferSend(s_ringbuf, pcm, len_bytes, 0);
    return (ret == pdTRUE) ? (int)len_bytes : 0;
}

/* 丢弃 ring buffer 所有待播数据。
 *
 * 用途：游戏退出、界面切换时立即静音，防止残留音频继续播 1-2 秒。
 * 不动 I2S 通道，也不停任务：spk_tx_task 看到 ring 空后会自动走
 * 静音填充分支（每 20ms 写一小段 0），保证 MAX98357A 输出稳定零电平。
 *
 * 实现：循环 receive + return 把所有 item 消费掉。byte mode ringbuf 的
 * xRingbufferReceive 是非阻塞取一个 item；用 timeout=0 空了立刻返回 NULL。 */
void speaker_flush(void)
{
    if (!s_ringbuf) return;
    size_t n = 0;
    while (1) {
        void *item = xRingbufferReceive(s_ringbuf, &n, 0);
        if (!item) break;
        vRingbufferReturnItem(s_ringbuf, item);
    }
}

/* ================================================================
 * A2DP 蓝牙音箱：让出/恢复 I2S_NUM_1（保留，未来可用）
 * ================================================================ */
