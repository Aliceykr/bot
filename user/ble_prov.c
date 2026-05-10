#include "ble_prov.h"
#include "wifi.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdio.h>

#define TAG "BLE_PROV"
#define DEVICE_NAME "ESP32-Bot"

/* ================================================================
 * NimBLE 配网（HM-10 兼容）
 *
 * 并发模型：
 *   任务 A = NimBLE host 任务（GATT 回调、on_sync、GAP 事件）
 *   任务 B = Timer service 任务（rx_timer_cb）
 *   任务 C = Worker 任务（rx_worker_task，parse_and_dispatch）
 *   任务 D = LVGL 任务（调 start/stop/is_active/init/deinit）
 *   任务 E = 配网后台任务（my_demo: ble_cred_apply_task，调 deinit）
 *
 * 同步：
 *   s_mutex — 保护 UI 回调指针、RX 缓冲、核心状态位
 *              （s_active / s_want_advertising / s_initialized）
 *   所有"读状态+动作+改状态"的序列必须整段在锁内，
 *   避免 on_sync 与 stop 的竞态（问题1/2）、双 deinit 双 timer delete（问题3/4）。
 * ================================================================ */

static const ble_uuid16_t s_svc_uuid    = BLE_UUID16_INIT(0xFFE0);
static const ble_uuid16_t s_io_chr_uuid = BLE_UUID16_INIT(0xFFE1);

/* 模块级互斥锁：首次进入 ble_prov_init 时创建，后续永远复用。 */
static SemaphoreHandle_t s_mutex = NULL;

static inline void ensure_mutex(void)
{
    if (!s_mutex) {
        /* 进入 init 之前还没有其他任务会访问，这里无需再加锁 */
        s_mutex = xSemaphoreCreateMutex();
    }
}
#define LOCK()   do { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); } while (0)
#define UNLOCK() do { if (s_mutex) xSemaphoreGive(s_mutex); } while (0)

/* 核心状态：写必须锁内，读可以在锁外做一次性快照 */
static bool s_active      = false;
static bool s_initialized = false;
static bool s_was_active  = false;
/* want_advertising = true 表示上层"请求广播"；由 on_sync 与 start 共同消费 */
static bool s_want_advertising = false;
/* deinit 防重入门：LVGL 与配网后台任务都可能调 deinit，互斥避免双拆 */
static bool s_deinit_in_progress = false;

static ble_prov_status_cb_t s_status_cb = NULL;
static ble_prov_status_cb_t s_saved_cb  = NULL;
static ble_prov_cred_cb_t   s_cred_cb   = NULL;

static uint16_t s_conn_handle    = 0xFFFF;
static uint16_t s_io_val_handle  = 0;
static bool     s_notify_enabled = false;
static uint8_t  s_own_addr_type  = 0;

/* RX 累积缓冲 */
#define RX_ACCUM_CAP 256
static char s_rx_accum[RX_ACCUM_CAP];
static size_t s_rx_len = 0;
static TimerHandle_t s_rx_timer = NULL;

static int start_advertising(void);

/* ================================================================
 * 辅助
 * ================================================================ */
static void notify_status(const char *status)
{
    ESP_LOGI(TAG, "%s", status);
    ble_prov_status_cb_t cb;
    LOCK();
    cb = s_status_cb;
    UNLOCK();
    if (cb) cb(status);
}

static void ble_send_notify(const char *msg)
{
    if (s_conn_handle == 0xFFFF || !s_notify_enabled || !s_io_val_handle) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, strlen(msg));
    if (om) ble_gatts_notify_custom(s_conn_handle, s_io_val_handle, om);
}

/* ================================================================
 * Worker：解析 → wifi_set_credentials → s_cred_cb（独立 4KB 栈）
 * ================================================================ */
static void parse_and_dispatch(char *buf)
{
    ESP_LOGI(TAG, "处理完整消息: %s", buf);

    char *ssid_start = strstr(buf, "SSID_");
    if (!ssid_start) { ble_send_notify("格式错误"); return; }
    ssid_start += 5;
    char *pwd_start = strstr(ssid_start, " password_");
    if (!pwd_start) { ble_send_notify("缺少 password_"); return; }
    *pwd_start = '\0';
    pwd_start += 10;

    char ssid[33] = {0}, password[65] = {0};
    strncpy(ssid, ssid_start, sizeof(ssid) - 1);
    strncpy(password, pwd_start, sizeof(password) - 1);

    ESP_LOGI(TAG, "凭据解析成功: SSID=%s", ssid);
    ble_send_notify("凭据已收到");
    notify_status("凭据已收到");

    wifi_set_credentials(ssid, password);

    ble_prov_cred_cb_t cb = s_cred_cb;
    if (cb) cb(ssid, password);
    else    ESP_LOGW(TAG, "未注册 cred_cb，凭据已保存但 WiFi 不会自动连接");
}

