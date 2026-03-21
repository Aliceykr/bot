#include "my_demo.h"
#include "lvgl.h"
#include "lcd.h"
#include "wifi.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static lv_obj_t *selected_label;
static lv_obj_t *list;
static lv_group_t *group;
static lv_obj_t *spinner_cont = NULL;
static TaskHandle_t wifi_task_handle = NULL;

typedef struct {
    bool success;
    bool cancelled;
    char ip[20];
} wifi_result_t;

static QueueHandle_t wifi_result_queue = NULL;
static bool wifi_connecting = false;

// 取消按钮事件：断开WiFi，发送cancelled结果
static void cancel_btn_cb(lv_event_t *e)
{
    wifi_disconnect();
    wifi_result_t result = { .success = false, .cancelled = true };
    xQueueSend(wifi_result_queue, &result, 0);
}

static void wifi_connect_task(void *arg)
{
    bool ok = wifi_connect();
    // 如果已被取消，不发送结果
    wifi_result_t result;
    result.success = ok;
    result.cancelled = false;
    if (ok) strncpy(result.ip, wifi_get_ip(), sizeof(result.ip)-1);
    else result.ip[0] = '\0';
    // 只有队列还空才发（取消时队列已有cancelled消息）
    xQueueSend(wifi_result_queue, &result, 0);
    wifi_task_handle = NULL;
    vTaskDelete(NULL);
}

static void close_btn_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_user_data(e);
    lv_obj_delete(mbox);
    // 恢复编码器到列表
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            lv_indev_set_group(indev, group);
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

static void show_result_box(bool ok, bool cancelled, const char *ip)
{
    wifi_connecting = false;
    if (spinner_cont) {
        lv_obj_delete(spinner_cont);
        spinner_cont = NULL;
    }
    if (cancelled) {
        // 恢复编码器
        lv_indev_t *indev = lv_indev_get_next(NULL);
        while (indev) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, group);
                break;
            }
            indev = lv_indev_get_next(indev);
        }
        return;
    }

    // 结果弹窗，高度足够放文字和按钮
    lv_obj_t *mbox = lv_obj_create(lv_screen_active());
    lv_obj_set_size(mbox, 200, 120);
    lv_obj_center(mbox);
    lv_obj_set_style_bg_color(mbox, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_color(mbox, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_border_width(mbox, 2, 0);
    lv_obj_set_style_pad_all(mbox, 8, 0);
    lv_obj_set_layout(mbox, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(mbox, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mbox, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *result = lv_label_create(mbox);
    lv_obj_set_style_text_align(result, LV_TEXT_ALIGN_CENTER, 0);
    if (ok) {
        lv_label_set_text_fmt(result, LV_SYMBOL_OK " Connected!\n%s", ip);
        lv_obj_set_style_text_color(result, lv_color_hex(0x00ff00), 0);
    } else {
        lv_label_set_text(result, LV_SYMBOL_CLOSE " WiFi Failed!");
        lv_obj_set_style_text_color(result, lv_color_hex(0xff0000), 0);
    }

    lv_obj_t *close_btn = lv_button_create(mbox);
    lv_obj_set_size(close_btn, 80, 32);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0xe94560), 0);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "OK");
    lv_obj_center(close_lbl);

    // 弹窗用独立group，隔离主列表编码器操作
    lv_group_t *popup_group = lv_group_create();
    lv_group_add_obj(popup_group, close_btn);
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            lv_indev_set_group(indev, popup_group);
            break;
        }
        indev = lv_indev_get_next(indev);
    }
    lv_group_focus_obj(close_btn);
}

static void wifi_status_timer_cb(lv_timer_t *timer)
{
    wifi_result_t result;
    if (xQueueReceive(wifi_result_queue, &result, 0) == pdTRUE) {
        show_result_box(result.success, result.cancelled, result.ip);
    }
}

