#ifndef MY_DEMO_H
#define MY_DEMO_H

#include "lvgl.h"

/* my_demo：创建主菜单 UI，并初始化菜单级队列/定时器。
 *
 * 只能在 LVGL 任务中调用一次；后续所有界面切换和 LVGL 对象更新都应回到
 * 同一个 LVGL 线程执行。 */
void my_demo(void);

/* my_demo_get_group：返回主菜单编码器 group。
 *
 * main/lvgl_task 初始化输入设备时使用；其他临时弹窗或页面关闭后也用它恢复
 * 编码器焦点到主菜单。 */
lv_group_t *my_demo_get_group(void);

#endif
