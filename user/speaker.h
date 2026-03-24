#ifndef __SPEAKER_H
#define __SPEAKER_H

#include <stdint.h>
#include <stddef.h>

/* ================================================================
 * MAX98357A I2S 音频输出驱动
 *
 * 硬件连接：
 *   BCLK  → GPIO15
 *   LRC   → GPIO16
 *   DIN   → GPIO17
 *   SD    → 3.3V（常开）
 *   GAIN  → 悬空（默认 9 dB）
 *
 * 使用 I2S_NUM_1（I2S_NUM_0 已被 INMP441 麦克风占用）
 * ================================================================ */

/* 播放采样率，须与 PC 端脚本保持一致 */
#define SPK_SAMPLE_RATE   16000

/* I2S 引脚 */
#define SPK_BCLK_PIN      15
#define SPK_LRCK_PIN      16
#define SPK_DOUT_PIN      17

/**
 * @brief 初始化 MAX98357A I2S TX 驱动及播放任务
 *        在 app_main 中调用一次，早于任何 speaker_play 调用
 */
void speaker_init(void);

/**
 * @brief 非阻塞地将 PCM 数据推入播放缓冲
 *
 * @param pcm       16-bit 有符号 PCM 样本数组（mono）
 * @param len_bytes 字节数（样本数 × 2）
 * @return          实际写入字节数；缓冲满时返回 0（数据被丢弃）
 */
int speaker_play(const int16_t *pcm, size_t len_bytes);

#endif /* __SPEAKER_H */
