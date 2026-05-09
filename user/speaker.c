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

/* 播放任务栈大小与优先级（与 usb_tx_task 同优先级，低于 LVGL 任务）*/
#define SPK_TASK_STACK      2048
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
 * ================================================================ */
static void spk_tx_task(void *arg)
{
    (void)arg;
    for (;;) {
        size_t recv_size = 0;
        void *item = xRingbufferReceiveUpTo(
            s_ringbuf, &recv_size, pdMS_TO_TICKS(20), SPK_TX_CHUNK);

        if (item == NULL) {
            /* RingBuffer 暂无数据，让出 CPU */
            taskYIELD();
            continue;
        }

        /* mono PCM → stereo：每个 int16 样本复制为左右各一份，
         * MAX98357A L/R 接 GND 取左声道。 */
        int16_t *src = (int16_t *)item;
        size_t n_samples = recv_size / sizeof(int16_t);
        /* 调试：每100次打印一次样本值，确认数据正常 */
        static int dbg_cnt = 0;
        if (++dbg_cnt >= 100) {
            dbg_cnt = 0;
            ESP_LOGI(TAG, "pcm[0]=%d pcm[1]=%d n=%d", src[0], src[1], (int)n_samples);
        }
        /* stereo_buf 需容纳 n_samples * 2 个 int16。
         * 单次从 RingBuffer 取最多 SPK_TX_CHUNK 字节（=SPK_TX_CHUNK/2 个 mono 样本），
         * 展开为 stereo 后 = SPK_TX_CHUNK 个 int16 = SPK_TX_CHUNK*2 字节。
         * 静态断言防止将来修改 SPK_TX_CHUNK 时发生栈溢出 */
        static int16_t stereo_buf[SPK_TX_CHUNK];
        _Static_assert(sizeof(stereo_buf) >= (size_t)SPK_TX_CHUNK * 2,
                       "stereo_buf too small for SPK_TX_CHUNK worth of stereo samples");
        for (size_t i = 0; i < n_samples; i++) {
            stereo_buf[i * 2]     = src[i];  /* L */
            stereo_buf[i * 2 + 1] = src[i];  /* R */
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
