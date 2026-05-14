#include "psram_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <string.h>

#define TAG "PSRAM_TASK"

typedef struct {
    TaskHandle_t  handle;
    StaticTask_t *tcb;
    StackType_t  *stack;
} cleanup_item_t;

static QueueHandle_t s_cleanup_queue = NULL;

/* Cleaner：等目标任务彻底被 scheduler 销毁后释放栈/TCB */
static void cleaner_task(void *arg)
{
    (void)arg;
    cleanup_item_t item;
    while (1) {
        if (xQueueReceive(s_cleanup_queue, &item, portMAX_DELAY) != pdTRUE) continue;

        /* 等目标任务进入 eDeleted 状态（IDLE 才会释放内部资源）*/
        int tries = 40;
        while (tries-- > 0) {
            eTaskState st = eTaskGetState(item.handle);
            if (st == eDeleted || st == eInvalid) break;
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        vTaskDelay(pdMS_TO_TICKS(10));

        if (item.stack) heap_caps_free(item.stack);
        if (item.tcb)   heap_caps_free(item.tcb);
    }
}

void psram_task_init(void)
{
    if (s_cleanup_queue) return;
    s_cleanup_queue = xQueueCreate(8, sizeof(cleanup_item_t));
    xTaskCreate(cleaner_task, "psram_clean", 3072, NULL, 2, NULL);
}

/* 入口包装：堆上分配 ctx 持有用户函数、参数和 cleanup 信息。
 * 调用完用户函数后：
 *   1. 把 cleanup 信息投递到 cleaner 队列
 *   2. 释放 ctx（避免堆泄漏）
 *   3. vTaskDelete(NULL) 销毁自己
 * 用户函数必须通过 return 正常结束；不能调 vTaskDelete 提前退出。 */
typedef struct {
    TaskFunction_t user_func;
    void          *user_arg;
    cleanup_item_t cleanup;
} entry_ctx_t;

static void task_entry(void *arg)
{
    entry_ctx_t *ctx = (entry_ctx_t *)arg;

    /* 让出一次 CPU：确保创建者有机会完成 ctx->cleanup.handle 赋值。
     * 正常情况下创建者在 xTaskCreateStatic 返回后立即赋值，但如果新任务
     * 优先级更高导致立即抢占，这里 yield 回去让创建者跑完赋值再继续。 */
    taskYIELD();

    /* 把用户函数跑完 */
    ctx->user_func(ctx->user_arg);

    /* 把 cleanup 信息副本送给 cleaner，然后释放 ctx */
    cleanup_item_t copy = ctx->cleanup;
    heap_caps_free(ctx);

    if (s_cleanup_queue) {
        xQueueSend(s_cleanup_queue, &copy, portMAX_DELAY);
    }
    vTaskDelete(NULL);  /* 自删；cleaner 稍后释放栈和 TCB */
}

BaseType_t xTaskCreatePSRAM(TaskFunction_t func, const char *name,
                             uint32_t stack_bytes, void *arg,
                             UBaseType_t prio, TaskHandle_t *handle_out)
{
    return xTaskCreatePSRAMPinnedToCore(func, name, stack_bytes, arg, prio,
                                         handle_out, tskNO_AFFINITY);
}

BaseType_t xTaskCreatePSRAMPinnedToCore(TaskFunction_t func, const char *name,
                                        uint32_t stack_bytes, void *arg,
                                        UBaseType_t prio,
                                        TaskHandle_t *handle_out,
                                        BaseType_t core_id)
{
    if (!s_cleanup_queue) psram_task_init();
    if (!s_cleanup_queue) return pdFAIL;

    /* 4 字节对齐 */
    stack_bytes = (stack_bytes + 3) & ~3;

    StackType_t  *stack = heap_caps_malloc(stack_bytes, MALLOC_CAP_SPIRAM);
    StaticTask_t *tcb   = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    entry_ctx_t  *ctx   = heap_caps_malloc(sizeof(entry_ctx_t), MALLOC_CAP_INTERNAL);

    if (!stack || !tcb || !ctx) {
        ESP_LOGE(TAG, "alloc %s: stack=%p tcb=%p ctx=%p", name, stack, tcb, ctx);
        if (stack) heap_caps_free(stack);
        if (tcb)   heap_caps_free(tcb);
        if (ctx)   heap_caps_free(ctx);
        return pdFAIL;
    }

    ctx->user_func     = func;
    ctx->user_arg      = arg;
    ctx->cleanup.tcb   = tcb;
    ctx->cleanup.stack = stack;

    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        task_entry, name, stack_bytes / sizeof(StackType_t),
        ctx, prio, stack, tcb, core_id);
    if (!h) {
        heap_caps_free(stack);
        heap_caps_free(tcb);
        heap_caps_free(ctx);
        return pdFAIL;
    }
    /* 必须在任务有机会运行前赋值 handle。由于 xTaskCreateStaticPinnedToCore
     * 在当前任务上下文中返回（新任务还没被调度），且新任务优先级 <= 当前任务
     * 或调度器在 critical section 内，这里赋值是安全的。
     * 但为防万一（新任务优先级更高立即抢占），在 task_entry 开头加 taskYIELD
     * 前先确保 handle 已写入。实际上 xTaskCreateStatic 返回后 handle 就是
     * 有效的，ctx->cleanup.handle 只是给 cleaner 用的副本。 */
    ctx->cleanup.handle = h;
    if (handle_out) *handle_out = h;
    return pdPASS;
}

void psram_task_exit(void)
{
    /* 保留此函数以兼容以前的调用，但在新设计下仅相当于 return。
     * 推荐用户直接 return。如果真的在函数中途退出，用户负责：
     *   - 让入口 task_entry 能正常返回（本函数直接调 vTaskDelete 会泄漏 ctx）
     * 所以这里不做 vTaskDelete，只是让调用方尽快 return。 */
    /* 空实现：等同 return */
}
