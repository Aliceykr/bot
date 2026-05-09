#ifndef __GB_AUDIO_H
#define __GB_AUDIO_H

#include <stdint.h>

/* GB 音频集成：
 *   - MiniGB APU 生成 16bit stereo PCM
 *   - 合并 L/R 为 mono 推进 speaker RingBuffer（16kHz mono）
 *
 * Walnut 需要全局 audio_read/audio_write；在 gb_audio.c 里实现。 */

/* 初始化：重置 APU 状态。进入游戏时调一次 */
void gb_audio_init(void);

/* 生成一帧（~59.7Hz 周期）的 PCM 并推入 speaker RingBuffer。
 * 在 gb_emu 主循环每帧末尾调用。 */
void gb_audio_emit_frame(void);

/* Walnut 从这些全局符号调用（在 peanut_gb.h 和 walnut_cgb.h 中引用）*/
uint8_t audio_read(const uint16_t addr);
void    audio_write(const uint16_t addr, const uint8_t val);

#endif
