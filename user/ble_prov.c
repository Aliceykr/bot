#include "ble_prov.h"
#include "wifi.h"
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define TAG "BLE_PROV"
#define DEVICE_NAME "ESP32-Bot"

/* GATT Service/Characteristic UUID */
#define SVC_UUID     0xFF00
#define CHR_RX_UUID  0xFF01
#define CHR_TX_UUID  0xFF02
#define GATT_APP_ID  0x55

/* ================================================================
 * 状态
 * ================================================================ */
static bool s_active      = false;
static bool s_initialized = false;
static bool s_was_active  = false;

static ble_prov_status_cb_t s_status_cb = NULL;

/* GATT handles */
static uint16_t s_service_handle    = 0;
static uint16_t s_rx_char_handle    = 0;
static uint16_t s_rx_val_handle     = 0;
static uint16_t s_tx_char_handle    = 0;
static uint16_t s_tx_val_handle     = 0;
static bool s_ble_connected = false;
static uint16_t s_ble_conn_id = 0;
static bool s_notify_enabled = false;

/* ================================================================
 * 工具函数
 * ================================================================ */
static void notify_status(const char *status)
{
    ESP_LOGI(TAG, "%s", status);
    if (s_status_cb) s_status_cb(status);
}

static void ble_send_notify(const char *msg)
{
    if (!s_ble_connected || !s_notify_enabled) return;
    esp_ble_gatts_send_indicate(s_ble_conn_id, s_service_handle,
                                s_tx_val_handle, strlen(msg), (uint8_t *)msg, false);
}

/* ================================================================
 * 解析 BLE 收到的数据
 * ================================================================ */
static void handle_ble_data(const char *data, int len)
{
    char buf[128] = {0};
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, data, len);

    /* WiFi 配网: "SSID_xxx password_xxx" */
    char *ssid_start = strstr(buf, "SSID_");
    if (!ssid_start) {
        ble_send_notify("格式错误");
        return;
    }
    ssid_start += 5;
    char *pwd_start = strstr(ssid_start, " password_");
    if (!pwd_start) {
        ble_send_notify("格式错误，缺少 password_");
        return;
    }
    *pwd_start = '\0';
    pwd_start += 10;

    char ssid[33] = {0}, password[65] = {0};
    strncpy(ssid, ssid_start, sizeof(ssid) - 1);
    strncpy(password, pwd_start, sizeof(password) - 1);

    ESP_LOGI(TAG, "配网: SSID=%s", ssid);
    notify_status("配网中...");
    ble_send_notify("连接中...");

    wifi_set_credentials(ssid, password);
    bool ok = wifi_connect();

    if (ok) {
        char resp[64];
        snprintf(resp, sizeof(resp), "连接成功! IP: %s", wifi_get_ip());
        ble_send_notify(resp);
        notify_status("WiFi 连接成功");
    } else {
        ble_send_notify("连接失败，请重试");
        notify_status("WiFi 连接失败");
    }
}

/* ================================================================
 * BLE GATTS 事件处理
 * ================================================================ */
static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                 esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATT 注册失败");
            return;
        }
        esp_gatt_srvc_id_t svc_id = {
            .is_primary = true,
            .id.inst_id = 0,
            .id.uuid.len = ESP_UUID_LEN_16,
            .id.uuid.uuid.uuid16 = SVC_UUID,
        };
        esp_ble_gatts_create_service(gatts_if, &svc_id, 4);
        break;

    case ESP_GATTS_CREATE_EVT: {
        s_service_handle = param->create.service_handle;
        esp_ble_gatts_start_service(s_service_handle);

        esp_bt_uuid_t rx_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = CHR_RX_UUID };
        esp_ble_gatts_add_char(s_service_handle, &rx_uuid,
                               ESP_GATT_PERM_WRITE_ENCRYPTED,
                               ESP_GATT_CHAR_PROP_BIT_WRITE, NULL, NULL);

        esp_bt_uuid_t tx_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = CHR_TX_UUID };
        esp_ble_gatts_add_char(s_service_handle, &tx_uuid,
                               ESP_GATT_PERM_READ,
                               ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY,
                               NULL, NULL);
        break;
    }

    case ESP_GATTS_ADD_CHAR_EVT:
        if (param->add_char.char_uuid.uuid.uuid16 == CHR_RX_UUID) {
            s_rx_char_handle = param->add_char.attr_handle;
            s_rx_val_handle  = param->add_char.attr_handle;
        } else if (param->add_char.char_uuid.uuid.uuid16 == CHR_TX_UUID) {
            s_tx_char_handle = param->add_char.attr_handle;
            s_tx_val_handle  = param->add_char.attr_handle;
            esp_bt_uuid_t cccd_uuid = { .len = ESP_UUID_LEN_16, .uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG };
            esp_ble_gatts_add_char_descr(s_service_handle, &cccd_uuid,
                                          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, NULL, NULL);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_ble_connected = true;
        s_ble_conn_id = param->connect.conn_id;
        ESP_LOGI(TAG, "BLE 手机已连接");
        notify_status("已连接，等待配网...");
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_ble_connected = false;
        s_notify_enabled = false;
        ESP_LOGI(TAG, "BLE 手机已断开");
        notify_status("已断开，重新广播...");
        if (s_active) {
            esp_ble_gap_start_advertising(&((esp_ble_adv_params_t){
                .adv_int_min = 0x20, .adv_int_max = 0x40,
                .adv_type = ADV_TYPE_IND,
                .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
                .channel_map = ADV_CHNL_ALL,
                .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
            }));
        }
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_rx_val_handle) {
            handle_ble_data((const char *)param->write.value, param->write.len);
        }
        if (param->write.is_prep == false && param->write.len == 2) {
            uint16_t descr_val = param->write.value[1] << 8 | param->write.value[0];
            if (descr_val == 0x0001) {
                s_notify_enabled = true;
            } else if (descr_val == 0x0000) {
                s_notify_enabled = false;
            }
        }
        break;

    default:
        break;
    }
}

