#include "gb_audio.h"
#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "speaker.h"

/* 使用 MiniGB APU。采样率与 speaker 的 SPK_SAMPLE_RATE(16000) 一致，
 * 避免重采样。stereo → mono 软合并。 */
#define AUDIO_SAMPLE_RATE 16000
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#include "minigb_apu.h"

#define TAG "GB_AUDIO"

/* APU 上下文。独占放在模块内，Walnut 全局回调访问。*/
static struct minigb_apu_ctx s_apu;

/* 每帧（一个 VSYNC 周期，~16.74ms）产生的样本数
 * = AUDIO_SAMPLE_RATE / VERTICAL_SYNC = 16000 / 59.73 ≈ 268 个 stereo 对
 * AUDIO_SAMPLES_TOTAL = AUDIO_SAMPLES * 2（stereo 交错） */
static audio_sample_t s_pcm_buf[AUDIO_SAMPLES_TOTAL];
/* Mono 输出缓冲：AUDIO_SAMPLES 个 */
static int16_t s_mono_buf[AUDIO_SAMPLES];

void gb_audio_init(void)
{
    minigb_apu_audio_init(&s_apu);
}

IRAM_ATTR void gb_audio_emit_frame(void)
{
    /* 让 APU 合成一帧 PCM（stereo 交错）*/
    minigb_apu_audio_callback(&s_apu, s_pcm_buf);

    /* Stereo 混合为 mono：(L + R) / 2，饱和截断防溢 */
    for (unsigned i = 0; i < AUDIO_SAMPLES; i++) {
        int32_t l = s_pcm_buf[i * 2];
        int32_t r = s_pcm_buf[i * 2 + 1];
        int32_t m = (l + r) / 2;
        if (m >  32767) m =  32767;
        if (m < -32768) m = -32768;
        s_mono_buf[i] = (int16_t)m;
    }

    /* 推入 speaker RingBuffer（非阻塞；满时丢帧保证仿真帧率不受影响）*/
    speaker_play(s_mono_buf, AUDIO_SAMPLES * sizeof(int16_t));
}

/* ================================================================
 * Walnut-CGB 全局符号
 * ================================================================ */
IRAM_ATTR uint8_t audio_read(const uint16_t addr)
{
    return minigb_apu_audio_read(&s_apu, addr);
}

IRAM_ATTR void audio_write(const uint16_t addr, const uint8_t val)
{
    minigb_apu_audio_write(&s_apu, addr, val);
}
