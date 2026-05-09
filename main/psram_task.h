#ifndef __PSRAM_TASK_H
#define __PSRAM_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/*
 * 类似 xTaskCreate，但栈从 PSRAM 分配。TCB 仍在内部 DRAM。
 *
 * 用法：用户函数通过 return 正常结束，不要调 vTaskDelete(NULL)。
 * 入口包装器会自动处理：
 *   - 投递 cleanup 信息给 cleaner 任务
 *   - 调用 vTaskDelete(NULL) 销毁自己
 *   - cleaner 稍后释放 PSRAM 栈和 TCB
 *
 * 适用场景：一次性 HTTPS/cJSON 任务，栈 >= 8KB，运行一次即结束。
 */
BaseType_t xTaskCreatePSRAM(TaskFunction_t func, const char *name,
                             uint32_t stack_bytes, void *arg,
                             UBaseType_t prio, TaskHandle_t *handle);

/* 兼容旧 API：新代码不需要调，直接 return 即可。留着避免编译出错。 */
void psram_task_exit(void);

/* 首次使用前调一次（app_main 里）。幂等。 */
void psram_task_init(void);

#endif
