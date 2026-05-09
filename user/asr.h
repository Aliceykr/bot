#ifndef __ASR_H
#define __ASR_H

#include <stdbool.h>
#include <stdint.h>

#define ASR_MAX_RESULT 512

typedef struct {
    bool success;
    char result[ASR_MAX_RESULT];
    char error_msg[64];
} asr_result_t;

// 初始化 I2S 麦克风，需在使用前调用一次
void asr_mic_init(void);

// 开始录音
void asr_record_start(void);

// 停止录音，返回录音字节数
uint32_t asr_record_stop(void);

// 是否正在录音
bool asr_is_recording(void);

// 获取 access_token（首次调用会请求百度，之后缓存）
bool asr_get_token(void);

// 上传录音并识别（阻塞）
bool asr_recognize(uint32_t audio_len_bytes, asr_result_t *out);

#endif
