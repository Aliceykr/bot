#ifndef __BLE_PROV_H
#define __BLE_PROV_H

#include <stdbool.h>

/* BLE 状态回调（运行在 BT 任务上下文，非 LVGL 线程） */
typedef void (*ble_prov_status_cb_t)(const char *status);

/* 初始化 Bluedroid + BLE GATT（app_main 中调用一次） */
bool ble_prov_init(void);

/* 启动 BLE 广播 */
bool ble_prov_start(ble_prov_status_cb_t cb);

/* 停止 BLE 广播，断开连接 */
void ble_prov_stop(void);

/* BLE 是否正在运行 */
bool ble_prov_is_active(void);

/* 游戏时暂停：记录状态后释放 BT 资源 */
void ble_prov_suspend(void);

/* 游戏后恢复：仅当进入游戏前活跃时才恢复 */
void ble_prov_resume(void);

#endif /* __BLE_PROV_H */