/* BLE GAP 事件处理 */
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    (void)param;
    if (event == ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT) {
        esp_ble_gap_start_advertising(&((esp_ble_adv_params_t){
            .adv_int_min = 0x20, .adv_int_max = 0x40,
            .adv_type = ADV_TYPE_IND,
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .channel_map = ADV_CHNL_ALL,
            .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        }));
    }
}

/* ================================================================
 * 公共 API
 * ================================================================ */

bool ble_prov_init(void)
{
    ESP_LOGI(TAG, "初始化 Bluedroid...");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT 控制器初始化失败: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BT 控制器启用失败: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bluedroid_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid 初始化失败: %s", esp_err_to_name(ret));
        return false;
    }

    ret = esp_bluedroid_enable();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid 启用失败: %s", esp_err_to_name(ret));
        return false;
    }

    esp_ble_gatts_register_callback(gatts_event_handler);
    esp_ble_gap_register_callback(gap_event_handler);
    esp_ble_gatts_app_register(GATT_APP_ID);

    esp_ble_gap_set_device_name(DEVICE_NAME);

    esp_ble_adv_data_t adv_data = {
        .set_scan_rsp = false,
        .include_name = true,
        .include_txpower = false,
        .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
    };
    esp_ble_gap_config_adv_data(&adv_data);

    esp_ble_gatt_set_local_mtu(200);

    s_initialized = true;
    ESP_LOGI(TAG, "Bluedroid BLE 初始化完成");
    return true;
}

bool ble_prov_start(ble_prov_status_cb_t cb)
{
    if (s_active) return true;
    if (!s_initialized) return false;
    if (cb) s_status_cb = cb;

    s_active = true;
    notify_status("蓝牙已开启");
    ESP_LOGI(TAG, "BLE 广播已启动");
    return true;
}

void ble_prov_stop(void)
{
    if (!s_active) return;
    s_active = false;

    esp_ble_gap_stop_advertising();
    if (s_ble_connected) {
        esp_ble_gatts_close(s_ble_conn_id, s_service_handle);
    }
    s_ble_connected = false;
    s_notify_enabled = false;

    notify_status("蓝牙已关闭");
    s_status_cb = NULL;
    ESP_LOGI(TAG, "BLE 已停止");
}

bool ble_prov_is_active(void)
{
    return s_active;
}

void ble_prov_suspend(void)
{
    s_was_active = s_active;
    if (!s_active) return;

    s_active = false;
    esp_ble_gap_stop_advertising();
    if (s_ble_connected) {
        esp_ble_gatts_close(s_ble_conn_id, s_service_handle);
    }
    s_ble_connected = false;

    esp_bluedroid_disable();
    ESP_LOGI(TAG, "BT 已暂停（游戏）");
}

void ble_prov_resume(void)
{
    if (!s_was_active) return;
    s_was_active = false;

    ESP_LOGI(TAG, "BT 恢复中...");
    esp_bluedroid_enable();

    s_active = true;
    notify_status("蓝牙已恢复");
    ESP_LOGI(TAG, "BT 已恢复");
}
