#ifndef __MODEL_H
#define __MODEL_H

#include <stdbool.h>

#define MODEL_MAX_INPUT  512   // 用户输入最大长度
#define MODEL_MAX_OUTPUT 1024  // 模型输出最大长度

typedef struct {
    char input[MODEL_MAX_INPUT];
    char output[MODEL_MAX_OUTPUT];
    bool success;
    char error_msg[64];
} model_result_t;

/* model_chat：把用户文本发送到 OpenAI 兼容聊天接口并解析回复。
 *
 * 需要 WiFi 已连接；同步阻塞 HTTPS 请求，结果填入 out。
 * 应在后台任务中调用，避免阻塞 LVGL。 */
bool model_chat(const char *user_msg, model_result_t *out);

/* model_init：提前创建大模型模块 mutex。
 *
 * 必须在 app_main 启动阶段单线程调用一次，避免首次并发调用时创建锁竞态。 */
void model_init(void);

#endif
