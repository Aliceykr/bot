#include "my_demo.h"
#include "lvgl.h"
#include "lcd.h"
#include "wifi.h"
#include "weather.h"
#include "sntp_time.h"
#include "model.h"
#include "asr.h"
#include "tts.h"
#include "esp_sr.h"
#include "ble_prov.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "rom_loader.h"
#include "game_runtime.h"
#include "lv_port_disp.h"
#include "lv_port_indev.h"
#include "keypad.h"
#include "psram_task.h"
#include "music.h"
#include "speaker.h"
#include "bemfa.h"

/* 业务临时任务（weather / chat / asr / tts）均为一次性任务，末尾调
 * vTaskDelete(NULL) 后由 IDLE 自动回收栈和 TCB，必须使用 xTaskCreate
 * （动态分配），不要用 xTaskCreateStatic 否则永久泄漏。
 * 16KB 栈放内部 DRAM，HTTPS+cJSON 访问更快，这些任务互斥且短命。 */

LV_FONT_DECLARE(lv_font_simhei_16);

/* 前向声明 */
static void chat_back_btn_cb(lv_event_t *e);
static void close_btn_cb(lv_event_t *e);
static void show_game_screen(void);
static void game_back_btn_cb(lv_event_t *e);
static void ble_cred_cb(const char *ssid, const char *password);
static void show_music_screen(void);
static void show_volume_screen(void);
static void show_bemfa_screen(void);
static void show_chat_screen(void);
static void show_asr_screen(void);
static void show_ble_screen(void);

/* 全局 UI 对象 */
static lv_obj_t *list;
static lv_group_t *group;
static lv_obj_t *wifi_spinner_cont    = NULL;  /* WiFi 加载弹窗容器 */
static lv_obj_t *weather_spinner_cont = NULL;  /* 天气加载弹窗容器 */
/* 蓝牙结果弹窗句柄：WiFi 连接流程启动前会自动关闭它，
 * 避免"蓝牙已开启"弹窗还卡在屏上时 WiFi spinner 叠上来看不见 */
static lv_obj_t *ble_result_dialog    = NULL;
static TaskHandle_t wifi_task_handle = NULL;

// 通用：屏幕删除时释放关联的 lv_group
static void group_delete_cb(lv_event_t *e)
{
    lv_group_t *g = (lv_group_t *)lv_event_get_user_data(e);
    if (g) lv_group_delete(g);
}

/* 将编码器输入设备绑定到指定 group，g 为 NULL 时绑定主菜单 group */
static void indev_set_group(lv_group_t *g)
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_ENCODER) {
            lv_indev_set_group(indev, g ? g : group);
            break;
        }
        indev = lv_indev_get_next(indev);
    }
}

/* 安全删除加载弹窗：检查有效性后直接删除，清空指针防二次触发。
 * lv_obj_delete 会触发 LV_EVENT_DELETE，group_delete_cb 自动释放 group。 */
static void safe_delete_loading_dialog(lv_obj_t **pp)
{
    if (!pp || !*pp) return;
    lv_obj_t *obj = *pp;
    *pp = NULL;
    if (lv_obj_is_valid(obj)) lv_obj_delete(obj);
}

/* 创建通用加载弹窗（spinner + 提示文字 + 取消按钮）。
 * 弹窗持有独立 lv_group，销毁时由 group_delete_cb 自动释放，编码器自动切入弹窗 group。 */
static lv_obj_t *create_loading_dialog(const char *title_text, lv_event_cb_t cancel_cb)
{
    lv_obj_t *cont = lv_obj_create(lv_screen_active());
    lv_obj_set_size(cont, 200, 150);
    lv_obj_center(cont);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_color(cont, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_border_width(cont, 2, 0);
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *spinner = lv_spinner_create(cont);
    lv_obj_set_size(spinner, 50, 50);

    lv_obj_t *lbl = lv_label_create(cont);
    lv_label_set_text(lbl, title_text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);

    lv_obj_t *cancel_btn = lv_button_create(cont);
    lv_obj_set_size(cancel_btn, 80, 32);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x555555), 0);
    lv_obj_add_event_cb(cancel_btn, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);

    /* 独立 group 隔离主列表编码器，弹窗销毁时由 group_delete_cb 自动释放 */
    lv_group_t *lg = lv_group_create();
    lv_group_add_obj(lg, cancel_btn);
    lv_obj_add_event_cb(cont, group_delete_cb, LV_EVENT_DELETE, lg);
    indev_set_group(lg);
    lv_group_focus_obj(cancel_btn);
    return cont;
}

/* 创建通用结果弹窗（文字提示 + OK 按钮）。
 * 弹窗持有独立 lv_group，关闭时由 group_delete_cb 自动释放，修复原有 group 泄漏。 */
static lv_obj_t *create_result_dialog(const char *text, uint32_t text_color)
{
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

    lv_obj_t *lbl = lv_label_create(mbox);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(text_color), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_simhei_16, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *ok_btn = lv_button_create(mbox);
    lv_obj_set_size(ok_btn, 80, 32);
    lv_obj_set_style_bg_color(ok_btn, lv_color_hex(0xe94560), 0);
    lv_obj_add_event_cb(ok_btn, close_btn_cb, LV_EVENT_CLICKED, mbox);
    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "OK");
    lv_obj_center(ok_lbl);

    /* 独立 group，弹窗关闭时自动释放，修复原有 group 泄漏 */
    lv_group_t *pg = lv_group_create();
    lv_group_add_obj(pg, ok_btn);
    lv_obj_add_event_cb(mbox, group_delete_cb, LV_EVENT_DELETE, pg);
    indev_set_group(pg);
    lv_group_focus_obj(ok_btn);
    return mbox;
}

// 天气查询队列
typedef struct {
    bool success;
    weather_data_t data;
} weather_result_t;

static QueueHandle_t weather_result_queue = NULL;
static bool weather_fetching = false;
static volatile bool s_weather_cancelled = false;

/* BLE 异步启动结果（task → LVGL timer 消费）*/
typedef enum {
    BLE_START_OK = 0,
    BLE_START_FAIL_INIT,
    BLE_START_FAIL_ADV,
} ble_start_result_t;
static QueueHandle_t ble_start_result_queue = NULL;

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
    indev_set_group(group);
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
    indev_set_group(wg);
    lv_group_focus_obj(back_btn);
    lv_obj_add_event_cb(scr, group_delete_cb, LV_EVENT_DELETE, wg);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

static void weather_cancel_btn_cb(lv_event_t *e)
{
    s_weather_cancelled = true;
    weather_fetching = false;
    safe_delete_loading_dialog(&weather_spinner_cont);
    indev_set_group(group);
    /* drain 已有结果，避免迟到的旧结果污染下一次 fetch */
    weather_result_t drained;
    while (xQueueReceive(weather_result_queue, &drained, 0) == pdTRUE) {}
}

static void weather_fetch_task(void *arg)
{
    weather_result_t res;
    res.success = weather_fetch(&res.data);
    /* 取消后不投递结果，避免主循环弹出天气界面 */
    if (!s_weather_cancelled) {
        xQueueSend(weather_result_queue, &res, 0);
    }
    psram_task_exit();
}

// ================================================================
// 聊天助手界面
// ================================================================
typedef struct {
    bool success;
    model_result_t data;
} chat_result_t;

static QueueHandle_t chat_result_queue = NULL;
static bool chat_fetching = false;
static lv_obj_t *chat_log_label = NULL;   // 对话记录
static lv_obj_t *chat_input = NULL;        // 输入框
static lv_obj_t *chat_keyboard = NULL;     // 键盘
static lv_obj_t *chat_spinner = NULL;      // 加载动画
static lv_obj_t *chat_scr = NULL;          // 聊天屏幕
static char chat_log[1024] = "";           // 累积对话文本

/* 追加一行到 chat_log，满了按行滚动丢弃最早记录。
 * 调用方应保证 line 以 '\n' 结尾；'\n' 是 ASCII 字符，在 UTF-8 中不会与
 * 多字节字符的续字节冲突，故按 '\n' 切分不会截断汉字。 */
static void chat_log_append(const char *line)
{
    if (!line || !*line) return;
    size_t cap     = sizeof(chat_log);       /* 含末尾 '\0' */
    size_t used    = strlen(chat_log);
    size_t add_len = strlen(line);

    /* 如果新消息单条就超过缓冲，直接清空并截断存入 */
    if (add_len >= cap) {
        memcpy(chat_log, line, cap - 1);
        chat_log[cap - 1] = '\0';
        return;
    }

    /* 空间不够时按行丢弃最早记录 */
    while (used + add_len + 1 > cap) {
        char *nl = strchr(chat_log, '\n');
        if (!nl) {
            /* 没有换行符就整体清空（保护性分支）*/
            chat_log[0] = '\0';
            used = 0;
            break;
        }
        size_t drop = (size_t)(nl - chat_log) + 1;  /* 含 '\n' */
        memmove(chat_log, nl + 1, used - drop + 1); /* +1 拷贝 '\0' */
        used -= drop;
    }

    memcpy(chat_log + used, line, add_len + 1);
}

static void chat_fetch_task(void *arg)
{
    char *msg = (char *)arg;
    ESP_LOGI("CHAT", "chat_task 启动, 剩余堆: %lu", esp_get_free_heap_size());
    chat_result_t res;
    res.success = model_chat(msg, &res.data);
    free(msg);
    xQueueSend(chat_result_queue, &res, 0);
    psram_task_exit();
}

static void chat_send_cb(lv_event_t *e)
{
    if (chat_fetching) return;
    const char *txt = lv_textarea_get_text(chat_input);
    if (!txt || strlen(txt) == 0) return;

    // 追加用户消息到日志
    char user_line[MODEL_MAX_INPUT + 8];
    snprintf(user_line, sizeof(user_line), "You: %s\n", txt);
    chat_log_append(user_line);
    lv_label_set_text(chat_log_label, chat_log);

    // 复制输入内容给任务
    char *msg = malloc(MODEL_MAX_INPUT);
    if (!msg) return;
    strncpy(msg, txt, MODEL_MAX_INPUT - 1);
    lv_textarea_set_text(chat_input, "");

    // 显示加载动画
    chat_fetching = true;
    if (chat_spinner && lv_obj_is_valid(chat_spinner)) lv_obj_delete(chat_spinner);
    chat_spinner = lv_spinner_create(chat_scr);
    lv_obj_set_size(chat_spinner, 30, 30);
    lv_obj_align(chat_spinner, LV_ALIGN_TOP_RIGHT, -5, 5);

    /* PSRAM 栈 16KB + cleaner 自动回收 */
    BaseType_t ret = xTaskCreatePSRAM(chat_fetch_task, "chat_task", 16384, msg, 3, NULL);
    if (ret != pdPASS) {
        ESP_LOGE("CHAT", "chat_task 创建失败");
        chat_fetching = false;
        if (chat_spinner && lv_obj_is_valid(chat_spinner)) {
            lv_obj_delete(chat_spinner);
            chat_spinner = NULL;
        }
        free(msg);
    }
}

static void chat_kb_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        // 按下 Enter/OK 发送
        chat_send_cb(e);
    } else if (code == LV_EVENT_CANCEL) {
        // 按下键盘 × 键，触发返回
        chat_back_btn_cb(e);
    }
}

static void chat_back_btn_cb(lv_event_t *e)
{
    // 清空对话记录
    memset(chat_log, 0, sizeof(chat_log));
    chat_fetching = false;

    /* 排空 chat_result_queue：用户退出时后台 chat_fetch_task 可能还没完成，
     * 如果不清空，下次进入聊天后定时器会先消费这条"上一轮的残留回复"，
     * 显示顺序错乱（新问题下面先出现上一轮的答案）。 */
    if (chat_result_queue) {
        chat_result_t drop;
        while (xQueueReceive(chat_result_queue, &drop, 0) == pdTRUE) {
            /* 直接丢弃，不处理 */
        }
    }

    chat_log_label = NULL;
    chat_input = NULL;
    chat_keyboard = NULL;
    chat_spinner = NULL;
    chat_scr = NULL;
    lv_screen_load_anim(lv_obj_get_screen(list), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
    indev_set_group(group);
}

static void chat_result_check_cb(lv_timer_t *t)
{
    // 聊天界面已关闭，删除 timer
    if (!chat_log_label || !lv_obj_is_valid(chat_log_label)) {
        lv_timer_delete(t);
        return;
    }

    chat_result_t res;
    if (xQueueReceive(chat_result_queue, &res, 0) != pdTRUE) return;

    chat_fetching = false;
    if (chat_spinner && lv_obj_is_valid(chat_spinner)) {
        lv_obj_delete(chat_spinner);
        chat_spinner = NULL;
    }

    if (!chat_log_label || !lv_obj_is_valid(chat_log_label)) return;

    if (res.success) {
        char ai_line[MODEL_MAX_OUTPUT + 8];
        snprintf(ai_line, sizeof(ai_line), "AI: %s\n", res.data.output);
        chat_log_append(ai_line);
    } else {
        char err_line[80];
        snprintf(err_line, sizeof(err_line), "Error: %s\n", res.data.error_msg);
        chat_log_append(err_line);
    }
    lv_label_set_text(chat_log_label, chat_log);
    // 滚动到底部
    lv_obj_scroll_to_y(lv_obj_get_parent(chat_log_label), LV_COORD_MAX, LV_ANIM_ON);
}

// ================================================================
// 语音助手界面
// ================================================================
static lv_obj_t *asr_result_label = NULL;
static lv_obj_t *asr_status_label = NULL;
static lv_obj_t *asr_btn = NULL;
static lv_obj_t *asr_scr = NULL;
static bool asr_recording = false;
static bool asr_processing = false;
static uint32_t asr_audio_len = 0;

typedef struct {
    bool success;
    asr_result_t data;
} asr_task_result_t;

static QueueHandle_t asr_result_queue = NULL;
static QueueHandle_t asr_llm_result_queue = NULL;  /* ASR→LLM 结果队列 */

static void asr_recognize_task(void *arg)
{
    uint32_t audio_len = *(uint32_t *)arg;
    free(arg);
    asr_task_result_t res;
    res.success = asr_recognize(audio_len, &res.data);
    xQueueSend(asr_result_queue, &res, 0);
    psram_task_exit();
}

/* ASR→LLM 后台任务：将 ASR 识别文字送入模型，结果通过队列返回 LVGL 线程 */
static void asr_llm_task(void *arg)
{
    char *asr_text = (char *)arg;
    chat_result_t res;  /* 复用 chat_result_t */
    res.success = model_chat(asr_text, &res.data);
    free(asr_text);
    /* LLM 回复成功后，调用百度 TTS 合成语音并播放 */
    if (res.success && strlen(res.data.output) > 0) {
        tts_speak(res.data.output);
    }
    xQueueSend(asr_llm_result_queue, &res, 0);
    psram_task_exit();
}

static void asr_btn_cb(lv_event_t *e)
{
    if (asr_processing) return;
    if (!asr_recording) {
        asr_recording = true;
        asr_record_start();
        lv_label_set_text(asr_status_label, "录音中...");
        lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0xe94560), 0);
        lv_obj_t *lbl = lv_obj_get_child(asr_btn, 0);
        if (lbl) lv_label_set_text(lbl, "停止录音");
    } else {
        asr_recording = false;
        asr_audio_len = asr_record_stop();
        lv_obj_t *lbl = lv_obj_get_child(asr_btn, 0);
        if (lbl) lv_label_set_text(lbl, "开始录音");
        if (asr_audio_len == 0) {
            lv_label_set_text(asr_status_label, "录音太短，请重试");
            lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x16213e), 0);
            return;
        }
        asr_processing = true;
        lv_label_set_text(asr_status_label, "识别中...");
        lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x555555), 0);
        uint32_t *len_arg = malloc(sizeof(uint32_t));
        if (!len_arg) {
            /* DRAM 紧张：malloc 失败，安全回退（不解引用空指针） */
            lv_label_set_text(asr_status_label, "内存不足");
            lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x16213e), 0);
            asr_processing = false;
            return;
        }
        *len_arg = asr_audio_len;
        BaseType_t ret = xTaskCreatePSRAM(asr_recognize_task, "asr_task", 16384, len_arg, 3, NULL);
        if (ret != pdPASS) {
            lv_label_set_text(asr_status_label, "内存不足");
            asr_processing = false;
            free(len_arg);
        }
    }
}

