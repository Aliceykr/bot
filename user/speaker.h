#ifndef __SPEAKER_H
#define __SPEAKER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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

/**
 * @brief 动态切换 I2S 输出采样率（音乐播放需要 44.1kHz 等非默认速率）
 *
 * 切换前会 flush 当前 ring buffer，避免残留采样以新速率播出来变音调。
 * 合法范围 8000..48000 Hz。调用成功后 speaker_play 送入的 PCM 将按新速率输出。
 * 播放完后请调回 SPK_SAMPLE_RATE（16000），否则 TTS / GB 音频会变调。
 *
 * @param hz 新的采样率（Hz）
 * @return true 切换成功；false 参数非法或驱动失败
 */
bool speaker_set_sample_rate(uint32_t hz);

/**
 * @brief 查询当前 I2S 输出采样率（Hz）
 */
uint32_t speaker_get_sample_rate(void);

/**
 * @brief 设置播放音量（0..100 百分比）
 *
 * 内部按对数曲线换算成线性增益：100% → 1.0 倍，0% → 静音（-60dB 以下截为 0）。
 * 所有声源（TTS / 音乐 / GB / 提示音）都受这个全局音量影响。
 * 设置后立即生效；新值保存到 NVS，重启恢复。
 *
 * @param percent 0..100；越界会被钳制
 */
void speaker_set_volume(uint8_t percent);

/**
 * @brief 查询当前音量百分比（0..100）
 */
uint8_t speaker_get_volume(void);

/**
 * @brief 丢弃 RingBuffer 中所有待播 PCM，立即静音
 *
 * 场景：退出游戏 / 切换界面 / 用户取消等。
 * 不调用此函数时，模式切换后残留的旧音频还会播出 1-2 秒（ring 里 64KB 缓冲）。
 * 本函数不停止 I2S 通道，spk_tx_task 清空后会自然进入"静音填充"状态。
 */
void speaker_flush(void);

#endif /* __SPEAKER_H */
