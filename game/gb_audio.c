#include "gb_audio.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "speaker.h"

/* 使用 MiniGB APU。采样率与 speaker 的 SPK_SAMPLE_RATE(16000) 一致，
 * 避免重采样。stereo → mono 软合并。 */
#define AUDIO_SAMPLE_RATE 16000
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#include "minigb_apu.h"

#define TAG "GB_AUDIO"

/* ================================================================
 * APU 合成搬到独立任务跑 Core 0（主仿真跑 Core 1）
 *
 * Walnut-CGB 官方明确 APU 设计为独立线程运行，主线程仅负责寄存器
 * 写入，合成出 PCM 不占主帧预算。这样每帧能还给主线程 1~2 ms，
 * 让它把仿真 + 渲染稳定塞进 16.7 ms 内，FPS 稳住 60。
 *
 * 代价：audio_read/audio_write 与 audio_callback 存在 race，但：
 *   - 寄存器都是 uint8_t，原子读写
 *   - 视觉无关（只影响声音某一帧的微小瑕疵）
 *   - 官方参考实现也这么做，没有锁
 * ================================================================ */
static struct minigb_apu_ctx s_apu;

/* 每帧 ~268 对 stereo 样本。
 * minigb_apu.h 里 AUDIO_SAMPLES 用了 double 表达式 (AUDIO_SAMPLE_RATE / VERTICAL_SYNC)，
 * 不是整数常量表达式，导致在文件作用域定义数组时 gcc 报
 * "variably modified at file scope" 警告。这里直接用编译期整数常量：
 *   16000 Hz / 59.73 Hz ≈ 267.9 → 向上取 268 保证每帧 PCM 缓冲不溢出。
 * 运行时 minigb_apu_audio_callback 会填入 AUDIO_SAMPLES_TOTAL 个样本，
 * 实测 AUDIO_SAMPLES=268，这里给同样的 268 即可。 */
#define GB_AUDIO_SAMPLES_PER_FRAME       268
#define GB_AUDIO_SAMPLES_PER_FRAME_TOTAL (GB_AUDIO_SAMPLES_PER_FRAME * 2)

static audio_sample_t s_pcm_buf[GB_AUDIO_SAMPLES_PER_FRAME_TOTAL];
/* Mono 输出缓冲 */
static int16_t s_mono_buf[GB_AUDIO_SAMPLES_PER_FRAME];

/* 主线程向 APU 任务发信号：每帧仿真完生成一次。
 * 用计数信号量防止主线程在 APU 任务还没跑完时又发一次就丢失。
 * 但如果 APU 任务连续两帧跑不完，该帧音频会被跳过（合理）。 */
static SemaphoreHandle_t s_frame_signal = NULL;
static TaskHandle_t      s_apu_task     = NULL;

static void apu_task(void *arg)
{
    (void)arg;
    for (;;) {
        /* 等主线程的 "该出一帧音频" 信号，最长等 30ms，
         * 超时也不做事（防止 APU 任务孤儿死循环）*/
        if (xSemaphoreTake(s_frame_signal, pdMS_TO_TICKS(30)) != pdTRUE) {
            continue;
        }

        /* 合成一帧 stereo PCM。
         * minigb 每次填充 AUDIO_SAMPLES_TOTAL（约 536）个 int16，
         * 与 GB_AUDIO_SAMPLES_PER_FRAME_TOTAL 一致 */
        minigb_apu_audio_callback(&s_apu, s_pcm_buf);

        /* Stereo → mono：取平均后饱和截断。 */
        for (unsigned i = 0; i < GB_AUDIO_SAMPLES_PER_FRAME; i++) {
            int32_t l = s_pcm_buf[i * 2];
            int32_t r = s_pcm_buf[i * 2 + 1];
            int32_t m = (l + r) / 2;
            if (m >  32767) m =  32767;
            if (m < -32768) m = -32768;
            s_mono_buf[i] = (int16_t)m;
        }

        /* 推 speaker，非阻塞满则丢弃 */
        speaker_play(s_mono_buf, GB_AUDIO_SAMPLES_PER_FRAME * sizeof(int16_t));
    }
}

void gb_audio_init(void)
{
    minigb_apu_audio_init(&s_apu);

    if (!s_frame_signal) {
        /* 计数信号量，容量 2：最多允许一帧积压，再多就丢帧。
         * 这样 APU 任务偶尔慢一帧，下一帧主线程 give 不会阻塞。*/
        s_frame_signal = xSemaphoreCreateCounting(2, 0);
    }
    if (!s_apu_task) {
        /* Core 0：主仿真在 Core 1，APU 走另一个核真正并行。
         * 栈 4KB：minigb_apu_audio_callback 内部开 ~1KB 局部表，
         * 加上 snprintf/ESP_LOG 备用，4KB 稳。
         * 优先级 5：高于普通任务，低于主游戏任务（10），
         * 保证 APU 任务能及时醒但不会抢到主线程。 */
        xTaskCreatePinnedToCore(apu_task, "apu_task", 4096, NULL, 5,
                                &s_apu_task, 0);
    }
}

void gb_audio_deinit(void)
{
    /* 退出游戏时音频清理顺序严格：
     *   1. 删 APU 任务：停止往 speaker 推新 PCM
     *   2. 删信号量：防止悬空使用
     *   3. speaker_flush：把 ring buffer 里积的最多 2 秒游戏音频丢掉
     *      否则 spk_tx_task 会继续播这些残留数据，退出后还能听见最后
     *      一两秒的游戏声音，体验非常突兀。
     *
     * 任务删除是同步的，vTaskDelete 后立即从调度器摘除。 */
    if (s_apu_task) {
        vTaskDelete(s_apu_task);
        s_apu_task = NULL;
    }
    if (s_frame_signal) {
        vSemaphoreDelete(s_frame_signal);
        s_frame_signal = NULL;
    }

    /* 清空 speaker ring buffer，立刻静音。 */
    speaker_flush();
}

/* 主线程调：通知 APU 任务"该合成一帧了"。几乎零开销。 */
IRAM_ATTR void gb_audio_emit_frame(void)
{
    if (s_frame_signal) {
        /* 非阻塞 give，信号量满则自然丢弃。
         * ISR 版本不需要，此处在任务上下文 */
        xSemaphoreGive(s_frame_signal);
    }
}

/* ================================================================
 * Walnut-CGB 全局符号（被主线程高频调用）
 * ================================================================ */
IRAM_ATTR uint8_t audio_read(const uint16_t addr)
{
    return minigb_apu_audio_read(&s_apu, addr);
}

IRAM_ATTR void audio_write(const uint16_t addr, const uint8_t val)
{
    minigb_apu_audio_write(&s_apu, addr, val);
}
