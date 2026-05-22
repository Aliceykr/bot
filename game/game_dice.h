#ifndef __GAME_DICE_H
#define __GAME_DICE_H

/* 运行摇骰子本地游戏。
 * 调用方已经 suspend 了 LVGL，本函数独占 LCD 和游戏键盘输入。
 * 摇动 MPU6050 会触发掷骰动画，中间键长按退出。 */
void game_dice_run(void);

/* 请求退出摇骰子游戏（可从另一个任务调用）。 */
void game_dice_request_exit(void);

#endif /* __GAME_DICE_H */
