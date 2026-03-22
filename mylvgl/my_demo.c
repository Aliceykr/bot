#include "my_demo.h"
#include "lvgl.h"
#include "lcd.h"
#include "wifi.h"
#include "weather.h"
#include "sntp_time.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

LV_FONT_DECLARE(lv_font_simhei_16);

static lv_obj_t *selected_label;
static lv_obj_t *list;
static lv_group_t *group;
static lv_obj_t *spinner_cont = NULL;
static TaskHandle_t wifi_task_handle = NULL;

// 天气查询队列
typedef struct {
    bool success;
    weather_data_t data;
} weather_result_t;

static QueueHandle_t weather_result_queue = NULL;
static bool weather_fetching = false;

typedef struct {
    bool success;
    bool cancelled;
    char ip[20];
} wifi_result_t;

static QueueHandle_t wifi_result_queue = NULL;
static bool wifi_connecting = false;

// ================================================================
// 天气显示界面
// ================================================================

// NTP 实时时钟 timer 回调
static void clock_timer_cb(lv_timer_t *t)
{
    lv_obj_t *lbl = (lv_obj_t *)lv_timer_get_user_data(t);
    if (!lv_obj_is_valid(lbl)) {
        lv_timer_delete(t);
        return;
    }
    struct tm now;
    sntp_time_get(&now);
    lv_label_set_text_fmt(lbl, "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
}

static void back_btn_cb(lv_event_t *e)
{
    // auto_del=true 让动画结束后自动删除天气屏幕
    lv_screen_load_anim(lv_obj_get_screen(list), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) { lv_indev_set_group(indev, group); break; }
        indev = lv_indev_get_next(indev);
    }
}

static void show_weather_screen(const weather_data_t *d)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // 顶部标题栏
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text_fmt(title, "%s, %s", d->city, d->province);
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_simhei_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, LCD_W - 10, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 28);

    // 天气描述（直接用中文，不用符号图标）
    lv_obj_t *weather_lbl = lv_label_create(scr);
    lv_label_set_text(weather_lbl, d->weather);
    lv_obj_set_style_text_color(weather_lbl, lv_color_hex(0xffd700), 0);
    lv_obj_set_style_text_font(weather_lbl, &lv_font_simhei_16, 0);
    lv_obj_align(weather_lbl, LV_ALIGN_TOP_LEFT, 15, 40);

    // 温度
    lv_obj_t *temp_lbl = lv_label_create(scr);
    lv_label_set_text_fmt(temp_lbl, "%s", d->temperature);
    lv_obj_set_style_text_color(temp_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(temp_lbl, &lv_font_simhei_16, 0);
    lv_obj_align(temp_lbl, LV_ALIGN_TOP_RIGHT, -15, 40);

    // 分割
    lv_obj_t *line2 = lv_obj_create(scr);
    lv_obj_set_size(line2, LCD_W - 20, 1);
    lv_obj_set_style_bg_color(line2, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(line2, 0, 0);
    lv_obj_set_style_pad_all(line2, 0, 0);
    lv_obj_align(line2, LV_ALIGN_TOP_MID, 0, 65);

    // 湿度
    lv_obj_t *hum_icon = lv_label_create(scr);
    lv_label_set_text(hum_icon, LV_SYMBOL_DOWNLOAD);
    lv_obj_set_style_text_color(hum_icon, lv_color_hex(0x00cfff), 0);
    lv_obj_align(hum_icon, LV_ALIGN_TOP_LEFT, 15, 75);
    lv_obj_t *hum_lbl = lv_label_create(scr);
    lv_label_set_text_fmt(hum_lbl, " 湿度: %s%%", d->humidity);
    lv_obj_set_style_text_color(hum_lbl, lv_color_hex(0x00cfff), 0);
    lv_obj_set_style_text_font(hum_lbl, &lv_font_simhei_16, 0);
    lv_obj_align_to(hum_lbl, hum_icon, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

    // 风
    lv_obj_t *wind_icon = lv_label_create(scr);
    lv_label_set_text(wind_icon, LV_SYMBOL_LOOP);
    lv_obj_set_style_text_color(wind_icon, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(wind_icon, LV_ALIGN_TOP_LEFT, 15, 100);
    lv_obj_t *wind_lbl = lv_label_create(scr);
    lv_label_set_text_fmt(wind_lbl, " %s %s", d->wind_direction, d->wind_power);
    lv_obj_set_style_text_color(wind_lbl, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(wind_lbl, &lv_font_simhei_16, 0);
    lv_obj_align_to(wind_lbl, wind_icon, LV_ALIGN_OUT_RIGHT_MID, 0, 0);

    // 分割
    lv_obj_t *line3 = lv_obj_create(scr);
    lv_obj_set_size(line3, LCD_W - 20, 1);
    lv_obj_set_style_bg_color(line3, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(line3, 0, 0);
    lv_obj_set_style_pad_all(line3, 0, 0);
    lv_obj_align(line3, LV_ALIGN_TOP_MID, 0, 125);

    // 日期
    lv_obj_t *date_lbl = lv_label_create(scr);
    lv_label_set_text_fmt(date_lbl, "%s", d->date);
    lv_obj_set_style_text_color(date_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(date_lbl, &lv_font_simhei_16, 0);
    lv_obj_align(date_lbl, LV_ALIGN_TOP_LEFT, 15, 135);

    // 实时时钟（NTP，每秒更新）
    lv_obj_t *time_lbl = lv_label_create(scr);
    struct tm now;
    sntp_time_get(&now);
    lv_label_set_text_fmt(time_lbl, "%02d:%02d:%02d", now.tm_hour, now.tm_min, now.tm_sec);
    lv_obj_set_style_text_color(time_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(time_lbl, &lv_font_simhei_16, 0);
    lv_obj_align(time_lbl, LV_ALIGN_TOP_LEFT, 15, 160);
    lv_timer_create(clock_timer_cb, 1000, time_lbl);

    // 返回按钮
    lv_obj_t *back_btn = lv_button_create(scr);
    lv_obj_set_size(back_btn, 100, 36);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe94560), 0);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, scr);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT" Back");
    lv_obj_center(back_lbl);

    lv_group_t *wg = lv_group_create();
    lv_group_add_obj(wg, back_btn);
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) { lv_indev_set_group(indev, wg); break; }
        indev = lv_indev_get_next(indev);
    }
    lv_group_focus_obj(back_btn);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

static void weather_cancel_btn_cb(lv_event_t *e)
{
    weather_fetching = false;
    if (spinner_cont) { lv_obj_delete(spinner_cont); spinner_cont = NULL; }
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) { lv_indev_set_group(indev, group); break; }
        indev = lv_indev_get_next(indev);
    }
}

