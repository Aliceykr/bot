#ifndef __KEYPAD_H
#define __KEYPAD_H

#include <stdint.h>
#include <stdbool.h>

/* ================================================================
 * 3x3 矩阵键盘驱动（独立扫描任务 + 去抖）
 *
 * 硬件接线：
 *   行 ROW0 -> GPIO 1   （输出，逐行拉低扫描）
 *   行 ROW1 -> GPIO 2
 *   行 ROW2 -> GPIO 14
 *   列 COL0 -> GPIO 41  （输入 + 内部上拉）
 *   列 COL1 -> GPIO 42
 *   列 COL2 -> GPIO 47
 *
 * 按键布局（给 GB 模拟器用）：
 *                 C0         C1         C2
 *   R0         [  B  ]    [ UP ]    [  A  ]
 *   R1         [LEFT ]    [ -- ]    [RIGHT]
 *   R2         [SELECT]   [DOWN]    [START]
 *
 * 位图 keypad_bits_t：9 个按键，低位开始按行优先排列，
 *   bit0=R0C0  bit1=R0C1  bit2=R0C2
 *   bit3=R1C0  bit4=R1C1  bit5=R1C2
 *   bit6=R2C0  bit7=R2C1  bit8=R2C2
 * 按下为 1，抬起为 0。
 *
 * 扫描频率：2 ms 一轮（3 行），延迟 <6 ms，足够游戏输入。
 * 去抖：连续两轮读数一致才认。
 * ================================================================ */

#define KEYPAD_BIT_R0C0   (1U << 0)   /* B      */
#define KEYPAD_BIT_R0C1   (1U << 1)   /* UP     */
#define KEYPAD_BIT_R0C2   (1U << 2)   /* A      */
#define KEYPAD_BIT_R1C0   (1U << 3)   /* LEFT   */
#define KEYPAD_BIT_R1C1   (1U << 4)   /* 预留   */
#define KEYPAD_BIT_R1C2   (1U << 5)   /* RIGHT  */
#define KEYPAD_BIT_R2C0   (1U << 6)   /* SELECT */
#define KEYPAD_BIT_R2C1   (1U << 7)   /* DOWN   */
#define KEYPAD_BIT_R2C2   (1U << 8)   /* START  */

/**
 * @brief 初始化矩阵键盘 GPIO + 启动扫描任务
 *        app_main 中调用一次。重复调用不会重建任务。
 */
void keypad_init(void);

/**
 * @brief 获取当前去抖后的按键位图
 * @return 9 位的按键状态，按下为 1
 */
uint16_t keypad_get_bits(void);

#endif /* __KEYPAD_H */