static void rx_worker_task(void *arg)
{
    char *buf = (char *)arg;
    parse_and_dispatch(buf);
    free(buf);
    vTaskDelete(NULL);
}

static void rx_timer_cb(TimerHandle_t t)
{
    (void)t;
    char *buf = NULL;
    size_t n = 0;

    LOCK();
    if (s_rx_len > 0) {
        buf = malloc(RX_ACCUM_CAP);
        if (buf) {
            n = s_rx_len < RX_ACCUM_CAP - 1 ? s_rx_len : RX_ACCUM_CAP - 1;
            memcpy(buf, s_rx_accum, n);
            buf[n] = '\0';
        }
        s_rx_len = 0;
    }
    UNLOCK();

    if (!buf) {
        if (n > 0) ESP_LOGE(TAG, "rx_timer: malloc 失败");
        return;
    }
    if (xTaskCreate(rx_worker_task, "ble_rx_wk", 4096, buf, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "rx_worker 创建失败");
        free(buf);
    }
}

static void accumulate_rx(const uint8_t *data, int len)
{
    ESP_LOGI(TAG, "BLE 收到 %d 字节: %.*s", len, len, (const char *)data);
    if (len <= 0) return;

    LOCK();
    size_t room = (RX_ACCUM_CAP - 1) - s_rx_len;
    if ((size_t)len > room) len = (int)room;
    if (len > 0) {
        memcpy(s_rx_accum + s_rx_len, data, len);
        s_rx_len += len;
    }
    UNLOCK();

    if (!s_rx_timer) {
        s_rx_timer = xTimerCreate("ble_rx", pdMS_TO_TICKS(50), pdFALSE,
                                   NULL, rx_timer_cb);
    }
    if (s_rx_timer) xTimerReset(s_rx_timer, pdMS_TO_TICKS(10));
}

/* ================================================================
 * GATT 服务
 * ================================================================ */
static int io_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        const char *hello = "ESP32-Bot ready";
        os_mbuf_append(ctxt->om, hello, strlen(hello));
        return 0;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len == 0 || len > RX_ACCUM_CAP) return BLE_ATT_ERR_UNLIKELY;
        uint8_t tmp[RX_ACCUM_CAP];
        uint16_t copied = 0;
        ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &copied);
        accumulate_rx(tmp, copied);
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static const struct ble_gatt_chr_def s_chr_defs[] = {
    {
        .uuid       = &s_io_chr_uuid.u,
        .access_cb  = io_chr_access,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_io_val_handle,
    },
    { 0 }
};

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_chr_defs,
    },
    { 0 }
};

/* ================================================================
 * 广播 + GAP
 * ================================================================ */
static int gap_event_cb(struct ble_gap_event *event, void *arg);

static int start_advertising(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    ble_uuid16_t uuids16[1] = { s_svc_uuid };
    fields.uuids16 = uuids16;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) { ESP_LOGE(TAG, "adv_set_fields rc=%d", rc); return rc; }

    struct ble_gap_adv_params adv_params = { 0 };
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) ESP_LOGE(TAG, "adv_start rc=%d", rc);
    return rc;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE 手机已连接 conn=%d", s_conn_handle);
            notify_status("已连接，等待配网...");
        } else {
            /* 连接失败才重启广播；这里在 host 任务里，读 s_active 是一致的快照 */
            bool need = false;
            LOCK();
            need = s_active;
            UNLOCK();
            if (need) start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE 手机已断开 reason=%d", event->disconnect.reason);
        s_conn_handle = 0xFFFF;
        s_notify_enabled = false;
        {
            bool need = false;
            LOCK();
            need = s_active;
            UNLOCK();
            if (need) {
                notify_status("已断开，重新广播...");
                start_advertising();
            }
        }
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_io_val_handle) {
            s_notify_enabled = event->subscribe.cur_notify;
            if (s_notify_enabled) ble_send_notify("ESP32-Bot ready!");
        }
        return 0;

    default:
        return 0;
    }
}