static void weather_fetch_task(void *arg)
{
    weather_result_t res;
    res.success = weather_fetch(&res.data);
    xQueueSend(weather_result_queue, &res, 0);
    vTaskDelete(NULL);
}

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
    if (ok) {
        // WiFi 连接成功后同步 NTP，超时 10 秒
        sntp_time_sync(10000);
    }
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

    // 检查天气查询结果
    weather_result_t wresp;
    if (xQueueReceive(weather_result_queue, &wresp, 0) == pdTRUE) {
        weather_fetching = false;
        if (spinner_cont) {
            lv_obj_delete(spinner_cont);
            spinner_cont = NULL;
        }
        if (wresp.success) {
            show_weather_screen(&wresp.data);
        } else {
            // 显示错误弹窗
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
            lv_obj_t *err_lbl = lv_label_create(mbox);
            lv_label_set_text_fmt(err_lbl, LV_SYMBOL_CLOSE " %s", wresp.data.error_msg);
            lv_obj_set_style_text_color(err_lbl, lv_color_hex(0xff0000), 0);
            lv_obj_set_style_text_align(err_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_t *ok_btn = lv_button_create(mbox);
            lv_obj_set_size(ok_btn, 80, 32);
            lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0xe94560), 0);
            lv_obj_add_event_cb(ok_btn, close_btn_cb, LV_EVENT_CLICKED, mbox);
            lv_obj_t *ok_lbl = lv_label_create(ok_btn);
            lv_label_set_text(ok_lbl, "OK");
            lv_obj_center(ok_lbl);
            lv_group_t *eg = lv_group_create();
            lv_group_add_obj(eg, ok_btn);
            lv_indev_t *indev = lv_indev_get_next(NULL);
            while (indev) {
                if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) { lv_indev_set_group(indev, eg); break; }
                indev = lv_indev_get_next(indev);
            }
            lv_group_focus_obj(ok_btn);
        }
    }
}

static void list_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    // child(0) 是图标，child(1) 是文字
    lv_obj_t *label = lv_obj_get_child(btn, 1);
    const char *txt = lv_label_get_text(label);

    if (strstr(txt, "WiFi")) {
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
    } else if (strstr(txt, "天气")) {
        if (weather_fetching) return;
        weather_fetching = true;

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
        lv_label_set_text(lbl, "Fetching Weather...");
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);

        lv_obj_t *cancel_btn = lv_button_create(spinner_cont);
        lv_obj_set_size(cancel_btn, 80, 32);
        lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x555555), 0);
        lv_obj_add_event_cb(cancel_btn, weather_cancel_btn_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
        lv_label_set_text(cancel_lbl, "Cancel");
        lv_obj_center(cancel_lbl);

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
        xTaskCreate(weather_fetch_task, "weather_task", 16384, NULL, 3, NULL);
    } else {
        lv_label_set_text_fmt(selected_label, "Selected: %s", txt);
    }
}

void my_demo(void)
{
    wifi_result_queue = xQueueCreate(2, sizeof(wifi_result_t));
    weather_result_queue = xQueueCreate(2, sizeof(weather_result_t));

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x1a1a2e), 0);

    lv_obj_t *title = lv_label_create(lv_screen_active());
    lv_label_set_text(title, "ESP32-S3 菜单");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_simhei_16, 0);
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

    static const char *icons[] = {
        LV_SYMBOL_WIFI,
        LV_SYMBOL_EYE_OPEN,
        LV_SYMBOL_BATTERY_FULL,
        LV_SYMBOL_SETTINGS,
        LV_SYMBOL_LOOP,
        LV_SYMBOL_POWER,
    };
    static const char *labels[] = {
        "WiFi 连接",
        "天气与日期",
        "电池",
        "系统设置",
        "固件更新",
        "重启",
    };

    group = lv_group_create();

    for (int i = 0; i < 6; i++) {
        lv_obj_t *btn = lv_list_add_button(list, icons[i], labels[i]);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x16213e), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xe94560), LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xffffff), 0);
        // 只对文字子标签设置 simhei 字体，图标子标签保持默认符号字体
        lv_obj_t *txt_lbl = lv_obj_get_child(btn, 1);
        if (txt_lbl) lv_obj_set_style_text_font(txt_lbl, &lv_font_simhei_16, 0);
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
