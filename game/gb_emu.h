#ifndef __GB_EMU_H
#define __GB_EMU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Peanut-GB 集成层
 *
 * 职责：
 *   - 用已加载到内存的 ROM 数据初始化 Peanut-GB
 *   - 分配 cart RAM（保存数据）
 *   - 每帧调用 gb_run_frame，把 160x144 画面按行 1.5× 缩放到 LCD
 *   - 轮询退出信号
 *
 * 不负责：
 *   - 文件读取（rom_loader 做）
 *   - 按键输入（gb_input 做，本步占位为全松开）
 *   - 音频（下一步）
 *   - 持久化存档（下一步，目前每次启动状态全新）
 */

/* 启动模拟器，阻塞直到 gb_emu_request_exit() 或致命错误。
 * rom_data：指向已加载的 ROM（调用者负责生命周期，至少在本函数返回前保持有效）
 * rom_size：ROM 字节数
 * 返回 true 表示正常退出，false 表示初始化失败（ROM 不合法等）*/
bool gb_emu_run(const uint8_t *rom_data, size_t rom_size);

/* 请求模拟器退出。可从另一任务/中断上下文调用 */
void gb_emu_request_exit(void);

#endif
