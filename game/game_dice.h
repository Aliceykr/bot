#ifndef __GAME_DICE_H
#define __GAME_DICE_H

/* 运行摇骰子本地游戏。
 * 调用方已经 suspend 了 LVGL，本函数独占 LCD 和游戏键盘输入。
 * 摇动 MPU6050 会触发掷骰动画，中间键长按退出。 */
void game_dice_run(void);

/* game_dice_request_exit：请求摇骰子主循环尽快退出。
 *
 * 可从另一个任务调用；主循环等待/动画中会周期检查退出标志。 */
void game_dice_request_exit(void);

#endif /* __GAME_DICE_H */
