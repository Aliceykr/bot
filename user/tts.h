#ifndef __TTS_H
#define __TTS_H

#include <stdbool.h>

/**
 * @brief 调用百度 TTS 将文本合成为 PCM 音频并送入喇叭播放（阻塞至 HTTP 响应结束）
 *
 * 在独立 FreeRTOS 任务中调用（如 asr_llm_task），不可在 LVGL 任务中调用。
 * PCM 格式：16kHz 16bit mono，直接送 speaker_play()。
 *
 * @param text UTF-8 文本，最长 512 字节
 * @return true=合成并推送成功；false=token获取失败/网络错误/API返回错误
 */
bool tts_speak(const char *text);

#endif /* __TTS_H */
