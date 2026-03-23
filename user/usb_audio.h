#ifndef __USB_AUDIO_H
#define __USB_AUDIO_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief 初始化 USB CDC 设备及发送任务
 *        需在 app_main 中调用一次，早于任何 usb_audio_send 调用
 */
void usb_audio_init(void);

/**
 * @brief 非阻塞地将 PCM 数据推入发送缓冲
 *
 * @param pcm       16-bit 有符号 PCM 样本数组
 * @param len_bytes 字节数（样本数 * 2）
 * @return          实际写入的字节数（缓冲满时可能小于 len_bytes）
 */
int usb_audio_send(const int16_t *pcm, size_t len_bytes);

#endif /* __USB_AUDIO_H */
