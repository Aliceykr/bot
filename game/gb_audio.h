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

/* 销毁 APU 任务 + 信号量，释放相关 RAM。退出游戏时调一次。 */
void gb_audio_deinit(void);

/* 通知 APU 任务合成一帧 PCM 并推入 speaker RingBuffer。
 * 在 gb_emu 主循环每帧末尾调用。实际合成由 Core0 的 apu_task 执行。 */
void gb_audio_emit_frame(void);

/* audio_read：供 Walnut-CGB 读取 Game Boy APU 寄存器。
 *
 * 由模拟器核心按地址调用，不应由业务层直接调用。 */
uint8_t audio_read(const uint16_t addr);

/* audio_write：供 Walnut-CGB 写入 Game Boy APU 寄存器。
 *
 * 由模拟器核心按地址调用，不应由业务层直接调用。 */
void    audio_write(const uint16_t addr, const uint8_t val);

#endif
