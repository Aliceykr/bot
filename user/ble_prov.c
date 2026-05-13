#include "ble_prov.h"
#include "wifi.h"
#include "music.h"
#include "esp_log.h"
#include "esp_bt.h"
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
#include <stdlib.h>

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

static volatile uint16_t s_conn_handle    = 0xFFFF;
static uint16_t s_io_val_handle  = 0;
static volatile bool     s_notify_enabled = false;
static uint8_t  s_own_addr_type  = 0;

/* RX 累积缓冲 */
#define RX_ACCUM_CAP 256
static char s_rx_accum[RX_ACCUM_CAP];
static size_t s_rx_len = 0;
static TimerHandle_t s_rx_timer = NULL;

/* Pending host-API 调用计数：worker / timer 进入 ble_send_notify 前 +1，
 * 调用完 -1。deinit 会先设门锁阻止新 pending 进入，再等现有 pending 归零，
 * 才调 force_nimble_teardown，彻底消除 ble_send_notify 的 UAF 窗口。*/
static volatile int32_t s_pending_host_calls = 0;
static SemaphoreHandle_t s_pending_done_sem  = NULL;   /* binary, 用于唤醒 deinit */

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
    /* 跨任务调用点：worker task / timer service 都会调这个。
     *
     * NimBLE API 本身是线程安全的（内部 host mutex），但前提是 nimble_port 还
     * 存活。deinit 会等 pending 计数归零才拆 host，所以只要我们在锁内通过门
     * 锁 +1 成功，host 就保证活到 -1 前。 */
    uint16_t conn;
    bool notify_en;
    uint16_t val_handle;
    bool allowed = false;

    LOCK();
    if (s_initialized && !s_deinit_in_progress) {
        conn       = s_conn_handle;
        notify_en  = s_notify_enabled;
        val_handle = s_io_val_handle;
        s_pending_host_calls++;   /* 在锁内 ++，确保 deinit 的快照一致 */
        allowed = true;
    }
    UNLOCK();

    if (!allowed) return;

    if (conn != 0xFFFF && notify_en && val_handle) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(msg, strlen(msg));
        if (om) ble_gatts_notify_custom(conn, val_handle, om);
    }

    /* pending -1。如果归零且 deinit 在等，give sem */
    LOCK();
    bool last = (--s_pending_host_calls == 0);
    bool deinit_waiting = s_deinit_in_progress;
    UNLOCK();
    if (last && deinit_waiting && s_pending_done_sem) {
        xSemaphoreGive(s_pending_done_sem);
    }
}

/* ================================================================
 * Worker：解析 → 分发到 WiFi 配网 / 音乐控制 / ...
 *
 * 协议：
 *   "SSID_xxx password_yyy" → WiFi 配网（原协议）
 *   "/music on"              → 列出所有音乐（多行 notify）
 *   "/music off"             → 停止播放
 *   "/<歌名>"                → 前缀匹配播放
 * ================================================================ */
static void handle_wifi_prov(char *buf);
static void handle_music_command(const char *cmd);

static void parse_and_dispatch(char *buf)
{
    ESP_LOGI(TAG, "处理完整消息: %s", buf);

    /* 首字符 '/' 视为音乐 / 控制命令；否则走 WiFi 配网。
     * 优先级这样排：真实配网消息以 "SSID_" 开头，永远不是 '/'，安全 */
    if (buf[0] == '/') {
        handle_music_command(buf + 1);  /* 跳过 '/' */
        return;
    }
    handle_wifi_prov(buf);
}

/* 原 WiFi 配网解析，逻辑不变 */
static void handle_wifi_prov(char *buf)
{
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

    /* 锁内快照 cred_cb + s_initialized：
     *   deinit 把 s_initialized=false 和 s_cred_cb=NULL 放在同一个锁区间里，
     *   这里一次性读出一致快照，要么 "还在初始化状态 + cb 有效"，要么 "已拆完"。
     *   避免"cb 非空但 UI 侧 BLE 菜单已释放"这种悬空调用。 */
    ble_prov_cred_cb_t cb = NULL;
    LOCK();
    if (s_initialized && !s_deinit_in_progress) {
        cb = s_cred_cb;
    }
    UNLOCK();
    if (cb) cb(ssid, password);
    else    ESP_LOGW(TAG, "cred_cb 无效（已 deinit 或未注册），凭据已保存但 WiFi 不会自动连接");
}

