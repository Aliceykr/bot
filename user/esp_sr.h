#ifndef __ESP_SR_H
#define __ESP_SR_H

#include <stdbool.h>

/* 命令识别结果回调（运行在 detect 任务上下文，非 LVGL 线程，需通过队列通知 UI） */
typedef void (*esp_sr_result_cb_t)(int command_id, const char *command_text, float probability);

/* 初始化 ESP-SR：加载模型、创建 AFE（WakeNet 禁用）+ MultiNet7 CN、注册命令词。
 * 懒加载：仅在进入语音命令界面时调用，不在 app_main 常驻。
 * 返回 true 表示模型加载成功。重复调用幂等（已初始化直接返回 true）。 */
bool esp_sr_init(void);

/* 释放 ESP-SR 所有资源（AFE + MultiNet + 模型列表），归还 DRAM。
 * 退出语音命令界面时调用。未初始化时调用无副作用。 */
void esp_sr_deinit(void);

/* 启动命令词识别（按键触发）。
 * 内部：asr_mic_deinit() → 创建 ESP-SR 专属 I2S_NUM_0 → 启动 feed/detect 任务。
 * on_result: 识别到命令或超时时回调。 */
bool esp_sr_start_listening(esp_sr_result_cb_t on_result);

/* 停止命令词识别（按键松开 / 超时 / 退出触发）。
 * 内部：停止任务 → 释放 I2S → asr_mic_reinit()。 */
void esp_sr_stop_listening(void);

/* 查询当前是否正在监听 */
bool esp_sr_is_listening(void);

#endif /* __ESP_SR_H */
