#ifndef __GAME_RUNTIME_H
#define __GAME_RUNTIME_H

#include <stdbool.h>

/* 启动模拟器运行一个 ROM。
 * 当前为占位实现：仅显示运行状态，等待返回信号。
 * 后续阶段会加入 Peanut-GB 核心，真正执行游戏。
 *
 * 这个函数阻塞调用者直到玩家请求退出。
 * 调用方负责准备好屏幕资源（已从 LVGL 接管），本函数不访问 LVGL。 */
void game_runtime_run(const char *rom_name);

/* 请求 game_runtime_run 退出（可从另一个任务调用，例如响应退出按键）*/
void game_runtime_request_exit(void);

#endif
