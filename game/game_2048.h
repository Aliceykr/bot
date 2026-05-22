#ifndef __GAME_2048_H
#define __GAME_2048_H

#include <stdbool.h>
#include <stdint.h>

/* 运行 2048 游戏主循环。
 * 阻塞到用户长按矩阵键盘中间键退出后才返回。
 * 调用方已经 suspend 了 LVGL，本函数独占 LCD 并驱动矩阵键盘。
 *
 * 控制（复用 Game Boy 的矩阵键盘映射，游戏模式下生效）：
 *   R0C1 (UP)    → 向上移动
 *   R2C1 (DOWN)  → 向下移动
 *   R1C0 (LEFT)  → 向左移动
 *   R1C2 (RIGHT) → 向右移动
 *   R1C1 长按 800ms → 退出游戏（由 keypad_consume_exit_request 触发）
 *
 * 后续阶段会把 MPU6050 倾斜映射叠加到同样的方向输入。
 * 当前只实现显示 + 逻辑 + 按键控制。 */
void game_2048_run(void);

/* game_2048_request_exit：请求 2048 主循环尽快退出。
 *
 * 可从另一个任务调用；主循环每帧检查该标志。 */
void game_2048_request_exit(void);

#endif