/* ================================================================
 * NimBLE host
 * ================================================================ */
static void nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* on_sync 原子地"消费" want_advertising：
 *   必须在锁内读 + 判断 + 清标志 + 启动 + 置 s_active。
 *   否则 stop() 在中间把 want 清零后还可能被 on_sync 幽灵启动。*/
static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    uint8_t addr[6];
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE 同步完成, MAC=%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    bool do_start = false;
    LOCK();
    if (s_want_advertising && !s_active) {
        s_want_advertising = false;   /* 消费掉，避免后续再次触发 */
        do_start = true;
    }
    UNLOCK();

    if (!do_start) return;

    int rc = start_advertising();
    LOCK();
    bool ok = (rc == 0);
    if (ok) s_active = true;
    UNLOCK();

    if (ok) {
        notify_status("蓝牙已开启");
        ESP_LOGI(TAG, "BLE 广播已启动（on_sync）");
    } else {
        /* 延迟启动失败：通知 UI，避免用户看到假的"已开启" */
        notify_status("BLE 启动失败");
        ESP_LOGE(TAG, "on_sync 内广播启动失败 rc=%d", rc);
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset reason=%d", reason);
}

/* ================================================================
 * 强制拆除 NimBLE + controller（init 失败回滚 / deinit 主流程）
 * ================================================================ */
static void force_nimble_teardown(void)
{
    esp_bt_controller_status_t ctl_st = esp_bt_controller_get_status();
    bool nimble_touched = s_initialized ||
                          ctl_st == ESP_BT_CONTROLLER_STATUS_INITED ||
                          ctl_st == ESP_BT_CONTROLLER_STATUS_ENABLED;
    if (nimble_touched) {
        (void)nimble_port_stop();
        (void)nimble_port_deinit();
    }

    uint32_t waited = 0;
    const uint32_t step = 20;
    while (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        if (esp_bt_controller_disable() == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
        if (waited >= 1000) { ESP_LOGW(TAG, "强制 disable 超时"); break; }
    }
    waited = 0;
    while (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        if (esp_bt_controller_deinit() == ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(step));
        waited += step;
        if (waited >= 1000) { ESP_LOGW(TAG, "强制 deinit 超时"); break; }
    }
}

/* ================================================================
 * 公共 API
 * ================================================================ */
bool ble_prov_init(void)
{
    ensure_mutex();
    if (!s_mutex) { ESP_LOGE(TAG, "mutex 创建失败"); return false; }

    /* init 本身也要和 deinit 串行化，防止并发 init + deinit 撕裂状态 */
    LOCK();
    if (s_initialized) { UNLOCK(); return true; }
    if (s_deinit_in_progress) {
        /* deinit 正在做，不要抢 */
        UNLOCK();
        ESP_LOGW(TAG, "init: deinit 进行中，放弃本次");
        return false;
    }
    UNLOCK();

    ESP_LOGI(TAG, "初始化 NimBLE...");

    force_nimble_teardown();

    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init 失败: %s", esp_err_to_name(ret));
        force_nimble_teardown();
        return false;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "count_cfg rc=%d", rc);
        force_nimble_teardown();
        return false;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs rc=%d", rc);
        force_nimble_teardown();
        return false;
    }

    ble_svc_gap_device_name_set(DEVICE_NAME);
    nimble_port_freertos_init(nimble_host_task);

    LOCK();
    s_initialized = true;
    UNLOCK();
    ESP_LOGI(TAG, "NimBLE 初始化完成, DRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

/* 非阻塞启动：
 *   1) host 已 sync：直接 start_advertising；成功后在锁内置 s_active。
 *   2) host 未 sync：标记 want_advertising，由 on_sync 消费。
 *
 * 对应问题 2：未 sync 情况下我们不再返回 true，而是返回"pending"语义
 * （仍是 true，但 UI 要通过 status_cb 才能确认"蓝牙已开启"是否真的成立）。
 * 如果 on_sync 启动失败，notify_status("BLE 启动失败") 会告诉 UI；
 * 上层原本就应该以 status_cb 为准，不能仅凭 start() 返回值写死 UI。 */
bool ble_prov_start(ble_prov_status_cb_t cb)
{
    bool need_start_now = false;
    bool is_active      = false;
    bool is_inited      = false;

    LOCK();
    is_inited = s_initialized;
    is_active = s_active;
    if (!is_inited) { UNLOCK(); return false; }
    if (is_active)  { UNLOCK(); return true; }

    if (cb) {
        s_status_cb = cb;
        s_saved_cb  = cb;
    }
    s_want_advertising = true;
    if (ble_hs_synced()) {
        /* 直接同步路径：消费 want + 启动 + 置 active 都在锁内 */
        s_want_advertising = false;
        need_start_now = true;
    }
    UNLOCK();

    if (!need_start_now) {
        ESP_LOGI(TAG, "host 未 sync，广播将在 on_sync 触发");
        return true;
    }

    if (s_own_addr_type == 0) ble_hs_id_infer_auto(0, &s_own_addr_type);
    int rc = start_advertising();
    LOCK();
    bool ok = (rc == 0);
    if (ok) s_active = true;
    UNLOCK();

    if (!ok) {
        notify_status("BLE 启动失败");
        return false;
    }
    notify_status("蓝牙已开启");
    ESP_LOGI(TAG, "BLE 广播已启动");
    return true;
}

void ble_prov_stop(void)
{
    /* stop 必须原子地清除 want_advertising 和 s_active，
     * 否则 on_sync 会读到旧 want=true 继续"幽灵启动" */
    bool was_active = false;
    LOCK();
    was_active = s_active;
    s_active = false;
    s_want_advertising = false;
    UNLOCK();

    if (!was_active) return;

    ble_gap_adv_stop();
    if (s_conn_handle != 0xFFFF) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        s_conn_handle = 0xFFFF;
    }
    s_notify_enabled = false;

    notify_status("蓝牙已关闭");

    LOCK();
    s_status_cb = NULL;
    UNLOCK();
    ESP_LOGI(TAG, "BLE 已停止");
}

