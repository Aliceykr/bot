#ifndef LV_PORT_INDEV_H
#define LV_PORT_INDEV_H

#include <stdbool.h>
#include "lvgl.h"

void lv_port_indev_init(void);

/* 菜单/游戏模式开关
 *   true  = 菜单模式：编码器旋转/按键正常上报给 LVGL
 *   false = 游戏模式：编码器输入被吃掉（不影响 LVGL group），PCNT 累积被清空
 * 默认 true，在进入游戏前置 false，退出后恢复 true。*/
void lv_port_indev_set_menu_mode(bool enable);

#endif
