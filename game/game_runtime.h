#ifndef __GAME_RUNTIME_H
#define __GAME_RUNTIME_H

#include <stdbool.h>

/* game_runtime_run：运行一个游戏条目直到退出。
 *
 * rom_name 为内置游戏特殊名时直接进入对应实现；否则从 SD 卡加载 GB/GBC ROM，
 * 再启动 Walnut-CGB 模拟器。这个函数阻塞调用者直到玩家请求退出。
 * 调用方负责先暂停 LVGL 显示并接管 LCD/SPI，本函数不访问 LVGL 对象。 */
void game_runtime_run(const char *rom_name);

/* game_runtime_request_exit：请求当前运行中的内置游戏或 GB 模拟器退出。
 *
 * 可从另一个任务调用，例如 UI 取消、矩阵键盘退出事件或系统切换模式。 */
void game_runtime_request_exit(void);

#endif