bool ble_prov_is_active(void)
{
    bool a;
    LOCK(); a = s_active; UNLOCK();
    return a;
}

void ble_prov_suspend(void)
{
    LOCK();
    s_was_active = s_active;
    UNLOCK();
    if (!s_was_active) return;
    ble_prov_stop();
}

void ble_prov_resume(void)
{
    bool was;
    LOCK();
    was = s_was_active;
    s_was_active = false;
    UNLOCK();
    if (!was) return;
    if (s_initialized) ble_prov_start(s_saved_cb);
}

void ble_prov_set_cred_cb(ble_prov_cred_cb_t cb)
{
    LOCK();
    s_cred_cb = cb;
    UNLOCK();
}

/* deinit 入口：互斥门锁防双拆。
 * - 用 s_deinit_in_progress 做原子门：第二个调用者看到 true 直接返回
 * - 第一个调用者执行完后清门
 * 同时保证 timer 只 delete 一次（在门内拷贝+置 NULL） */
void ble_prov_deinit(void)
{
    bool claim = false;
    TimerHandle_t timer_to_delete = NULL;

    LOCK();
    /* 原子判断"是否需要拆除" + "上门锁" */
    bool need_teardown = s_initialized ||
        esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_IDLE;
    if (need_teardown && !s_deinit_in_progress) {
        s_deinit_in_progress = true;
        claim = true;
        /* timer 句柄在锁内抢走：让后续任何 timer 操作看到 NULL */
        timer_to_delete = s_rx_timer;
        s_rx_timer = NULL;
    }
    UNLOCK();

    if (!claim) {
        /* 或是已经全干净，或是另一个任务正在拆。两种都不应继续。 */
        return;
    }

    if (s_active) ble_prov_stop();

    if (timer_to_delete) {
        xTimerDelete(timer_to_delete, portMAX_DELAY);
    }

    force_nimble_teardown();

    LOCK();
    s_status_cb        = NULL;
    s_saved_cb         = NULL;
    s_rx_len           = 0;
    s_initialized      = false;
    s_conn_handle      = 0xFFFF;
    s_io_val_handle    = 0;
    s_notify_enabled   = false;
    s_want_advertising = false;
    s_deinit_in_progress = false;   /* 拆除完毕，放门 */
    UNLOCK();

    ESP_LOGI(TAG, "NimBLE 已完全释放, DRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