static void list_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *txt = lv_label_get_text(label);

    if (strstr(txt, "WiFi Connect")) {
        if (wifi_connecting) return;
        wifi_connecting = true;

        // 加载弹窗
        spinner_cont = lv_obj_create(lv_screen_active());
        lv_obj_set_size(spinner_cont, 200, 150);
        lv_obj_center(spinner_cont);
        lv_obj_set_style_bg_color(spinner_cont, lv_color_hex(0x16213e), 0);
        lv_obj_set_style_border_color(spinner_cont, lv_color_hex(0xe94560), 0);
        lv_obj_set_style_border_width(spinner_cont, 2, 0);
        lv_obj_set_style_pad_all(spinner_cont, 8, 0);
        lv_obj_set_layout(spinner_cont, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(spinner_cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(spinner_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t *spinner = lv_spinner_create(spinner_cont);
        lv_obj_set_size(spinner, 50, 50);

        lv_obj_t *lbl = lv_label_create(spinner_cont);
        lv_label_set_text(lbl, "WiFi Connecting...");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);

        lv_obj_t *cancel_btn = lv_button_create(spinner_cont);
        lv_obj_set_size(cancel_btn, 80, 32);
        lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x555555), 0);
        lv_obj_add_event_cb(cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
        lv_label_set_text(cancel_lbl, "Cancel");
        lv_obj_center(cancel_lbl);

        // 加载弹窗用独立group，隔离主列表
        lv_group_t *loading_group = lv_group_create();
        lv_group_add_obj(loading_group, cancel_btn);
        lv_indev_t *indev = lv_indev_get_next(NULL);
        while (indev) {
            if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
                lv_indev_set_group(indev, loading_group);
                break;
            }
            indev = lv_indev_get_next(indev);
        }
        lv_group_focus_obj(cancel_btn);

        xTaskCreate(wifi_connect_task, "wifi_task", 4096, NULL, 3, &wifi_task_handle);
    } else {
        lv_label_set_text_fmt(selected_label, "Selected: %s", txt);
    }
}

void my_demo(void)
{
    wifi_result_queue = xQueueCreate(2, sizeof(wifi_result_t));

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x1a1a2e), 0);

    lv_obj_t *title = lv_label_create(lv_screen_active());
    lv_label_set_text(title, "ESP32-S3 Menu");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *line = lv_obj_create(lv_screen_active());
    lv_obj_set_size(line, LCD_W - 10, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 30);

    list = lv_list_create(lv_screen_active());
    lv_obj_set_size(list, LCD_W - 10, LCD_H - 80);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 4, 0);

    const char *items[] = {
        LV_SYMBOL_WIFI     " WiFi Connect",
        LV_SYMBOL_BELL     " Notifications",
        LV_SYMBOL_BATTERY_FULL " Battery",
        LV_SYMBOL_SETTINGS " System Settings",
        LV_SYMBOL_LOOP     " Update Firmware",
        LV_SYMBOL_POWER    " Restart",
    };

    group = lv_group_create();

    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = lv_list_add_button(list, NULL, items[i]);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x16213e), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xe94560), LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xffffff), 0);
        lv_obj_add_event_cb(btn, list_event_cb, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(group, btn);
    }

    lv_obj_t *hint = lv_label_create(lv_screen_active());
    lv_label_set_text(hint, LV_SYMBOL_UP"/"LV_SYMBOL_DOWN" Scroll  "LV_SYMBOL_OK" Select");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    selected_label = lv_label_create(lv_screen_active());
    lv_label_set_text(selected_label, "Selected: none");
    lv_obj_set_style_text_color(selected_label, lv_color_hex(0xe94560), 0);
    lv_obj_align(selected_label, LV_ALIGN_BOTTOM_MID, 0, -5);

    lv_timer_create(wifi_status_timer_cb, 500, NULL);
}

lv_group_t *my_demo_get_group(void)
{
    return group;
}
