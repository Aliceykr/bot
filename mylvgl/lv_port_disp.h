#ifndef __LV_PORT_DISP_H
#define __LV_PORT_DISP_H

#include "lvgl.h"

/* lv_port_disp_init：创建 LVGL display，并绑定 LCD SPI flush 回调。
 *
 * 使用 PARTIAL 双缓冲 + SPI DMA；必须在 LCD_Init() 和 lv_init() 之后调用。 */
void lv_port_disp_init(void);

/* lv_port_disp_suspend：暂停 LVGL 的 SPI 输出（flush_cb 立即返回），并等待所有进行中的 DMA 完成。
 * 之后 SPI 总线空闲，其他模块（如游戏 runtime）可独占访问 LCD。 */
void lv_port_disp_suspend(void);

/* lv_port_disp_resume：恢复 LVGL 输出。调用后一般需要 lv_obj_invalidate(lv_screen_active())
 * 强制一次整屏重绘，覆盖游戏期间画面。 */
void lv_port_disp_resume(void);

/* lv_port_disp_is_suspended：查询当前是否处于暂停状态。
 * 暂停期间 lvgl_task 应跳过 lv_timer_handler，避免 LVGL 继续抢 SPI/CPU。 */
bool lv_port_disp_is_suspended(void);

#endif
