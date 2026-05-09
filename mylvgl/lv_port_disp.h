#ifndef __LV_PORT_DISP_H
#define __LV_PORT_DISP_H

#include "lvgl.h"

void lv_port_disp_init(void);

/* 暂停 LVGL 的 SPI 输出（fluish_cb 立即返回），并等待所有进行中的 DMA 完成。
 * 之后 SPI 总线空闲，其他模块（如游戏 runtime）可独占访问 LCD。 */
void lv_port_disp_suspend(void);

/* 恢复 LVGL 输出。调用后一般需要 lv_obj_invalidate(lv_screen_active())
 * 强制一次整屏重绘，覆盖游戏期间画面。 */
void lv_port_disp_resume(void);

#endif
