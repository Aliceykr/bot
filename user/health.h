#ifndef __HEALTH_H
#define __HEALTH_H

/* 启动系统健康监控任务：周期打印堆水位，便于发现泄漏。
 * 在 app_main 中调用一次即可。 */
void health_monitor_start(void);

#endif
