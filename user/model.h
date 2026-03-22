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

// 发送一条消息给模型，结果填入 out
// 阻塞直到收到响应或超时
bool model_chat(const char *user_msg, model_result_t *out);

#endif
