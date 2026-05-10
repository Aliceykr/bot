#ifndef __BLE_PROV_H
#define __BLE_PROV_H

#include <stdbool.h>

/* BLE 状态回调（运行在 BT 任务上下文，非 LVGL 线程） */
typedef void (*ble_prov_status_cb_t)(const char *status);

/* 凭据接收回调：BLE 收到有效的 SSID+password 时触发。
 * 已帮上层调过 wifi_set_credentials()，回调体内只需要触发 WiFi 连接流程即可。
 * 运行在 ble_prov 内部 worker 任务上下文，不要在里面做重活，用 lv_async_call
 * 或 xTaskCreate 投递到其他上下文。 */
typedef void (*ble_prov_cred_cb_t)(const char *ssid, const char *password);

/* 初始化 Bluedroid + BLE GATT。
 * 懒加载：仅在用户打开蓝牙功能时调用。重复调用幂等。 */
bool ble_prov_init(void);

/* 完全释放 Bluedroid 栈（disable + deinit + controller disable/deinit），
 * 归还 ~50-60KB DRAM。关闭蓝牙时调用。未初始化时调用无副作用。 */
void ble_prov_deinit(void);

/* 启动 BLE 广播 */
bool ble_prov_start(ble_prov_status_cb_t cb);

/* 注册凭据接收回调。收到 SSID_xxx password_yyy 后会调用。
 * 上层典型处理：ble_prov_deinit() 释放 BT 栈 → 触发 WiFi 连接。 */
void ble_prov_set_cred_cb(ble_prov_cred_cb_t cb);

/* 停止 BLE 广播，断开连接 */
void ble_prov_stop(void);

/* BLE 是否正在运行 */
bool ble_prov_is_active(void);

/* 游戏时暂停：记录状态后释放 BT 资源 */
void ble_prov_suspend(void);

/* 游戏后恢复：仅当进入游戏前活跃时才恢复 */
void ble_prov_resume(void);

#endif /* __BLE_PROV_H */