static void asr_timer_cb(lv_timer_t *t)
{
    if (!asr_scr || !lv_obj_is_valid(asr_scr)) { lv_timer_delete(t); return; }
    /* 录音由 asr_rec_task 后台任务处理，不在 LVGL 上下文读 I2S */
    if (!asr_result_queue) return;

    /* --- 阶段1：收到 ASR 识别结果，转发给 LLM --- */
    asr_task_result_t asr_res;
    if (xQueueReceive(asr_result_queue, &asr_res, 0) == pdTRUE) {
        if (asr_res.success && strlen(asr_res.data.result) > 0) {
            lv_label_set_text(asr_status_label, "思考中...");
            /* 将识别文字复制给 LLM 任务（独立 heap 分配，任务内 free） */
            char *text = malloc(ASR_MAX_RESULT);
            if (text) {
                strncpy(text, asr_res.data.result, ASR_MAX_RESULT - 1);
                text[ASR_MAX_RESULT - 1] = '\0';  /* 确保字符串以 null 结尾 */
                if (!asr_llm_result_queue)
                    asr_llm_result_queue = xQueueCreate(2, sizeof(chat_result_t));
                BaseType_t ret = xTaskCreatePSRAM(asr_llm_task, "asr_llm", 16384, text, 3, NULL);
                if (ret != pdPASS) {
                    /* 任务创建失败：释放内存，恢复状态，防止界面卡死 */
                    free(text);
                    lv_label_set_text(asr_result_label, "内存不足");
                    lv_label_set_text(asr_status_label, "错误");
                    lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x16213e), 0);
                    asr_processing = false;
                }
            } else {
                /* malloc 失败：恢复状态 */
                lv_label_set_text(asr_result_label, "内存不足");
                lv_label_set_text(asr_status_label, "错误");
                lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x16213e), 0);
                asr_processing = false;
            }
        } else {
            /* ASR 识别失败，直接显示错误，不送 LLM */
            lv_label_set_text(asr_result_label, "识别失败");
            lv_label_set_text(asr_status_label, asr_res.data.error_msg);
            lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x16213e), 0);
            asr_processing = false;
        }
    }

    /* --- 阶段2：收到 LLM 回复，显示到界面 --- */
    if (!asr_llm_result_queue) return;
    chat_result_t llm_res;
    if (xQueueReceive(asr_llm_result_queue, &llm_res, 0) != pdTRUE) return;

    asr_processing = false;
    lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x16213e), 0);
    if (llm_res.success) {
        lv_label_set_text(asr_result_label, llm_res.data.output);
        lv_label_set_text(asr_status_label, "完成");
    } else {
        lv_label_set_text(asr_result_label, "模型错误");
        lv_label_set_text(asr_status_label, llm_res.data.error_msg);
    }
}