/* ================================================================
 * 音乐命令处理
 *
 * "music on"        → 扫描 SD 卡 /music 目录，每首歌发一个 notify
 * "music off"       → 停止后台播放
 * "<歌名>"          → 前缀匹配 .mp3/.wav 并后台播放
 *
 * 内存策略：不缓存歌曲列表（最省 RAM），命令处理期间用一个 static 扫描
 * 数组（~4.8KB BSS，一直占着但不多一份），扫完 notify 完即丢，播放时
 * 再扫一次做前缀匹配。重复扫描代价小（FATFS 目录扫描几十 ms）。
 * worker task 串行处理命令，static 数组不会并发被写。
 * ================================================================ */
/* 一次性后台任务：异步调 music_stop 后自删。
 * BLE worker task 不能同步调 music_stop（最多阻塞 2s 等播放任务退出，
 * 期间手机后续命令排队）。把 stop 投到独立任务，BLE worker 立即返回。*/
static void ble_music_stop_task(void *arg)
{
    (void)arg;
    music_stop();
    vTaskDelete(NULL);
}

static void handle_music_command(const char *cmd)
{
    /* 把所有变量声明提到函数顶部，避免后面 goto out 时跨过初始化触发
     * GCC -Wjump-misses-init 警告。*/
    int count = 0;
    const music_entry_t *match = NULL;
    char *endp = NULL;
    long idx = 0;
    char path[128];
    char line[128];

    /* 扫描结果 ~4.8KB，从 PSRAM 临时分配代替 static BSS，常驻 0 DRAM。
     * BLE worker task 4KB 栈不够放在栈上，故走 heap。FATFS 扫描本身就几十
     * 毫秒，多一次 alloc/free 几乎无感。 */
    music_entry_t *s_scan = heap_caps_malloc(
        sizeof(music_entry_t) * MUSIC_MAX_COUNT, MALLOC_CAP_SPIRAM);
    if (!s_scan) {
        ble_send_notify("内存不足");
        return;
    }

    /* "music on" 列出所有歌 */
    if (strcmp(cmd, "music on") == 0) {
        if (!music_scan(s_scan, &count)) {
            ble_send_notify("SD 卡未挂载或目录不存在");
            goto out;
        }
        if (count == 0) {
            ble_send_notify("(无音乐文件)");
            goto out;
        }
        for (int i = 0; i < count; ++i) {
            /* %.*s 明确限长，避免 GCC format-truncation 告警（name 虽然是
             * char[64]，编译器不信它一定 \0 结尾在前 64 字节）*/
            snprintf(line, sizeof(line), "%d. %.*s",
                     i + 1, (int)(sizeof(s_scan[i].name) - 1), s_scan[i].name);
            ble_send_notify(line);
            /* notify 包之间小间隔，给手机端和 BLE stack 喘息，避免丢包 */
            vTaskDelay(pdMS_TO_TICKS(30));
        }
        snprintf(line, sizeof(line), "--- 共 %d 首，/<名字> 播放 ---", count);
        ble_send_notify(line);
        goto out;
    }

    /* "music off" 异步停止播放 */
    if (strcmp(cmd, "music off") == 0) {
        BaseType_t r = xTaskCreate(ble_music_stop_task, "ble_mstop",
                                   2048, NULL, 3, NULL);
        if (r != pdPASS) {
            /* 创建失败降级同步 stop（接受 worker 阻塞，功能优先） */
            ESP_LOGW(TAG, "ble_music_stop_task 创建失败，降级同步");
            music_stop();
        }
        ble_send_notify("已停止");
        goto out;
    }

    /* 其他 "/xxx" 视为歌名（可带/不带后缀）或序号，匹配一首歌播放 */
    if (!music_scan(s_scan, &count) || count == 0) {
        ble_send_notify("无可用音乐");
        goto out;
    }

    /* 先尝试序号匹配（/1、/2 ...）。序号对 BLE 最友好：
     * 避免手机输入法把 '~' 打成全角 '～'、字母大小写不一致等问题 */
    idx = strtol(cmd, &endp, 10);
    if (endp != cmd && *endp == '\0' && idx >= 1 && idx <= count) {
        match = &s_scan[idx - 1];
    }

    /* 序号没命中就做文件名前缀匹配。
     * 规则：前缀字节相同 && cmd 之后紧跟 '.' 或结尾
     *   "/song01"     → 匹配 "song01.mp3" / "song01.wav"
     *   "/song01.mp3" → 精确匹配 */
    if (!match) {
        size_t cmd_len = strlen(cmd);
        for (int i = 0; i < count; ++i) {
            if (strncmp(s_scan[i].name, cmd, cmd_len) == 0) {
                char next = s_scan[i].name[cmd_len];
                if (next == '\0' || next == '.') {
                    match = &s_scan[i];
                    break;
                }
            }
        }
    }

    if (!match) {
        snprintf(line, sizeof(line), "未找到: %s", cmd);
        ble_send_notify(line);
        goto out;
    }

    music_full_path(match->name, path, sizeof(path));
    if (music_play(path)) {
        snprintf(line, sizeof(line), "播放: %.*s",
                 (int)(sizeof(match->name) - 1), match->name);
        ble_send_notify(line);
    } else {
        ble_send_notify("播放失败");
    }

out:
    /* 唯一释放点：所有提前返回都跳到这里。
     * s_scan 必为非 NULL（函数入口已经判过失败 return） */
    heap_caps_free(s_scan);
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

    /* 所有对 s_rx_timer / s_rx_len / s_rx_accum 的读写都在锁内完成，
     * 与 deinit（锁内抢走 timer 并置 NULL）互斥，消除 timer UAF（C4）。
     * lazy-create timer 也在锁内，避免两次 accumulate_rx 同时创建两个 timer。 */
    LOCK();
    size_t room = (RX_ACCUM_CAP - 1) - s_rx_len;
    if ((size_t)len > room) len = (int)room;
    if (len > 0) {
        memcpy(s_rx_accum + s_rx_len, data, len);
        s_rx_len += len;
    }

    /* 如果 deinit 正在进行（门锁置位），或 BLE 已经被 deinit 拆完，
     * 不再创建 / reset timer，避免 timer use-after-free */
    bool deinit_pending = s_deinit_in_progress;
    bool inited = s_initialized;
    if (!deinit_pending && inited && !s_rx_timer) {
        s_rx_timer = xTimerCreate("ble_rx", pdMS_TO_TICKS(50), pdFALSE,
                                   NULL, rx_timer_cb);
    }

    /* xTimerReset 必须在锁内调用，与 deinit 的 s_rx_timer = NULL 互斥，
     * 避免 reset 操作命中已被 delete 的句柄（UAF） */
    if (s_rx_timer) {
        xTimerReset(s_rx_timer, pdMS_TO_TICKS(10));
    }
    UNLOCK();
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
            LOCK();
            s_conn_handle = event->connect.conn_handle;
            UNLOCK();
            ESP_LOGI(TAG, "BLE 手机已连接 conn=%d", event->connect.conn_handle);
            notify_status("已连接，等待配网...");
        } else {
            /* 连接失败才重启广播 */
            bool need = false;
            LOCK();
            need = s_active;
            UNLOCK();
            if (need) start_advertising();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE 手机已断开 reason=%d", event->disconnect.reason);
        {
            bool need = false;
            LOCK();
            s_conn_handle    = 0xFFFF;
            s_notify_enabled = false;
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
            bool enable = event->subscribe.cur_notify;
            LOCK();
            s_notify_enabled = enable;
            UNLOCK();
            if (enable) ble_send_notify("ESP32-Bot ready!");
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
 *   否则 stop() 在中间把 want 清零后还可能被 on_sync 幽灵启动。
 *
 * 幽灵广播 (H5) 防护：
 *   光在启动前检查 want 不够用，因为 start_advertising 本身要跑一段时间，
 *   期间 stop 可能已经被调用清了状态。用"启动代 s_adv_gen"记账：
 *     - 每次 stop 都把代数 +1
 *     - on_sync / start 启动前记一份 my_gen，启动成功后在锁内比对；
 *       若代数已经变过 → 说明中间被 stop 过 → 回滚（停广播），不置 s_active=true */
static uint32_t s_adv_gen = 0;

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    uint8_t addr[6];
    ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    ESP_LOGI(TAG, "BLE 同步完成, MAC=%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);

    bool do_start = false;
    uint32_t my_gen = 0;
    LOCK();
    if (s_want_advertising && !s_active) {
        s_want_advertising = false;   /* 消费掉，避免后续再次触发 */
        my_gen = s_adv_gen;
        do_start = true;
    }
    UNLOCK();

    if (!do_start) return;

    int rc = start_advertising();
    bool committed = false;
    LOCK();
    bool ok = (rc == 0);
    /* 只有当启动成功 AND 期间没人 stop（代数未变）才真正置 active。
     * 否则回滚：此刻广播是跑起来的，我们必须停掉它，否则就成幽灵广播 */
    if (ok && my_gen == s_adv_gen) {
        s_active  = true;
        committed = true;
    }
    UNLOCK();

    if (ok && !committed) {
        /* 启动期间被 stop 了，立刻停掉刚起的广播 */
        ble_gap_adv_stop();
        ESP_LOGW(TAG, "on_sync: 启动中被 stop，回滚广播");
        return;
    }

    if (committed) {
        notify_status("蓝牙已开启");
        ESP_LOGI(TAG, "BLE 广播已启动（on_sync）");
    } else {
        /* 真正启动失败 */
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

    /* pending 信号量：给 deinit 等 host-call 归零用，一次性创建 */
    if (!s_pending_done_sem) {
        s_pending_done_sem = xSemaphoreCreateBinary();
        if (!s_pending_done_sem) {
            ESP_LOGE(TAG, "pending sem 创建失败");
            return false;
        }
    }

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
    uint32_t my_gen     = 0;

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
    my_gen = s_adv_gen;
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

    bool committed = false;
    LOCK();
    bool ok = (rc == 0);
    if (ok && my_gen == s_adv_gen) {
        s_active  = true;
        committed = true;
    }
    UNLOCK();

    if (ok && !committed) {
        /* 启动期间被 stop，停掉刚起的广播 */
        ble_gap_adv_stop();
        ESP_LOGW(TAG, "ble_prov_start: 启动中被 stop，回滚广播");
        return false;
    }
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
     * 并递增 s_adv_gen 让任何进行中的 start/on_sync 检测到"我被打断了"，
     * 回滚它自己刚起的广播（H5 幽灵广播防护） */
    bool was_active = false;
    LOCK();
    was_active = s_active;
    s_active = false;
    s_want_advertising = false;
    s_adv_gen++;
    UNLOCK();

    /* 无论之前 active 与否，都尝试 stop 广播，防止"on_sync 正在启动时"
     * 我们的 stop 赶在启动前执行完。由 on_sync/start 的 gen 检查兜底回滚。 */
    ble_gap_adv_stop();
    uint16_t conn_to_term;
    LOCK();
    conn_to_term = s_conn_handle;
    s_conn_handle = 0xFFFF;
    s_notify_enabled = false;
    UNLOCK();
    if (conn_to_term != 0xFFFF) {
        ble_gap_terminate(conn_to_term, BLE_ERR_REM_USER_CONN_TERM);
    }

    if (was_active) notify_status("蓝牙已关闭");

    LOCK();
    s_status_cb = NULL;
    UNLOCK();
    if (was_active) ESP_LOGI(TAG, "BLE 已停止");
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

    /* 先等现有 pending host-call（worker / timer 里的 ble_send_notify 等）
     * 全部结束，再拆 nimble_port。否则它们正在用的 mbuf/pool 会成 UAF。
     * 由于 s_deinit_in_progress 已经置位，不会再有新的 pending 进来。
     * 最多等 2 秒兜底，超时就强拆（此时极大概率是 pending 任务卡死）。*/
    if (s_pending_done_sem) {
        /* 先清一下旧 signal */
        xSemaphoreTake(s_pending_done_sem, 0);
    }
    for (int i = 0; i < 40; ++i) {     /* 40 × 50ms = 2s */
        int32_t pending;
        LOCK();
        pending = s_pending_host_calls;
        UNLOCK();
        if (pending <= 0) break;
        if (s_pending_done_sem) {
            xSemaphoreTake(s_pending_done_sem, pdMS_TO_TICKS(50));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }

    if (s_active) ble_prov_stop();

    if (timer_to_delete) {
        xTimerDelete(timer_to_delete, portMAX_DELAY);
    }

    force_nimble_teardown();

    LOCK();
    s_status_cb        = NULL;
    s_saved_cb         = NULL;
    s_cred_cb          = NULL;              /* 清除凭据回调，防止 rx worker 调悬空 */
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
