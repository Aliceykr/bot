#ifndef __BEMFA_H
#define __BEMFA_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* 开机一次性初始化：创建模块内部 mutex。
 * 建议在 app_main() 单线程阶段调用，避免首次 API 并发调用时的 lazy-init 竞态。
 * 多次调用安全（幂等）。未调用也能工作，但并发首次调用有极窄窗口可能泄漏 mutex。*/
void bemfa_init(void);

/* ================================================================
 * 巴法云 TCP 设备云 HTTP REST 客户端
 *
 * 两个核心 API：
 *   bemfa_list_devices       GET  /vb/api/v2/allTopic      拉取所有主题
 *   bemfa_push_msg           POST /va/postJsonMsg          推送 on/off 消息
 *
 * 用法：
 *   必须先 wifi_connect() 成功。所有 API 同步阻塞调用（内部做 HTTPS 握手），
 *   调用方应该在 PSRAM 栈的任务里调，不要放 LVGL 线程。
 *
 * 线程安全：
 *   模块内部用一个 mutex 串行化两个 API，避免两个 HTTPS client 同时跑引发
 *   mbedTLS 并发分配。
 * ================================================================ */

#define BEMFA_MAX_TOPIC_LEN    32   /* 主题值 topic（字母数字） */
#define BEMFA_MAX_NAME_LEN     48   /* 主题昵称 name（UTF-8） */
#define BEMFA_MAX_MSG_LEN      16   /* 消息体 msg（"on" / "off" 等） */
#define BEMFA_MAX_DEVICES      32   /* 同时容纳的设备数 */

typedef struct {
    char topic[BEMFA_MAX_TOPIC_LEN];  /* 主题值，例如 "light002" */
    char name[BEMFA_MAX_NAME_LEN];    /* 主题昵称，例如 "客厅灯" */
    char msg[BEMFA_MAX_MSG_LEN];      /* 最后一条消息，常见 "on" / "off" */
    bool online;                       /* 订阅端是否在线 */
} bemfa_device_t;

/**
 * @brief 拉取所有 TCP 设备云主题（type=3）
 *
 * @param out_list   调用方提供的数组，大小至少 BEMFA_MAX_DEVICES
 * @param out_count  返回实际写入数量
 * @return true 成功（包括 0 个设备的情况）；false HTTP 失败 / WiFi 未连
 *
 * 阻塞时长：通常 1-3 秒（HTTPS 握手 + 响应）
 */
bool bemfa_list_devices(bemfa_device_t *out_list, int *out_count);

/**
 * @brief 给指定主题推送消息
 *
 * @param topic  主题值
 * @param msg    消息内容（"on" / "off" / 自定义）
 * @return true 推送成功；false HTTP 失败
 *
 * 阻塞时长：通常 1-2 秒
 */
bool bemfa_push_msg(const char *topic, const char *msg);

/**
 * @brief 便捷封装：toggle 一个设备
 *
 * 根据传入的 current_msg 决定发 "on" 还是 "off"：
 *   current_msg == "on"  → 发 "off"，返回 "off"
 *   else                 → 发 "on"，返回 "on"
 *
 * @param topic        主题值
 * @param current_msg  当前消息（可以来自 bemfa_list_devices 的 msg 字段）
 * @param new_msg_out  成功时写入新消息（buf 至少 4 字节）
 * @return true 推送成功
 */
bool bemfa_toggle(const char *topic, const char *current_msg, char *new_msg_out, size_t cap);

#endif
