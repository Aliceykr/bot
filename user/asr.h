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

/* asr_mic_init：初始化在线 ASR 使用的 INMP441 I2S 麦克风。
 *
 * 分配 PSRAM 录音缓冲，创建常驻录音任务，并配置 I2S_NUM_0。
 * 应在 app_main / UI 初始化阶段调用一次。 */
void asr_mic_init(void);

/* asr_record_start：开始把麦克风 PCM 写入内部录音缓冲。
 *
 * 会清空上次录音位置并唤醒录音任务；调用前需确保 asr_mic_init 成功。 */
void asr_record_start(void);

/* asr_record_stop：停止录音并返回已采集 PCM 字节数。
 *
 * 会等待录音任务退出当前 I2S read，再 disable I2S 通道，确保缓冲长度稳定。 */
uint32_t asr_record_stop(void);

/* asr_is_recording：查询当前是否处于录音状态。
 *
 * 供 UI 按钮状态和防重复操作使用。 */
bool asr_is_recording(void);

/* asr_get_token：确认百度 access_token 可用。
 *
 * 兼容旧调用的轻量包装，实际 token 管理由 baidu_token 模块负责。 */
bool asr_get_token(void);

/* asr_recognize：上传录音 PCM 到百度 ASR 并解析识别结果。
 *
 * 同步阻塞 HTTP 调用，必须在后台任务中运行；audio_len_bytes 来自
 * asr_record_stop，out 返回成功文本或错误信息。 */
bool asr_recognize(uint32_t audio_len_bytes, asr_result_t *out);

/* asr_mic_deinit：释放 I2S_NUM_0 给离线 ESP-SR 等模块临时使用。
 *
 * 常驻录音任务不删除，只进入 idle；释放前会等待阻塞中的 i2s_read 退出。 */
void asr_mic_deinit(void);

/* asr_mic_reinit：按在线 ASR 配置重新创建 I2S_NUM_0。
 *
 * ESP-SR 停止后调用，用于恢复在线录音/识别功能。 */
void asr_mic_reinit(void);

#endif
