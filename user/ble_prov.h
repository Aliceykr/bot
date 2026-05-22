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

/* ble_prov_init：初始化 NimBLE Host、GATT 服务和 BLE controller。
 *
 * 懒加载：仅在用户打开蓝牙功能时调用。重复调用幂等。
 * 只完成初始化，不一定立刻广播；广播由 ble_prov_start 触发。 */
bool ble_prov_init(void);

/* ble_prov_deinit：完全释放 NimBLE 和 controller 资源。
 *
 * 关闭蓝牙或配网完成后调用，归还几十 KB 内部 DRAM。
 * 内部会等待 pending notify 调用结束，避免拆 host 时还有任务在用 NimBLE。 */
void ble_prov_deinit(void);

/* ble_prov_start：启动 HM-10 兼容 GATT 广播。
 *
 * cb 用于上报“蓝牙已开启/已连接/收到凭据”等状态；若 NimBLE host 尚未 sync，
 * 函数返回 true 表示已登记启动请求，真正成功以回调为准。 */
bool ble_prov_start(ble_prov_status_cb_t cb);

/* 注册凭据接收回调。收到 SSID_xxx password_yyy 后会调用。
 * 上层典型处理：ble_prov_deinit() 释放 BT 栈 → 触发 WiFi 连接。 */
void ble_prov_set_cred_cb(ble_prov_cred_cb_t cb);

/* ble_prov_stop：停止广播并断开当前连接。
 *
 * 不释放 controller 资源；若要彻底省内存，随后调用 ble_prov_deinit。 */
void ble_prov_stop(void);

/* ble_prov_is_active：查询当前是否处于广播/连接活跃状态。 */
bool ble_prov_is_active(void);

/* ble_prov_suspend：游戏进入前暂停 BLE。
 *
 * 记录进入游戏前是否 active，并停止广播/连接，避免和游戏争 DRAM/CPU。 */
void ble_prov_suspend(void);

/* ble_prov_resume：游戏退出后按 suspend 记录恢复 BLE 广播。 */
void ble_prov_resume(void);

#endif /* __BLE_PROV_H */
