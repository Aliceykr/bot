#ifndef __PSRAM_TASK_H
#define __PSRAM_TASK_H

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* xTaskCreatePSRAM：创建一个任务栈位于 PSRAM 的一次性 FreeRTOS 任务。
 *
 * TCB 仍在内部 DRAM。用户函数必须通过 return 正常结束，不要调 vTaskDelete(NULL)。
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

/* xTaskCreatePSRAMPinnedToCore：xTaskCreatePSRAM 的绑核版本。
 *
 * core_id 可以是 0、1 或 tskNO_AFFINITY。适合需要避开 WiFi/BT 系统任务的
 * 重 CPU 后台任务。 */
BaseType_t xTaskCreatePSRAMPinnedToCore(TaskFunction_t func, const char *name,
                                        uint32_t stack_bytes, void *arg,
                                        UBaseType_t prio,
                                        TaskHandle_t *handle,
                                        BaseType_t core_id);

/* psram_task_exit：兼容旧 API 的空实现。
 *
 * 新代码不需要调用，直接 return 即可。保留它只是避免旧调用点编译出错。 */
void psram_task_exit(void);

/* psram_task_init：创建 PSRAM 任务 cleaner 队列和 cleaner 任务。
 *
 * app_main 里提前调用一次；xTaskCreatePSRAM 也会兜底懒初始化。幂等。 */
void psram_task_init(void);

#endif
