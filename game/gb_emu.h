#ifndef __GB_EMU_H
#define __GB_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Walnut-CGB 集成层
 *
 * 职责：
 *   - 用已加载到内存的 ROM 数据初始化 Walnut-CGB
 *   - 分配 cart RAM（保存数据）
 *   - 每帧调用 gb_run_frame_dualfetch，把 160x144 画面按行 1.5× 缩放到 LCD
 *   - 轮询矩阵键盘并把按键映射到 GB joypad
 *   - 每帧生成 APU 音频并推给 speaker
 *
 * 不负责：
 *   - 文件读取（rom_loader 做）
 *   - 持久化存档（下一步，目前每次启动状态全新）
 */

/* 启动模拟器，阻塞直到 gb_emu_request_exit() 或致命错误。
 * rom_data：指向已加载的 ROM（调用者负责生命周期，至少在本函数返回前保持有效）
 * rom_size：ROM 字节数
 * 返回 true 表示正常退出，false 表示初始化失败（ROM 不合法等）*/
bool gb_emu_run(const uint8_t *rom_data, size_t rom_size);

/* gb_emu_request_exit：请求模拟器主循环在下一帧边界退出。
 *
 * 可从另一任务调用；不要在真正的硬中断里调用。 */
void gb_emu_request_exit(void);

#endif