static void asr_back_cb(lv_event_t *e)
{
    if (asr_recording) asr_record_stop();
    asr_recording = false;
    asr_processing = false;
    asr_scr = NULL;
    asr_result_label = NULL;
    asr_status_label = NULL;
    asr_btn = NULL;

    /* 排空两个 result queue：用户退出时后台 asr_recognize_task / asr_llm_task
     * 可能还没跑完，如果不清空，下次进入界面会先消费上一轮的残留结果，
     * 显示给用户看到错乱的"上次提问的回答"。
     * queue 本身保留不删（复用避免重建），只清空其中的未消费消息。 */
    if (asr_result_queue) {
        asr_task_result_t drop;
        while (xQueueReceive(asr_result_queue, &drop, 0) == pdTRUE) { }
    }
    if (asr_llm_result_queue) {
        chat_result_t drop;
        while (xQueueReceive(asr_llm_result_queue, &drop, 0) == pdTRUE) { }
    }

    lv_screen_load_anim(lv_obj_get_screen(list), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
    indev_set_group(group);
}

// ================================================================
// 语音命令开关（ESP-SR 离线命令词识别）
//
// 类似蓝牙开关：菜单点一下"语音命令"就 toggle 开/关，无独立界面。
//   - 开启：暂停 WiFi（释放 DRAM）→ esp_sr_init → start_listening
//   - 关闭：stop_listening → esp_sr_deinit → 恢复 WiFi
//   - 识别到命令：自动停止 SR → 异步等待资源就绪 → 跳转到对应功能
//
// 识别 → 跳转的核心流程（sr_dispatch_task）：
//   1. esp_sr_deinit 释放 ~100KB DRAM（同步阻塞 ~2s）
//   2. 如需 WiFi（聊天/语音助手/天气/智能设备）：恢复 WiFi 并轮询等待
//      最多 8 秒；超时则按"未联网"处理弹提示
//   3. 投递目标命令 ID 回 LVGL 线程（sr_pending_action 队列）
//   4. LVGL timer 拿到后调 show_xxx_screen 打开功能
//
//   全程串行化保证 SR 资源释放 + WiFi 就绪 + 功能启动按时序进行，
//   避免"识别后立刻判 WiFi 没连"或"BLE init 时 SR 还占着 DRAM"的竞态。
//
// WiFi 守卫：需要 WiFi 的功能（天气/聊天/语音助手/智能设备）若识别时未联网，
//   只弹"请连接 WiFi"提示，不开启功能。
// ================================================================

/* SR 状态机：
 *   IDLE      → 未启动
 *   STARTING  → 启动任务在跑（init+start_listening）
 *   ACTIVE    → 正在监听
 *   DISPATCH  → 已识别到命令，正在 teardown + 等 WiFi（不再分发新命令）
 *   STOPPING  → 用户主动关闭中
 * 用枚举替代多个布尔标志，避免 toggle 双击竞态（Bug 4）。 */
typedef enum {
    SR_STATE_IDLE = 0,
    SR_STATE_STARTING,
    SR_STATE_ACTIVE,
    SR_STATE_DISPATCH,
    SR_STATE_STOPPING,
} sr_state_t;

/* 用 atomic 简单起见用 volatile + 单写者约定（LVGL 线程是状态写入唯一来源，
 * 后台任务只读 + CAS-like 写）。FreeRTOS volatile uint32_t 读写在 ESP32-S3
 * 上是原子的（4 字节对齐 + 单核访问），不需要额外 atomic 类型。 */
static volatile sr_state_t sr_state = SR_STATE_IDLE;

/* SR 启用时是否暂停了 WiFi：teardown 时据此决定是否恢复 WiFi。 */
static bool sr_wifi_was_active = false;

/* SR 启动结果（异步任务投递回 LVGL timer）*/
typedef enum {
    SR_START_OK = 0,
    SR_START_FAIL_INIT,
    SR_START_FAIL_LISTEN,
} sr_start_result_t;
static QueueHandle_t sr_start_result_queue = NULL;

/* SR 识别结果（detect 任务回调投递回 LVGL timer）*/
typedef struct {
    int  command_id;
    float probability;
    char command_str[64];
} sr_cmd_result_t;
static QueueHandle_t sr_cmd_result_queue = NULL;

/* dispatch 任务结果（teardown 完成 + WiFi 就绪后通知 LVGL 线程开功能）。
 * 单 slot 队列：DISPATCH 期间不会有第二条命令被分发（state 机保证）。
 *
 * dispatch_failed=true 表示后台任务无法启动（malloc/task create 失败），
 * timer 收到此标志后只切回 IDLE，不删 dialog 也不开功能。 */
typedef struct {
    int  command_id;
    bool wifi_ok;          /* 需 WiFi 的命令：true 表示 WiFi 已就绪 */
    bool needs_wifi;       /* 该命令是否需要 WiFi */
    bool dispatch_failed;  /* fallback 标志：worker 启动失败 */
} sr_pending_action_t;
static QueueHandle_t sr_pending_action_queue = NULL;

/* SR 状态弹窗：进入"启动中"时显示，启动结果到达后切换为成功/失败提示。
 * 由用户 OK 关闭，或 sr_dispatch 主动关掉以让位功能屏幕。 */
static lv_obj_t *sr_status_dialog = NULL;
static void sr_status_dialog_delete_cb(lv_event_t *e)
{
    (void)e;
    sr_status_dialog = NULL;
}

/* ESP-SR 回调（运行在 detect 任务上下文）→ 通过队列通知 LVGL */
static void sr_cmd_result_cb(int id, const char *text, float prob)
{
    if (!sr_cmd_result_queue) return;
    sr_cmd_result_t res = {0};
    res.command_id = id;
    res.probability = prob;
    if (text) strncpy(res.command_str, text, sizeof(res.command_str) - 1);
    xQueueSend(sr_cmd_result_queue, &res, 0);
}

/* 是否需要 WiFi 的命令分类。dispatch 任务先 teardown SR 再据此决定
 * 是否等 WiFi。集中放一处方便维护。 */
static bool sr_cmd_needs_wifi(int command_id)
{
    switch (command_id) {
        case 2:  /* 打开天气和日期 */
        case 4:  /* 打开聊天助手 */
        case 5:  /* 打开语音助手 */
        case 9:  /* 打开智能设备 */
            return true;
        default:
            return false;
    }
}

/* ----------------------------------------------------------------
 * dispatch 任务：识别成功后的串行清理 + 等待 + 投递。
 *   1. 同步 esp_sr_deinit（~2s）
 *   2. 如需 WiFi 则 wifi_resume_after_game + 轮询等 CONNECTED（最多 8s）
 *   3. 投递 sr_pending_action 给 LVGL timer
 *   全程在独立任务做，LVGL 线程立刻返回继续渲染。
 * ---------------------------------------------------------------- */
typedef struct {
    int command_id;
} sr_dispatch_arg_t;

static void sr_dispatch_task(void *arg)
{
    sr_dispatch_arg_t *a = (sr_dispatch_arg_t *)arg;
    int command_id = a ? a->command_id : 0;
    if (a) free(a);

    bool needs_wifi = sr_cmd_needs_wifi(command_id);
    bool was_active = sr_wifi_was_active;
    sr_wifi_was_active = false;

    /* 1. 完整释放 SR 资源（~100KB DRAM）。同步等模型/AFE/任务退出。 */
    esp_sr_deinit();
    ESP_LOGI("SR_CMD", "SR 已释放, 准备执行命令 id=%d (needs_wifi=%d)",
             command_id, needs_wifi);

    /* 2. 处理 WiFi：
     *    - 命令需 WiFi 且 SR 启动前 WiFi 在跑 → resume + 等连上
     *    - 命令需 WiFi 但 SR 启动前就没 WiFi → 直接 wifi_ok=false（等也没用）
     *    - 命令不需 WiFi 但 SR 启动前 WiFi 在跑 → 仅 resume，不等（不阻塞）
     *    - 命令不需 WiFi 且 SR 启动前没 WiFi → 啥也不做 */
    bool wifi_ok = !needs_wifi;  /* 不需要 WiFi 的命令默认"OK"（不阻塞） */

    if (was_active) {
        wifi_resume_after_game();
        if (needs_wifi) {
            /* 轮询等 WiFi 重连（最多 8s）。WiFi 守护任务接管重连，
             * 这里只是观察 status 变化。 */
            for (int i = 0; i < 80; i++) {
                if (wifi_get_status() == WIFI_STATUS_CONNECTED) {
                    wifi_ok = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (!wifi_ok) {
                ESP_LOGW("SR_CMD", "等 WiFi 超时(8s)，按未联网处理");
            }
        }
    } else if (needs_wifi) {
        /* SR 启动前就没 WiFi，命令需要 WiFi：直接判失败 */
        wifi_ok = false;
    }

    /* 3. 投递回 LVGL 线程。LVGL timer 拿到后会切到目标功能。 */
    sr_pending_action_t action = {
        .command_id      = command_id,
        .wifi_ok         = wifi_ok,
        .needs_wifi      = needs_wifi,
        .dispatch_failed = false,
    };
    if (sr_pending_action_queue) {
        xQueueSend(sr_pending_action_queue, &action, 0);
    }

    /* 不在这里设 state = IDLE！由 LVGL timer 处理完 pending_action 后再设。
     * 原因：state 切换必须全部在 LVGL 线程做，否则会出现：
     *   1. 此处 state = IDLE
     *   2. 用户点菜单（输入事件排队）
     *   3. LVGL 先处理输入：show_sr_cmd_screen 看 IDLE → 开新 SR
     *   4. LVGL 再处理 timer：执行 pending_action → 切屏 + 关 SR dialog
     * 这样新开的 SR 状态弹窗被误删，且屏幕被切走，体验混乱。 */
    vTaskDelete(NULL);
}

/* 启动 dispatch 任务。仅在 SR_STATE_ACTIVE 时由 timer 调用，
 * 调用前会把 sr_state 设为 SR_STATE_DISPATCH（一次性）。
 *
 * 失败处理：
 *   malloc / xTaskCreate 失败时不能在 LVGL 线程做同步 esp_sr_deinit
 *   （会阻塞 ~2s 卡死 UI）。退而求其次：投递 dispatch_failed 标志的
 *   pending_action，让 timer 把 state 切回 IDLE。
 *   SR 资源会泄漏（未 deinit），用户下次再开会失败 —— 详见
 *   fail_post_pending 处的注释。 */
static void sr_kick_dispatch(int command_id)
{
    sr_dispatch_arg_t *arg = malloc(sizeof(*arg));
    if (!arg) {
        ESP_LOGE("SR_CMD", "dispatch arg 分配失败");
        goto fail_post_pending;
    }
    arg->command_id = command_id;
    if (xTaskCreate(sr_dispatch_task, "sr_disp", 4096, arg, 3, NULL) == pdPASS) {
        return;
    }
    ESP_LOGE("SR_CMD", "dispatch task 创建失败");
    free(arg);

fail_post_pending:
    /* 投递 dispatch_failed 标志：execute_action 看到此标志会跳过功能执行，
     * 也不会删 sr_status_dialog（用户刚显示的"已关闭"提示需要保留）。
     * timer 处理完此 action 后切回 IDLE，避免 LVGL 阻塞。
     *
     * 注意：SR 资源此处仍占用（无法在 LVGL 线程同步 deinit）。state 已
     * 切回 IDLE 但底层任务/AFE 还在跑，用户下次菜单点击会走"开启"分支
     * 调 esp_sr_init —— 此时 esp_sr.c 内部 s_listening=true，start_listening
     * 会失败。用户需先 toggle 一次（state=IDLE→STARTING→FAIL→IDLE 会
     * 触发 sr_kick_stop 走 deinit 路径）才能恢复。这是已知降级行为，
     * malloc/task create 失败本身就是 OOM 边缘场景，不再做更复杂的恢复。 */
    if (sr_pending_action_queue) {
        sr_pending_action_t act = {
            .command_id      = command_id,
            .wifi_ok         = false,
            .needs_wifi      = false,
            .dispatch_failed = true,
        };
        xQueueSend(sr_pending_action_queue, &act, 0);
    }
}

/* 用户主动关闭 SR：teardown 任务（不投 dispatch action）。
 * 仅在 SR_STATE_ACTIVE / SR_STATE_STARTING 时调用，调用前置 STOPPING。 */
typedef struct {
    bool resume_wifi;
} sr_stop_arg_t;

static void sr_stop_task(void *arg)
{
    sr_stop_arg_t *a = (sr_stop_arg_t *)arg;
    bool resume_wifi = a ? a->resume_wifi : false;
    if (a) free(a);

    esp_sr_deinit();
    if (resume_wifi) {
        ESP_LOGI("SR_CMD", "用户关闭 SR，恢复 WiFi");
        wifi_resume_after_game();
    }
    sr_state = SR_STATE_IDLE;
    vTaskDelete(NULL);
}

/* 失败时同 sr_kick_dispatch：不在 LVGL 线程同步 deinit，投递 pending_action
 * 让 timer 切回 IDLE。 */
static void sr_kick_stop(void)
{
    bool actually_resume = sr_wifi_was_active;
    sr_wifi_was_active = false;

    sr_stop_arg_t *arg = malloc(sizeof(*arg));
    if (!arg) {
        ESP_LOGE("SR_CMD", "stop arg 分配失败");
        goto fail_post_pending;
    }
    arg->resume_wifi = actually_resume;
    if (xTaskCreate(sr_stop_task, "sr_stop", 4096, arg, 3, NULL) == pdPASS) {
        return;
    }
    ESP_LOGE("SR_CMD", "stop task 创建失败");
    free(arg);

fail_post_pending:
    /* 投递 dispatch_failed=true：execute_action 会跳过执行，特别是不会删
     * sr_status_dialog（用户刚通过 show_sr_cmd_screen 显示了"已关闭"
     * 提示，必须保留让用户看到关闭意图已被记录）。
     *
     * SR 资源此处未释放，下次重新开启同 sr_kick_dispatch 注释（已知降级）。 */
    if (sr_pending_action_queue) {
        sr_pending_action_t act = {
            .command_id      = 0,
            .wifi_ok         = false,
            .needs_wifi      = false,
            .dispatch_failed = true,
        };
        xQueueSend(sr_pending_action_queue, &act, 0);
    }
}

/* 在 LVGL 线程执行真正的功能跳转。dispatch task 投递 sr_pending_action
 * 后，timer 取到这个结构 → 调用本函数。
 *
 * 此时 SR 已完全释放（~100KB DRAM 已归还），WiFi 已就绪（若需要），
 * 跳转目标资源无冲突。 */
static void sr_execute_action(const sr_pending_action_t *act)
{
    /* fallback 路径：worker 启动失败（malloc/xTaskCreate 失败），SR 资源
     * 仍然占用，且 sr_status_dialog 可能是用户刚显示的"已关闭"提示
     * （sr_kick_stop fallback 走这里）。这种情况下不能删 dialog 也不能
     * 开任何功能，timer 把 state 切回 IDLE 后直接返回。 */
    if (act->dispatch_failed) {
        ESP_LOGW("SR_CMD", "dispatch worker 启动失败，跳过功能执行");
        return;
    }

    /* 关掉 SR 状态弹窗，让位给目标功能 */
    if (sr_status_dialog && lv_obj_is_valid(sr_status_dialog)) {
        lv_obj_delete(sr_status_dialog);
        sr_status_dialog = NULL;
    }

    /* 需 WiFi 但未就绪：弹提示后退出 */
    if (act->needs_wifi && !act->wifi_ok) {
        const char *zh;
        switch (act->command_id) {
            case 2:  zh = "天气查询"; break;
            case 4:  zh = "聊天";     break;
            case 5:  zh = "语音助手"; break;
            case 9:  zh = "智能设备"; break;
            default: zh = "该功能";   break;
        }
        char msg[80];
        snprintf(msg, sizeof(msg), "请先连接 WiFi\n%s 需要联网", zh);
        create_result_dialog(msg, 0xffaa00);
        return;
    }

    switch (act->command_id) {
        case 1:  /* 打开环境监测 */
            create_result_dialog("环境监测\n功能开发中", 0x00cfff);
            break;
        case 2:  /* 打开天气和日期 */
            if (weather_fetching) return;
            s_weather_cancelled = false;
            { weather_result_t drained; while (xQueueReceive(weather_result_queue, &drained, 0) == pdTRUE) {} }
            weather_fetching = true;
            weather_spinner_cont = create_loading_dialog("Fetching Weather...", weather_cancel_btn_cb);
            xTaskCreatePSRAM(weather_fetch_task, "weather_task", 16384, NULL, 3, NULL);
            break;
        case 3:  /* 打开游戏 */
            show_game_screen();
            break;
        case 4:  /* 打开聊天助手 */
            show_chat_screen();
            break;
        case 5:  /* 打开语音助手 */
            show_asr_screen();
            break;
        case 6:  /* 打开蓝牙 */
            show_ble_screen();
            break;
        case 7:  /* 打开音乐 */
            show_music_screen();
            break;
        case 8:  /* 打开音量 */
            show_volume_screen();
            break;
        case 9:  /* 打开智能设备 */
            show_bemfa_screen();
            break;
        default:
            ESP_LOGW("SR_CMD", "未知命令 ID=%d", act->command_id);
            break;
    }
}

/* LVGL timer：轮询三个队列：
 *   - 启动结果（start_task → 显示成功/失败弹窗）
 *   - 识别结果（detect_task → 触发 dispatch task）
 *   - 待执行命令（dispatch_task → 真正打开功能） */
static void sr_cmd_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* 启动结果 */
    if (sr_start_result_queue) {
        sr_start_result_t sr_res;
        if (xQueueReceive(sr_start_result_queue, &sr_res, 0) == pdTRUE) {
            if (sr_status_dialog && lv_obj_is_valid(sr_status_dialog)) {
                lv_obj_delete(sr_status_dialog);
                sr_status_dialog = NULL;
            }
            switch (sr_res) {
                case SR_START_OK:
                    sr_state = SR_STATE_ACTIVE;
                    sr_status_dialog = create_result_dialog(
                        "语音命令已开启\n请说出命令", 0x00cfff);
                    break;
                case SR_START_FAIL_INIT:
                    sr_status_dialog = create_result_dialog(
                        "语音识别初始化失败\n检查 model 分区", 0xff0000);
                    /* 启动失败需要把 WiFi 还原回去 */
                    sr_state = SR_STATE_STOPPING;
                    sr_kick_stop();
                    break;
                case SR_START_FAIL_LISTEN:
                    sr_status_dialog = create_result_dialog(
                        "语音监听启动失败", 0xff0000);
                    sr_state = SR_STATE_STOPPING;
                    sr_kick_stop();
                    break;
            }
            if (sr_status_dialog) {
                lv_obj_add_event_cb(sr_status_dialog, sr_status_dialog_delete_cb,
                                    LV_EVENT_DELETE, NULL);
            }
        }
    }

    /* 识别结果 → 触发 dispatch */
    if (sr_cmd_result_queue) {
        sr_cmd_result_t res;
        if (xQueueReceive(sr_cmd_result_queue, &res, 0) == pdTRUE) {
            /* 只在 ACTIVE 状态下接受新命令；其他状态丢弃（防止 dispatch
             * 进行中又收到第二条识别结果导致并发） */
            if (sr_state == SR_STATE_ACTIVE) {
                ESP_LOGI("SR_CMD", "命令分发: id=%d, str=%s, prob=%.2f",
                         res.command_id, res.command_str, res.probability);
                sr_state = SR_STATE_DISPATCH;
                sr_kick_dispatch(res.command_id);
            } else {
                ESP_LOGW("SR_CMD", "状态=%d，忽略识别结果 id=%d",
                         (int)sr_state, res.command_id);
            }
        }
    }

    /* 待执行命令 → 切换到目标功能 */
    if (sr_pending_action_queue) {
        sr_pending_action_t act;
        if (xQueueReceive(sr_pending_action_queue, &act, 0) == pdTRUE) {
            sr_execute_action(&act);
            /* 执行完才切回 IDLE。这一步必须在 LVGL 线程做，避免与
             * show_sr_cmd_screen 的 state 检查竞态（dispatch worker
             * 提前设 IDLE 会导致 toggle 重开 → 屏幕被 action 抢走）。 */
            sr_state = SR_STATE_IDLE;
        }
    }
}

/* SR 异步启动任务：避免 wifi_suspend + esp_sr_init 阻塞 LVGL 线程。
 * 总耗时可达 ~2.5s（300ms wifi 释放 + 2s 模型加载 + AFE 创建）。 */
static void sr_start_task(void *arg)
{
    (void)arg;

    /* 1. 暂停 WiFi（如在跑）：ESP-SR + WiFi 共存时内部 DRAM 紧张
     * esp_timer_create 会 ESP_ERR_NO_MEM 崩溃。 */
    if (sr_wifi_was_active) {
        ESP_LOGI("SR_CMD", "WiFi 活跃，先挂起以释放 DRAM");
        wifi_suspend_for_game();
        vTaskDelay(pdMS_TO_TICKS(300));  /* 等 WiFi buffer 归还 */
    }

    /* 2. esp_sr_init：加载模型 + 创建 AFE + MultiNet */
    if (!esp_sr_init()) {
        sr_start_result_t r = SR_START_FAIL_INIT;
        if (sr_start_result_queue) xQueueSend(sr_start_result_queue, &r, 0);
        vTaskDelete(NULL);
        return;
    }

    /* 3. start_listening：创建 read/feed/detect 任务 */
    if (!esp_sr_start_listening(sr_cmd_result_cb)) {
        sr_start_result_t r = SR_START_FAIL_LISTEN;
        if (sr_start_result_queue) xQueueSend(sr_start_result_queue, &r, 0);
        vTaskDelete(NULL);
        return;
    }

    sr_start_result_t r = SR_START_OK;
    if (sr_start_result_queue) xQueueSend(sr_start_result_queue, &r, 0);
    vTaskDelete(NULL);
}

/* 菜单点击"语音命令"：toggle 开关。
 * 状态机保证 STARTING/DISPATCH/STOPPING 中点击不会导致并发。 */
static void show_sr_cmd_screen(void)
{
    /* 中间态：忽略点击，避免双击竞态（Bug 4） */
    if (sr_state == SR_STATE_STARTING ||
        sr_state == SR_STATE_DISPATCH ||
        sr_state == SR_STATE_STOPPING) {
        ESP_LOGW("SR_CMD", "SR 忙(state=%d)，忽略 toggle", (int)sr_state);
        return;
    }

    if (sr_state == SR_STATE_ACTIVE) {
        /* 关闭 */
        sr_state = SR_STATE_STOPPING;
        sr_kick_stop();
        if (sr_status_dialog && lv_obj_is_valid(sr_status_dialog)) {
            lv_obj_delete(sr_status_dialog);
            sr_status_dialog = NULL;
        }
        sr_status_dialog = create_result_dialog("语音命令已关闭", 0x888888);
        if (sr_status_dialog) {
            lv_obj_add_event_cb(sr_status_dialog, sr_status_dialog_delete_cb,
                                LV_EVENT_DELETE, NULL);
        }
        return;
    }

    /* 开启：先创建结果队列（懒加载，避免常驻 RAM）*/
    if (!sr_cmd_result_queue) {
        sr_cmd_result_queue = xQueueCreate(4, sizeof(sr_cmd_result_t));
    }
    if (!sr_start_result_queue) {
        sr_start_result_queue = xQueueCreate(2, sizeof(sr_start_result_t));
    }
    if (!sr_pending_action_queue) {
        sr_pending_action_queue = xQueueCreate(2, sizeof(sr_pending_action_t));
    }
    if (!sr_cmd_result_queue || !sr_start_result_queue || !sr_pending_action_queue) {
        create_result_dialog("内存不足", 0xff0000);
        return;
    }

    /* 记录 WiFi 状态用于后续恢复 */
    wifi_status_t wst = wifi_get_status();
    sr_wifi_was_active = (wst == WIFI_STATUS_CONNECTED ||
                          wst == WIFI_STATUS_CONNECTING ||
                          wst == WIFI_STATUS_RECONNECTING);

    sr_state = SR_STATE_STARTING;

    if (xTaskCreate(sr_start_task, "sr_start", 4096, NULL, 3, NULL) != pdPASS) {
        sr_state = SR_STATE_IDLE;
        create_result_dialog("启动任务创建失败", 0xff0000);
        return;
    }

    const char *msg = sr_wifi_was_active
        ? "正在启动语音识别...\n(暂停 WiFi 中)"
        : "正在启动语音识别...";
    sr_status_dialog = create_result_dialog(msg, 0x1E90FF);
    if (sr_status_dialog) {
        lv_obj_add_event_cb(sr_status_dialog, sr_status_dialog_delete_cb,
                            LV_EVENT_DELETE, NULL);
    }
}

// ================================================================
// 蓝牙开关（菜单直接切换，无独立界面）
// ================================================================

/* 弹窗对象正在销毁时统一清零全局句柄，覆盖所有销毁路径
 * （OK 按钮 / 父屏切换 / 显式 lv_obj_delete），避免悬空指针 */
static void ble_result_dialog_delete_cb(lv_event_t *e)
{
    (void)e;
    ble_result_dialog = NULL;
}

/* 异步 BLE 启动 task：避免 wifi_full_shutdown_for_ble 的 500ms 阻塞 LVGL 线程 */
typedef struct {
    bool wifi_was_active;
} ble_start_args_t;

static void ble_start_task(void *arg)
{
    ble_start_args_t *args = (ble_start_args_t *)arg;
    bool wifi_was_active = args ? args->wifi_was_active : false;
    if (args) free(args);

    if (wifi_was_active) {
        ESP_LOGI("BLE_SCR", "WiFi 在运行，先关闭以释放 DRAM 给 BLE...");
        wifi_full_shutdown_for_ble();
        vTaskDelay(pdMS_TO_TICKS(500));  /* 等 WiFi 驱动栈完全释放 */
        ESP_LOGI("BLE_SCR", "WiFi 已关闭，DRAM free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }

    if (!ble_prov_init()) {
        ble_prov_deinit();
        ESP_LOGE("BLE_SCR", "BLE 初始化失败，DRAM free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        ble_start_result_t r = BLE_START_FAIL_INIT;
        if (ble_start_result_queue) xQueueSend(ble_start_result_queue, &r, 0);
        vTaskDelete(NULL);
        return;
    }
    ble_prov_set_cred_cb(ble_cred_cb);
    if (!ble_prov_start(NULL)) {
        ble_prov_deinit();
        ESP_LOGE("BLE_SCR", "BLE 广播启动失败");
        ble_start_result_t r = BLE_START_FAIL_ADV;
        if (ble_start_result_queue) xQueueSend(ble_start_result_queue, &r, 0);
    } else {
        ESP_LOGI("BLE_SCR", "BLE 已启动");
        ble_start_result_t r = BLE_START_OK;
        if (ble_start_result_queue) xQueueSend(ble_start_result_queue, &r, 0);
    }
    vTaskDelete(NULL);
}

static void show_ble_screen(void)
{
    if (ble_prov_is_active()) {
        /* 关闭蓝牙，释放 BLE controller 内存。WiFi 不自动恢复，由用户主动连。*/
        ble_prov_stop();
        ble_prov_deinit();
        ble_result_dialog = create_result_dialog("蓝牙已关闭", 0x888888);
        if (ble_result_dialog) {
            lv_obj_add_event_cb(ble_result_dialog, ble_result_dialog_delete_cb, LV_EVENT_DELETE, NULL);
        }
        return;
    }

    /* WiFi 和 BLE 互斥：BLE controller 启动需要 ~33KB 连续 DRAM，
     * WiFi 在跑时 DRAM 已碎片化无法分配。必须 deinit WiFi 释放完整 DRAM。
     * status 一次性读到局部变量，避免 TOCTOU 三连读期间状态变化 */
    wifi_status_t st = wifi_get_status();
    bool wifi_was_active = (st == WIFI_STATUS_CONNECTED ||
                            st == WIFI_STATUS_RECONNECTING ||
                            st == WIFI_STATUS_CONNECTING);

    /* 异步启动：避免 500ms WiFi 释放 + BLE init 阻塞 LVGL 线程
     * UI 立刻显示"启动中..."提示，后台任务做实际工作 */
    ble_start_args_t *args = malloc(sizeof(*args));
    if (!args) {
        ble_result_dialog = create_result_dialog("内存不足", 0xff0000);
        if (ble_result_dialog) {
            lv_obj_add_event_cb(ble_result_dialog, ble_result_dialog_delete_cb, LV_EVENT_DELETE, NULL);
        }
        return;
    }
    args->wifi_was_active = wifi_was_active;

    BaseType_t r = xTaskCreate(ble_start_task, "ble_start", 4096, args, 3, NULL);
    if (r != pdPASS) {
        free(args);
        ble_result_dialog = create_result_dialog("BLE 任务创建失败", 0xff0000);
        if (ble_result_dialog) {
            lv_obj_add_event_cb(ble_result_dialog, ble_result_dialog_delete_cb, LV_EVENT_DELETE, NULL);
        }
        return;
    }

    const char *msg = wifi_was_active
        ? "正在启动蓝牙...\n(关闭 WiFi 中)"
        : "正在启动蓝牙...";
    ble_result_dialog = create_result_dialog(msg, 0x1E90FF);
    if (ble_result_dialog) {
        lv_obj_add_event_cb(ble_result_dialog, ble_result_dialog_delete_cb, LV_EVENT_DELETE, NULL);
    }
}

static void show_asr_screen(void)
{
    /* 语音助手依赖百度在线 ASR/LLM/TTS，必须联网。
     * 未连 WiFi 时入口就拦截，否则后续 HTTP 请求会因 lwIP 未就绪 panic。 */
    wifi_status_t wst = wifi_get_status();
    if (wst != WIFI_STATUS_CONNECTED) {
        create_result_dialog("请先连接 WiFi\n语音助手需要联网", 0xffaa00);
        return;
    }

    if (!asr_result_queue)
        asr_result_queue = xQueueCreate(2, sizeof(asr_task_result_t));

    asr_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(asr_scr, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_pad_all(asr_scr, 0, 0);

    lv_obj_t *title = lv_label_create(asr_scr);
    lv_label_set_text(title, "语音助手");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_simhei_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *back_btn = lv_button_create(asr_scr);
    lv_obj_set_size(back_btn, 40, 24);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe94560), 0);
    lv_obj_add_event_cb(back_btn, asr_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    lv_obj_t *result_cont = lv_obj_create(asr_scr);
    lv_obj_set_size(result_cont, LCD_W - 10, 180);
    lv_obj_align(result_cont, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(result_cont, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(result_cont, 0, 0);
    lv_obj_set_style_pad_all(result_cont, 6, 0);
    lv_obj_set_scroll_dir(result_cont, LV_DIR_VER);

    asr_result_label = lv_label_create(result_cont);
    lv_label_set_text(asr_result_label, "点击下方按钮开始录音");
    lv_obj_set_style_text_color(asr_result_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(asr_result_label, &lv_font_simhei_16, 0);
    lv_label_set_long_mode(asr_result_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(asr_result_label, LCD_W - 22);

    asr_status_label = lv_label_create(asr_scr);
    lv_label_set_text(asr_status_label, "就绪");
    lv_obj_set_style_text_color(asr_status_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(asr_status_label, &lv_font_simhei_16, 0);
    lv_obj_align(asr_status_label, LV_ALIGN_BOTTOM_MID, 0, -50);

    asr_btn = lv_button_create(asr_scr);
    lv_obj_set_size(asr_btn, 120, 40);
    lv_obj_align(asr_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_color(asr_btn, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_border_width(asr_btn, 2, 0);
    lv_obj_add_event_cb(asr_btn, asr_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *btn_lbl = lv_label_create(asr_btn);
    lv_label_set_text(btn_lbl, "开始录音");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(btn_lbl, &lv_font_simhei_16, 0);
    lv_obj_center(btn_lbl);

    lv_group_t *ag = lv_group_create();
    lv_group_add_obj(ag, asr_btn);
    lv_group_add_obj(ag, back_btn);
    indev_set_group(ag);
    lv_group_focus_obj(asr_btn);
    lv_obj_add_event_cb(asr_scr, group_delete_cb, LV_EVENT_DELETE, ag);
    lv_timer_create(asr_timer_cb, 20, NULL);
    lv_screen_load_anim(asr_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

static void show_chat_screen(void)
{
    /* 聊天助手依赖在线大模型 API，必须联网 */
    wifi_status_t wst = wifi_get_status();
    if (wst != WIFI_STATUS_CONNECTED) {
        create_result_dialog("请先连接 WiFi\n聊天需要联网", 0xffaa00);
        return;
    }

    if (!chat_result_queue)
        chat_result_queue = xQueueCreate(2, sizeof(chat_result_t));

    lv_obj_t *scr = lv_obj_create(NULL);
    chat_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    // 顶部标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "聊天助手");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_simhei_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 5);

    // 返回按钮
    lv_obj_t *back_btn = lv_button_create(scr);
    lv_obj_set_size(back_btn, 40, 24);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe94560), 0);
    lv_obj_add_event_cb(back_btn, chat_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    // 对话记录滚动区域
    lv_obj_t *log_cont = lv_obj_create(scr);
    lv_obj_set_size(log_cont, LCD_W, 130);
    lv_obj_align(log_cont, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_set_style_bg_color(log_cont, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(log_cont, 0, 0);
    lv_obj_set_style_pad_all(log_cont, 4, 0);
    lv_obj_set_scroll_dir(log_cont, LV_DIR_VER);

    chat_log_label = lv_label_create(log_cont);
    lv_label_set_text(chat_log_label, chat_log[0] ? chat_log : "开始对话...");
    lv_obj_set_style_text_color(chat_log_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(chat_log_label, &lv_font_simhei_16, 0);
    lv_label_set_long_mode(chat_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(chat_log_label, LCD_W - 12);

    // 输入框
    chat_input = lv_textarea_create(scr);
    lv_obj_set_size(chat_input, LCD_W - 8, 40);
    lv_obj_align(chat_input, LV_ALIGN_TOP_MID, 0, 162);
    lv_textarea_set_placeholder_text(chat_input, "Ask me...");
    lv_textarea_set_one_line(chat_input, true);
    lv_obj_set_style_bg_color(chat_input, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_text_color(chat_input, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_color(chat_input, lv_color_hex(0xe94560), 0);

    // 键盘
    chat_keyboard = lv_keyboard_create(scr);
    lv_obj_set_size(chat_keyboard, LCD_W, 118);
    lv_obj_align(chat_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(chat_keyboard, chat_input);
    lv_obj_add_event_cb(chat_keyboard, chat_kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(chat_keyboard, chat_kb_event_cb, LV_EVENT_CANCEL, NULL);

    // 编码器 group：键盘优先
    lv_group_t *cg = lv_group_create();
    lv_group_add_obj(cg, chat_keyboard);
    lv_group_add_obj(cg, back_btn);
    indev_set_group(cg);
    lv_group_focus_obj(chat_keyboard);

    // 定时检查 AI 回复
    lv_timer_create(chat_result_check_cb, 300, NULL);
    lv_obj_add_event_cb(scr, group_delete_cb, LV_EVENT_DELETE, cg);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
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
    if (ok) wifi_copy_ip(result.ip, sizeof(result.ip));
    else result.ip[0] = '\0';
    // 只有队列还空才发（取消时队列已有cancelled消息）
    xQueueSend(wifi_result_queue, &result, 0);
    wifi_task_handle = NULL;
    psram_task_exit();
}

/* 启动 WiFi 连接流程（弹 spinner + 起后台任务）。
 * 仅由蓝牙配网回调调用：BLE 收到凭据 → deinit BLE → 调本函数连 WiFi。
 * （菜单里已去掉独立的"WiFi 连接"入口，配网后自动触发是唯一入口）
 * 必须在 LVGL 上下文调用（创建 UI 对象不是线程安全的），
 * 非 LVGL 调用方用 lv_async_call 投递过来。 */
static void start_wifi_connection_flow(void)
{
    if (wifi_connecting) return;

    /* 如果屏幕上还留着蓝牙结果弹窗（用户没手动点 OK），先关掉再弹 WiFi spinner，
     * 避免两层弹窗堆叠看不清。lv_obj_is_valid 保护：
     * 用户如果已经手动关过，句柄指向已释放的对象，直接跳过 */
    if (ble_result_dialog && lv_obj_is_valid(ble_result_dialog)) {
        lv_obj_delete(ble_result_dialog);
    }
    ble_result_dialog = NULL;

    wifi_connecting = true;
    wifi_spinner_cont = create_loading_dialog("WiFi Connecting...", cancel_btn_cb);
    xTaskCreatePSRAM(wifi_connect_task, "wifi_task", 6144, NULL, 3, &wifi_task_handle);
}

/* 蓝牙配网回调：BLE 收到凭据后触发（运行在 timer service task）。
 * 不直接碰 LVGL，投递到 LVGL 线程；真正的"deinit BLE + 连 WiFi"
 * 由另一个后台任务完成，避免在 timer 里阻塞。 */
static void ble_cred_apply_task(void *arg)
{
    (void)arg;
    /* 让 BLE notify("凭据已收到") 有时间送达手机，再关蓝牙 */
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI("BLE_CRED", "deinit BLE, 释放 DRAM 给 WiFi...");
    ble_prov_deinit();
    vTaskDelay(pdMS_TO_TICKS(200));

    /* 回到 LVGL 线程启动 WiFi 连接（走标准流程：spinner + 结果弹窗） */
    lv_async_call((lv_async_cb_t)start_wifi_connection_flow, NULL);
    vTaskDelete(NULL);
}

static void ble_cred_cb(const char *ssid, const char *password)
{
    (void)ssid; (void)password;  /* 已经在 wifi_set_credentials 里存好了 */
    ESP_LOGI("BLE_CRED", "收到凭据，启动 deinit+WiFi 流程");
    /* 独立任务跑：不阻塞 timer service task，也不自删 */
    xTaskCreate(ble_cred_apply_task, "ble_apply", 4096, NULL, 4, NULL);
}

static void close_btn_cb(lv_event_t *e)
{
    lv_obj_t *mbox = lv_event_get_user_data(e);
    lv_obj_delete(mbox);  /* group_delete_cb 自动释放 popup_group */
    indev_set_group(group);  /* 恢复主菜单编码器 */
}

static void show_result_box(bool ok, bool cancelled, const char *ip)
{
    wifi_connecting = false;
    safe_delete_loading_dialog(&wifi_spinner_cont);

    if (cancelled) {
        indev_set_group(group);
        return;
    }

    /* 构造结果文字后创建结果弹窗 */
    char msg[64];
    if (ok)
        snprintf(msg, sizeof(msg), "WiFi 已连接\n%s", ip);
    else
        snprintf(msg, sizeof(msg), "WiFi 连接失败");

    create_result_dialog(msg, ok ? 0x00ff00 : 0xff0000);
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
        if (s_weather_cancelled) {
            /* 防御：drain 漏过的迟到结果，取消后不显示天气界面 */
            weather_fetching = false;
            return;
        }
        weather_fetching = false;
        safe_delete_loading_dialog(&weather_spinner_cont);
        if (wresp.success) {
            show_weather_screen(&wresp.data);
        } else {
            /* 构造错误信息后创建结果弹窗 */
            char err_msg[80];
            snprintf(err_msg, sizeof(err_msg), "%s", wresp.data.error_msg);
            create_result_dialog(err_msg, 0xff0000);
        }
    }

    // 检查 BLE 异步启动结果，更新提示弹窗
    ble_start_result_t bresp;
    if (ble_start_result_queue &&
        xQueueReceive(ble_start_result_queue, &bresp, 0) == pdTRUE) {
        /* 先关掉"正在启动蓝牙..."的提示 dialog */
        if (ble_result_dialog && lv_obj_is_valid(ble_result_dialog)) {
            lv_obj_delete(ble_result_dialog);
        }
        ble_result_dialog = NULL;

        const char *msg;
        uint32_t color;
        switch (bresp) {
            case BLE_START_OK:
                msg = "蓝牙已开启\n设备: ESP32-Bot";
                color = 0x1E90FF;
                break;
            case BLE_START_FAIL_INIT:
                msg = "蓝牙初始化失败\nDRAM 不足";
                color = 0xff0000;
                break;
            case BLE_START_FAIL_ADV:
                msg = "蓝牙广播失败";
                color = 0xff0000;
                break;
            default:
                msg = "蓝牙状态未知";
                color = 0xff0000;
                break;
        }
        ble_result_dialog = create_result_dialog(msg, color);
        if (ble_result_dialog) {
            lv_obj_add_event_cb(ble_result_dialog, ble_result_dialog_delete_cb,
                                LV_EVENT_DELETE, NULL);
        }
    }
}

// ================================================================
// 游戏界面：ROM 列表 + 进入 runtime
// ================================================================

/* runtime 任务保活句柄。游戏运行时非 NULL */
static TaskHandle_t s_game_task = NULL;
/* 标记游戏是否正在运行中（runtime 占用屏幕期间 LVGL 不再 flush）*/
static volatile bool s_game_active = false;

/* 游戏退出完成回调（由运行任务退出后 lv_async_call 投递到 LVGL 线程）*/
static void game_task_exited_cb(void *user_data)
{
    s_game_active = false;

    /* 输入恢复到菜单模式：编码器生效、键盘失效 */
    keypad_set_game_mode(false);
    lv_port_indev_set_menu_mode(true);

    /* disp 已在 game_run_task 退出前先 resume，这里只做强制整屏重绘，
     * 覆盖游戏期间残留画面 */
    lv_obj_invalidate(lv_screen_active());
    /* 回到菜单 */
    lv_screen_load_anim(lv_obj_get_screen(list), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
    indev_set_group(group);

    /* 游戏结束后恢复 WiFi（BLE 不自动恢复，用户需要时从菜单手动开启） */
    ESP_LOGI("GAME", "游戏退出，恢复 WiFi");
    wifi_resume_after_game();
}

/* 游戏运行任务：阻塞调用 game_runtime_run，退出后通知 LVGL */
static void game_run_task(void *arg)
{
    char *rom_name = (char *)arg;
    ESP_LOGI("GAME_TASK", "task started, rom=%s", rom_name);
    game_runtime_run(rom_name);
    ESP_LOGI("GAME_TASK", "runtime returned");
    free(rom_name);
    s_game_task = NULL;

    /* 先恢复 LVGL 显示再投 async：
     * 主循环里 if(disp_suspended) vTaskDelay(100) continue 会跳过 lv_timer_handler，
     * 而 lv_async_call 的回调必须由 lv_timer_handler 调度。如果不在这里 resume，
     * async 回调永远不会执行——游戏虽已退出但菜单始终不回来（卡黑屏）。 */
    lv_port_disp_resume();

    /* 把剩余恢复逻辑投递到 LVGL 线程执行（UI 操作必须在 LVGL 上下文）*/
    lv_async_call(game_task_exited_cb, NULL);
    vTaskDelete(NULL);
}

/* 列表项被点击：启动该 ROM */
static void rom_item_cb(lv_event_t *e)
{
    ESP_LOGI("ROM_CB", "clicked, s_game_active=%d", s_game_active);
    if (s_game_active) return;
    const char *rom_name = (const char *)lv_event_get_user_data(e);
    if (!rom_name) { ESP_LOGE("ROM_CB", "rom_name NULL"); return; }
    ESP_LOGI("ROM_CB", "rom_name=%s", rom_name);

    /* 复制名字给任务（task 运行期间 user_data 源对象可能被 LVGL 销毁）*/
    char *copy = strdup(rom_name);
    if (!copy) { ESP_LOGE("ROM_CB", "strdup failed"); return; }

    s_game_active = true;
    ESP_LOGI("ROM_CB", "calling lv_port_disp_suspend...");
    /* 暂停 LVGL 输出，等待当前 DMA 完成，让游戏 runtime 独占 SPI 总线 */
    lv_port_disp_suspend();

    /* 切换输入模式：键盘启用（含长按退出检测），编码器屏蔽 */
    keypad_set_game_mode(true);
    lv_port_indev_set_menu_mode(false);

    ESP_LOGI("ROM_CB", "suspended, fill black");
    /* 切到黑屏，游戏任务会自己填充屏幕 */
    LCD_Fill(0, 0, LCD_W, LCD_H, 0x0000);
    ESP_LOGI("ROM_CB", "fill done");

    /* 游戏不需要 WiFi，关掉腾出 ~80KB 内部 DRAM 给游戏任务栈和 scaled buffer。
     * 退出游戏时 game_task_exited_cb 会调 wifi_resume_after_game 重连。 */
    if (wifi_get_status() == WIFI_STATUS_CONNECTED ||
        wifi_get_status() == WIFI_STATUS_RECONNECTING ||
        wifi_get_status() == WIFI_STATUS_CONNECTING) {
        ESP_LOGI("ROM_CB", "stopping WiFi for game");
        wifi_suspend_for_game();
        /* 等 WiFi 内部清理 buffer，一般 100-200ms 就够 */
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    /* 蓝牙也需要暂停，完全释放 Bluedroid 栈腾出 DRAM */
    if (ble_prov_is_active()) {
        ble_prov_stop();
    }
    ble_prov_deinit();

    ESP_LOGI("ROM_CB", "creating task, DRAM free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    /* 游戏任务栈 12KB 放内部 DRAM（不能用 PSRAM：SPIFFS 读 ROM 时要关 flash cache，
     * 关 cache 后 PSRAM 不可访问会 panic）。WiFi 已停，DRAM 够用。
     * 12KB 栈余量给 Walnut 的深 opcode dispatch + 音频 mix + fopen/SPIFFS。
     * 绑 Core 1 避免 Core 0 的系统任务打扰 */
    if (xTaskCreatePinnedToCore(game_run_task, "game_run", 12288, copy, 10,
                                 &s_game_task, 1) != pdPASS) {
        ESP_LOGE("ROM_CB", "xTaskCreatePinnedToCore failed");
        free(copy);
        s_game_active = false;
        /* 启动失败也要把输入模式恢复回去 */
        keypad_set_game_mode(false);
        lv_port_indev_set_menu_mode(true);
        /* 游戏启动失败，恢复 WiFi */
        wifi_resume_after_game();
        return;
    }
    ESP_LOGI("ROM_CB", "task created, rom_item_cb returning");
}

static void game_back_btn_cb(lv_event_t *e)
{
    lv_screen_load_anim(lv_obj_get_screen(list), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
    indev_set_group(group);
}

static void show_game_screen(void)
{
    rom_loader_init();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "游戏 - 选择 ROM");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_simhei_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* 返回按钮 */
    lv_obj_t *back_btn = lv_button_create(scr);
    lv_obj_set_size(back_btn, 40, 24);
    lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe94560), 0);
    lv_obj_add_event_cb(back_btn, game_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(back_lbl);

    /* ROM 列表 */
    lv_obj_t *rlist = lv_list_create(scr);
    lv_obj_set_size(rlist, LCD_W - 10, LCD_H - 50);
    lv_obj_align(rlist, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(rlist, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(rlist, 0, 0);
    lv_obj_set_style_radius(rlist, 4, 0);

    /* 编码器 group */
    lv_group_t *gg = lv_group_create();
    lv_group_add_obj(gg, back_btn);

    /* 扫描 ROM：放在 static 里让 user_data 指针长期有效 */
    static rom_entry_t s_roms[ROM_MAX_COUNT];
    int count = 0;
    bool ok = rom_loader_scan(s_roms, &count);

    /* 内置游戏"2048"：不走 ROM 扫描，手动加一条列表项。
     * rom_item_cb 通过 user_data 传 __builtin_2048 触发 game_runtime_run
     * 里的内置分派。放在列表顶部：即使 SD 卡上没有 ROM 也能玩 2048。
     * 字符串是程序段常量，可长期作为 user_data。*/
    static const char *kBuiltin2048 = "__builtin_2048";
    lv_obj_t *btn_2048 = lv_list_add_button(rlist, LV_SYMBOL_PLAY, "2048 (内置)");
    lv_obj_set_style_bg_color(btn_2048, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_color(btn_2048, lv_color_hex(0xe94560), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(btn_2048, lv_color_hex(0xffffff), 0);
    lv_obj_t *lbl_2048 = lv_obj_get_child(btn_2048, 1);
    if (lbl_2048) lv_obj_set_style_text_font(lbl_2048, &lv_font_simhei_16, 0);
    lv_obj_add_event_cb(btn_2048, rom_item_cb, LV_EVENT_CLICKED, (void *)kBuiltin2048);
    lv_group_add_obj(gg, btn_2048);

    if (!ok) {
        lv_obj_t *msg = lv_list_add_text(rlist, "SD 卡未挂载或 /rom 目录不存在");
        lv_obj_set_style_text_color(msg, lv_color_hex(0xff4040), 0);
        lv_obj_set_style_text_font(msg, &lv_font_simhei_16, 0);
    } else if (count == 0) {
        lv_obj_t *msg = lv_list_add_text(rlist, "SD 卡 /rom 目录暂无 .gb / .gbc");
        lv_obj_set_style_text_color(msg, lv_color_hex(0xaaaaaa), 0);
        lv_obj_set_style_text_font(msg, &lv_font_simhei_16, 0);
    } else {
        for (int i = 0; i < count; i++) {
            char label[ROM_MAX_NAME + 32];
            /* %.48s 明确限长，避免编译器 format-truncation 告警 */
            snprintf(label, sizeof(label), "%.48s  (%uK)",
                     s_roms[i].name, (unsigned)(s_roms[i].size / 1024));
            lv_obj_t *btn = lv_list_add_button(rlist, LV_SYMBOL_PLAY, label);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x16213e), 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0xe94560), LV_STATE_FOCUSED);
            lv_obj_set_style_text_color(btn, lv_color_hex(0xffffff), 0);
            lv_obj_add_event_cb(btn, rom_item_cb, LV_EVENT_CLICKED, s_roms[i].name);
            lv_group_add_obj(gg, btn);
        }
    }

    indev_set_group(gg);
    lv_obj_add_event_cb(scr, group_delete_cb, LV_EVENT_DELETE, gg);

    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

// ================================================================
// 音乐界面：SD 卡 WAV 播放
//
// 设计：
//   单屏模式 — 列表在主体区，底部一个状态条显示"正在播放: xxx"。
//   交互：
//     旋转编码器         → 在列表项之间移动焦点
//     按编码器键        → 点击当前列表项 = 切换播放/停止该曲
//   进/退：
//     列表左上角 "←返回" 按钮（焦点可到达，按编码器键触发）
//
// 不暂停 WiFi / BLE，不关 LVGL，只是前台播放 WAV。
// 退出音乐屏幕时自动 stop 当前播放。
// ================================================================

static lv_obj_t    *music_scr        = NULL;
static lv_obj_t    *music_list       = NULL;
static lv_obj_t    *music_status_lbl = NULL;
static lv_timer_t  *music_status_timer = NULL;
static char         music_playing_name[96] = "";  /* 当前播放文件名（UI 用） */
/* 扫描缓冲：music_entry_t * MUSIC_MAX_COUNT ≈ 4.8KB。
 * 用函数内 PSRAM 堆分配代替 BSS 静态数组，退出时 free，省内部 DRAM。*/
static music_entry_t *s_music_scan_buf = NULL;

/* 状态条定时刷新：显示当前播放文件名和状态 */
static void music_status_tick(lv_timer_t *t)
{
    (void)t;
    if (!music_status_lbl || !lv_obj_is_valid(music_status_lbl)) return;
    music_state_t st = music_state();
    const char *name = music_current_name();
    if (st == MUSIC_STATE_PLAYING && name && name[0]) {
        char buf[128];
        snprintf(buf, sizeof(buf), "正在播放: %s", name);
        lv_label_set_text(music_status_lbl, buf);
    } else if (st == MUSIC_STATE_PAUSED && name && name[0]) {
        char buf[128];
        snprintf(buf, sizeof(buf), "已暂停: %s", name);
        lv_label_set_text(music_status_lbl, buf);
    } else {
        lv_label_set_text(music_status_lbl, "空闲（旋转选曲，按键播放/停止）");
    }
}

/* 异步播放请求：UI 线程调 music_play 同样会阻塞（内部 music_stop 要等 2s），
 * 用一次性 task 投递，LVGL 线程立刻返回。
 * path 是 strdup 出来的，任务自己 free。*/
/* 一次性任务：异步调 music_stop 后自删。
 * 背景：LVGL 线程直接调 music_stop 会阻塞最多 2s 等音乐任务退出，
 * UI 明显卡顿。把 stop 放到另一个小任务做，UI 立即返回，stop 在后台执行。*/
static void music_stop_task_cb(void *arg)
{
    (void)arg;
    music_stop();
    vTaskDelete(NULL);
}

static void music_play_task(void *arg)
{
    char *path = (char *)arg;
    music_play(path);   /* music_play 内部再 strdup，所以我们的副本要释放 */
    free(path);
    vTaskDelete(NULL);
}

/* 屏幕销毁回调：无论哪条退出路径（back_btn / 未来手势 / 异常删除），
 * 只要 music_scr 被 LVGL 删除就释放扫描缓冲。
 * 跟 group_delete_cb 一样挂在 LV_EVENT_DELETE 上，LVGL 保证单线程调用。*/
static void music_scan_buf_delete_cb(lv_event_t *e)
{
    (void)e;
    if (s_music_scan_buf) {
        heap_caps_free(s_music_scan_buf);
        s_music_scan_buf = NULL;
    }
    /* 同时清零全局 UI 句柄，防止屏幕删除后其他回调（如 status_tick 已通过
     * lv_obj_is_valid 做了兜底，但清 NULL 更保险）继续访问野指针 */
    music_status_lbl = NULL;
    music_list       = NULL;
    music_scr        = NULL;
    /* 状态 timer 理论上应该在 back_btn 里已经 delete，但保险起见再兜底一次：
     * timer 指针在 back_btn 里会清为 NULL，若因异常路径没清，这里 delete。 */
    if (music_status_timer) {
        lv_timer_delete(music_status_timer);
        music_status_timer = NULL;
    }
}

static void music_back_btn_cb(lv_event_t *e)
{
    (void)e;
    /* 退出音乐屏幕。
     *
     * 实际的 UI 清理（timer / scan_buf / 全局句柄）放在
     * music_scan_buf_delete_cb 里由 LVGL 的 LV_EVENT_DELETE 触发，
     * 这样手势、异常退出等其他路径也能自动清理。
     *
     * 这里只做两件事：
     *   1. 切回主菜单屏幕（lv_screen_load_anim auto_del=true 会触发老屏幕
     *      的 LV_EVENT_DELETE → 清理回调被调）
     *   2. 异步 stop 当前播放（LVGL 线程不能同步等 2s） */
    if (music_state() != MUSIC_STATE_IDLE) {
        BaseType_t r = xTaskCreate(music_stop_task_cb, "music_stop",
                                   2048, NULL, 3, NULL);
        if (r != pdPASS) {
            ESP_LOGW("DEMO", "music_stop_task 创建失败，降级同步 stop");
            music_stop();
        }
    }

    lv_screen_load_anim(lv_obj_get_screen(list), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
    indev_set_group(group);
}

/* 关闭后台音乐按钮：
 *   正在播（IDLE 之外）→ 异步 stop，弹窗"已关闭"
 *   空闲 → 弹窗"无后台音乐"
 *
 * "后台音乐"包含从本界面点的和 BLE 发的 /<歌名> 触发的，
 * 都是同一个 music 模块，stop 都能关。*/
static void music_bg_stop_btn_cb(lv_event_t *e)
{
    (void)e;
    if (music_state() == MUSIC_STATE_IDLE) {
        create_result_dialog("无后台音乐", 0x888888);
        return;
    }

    BaseType_t r = xTaskCreate(music_stop_task_cb, "music_stop",
                               2048, NULL, 3, NULL);
    if (r != pdPASS) {
        ESP_LOGW("DEMO", "music_stop_task 创建失败，降级同步 stop");
        music_stop();
    }
    create_result_dialog("已关闭后台音乐", 0x1E90FF);
}

/* 点击列表项：
 *   - 如果正在播放这首 → stop（异步）
 *   - 否则（空闲 / 播放别的 / 暂停） → 切换到这首重新播（异步）
 *
 * 所有 music_stop / music_play 都走后台任务，LVGL 线程立即返回。
 * music_play 内部会先 music_stop 旧任务，这段 2s 等待也在后台任务里做。 */
static void music_item_click_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 1);
    if (!lbl) return;
    const char *name = lv_label_get_text(lbl);
    if (!name) return;

    const char *cur = music_current_name();
    if (music_state() != MUSIC_STATE_IDLE && cur && strcmp(cur, name) == 0) {
        /* 同一首：异步停 */
        BaseType_t r = xTaskCreate(music_stop_task_cb, "music_stop",
                                   2048, NULL, 3, NULL);
        if (r != pdPASS) {
            ESP_LOGW("DEMO", "music_stop_task 创建失败，降级同步 stop");
            music_stop();
        }
        return;
    }

    /* 切换到这首：异步播放。
     * 路径用 strdup 放堆，music_play_task 结束时释放；
     * music_play 内部会再 strdup 一份给 music_task 用，所以释放安全。 */
    char path[128];
    music_full_path(name, path, sizeof(path));
    snprintf(music_playing_name, sizeof(music_playing_name), "%s", name);

    char *path_dup = strdup(path);
    if (!path_dup) {
        ESP_LOGE("DEMO", "strdup 失败，降级同步 play");
        music_play(path);
        return;
    }
    BaseType_t r = xTaskCreate(music_play_task, "music_play",
                               2048, path_dup, 3, NULL);
    if (r != pdPASS) {
        ESP_LOGW("DEMO", "music_play_task 创建失败，降级同步 play");
        free(path_dup);
        music_play(path);
    }
}

static void show_music_screen(void)
{
    /* SD 卡挂载检查：没 SD 就弹窗 */
    extern bool sdcard_is_mounted(void);
    if (!sdcard_is_mounted()) {
        create_result_dialog("未检测到 SD 卡\n请插卡后重启", 0xffaa00);
        return;
    }

    /* 扫歌：用 PSRAM 动态分配代替 static BSS，省内部 DRAM */
    if (!s_music_scan_buf) {
        s_music_scan_buf = heap_caps_malloc(sizeof(music_entry_t) * MUSIC_MAX_COUNT,
                                             MALLOC_CAP_SPIRAM);
        if (!s_music_scan_buf) {
            create_result_dialog("PSRAM 不足，无法扫描", 0xff0000);
            return;
        }
    }
    int count = 0;
    if (!music_scan(s_music_scan_buf, &count)) {
        create_result_dialog("扫描音乐失败", 0xff0000);
        /* 扫描失败也释放缓冲，避免永久占用 */
        heap_caps_free(s_music_scan_buf);
        s_music_scan_buf = NULL;
        return;
    }

    music_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(music_scr, lv_color_hex(0x1a1a2e), 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(music_scr);
    lv_label_set_text(title, "音乐 - SD 卡");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_simhei_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* 底部状态条 */
    music_status_lbl = lv_label_create(music_scr);
    lv_obj_set_width(music_status_lbl, LCD_W - 16);
    lv_label_set_long_mode(music_status_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(music_status_lbl, "空闲（旋转选曲，按键播放/停止）");
    lv_obj_set_style_text_color(music_status_lbl, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(music_status_lbl, &lv_font_simhei_16, 0);
    lv_obj_align(music_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* 列表区（留出底部状态条） */
    music_list = lv_list_create(music_scr);
    lv_obj_set_size(music_list, LCD_W - 10, LCD_H - 90);
    lv_obj_align(music_list, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(music_list, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(music_list, 0, 0);
    lv_obj_set_style_radius(music_list, 4, 0);

    lv_group_t *mg = lv_group_create();

    /* 返回按钮 */
    lv_obj_t *back_btn = lv_list_add_button(music_list, LV_SYMBOL_LEFT, "返回");
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe94560), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xffffff), 0);
    lv_obj_t *back_txt_lbl = lv_obj_get_child(back_btn, 1);
    if (back_txt_lbl) lv_obj_set_style_text_font(back_txt_lbl, &lv_font_simhei_16, 0);
    lv_obj_add_event_cb(back_btn, music_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(mg, back_btn);

    /* 关闭后台音乐按钮 */
    lv_obj_t *bg_stop_btn = lv_list_add_button(music_list, LV_SYMBOL_STOP, "关闭后台音乐");
    lv_obj_set_style_bg_color(bg_stop_btn, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_color(bg_stop_btn, lv_color_hex(0xe94560), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(bg_stop_btn, lv_color_hex(0xffffff), 0);
    lv_obj_t *bg_stop_txt_lbl = lv_obj_get_child(bg_stop_btn, 1);
    if (bg_stop_txt_lbl) lv_obj_set_style_text_font(bg_stop_txt_lbl, &lv_font_simhei_16, 0);
    lv_obj_add_event_cb(bg_stop_btn, music_bg_stop_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(mg, bg_stop_btn);

    if (count == 0) {
        /* 空列表提示项（不能点击）*/
        lv_obj_t *empty = lv_list_add_text(music_list, "(music/ 目录为空或无 WAV)");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(empty, &lv_font_simhei_16, 0);
    } else {
        for (int i = 0; i < count; ++i) {
            lv_obj_t *btn = lv_list_add_button(music_list, LV_SYMBOL_AUDIO, s_music_scan_buf[i].name);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x16213e), 0);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0xe94560), LV_STATE_FOCUSED);
            lv_obj_set_style_text_color(btn, lv_color_hex(0xffffff), 0);
            lv_obj_t *txt_lbl = lv_obj_get_child(btn, 1);
            if (txt_lbl) lv_obj_set_style_text_font(txt_lbl, &lv_font_simhei_16, 0);
            lv_obj_add_event_cb(btn, music_item_click_cb, LV_EVENT_CLICKED, NULL);
            lv_group_add_obj(mg, btn);
        }
    }

    indev_set_group(mg);
    lv_obj_add_event_cb(music_scr, group_delete_cb, LV_EVENT_DELETE, mg);
    /* 挂扫描缓冲释放回调：任何退出路径（包括未来加的手势、异常删除）都会触发 */
    lv_obj_add_event_cb(music_scr, music_scan_buf_delete_cb, LV_EVENT_DELETE, NULL);

    /* 状态刷新 timer：500ms 刷一次 */
    music_status_timer = lv_timer_create(music_status_tick, 500, NULL);

    lv_screen_load_anim(music_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

// ================================================================
// 音量调节界面
//
// 单屏：一个水平 slider (0..100) + 中央百分比大号数字。
// 交互：
//   旋转编码器  → slider 值 ±5 / step（LVGL slider 默认编码器每格 1，但
//                设成 step=5 让调整快一些）
//   按编码器键  → 返回主菜单
// ================================================================
static lv_obj_t *vol_scr         = NULL;
static lv_obj_t *vol_slider      = NULL;
static lv_obj_t *vol_pct_label   = NULL;

static void vol_slider_value_changed(lv_event_t *e)
{
    lv_obj_t *sl = lv_event_get_target(e);
    int32_t v = lv_slider_get_value(sl);
    /* 实时应用音量 + 更新百分比显示。NVS 写入由 speaker 内部完成。*/
    speaker_set_volume((uint8_t)v);
    if (vol_pct_label && lv_obj_is_valid(vol_pct_label)) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%ld %%", (long)v);
        lv_label_set_text(vol_pct_label, buf);
    }
}

/* "返回" 按钮 */
static void vol_back_btn_cb(lv_event_t *e)
{
    (void)e;
    vol_slider    = NULL;
    vol_pct_label = NULL;
    vol_scr       = NULL;
    lv_screen_load_anim(lv_obj_get_screen(list), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
    indev_set_group(group);
}

static void show_volume_screen(void)
{
    vol_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(vol_scr, lv_color_hex(0x1a1a2e), 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(vol_scr);
    lv_label_set_text(title, "音量调节");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_simhei_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* 中央大号百分比 */
    vol_pct_label = lv_label_create(vol_scr);
    uint8_t cur = speaker_get_volume();
    char buf[16];
    snprintf(buf, sizeof(buf), "%u %%", (unsigned)cur);
    lv_label_set_text(vol_pct_label, buf);
    lv_obj_set_style_text_color(vol_pct_label, lv_color_hex(0xffffff), 0);
    /* 用 Montserrat 内置大字号（纯数字 + %，不需要中文）。LVGL 9 里如果
     * 没开 42 号字，退一档。*/
    lv_obj_set_style_text_font(vol_pct_label, &lv_font_montserrat_14, 0);
    lv_obj_align(vol_pct_label, LV_ALIGN_CENTER, 0, -24);

    /* 滑块 */
    vol_slider = lv_slider_create(vol_scr);
    lv_obj_set_width(vol_slider, LCD_W - 60);
    lv_slider_set_range(vol_slider, 0, 100);
    lv_slider_set_value(vol_slider, cur, LV_ANIM_OFF);
    lv_obj_align(vol_slider, LV_ALIGN_CENTER, 0, 16);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0x404060), LV_PART_MAIN);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0xe94560), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vol_slider, lv_color_hex(0xe94560), LV_PART_KNOB);
    lv_obj_add_event_cb(vol_slider, vol_slider_value_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* 提示条 */
    lv_obj_t *tip = lv_label_create(vol_scr);
    lv_label_set_text(tip, "旋转：调节       按键：返回");
    lv_obj_set_style_text_color(tip, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(tip, &lv_font_simhei_16, 0);
    lv_obj_align(tip, LV_ALIGN_BOTTOM_MID, 0, -12);

    /* 编码器 group：滑块 + 一个不可见的"返回触发器"，按键默认行为是切换
     * slider 的 edit/focus 模式，LVGL 9 编码器单按一次先进入 edit 模式再次
     * 按才退出 — 这导致用户"按一下就返回"的期望体验不同。为了让按键直接
     * 返回，我们给 slider 单独处理 LV_EVENT_CLICKED 事件。*/
    static bool vol_slider_edit_active = false;
    (void)vol_slider_edit_active;  /* 静态状态由 lv_indev 管，这里不用手动管 */

    lv_group_t *vg = lv_group_create();
    lv_group_add_obj(vg, vol_slider);
    /* slider 被按下时返回 */
    lv_obj_add_event_cb(vol_slider, vol_back_btn_cb, LV_EVENT_CLICKED, NULL);
    /* 默认让编码器直接在 slider 上调值（不进入 edit 模式） */
    lv_group_set_editing(vg, true);

    indev_set_group(vg);
    lv_obj_add_event_cb(vol_scr, group_delete_cb, LV_EVENT_DELETE, vg);

    lv_screen_load_anim(vol_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
}

// ================================================================
// 智能设备界面（巴法云 TCP 设备云）
//
// 流程：
//   1. 进入界面 → 后台 PSRAM 栈 bemfa_list_task 调 bemfa_list_devices
//   2. 结果投 queue，UI 定时 timer 消费 → bemfa_render_list 绘制
//   3. 点击设备 → 弹 "开启 / 关闭 / 取消" 对话框，由用户决定要发的 msg
//   4. 用户选 on/off → 后台 bemfa_send_task 调 bemfa_send 推送
//   5. 推送成功后做 optimistic 更新（行 label 立即翻状态），并触发后台
//      bemfa_info_task 单设备查询，回填权威状态
//
// 设计要点（对比旧实现）：
//   - 旧版只有"toggle"，依赖 list 返回的 msg 字段推断下一个发什么；
//     msg 字段缺失时永远只能发 on，无法直接关。新版让用户明确选择。
//   - 旧版 toggle 后整表 allTopic 重拉（响应可能 10KB+/2s），新版只查
//     单设备（topicInfo，<200B），UI 反馈更快。
//   - "返回"按钮和滚动刷新（手动）保留为整表刷新路径。
//
// 前提：
//   WiFi 必须已连接，未连提示"请先连接 WiFi"
// ================================================================

#define BEMFA_DEVICE_TAG  "BEMFA_UI"

static lv_obj_t    *bemfa_scr         = NULL;
static lv_obj_t    *bemfa_list        = NULL;
static lv_obj_t    *bemfa_spinner     = NULL;
static lv_obj_t    *bemfa_status_lbl  = NULL;
static lv_timer_t  *bemfa_poll_timer  = NULL;
static QueueHandle_t bemfa_list_queue  = NULL;   /* bemfa_list_result_t* */
static QueueHandle_t bemfa_send_queue  = NULL;   /* bemfa_send_result_t */
static QueueHandle_t bemfa_info_queue  = NULL;   /* bemfa_info_result_t* */
static lv_group_t  *bemfa_group       = NULL;
static lv_obj_t    *bemfa_action_dlg  = NULL;    /* 当前打开的发送选择对话框 */

/* 后台任务与 LVGL 线程的同步（H1 防护）：
 *   bemfa_scr_delete_cb 置 active=false 并持锁清空 queue 句柄
 *   后台任务完成时锁内检查 active：
 *     active=true  → 把结果塞进 queue
 *     active=false → 屏幕已退出，直接丢弃（否则 xQueueSend 会操作已删除 queue 崩溃）
 * 锁同时保护 list/send/info 三个 queue 指针，避免 TOCTOU */
static SemaphoreHandle_t bemfa_ui_mtx        = NULL;
static volatile bool     bemfa_screen_active = false;

static inline void bemfa_ui_lock(void)
{
    if (!bemfa_ui_mtx) bemfa_ui_mtx = xSemaphoreCreateMutex();
    if (bemfa_ui_mtx) xSemaphoreTake(bemfa_ui_mtx, portMAX_DELAY);
}
static inline void bemfa_ui_unlock(void)
{
    if (bemfa_ui_mtx) xSemaphoreGive(bemfa_ui_mtx);
}

/* ----- 数据结构 ----- */

/* 列表查询结果（PSRAM 动态分配）*/
typedef struct {
    bool    ok;
    int     count;
    bemfa_device_t devices[BEMFA_MAX_DEVICES];
} bemfa_list_result_t;

/* 单设备发送任务参数 + 完成后投递的结果 */
typedef struct {
    char topic[BEMFA_MAX_TOPIC_LEN];
    char msg[BEMFA_MAX_MSG_LEN];      /* 要发送的明确消息（"on" / "off"）*/
} bemfa_send_args_t;

typedef struct {
    bool ok;
    char topic[BEMFA_MAX_TOPIC_LEN];
    char msg[BEMFA_MAX_MSG_LEN];      /* 已发送的消息，用于 status 文案 */
} bemfa_send_result_t;

/* 单设备查询参数 + 结果（PSRAM 动态分配 result，避免 queue 携带大结构）*/
typedef struct {
    char topic[BEMFA_MAX_TOPIC_LEN];
    int  delay_ms;                     /* 发送后延迟多久查询，让服务端同步 */
} bemfa_info_args_t;

typedef struct {
    bool           ok;
    bemfa_device_t dev;
} bemfa_info_result_t;

/* ----- 后台 list 拉取任务 ----- */
static void bemfa_list_task(void *arg)
{
    (void)arg;
    bemfa_list_result_t *r = heap_caps_malloc(sizeof(*r), MALLOC_CAP_SPIRAM);
    if (!r) { vTaskDelete(NULL); return; }
    memset(r, 0, sizeof(*r));

    r->ok = bemfa_list_devices(r->devices, &r->count);

    bemfa_ui_lock();
    bool delivered = false;
    if (bemfa_screen_active && bemfa_list_queue) {
        if (xQueueSend(bemfa_list_queue, &r, 0) == pdTRUE) {
            delivered = true;
        }
    }
    bemfa_ui_unlock();

    if (!delivered) heap_caps_free(r);
    vTaskDelete(NULL);
}

/* ----- 后台 send 任务（推送明确 msg） ----- */
static void bemfa_send_task(void *arg)
{
    bemfa_send_args_t *a = (bemfa_send_args_t *)arg;
    bemfa_send_result_t res = {0};
    res.ok = bemfa_send(a->topic, a->msg);
    strncpy(res.topic, a->topic, sizeof(res.topic) - 1);
    strncpy(res.msg,   a->msg,   sizeof(res.msg)   - 1);
    free(a);

    bemfa_ui_lock();
    if (bemfa_screen_active && bemfa_send_queue) {
        xQueueSend(bemfa_send_queue, &res, 0);
    }
    bemfa_ui_unlock();
    vTaskDelete(NULL);
}

/* ----- 后台 info 任务（查单设备最新状态） ----- */
static void bemfa_info_task(void *arg)
{
    bemfa_info_args_t *a = (bemfa_info_args_t *)arg;

    /* 推送后服务端 msg 字段需要 1-2 秒同步，等一下再查更可能拿到新状态 */
    if (a->delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(a->delay_ms));
    }

    bemfa_info_result_t *r = heap_caps_malloc(sizeof(*r), MALLOC_CAP_SPIRAM);
    if (!r) { free(a); vTaskDelete(NULL); return; }
    memset(r, 0, sizeof(*r));
    r->ok = bemfa_get_topic_info(a->topic, &r->dev);
    free(a);

    bemfa_ui_lock();
    bool delivered = false;
    if (bemfa_screen_active && bemfa_info_queue) {
        if (xQueueSend(bemfa_info_queue, &r, 0) == pdTRUE) {
            delivered = true;
        }
    }
    bemfa_ui_unlock();

    if (!delivered) heap_caps_free(r);
    vTaskDelete(NULL);
}

/* ----- UI 辅助 ----- */

/* 启动后台整表 list 拉取（进入界面 + 用户主动刷新走这条路径）*/
static void bemfa_kick_refresh(void)
{
    if (bemfa_status_lbl && lv_obj_is_valid(bemfa_status_lbl)) {
        lv_label_set_text(bemfa_status_lbl, "加载中...");
    }
    BaseType_t r = xTaskCreatePSRAM(bemfa_list_task, "bemfa_list", 8192, NULL, 3, NULL);
    if (r != pdPASS) {
        ESP_LOGE(BEMFA_DEVICE_TAG, "bemfa_list_task 创建失败");
        if (bemfa_status_lbl && lv_obj_is_valid(bemfa_status_lbl)) {
            lv_label_set_text(bemfa_status_lbl, "内存不足，请重试");
        }
    }
}

/* 销毁当前列表条目（保留"返回"按钮）*/
static void bemfa_clear_list(void)
{
    if (!bemfa_list || !lv_obj_is_valid(bemfa_list)) return;
    /* list 的第 0 个子是"返回"，从第 1 个开始删 */
    uint32_t n = lv_obj_get_child_count(bemfa_list);
    while (n > 1) {
        lv_obj_t *child = lv_obj_get_child(bemfa_list, 1);
        if (!child) break;
        if (bemfa_group) lv_group_remove_obj(child);
        lv_obj_delete(child);
        n--;
    }
}

/* 列表按钮的 user_data：保存 topic + 上次已知 msg，便于点击时构造发送菜单 */
typedef struct {
    char topic[BEMFA_MAX_TOPIC_LEN];
    char name[BEMFA_MAX_NAME_LEN];
    char msg[BEMFA_MAX_MSG_LEN];
} bemfa_btn_data_t;

static void bemfa_device_click_cb(lv_event_t *e);

static void bemfa_btn_data_free_cb(lv_event_t *e)
{
    bemfa_btn_data_t *d = (bemfa_btn_data_t *)lv_event_get_user_data(e);
    if (d) free(d);
}

/* 把单行 label 文字按 "name  [state]" 格式重写。供 optimistic / info 回填使用。
 * 同时同步更新挂在按钮上的 bemfa_btn_data_t.msg。 */
static void bemfa_update_row(const char *topic, const char *new_msg)
{
    if (!bemfa_list || !lv_obj_is_valid(bemfa_list)) return;

    uint32_t n = lv_obj_get_child_count(bemfa_list);
    /* 跳过第 0 个"返回" */
    for (uint32_t i = 1; i < n; ++i) {
        lv_obj_t *btn = lv_obj_get_child(bemfa_list, i);
        if (!btn) continue;

        bemfa_btn_data_t *bd = NULL;
        uint32_t cnt = lv_obj_get_event_count(btn);
        for (uint32_t k = 0; k < cnt; ++k) {
            lv_event_dsc_t *dsc = lv_obj_get_event_dsc(btn, k);
            if (!dsc) continue;
            if (lv_event_dsc_get_cb(dsc) == bemfa_device_click_cb) {
                bd = (bemfa_btn_data_t *)lv_event_dsc_get_user_data(dsc);
                break;
            }
        }
        if (!bd || strcmp(bd->topic, topic) != 0) continue;

        strncpy(bd->msg, new_msg ? new_msg : "", sizeof(bd->msg) - 1);
        bd->msg[sizeof(bd->msg) - 1] = '\0';

        /* 子 1 是文字 label */
        lv_obj_t *txt_lbl = lv_obj_get_child(btn, 1);
        if (txt_lbl) {
            const char *state = (bd->msg[0]) ? bd->msg : "-";
            char label[BEMFA_MAX_NAME_LEN + 24];
            snprintf(label, sizeof(label), "%s  [%s]", bd->name, state);
            lv_label_set_text(txt_lbl, label);
        }
        return;
    }
}

/* 渲染整张设备列表（首次加载和用户手动刷新调用）*/
static void bemfa_render_list(const bemfa_device_t *list, int count)
{
    bemfa_clear_list();

    if (count == 0) {
        lv_obj_t *empty = lv_list_add_text(bemfa_list, "(未创建任何设备)");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(empty, &lv_font_simhei_16, 0);
        return;
    }

    for (int i = 0; i < count; ++i) {
        const bemfa_device_t *d = &list[i];
        char label[BEMFA_MAX_NAME_LEN + 24];
        const char *state = (d->msg[0]) ? d->msg : "-";
        snprintf(label, sizeof(label), "%s  [%s]", d->name, state);

        lv_obj_t *btn = lv_list_add_button(bemfa_list, LV_SYMBOL_HOME, label);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x16213e), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xe94560), LV_STATE_FOCUSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xffffff), 0);
        lv_obj_t *txt_lbl = lv_obj_get_child(btn, 1);
        if (txt_lbl) lv_obj_set_style_text_font(txt_lbl, &lv_font_simhei_16, 0);

        bemfa_btn_data_t *bd = malloc(sizeof(*bd));
        if (bd) {
            memset(bd, 0, sizeof(*bd));
            strncpy(bd->topic, d->topic, sizeof(bd->topic) - 1);
            strncpy(bd->name,  d->name,  sizeof(bd->name)  - 1);
            strncpy(bd->msg,   d->msg,   sizeof(bd->msg)   - 1);
            lv_obj_add_event_cb(btn, bemfa_device_click_cb, LV_EVENT_CLICKED, bd);
            lv_obj_add_event_cb(btn, bemfa_btn_data_free_cb, LV_EVENT_DELETE, bd);
        }

        if (bemfa_group) lv_group_add_obj(bemfa_group, btn);
    }
}

/* ----- 发送 ----- */

/* 投递一个 send 任务：UI 立即把行翻成新状态，1.2s 后查实际状态回填 */
static void bemfa_kick_send(const char *topic, const char *msg)
{
    bemfa_send_args_t *a = malloc(sizeof(*a));
    if (!a) return;
    memset(a, 0, sizeof(*a));
    strncpy(a->topic, topic, sizeof(a->topic) - 1);
    strncpy(a->msg,   msg,   sizeof(a->msg)   - 1);

    if (bemfa_status_lbl && lv_obj_is_valid(bemfa_status_lbl)) {
        char buf[64];
        snprintf(buf, sizeof(buf), "发送 %s ...", msg);
        lv_label_set_text(bemfa_status_lbl, buf);
    }

    BaseType_t r = xTaskCreatePSRAM(bemfa_send_task, "bemfa_send", 8192, a, 3, NULL);
    if (r != pdPASS) {
        ESP_LOGE(BEMFA_DEVICE_TAG, "bemfa_send_task 创建失败");
        free(a);
        if (bemfa_status_lbl && lv_obj_is_valid(bemfa_status_lbl)) {
            lv_label_set_text(bemfa_status_lbl, "内存不足，请重试");
        }
        return;
    }

    /* Optimistic UI：立即翻状态，给用户即时反馈。
     * 服务端如果实际没收到（HTTP 失败），send 结果回到 timer 后会改成"失败"
     * 提示 + 触发整表刷新还原真实状态。 */
    bemfa_update_row(topic, msg);
}

/* 单设备查询投递（发送成功后调用）*/
static void bemfa_kick_info(const char *topic, int delay_ms)
{
    bemfa_info_args_t *a = malloc(sizeof(*a));
    if (!a) return;
    memset(a, 0, sizeof(*a));
    strncpy(a->topic, topic, sizeof(a->topic) - 1);
    a->delay_ms = delay_ms;

    BaseType_t r = xTaskCreatePSRAM(bemfa_info_task, "bemfa_info", 8192, a, 3, NULL);
    if (r != pdPASS) {
        ESP_LOGE(BEMFA_DEVICE_TAG, "bemfa_info_task 创建失败");
        free(a);
    }
}

/* ----- 设备点击 → "开 / 关 / 取消" 对话框 ----- */

typedef struct {
    char topic[BEMFA_MAX_TOPIC_LEN];
    char msg[BEMFA_MAX_MSG_LEN];
} bemfa_action_btn_t;

static void bemfa_action_btn_free_cb(lv_event_t *e)
{
    bemfa_action_btn_t *b = (bemfa_action_btn_t *)lv_event_get_user_data(e);
    if (b) free(b);
}

static void bemfa_action_dlg_delete_cb(lv_event_t *e)
{
    (void)e;
    bemfa_action_dlg = NULL;
    /* 关闭对话框后 indev 焦点交回设备列表。
     *
     * 但屏幕销毁路径上不要切：scr_delete_cb 先把 active 置 false，再
     * lv_obj_delete(dlg) 同步触发本回调；如果这里 indev_set_group(bemfa_group)
     * 把焦点切回 bemfa_group，紧接着 scr_delete_cb 会 lv_group_delete(bemfa_group)
     * 让 indev 持有悬空指针。 */
    if (!bemfa_screen_active) return;
    if (bemfa_group) indev_set_group(bemfa_group);
}

/* dlg 上挂的临时 group 由本回调统一释放，避免泄漏。
 * 与 bemfa_action_dlg_delete_cb 分两个回调：一个清模块全局指针并切焦点，
 * 一个负责销毁本对话框专属的 group，互不干扰。 */
static void bemfa_action_dlg_group_free_cb(lv_event_t *e)
{
    lv_group_t *g = (lv_group_t *)lv_event_get_user_data(e);
    if (g) lv_group_delete(g);
}

static void bemfa_action_close(void)
{
    if (bemfa_action_dlg && lv_obj_is_valid(bemfa_action_dlg)) {
        lv_obj_delete(bemfa_action_dlg);
    }
    bemfa_action_dlg = NULL;
}

static void bemfa_action_send_cb(lv_event_t *e)
{
    bemfa_action_btn_t *b = (bemfa_action_btn_t *)lv_event_get_user_data(e);
    if (!b) { bemfa_action_close(); return; }

    bemfa_kick_send(b->topic, b->msg);
    bemfa_action_close();
}

static void bemfa_action_cancel_cb(lv_event_t *e)
{
    (void)e;
    bemfa_action_close();
}

/* 点击设备 → 弹出"开启 / 关闭 / 取消"对话框 */
static void bemfa_device_click_cb(lv_event_t *e)
{
    const bemfa_btn_data_t *bd = (const bemfa_btn_data_t *)lv_event_get_user_data(e);
    if (!bd || !bd->topic[0]) return;

    /* 已有对话框先关掉，避免叠加 */
    bemfa_action_close();

    lv_obj_t *dlg = lv_obj_create(bemfa_scr ? bemfa_scr : lv_screen_active());
    bemfa_action_dlg = dlg;
    lv_obj_set_size(dlg, LCD_W - 40, 160);
    lv_obj_center(dlg);
    lv_obj_set_style_bg_color(dlg, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_color(dlg, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_border_width(dlg, 2, 0);
    lv_obj_set_style_radius(dlg, 8, 0);
    lv_obj_set_style_pad_all(dlg, 8, 0);
    lv_obj_set_flex_flow(dlg, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(dlg, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(dlg, bemfa_action_dlg_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_t *title = lv_label_create(dlg);
    char title_buf[BEMFA_MAX_NAME_LEN + 16];
    snprintf(title_buf, sizeof(title_buf), "%s", bd->name[0] ? bd->name : bd->topic);
    lv_label_set_text(title, title_buf);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_simhei_16, 0);

    lv_obj_t *sub = lv_label_create(dlg);
    char sub_buf[64];
    snprintf(sub_buf, sizeof(sub_buf), "当前: %s", bd->msg[0] ? bd->msg : "-");
    lv_label_set_text(sub, sub_buf);
    lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(sub, &lv_font_simhei_16, 0);

    /* 三个按钮：开 / 关 / 取消，作为对话框临时 group */
    lv_group_t *dlg_group = lv_group_create();

    static const struct {
        const char *label;
        const char *msg;        /* 要发送的内容；NULL = 取消 */
        uint32_t    bg_color;
    } actions[] = {
        { "开启", "on",  0x00aa44 },
        { "关闭", "off", 0xaa3344 },
        { "取消", NULL,  0x444466 },
    };

    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        lv_obj_t *btn = lv_button_create(dlg);
        lv_obj_set_width(btn, LV_PCT(90));
        lv_obj_set_height(btn, 32);
        lv_obj_set_style_bg_color(btn, lv_color_hex(actions[i].bg_color), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xe94560), LV_STATE_FOCUSED);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, actions[i].label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_simhei_16, 0);
        lv_obj_center(lbl);

        if (actions[i].msg) {
            bemfa_action_btn_t *abtn = malloc(sizeof(*abtn));
            if (abtn) {
                memset(abtn, 0, sizeof(*abtn));
                strncpy(abtn->topic, bd->topic, sizeof(abtn->topic) - 1);
                strncpy(abtn->msg,   actions[i].msg, sizeof(abtn->msg) - 1);
                lv_obj_add_event_cb(btn, bemfa_action_send_cb, LV_EVENT_CLICKED, abtn);
                lv_obj_add_event_cb(btn, bemfa_action_btn_free_cb, LV_EVENT_DELETE, abtn);
            }
        } else {
            lv_obj_add_event_cb(btn, bemfa_action_cancel_cb, LV_EVENT_CLICKED, NULL);
        }

        lv_group_add_obj(dlg_group, btn);
    }

    indev_set_group(dlg_group);
    /* group 跟着 dlg 走：dlg 被删除时一起释放 group。 */
    lv_obj_add_event_cb(dlg, bemfa_action_dlg_group_free_cb, LV_EVENT_DELETE, dlg_group);
}

/* 定时轮询：检查后台任务结果是否到达 */
static void bemfa_poll_tick(lv_timer_t *t)
{
    (void)t;
    if (!bemfa_scr || !lv_obj_is_valid(bemfa_scr)) return;

    /* 1) list 整表刷新结果 */
    bemfa_list_result_t *lr = NULL;
    if (bemfa_list_queue && xQueueReceive(bemfa_list_queue, &lr, 0) == pdTRUE && lr) {
        if (lr->ok) {
            bemfa_render_list(lr->devices, lr->count);
            if (bemfa_status_lbl && lv_obj_is_valid(bemfa_status_lbl)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "共 %d 个设备", lr->count);
                lv_label_set_text(bemfa_status_lbl, buf);
            }
        } else {
            if (bemfa_status_lbl && lv_obj_is_valid(bemfa_status_lbl)) {
                lv_label_set_text(bemfa_status_lbl, "加载失败，请重试");
            }
        }
        heap_caps_free(lr);
    }

    /* 2) send 推送结果 */
    bemfa_send_result_t sr;
    if (bemfa_send_queue && xQueueReceive(bemfa_send_queue, &sr, 0) == pdTRUE) {
        if (sr.ok) {
            if (bemfa_status_lbl && lv_obj_is_valid(bemfa_status_lbl)) {
                char buf[64];
                snprintf(buf, sizeof(buf), "已发送 %s", sr.msg);
                lv_label_set_text(bemfa_status_lbl, buf);
            }
            /* 1.2 秒后单设备查询权威状态。延迟原因：服务端 msg 字段
             * 异步同步，立即查可能返回旧值。 */
            bemfa_kick_info(sr.topic, 1200);
        } else {
            /* 发送失败：optimistic 更新需要还原，整表重拉拿权威状态 */
            if (bemfa_status_lbl && lv_obj_is_valid(bemfa_status_lbl)) {
                lv_label_set_text(bemfa_status_lbl, "发送失败");
            }
            bemfa_kick_refresh();
        }
    }

    /* 3) info 单设备查询结果 */
    bemfa_info_result_t *ir = NULL;
    if (bemfa_info_queue && xQueueReceive(bemfa_info_queue, &ir, 0) == pdTRUE && ir) {
        if (ir->ok) {
            bemfa_update_row(ir->dev.topic, ir->dev.msg);
        }
        /* 失败不弹错（用户已经看到 optimistic 状态），仅日志 */
        heap_caps_free(ir);
    }
}

static void bemfa_back_btn_cb(lv_event_t *e)
{
    (void)e;
    /* UI 清理放 LV_EVENT_DELETE 回调里，这里只负责切屏 */
    lv_screen_load_anim(lv_obj_get_screen(list), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
    indev_set_group(group);
}

static void bemfa_scr_delete_cb(lv_event_t *e)
{
    (void)e;

    /* 防御：屏幕销毁前先把 indev 强行切到主菜单 group。
     *
     * 正常 back_btn_cb 路径已经做过这一步，但如果对话框仍开着时屏幕被
     * 外部销毁（动画 auto_del / 上层强切等假想路径），indev 此时指向
     * dlg_group。下面的 lv_obj_delete(dlg) 会同步触发 group_free_cb 释放
     * dlg_group，让 indev 悬空。这里先切走，确保被删的 group 都不再被
     * 任何 indev 引用。 */
    if (group) indev_set_group(group);

    /* 先取锁原子地置 inactive + 抓走 queue 句柄。
     * 锁后的时刻起，任何后台任务投递尝试都会因 active=false 放弃，
     * 不会再访问即将删除的 queue（H1 核心防护）。*/
    bemfa_ui_lock();
    bemfa_screen_active = false;
    QueueHandle_t lq = bemfa_list_queue;
    QueueHandle_t sq = bemfa_send_queue;
    QueueHandle_t iq = bemfa_info_queue;
    bemfa_list_queue = NULL;
    bemfa_send_queue = NULL;
    bemfa_info_queue = NULL;
    bemfa_ui_unlock();

    if (bemfa_poll_timer) {
        lv_timer_delete(bemfa_poll_timer);
        bemfa_poll_timer = NULL;
    }

    /* 关掉可能还开着的发送对话框（dlg_group 一同被销毁）*/
    if (bemfa_action_dlg && lv_obj_is_valid(bemfa_action_dlg)) {
        lv_obj_delete(bemfa_action_dlg);
    }
    bemfa_action_dlg = NULL;

    /* drain + delete。此时后台任务不会再投递新消息，安全销毁。*/
    if (lq) {
        bemfa_list_result_t *r = NULL;
        while (xQueueReceive(lq, &r, 0) == pdTRUE) {
            if (r) heap_caps_free(r);
        }
        vQueueDelete(lq);
    }
    if (sq) {
        /* send_queue 投递的是 bemfa_send_result_t（栈值），无需 free */
        vQueueDelete(sq);
    }
    if (iq) {
        bemfa_info_result_t *r = NULL;
        while (xQueueReceive(iq, &r, 0) == pdTRUE) {
            if (r) heap_caps_free(r);
        }
        vQueueDelete(iq);
    }
    if (bemfa_group) {
        lv_group_delete(bemfa_group);
        bemfa_group = NULL;
    }
    bemfa_scr        = NULL;
    bemfa_list       = NULL;
    bemfa_spinner    = NULL;
    bemfa_status_lbl = NULL;
}

static void show_bemfa_screen(void)
{
    if (wifi_get_status() != WIFI_STATUS_CONNECTED) {
        create_result_dialog("请先连接 WiFi\n智能设备需要联网", 0xffaa00);
        return;
    }

    bemfa_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(bemfa_scr, lv_color_hex(0x1a1a2e), 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(bemfa_scr);
    lv_label_set_text(title, "智能设备");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_simhei_16, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    /* 底部状态条 */
    bemfa_status_lbl = lv_label_create(bemfa_scr);
    lv_obj_set_width(bemfa_status_lbl, LCD_W - 16);
    lv_label_set_long_mode(bemfa_status_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(bemfa_status_lbl, "加载中...");
    lv_obj_set_style_text_color(bemfa_status_lbl, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(bemfa_status_lbl, &lv_font_simhei_16, 0);
    lv_obj_align(bemfa_status_lbl, LV_ALIGN_BOTTOM_MID, 0, -8);

    /* 列表区 */
    bemfa_list = lv_list_create(bemfa_scr);
    lv_obj_set_size(bemfa_list, LCD_W - 10, LCD_H - 90);
    lv_obj_align(bemfa_list, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_set_style_bg_color(bemfa_list, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(bemfa_list, 0, 0);
    lv_obj_set_style_radius(bemfa_list, 4, 0);

    bemfa_group = lv_group_create();

    /* 返回按钮（固定第一项）*/
    lv_obj_t *back_btn = lv_list_add_button(bemfa_list, LV_SYMBOL_LEFT, "返回");
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xe94560), LV_STATE_FOCUSED);
    lv_obj_set_style_text_color(back_btn, lv_color_hex(0xffffff), 0);
    lv_obj_t *back_txt_lbl = lv_obj_get_child(back_btn, 1);
    if (back_txt_lbl) lv_obj_set_style_text_font(back_txt_lbl, &lv_font_simhei_16, 0);
    lv_obj_add_event_cb(back_btn, bemfa_back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_group_add_obj(bemfa_group, back_btn);

    /* 创建 queue + 定时轮询 timer */
    bemfa_list_queue   = xQueueCreate(2, sizeof(bemfa_list_result_t *));
    bemfa_send_queue   = xQueueCreate(4, sizeof(bemfa_send_result_t));
    bemfa_info_queue   = xQueueCreate(4, sizeof(bemfa_info_result_t *));
    bemfa_poll_timer   = lv_timer_create(bemfa_poll_tick, 200, NULL);

    indev_set_group(bemfa_group);
    lv_obj_add_event_cb(bemfa_scr, bemfa_scr_delete_cb, LV_EVENT_DELETE, NULL);

    /* 标记屏幕活跃：这之后后台任务完成时可以投递 queue */
    bemfa_ui_lock();
    bemfa_screen_active = true;
    bemfa_ui_unlock();

    /* 切屏完成后启动后台拉取 */
    lv_screen_load_anim(bemfa_scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, false);
    bemfa_kick_refresh();
}

static void list_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    // child(0) 是图标，child(1) 是文字
    lv_obj_t *label = lv_obj_get_child(btn, 1);
    const char *txt = lv_label_get_text(label);

    if (strstr(txt, "环境监测")) {
        /* TODO: 接入 DHT22 / WS2812 等传感器，显示温湿度/光照等环境数据。
         * 占位：先弹提示窗。 */
        create_result_dialog("环境监测\n功能开发中", 0x00cfff);
    } else if (strstr(txt, "天气")) {
        /* 天气依赖 HTTP API，必须联网 */
        if (wifi_get_status() != WIFI_STATUS_CONNECTED) {
            create_result_dialog("请先连接 WiFi\n天气查询需要联网", 0xffaa00);
            return;
        }
        if (weather_fetching) return;
        s_weather_cancelled = false;
        { weather_result_t drained; while (xQueueReceive(weather_result_queue, &drained, 0) == pdTRUE) {} }
        weather_fetching = true;
        weather_spinner_cont = create_loading_dialog("Fetching Weather...", weather_cancel_btn_cb);
        xTaskCreatePSRAM(weather_fetch_task, "weather_task", 16384, NULL, 3, NULL);
    } else if (strstr(txt, "聊天")) {
        show_chat_screen();
    } else if (strstr(txt, "语音助手")) {
        show_asr_screen();
    } else if (strstr(txt, "语音命令")) {
        show_sr_cmd_screen();
    } else if (strstr(txt, "蓝牙")) {
        show_ble_screen();
    } else if (strstr(txt, "音乐")) {
        show_music_screen();
    } else if (strstr(txt, "音量")) {
        show_volume_screen();
    } else if (strstr(txt, "智能设备")) {
        show_bemfa_screen();
    } else if (strstr(txt, "游戏")) {
        show_game_screen();
    }
}

void my_demo(void)
{
    wifi_result_queue = xQueueCreate(2, sizeof(wifi_result_t));
    weather_result_queue = xQueueCreate(2, sizeof(weather_result_t));
    ble_start_result_queue = xQueueCreate(2, sizeof(ble_start_result_t));
    asr_mic_init();

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
        LV_SYMBOL_CHARGE,      /* 环境监测：温湿度传感器数据 */
        LV_SYMBOL_EYE_OPEN,
        LV_SYMBOL_PLAY,        /* 游戏：用 PLAY 图标 */
        LV_SYMBOL_CALL,
        LV_SYMBOL_AUDIO,
        LV_SYMBOL_BELL,        /* 语音命令（ESP-SR 离线） */
        LV_SYMBOL_BLUETOOTH,   /* 蓝牙配网 */
        LV_SYMBOL_SD_CARD,     /* 音乐：SD 卡 WAV 播放 */
        LV_SYMBOL_VOLUME_MAX,  /* 音量调节 */
        LV_SYMBOL_HOME,        /* 智能设备（巴法云）*/
    };
    static const char *labels[] = {
        "环境监测",
        "天气与日期",
        "游戏",
        "聊天助手",
        "语音助手",
        "语音命令",
        "蓝牙",
        "音乐",
        "音量",
        "智能设备",
    };

    group = lv_group_create();

    for (int i = 0; i < 10; i++) {
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


    lv_timer_create(wifi_status_timer_cb, 500, NULL);
    /* SR 命令分发 timer：每 100ms 轮询启动结果队列 + 识别结果队列。
     * 即使 SR 未启用也跑（cheap，只查队列），无需根据 sr_cmd_active 启停。 */
    lv_timer_create(sr_cmd_timer_cb, 100, NULL);
}

lv_group_t *my_demo_get_group(void)
{
    return group;
}
