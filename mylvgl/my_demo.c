#include "my_demo.h"
#include "lvgl.h"
#include "lcd.h"
#include "wifi.h"
#include "weather.h"
#include "sntp_time.h"
#include "model.h"
#include "asr.h"
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"

LV_FONT_DECLARE(lv_font_simhei_16);

/* 前向声明 */
static void chat_back_btn_cb(lv_event_t *e);
static void close_btn_cb(lv_event_t *e);

/* 全局 UI 对象 */
static lv_obj_t *selected_label;
static lv_obj_t *list;
static lv_group_t *group;
static lv_obj_t *wifi_spinner_cont    = NULL;  /* WiFi 加载弹窗容器 */
static lv_obj_t *weather_spinner_cont = NULL;  /* 天气加载弹窗容器 */
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
    weather_fetching = false;
    safe_delete_loading_dialog(&weather_spinner_cont);
    indev_set_group(group);
}

static void weather_fetch_task(void *arg)
{
    weather_result_t res;
    res.success = weather_fetch(&res.data);
    xQueueSend(weather_result_queue, &res, 0);
    vTaskDelete(NULL);
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

static void chat_fetch_task(void *arg)
{
    char *msg = (char *)arg;
    ESP_LOGI("CHAT", "chat_task 启动, 剩余堆: %lu", esp_get_free_heap_size());
    chat_result_t res;
    res.success = model_chat(msg, &res.data);
    free(msg);
    xQueueSend(chat_result_queue, &res, 0);
    vTaskDelete(NULL);
}

static void chat_send_cb(lv_event_t *e)
{
    if (chat_fetching) return;
    const char *txt = lv_textarea_get_text(chat_input);
    if (!txt || strlen(txt) == 0) return;

    // 追加用户消息到日志
    char user_line[MODEL_MAX_INPUT + 8];
    snprintf(user_line, sizeof(user_line), "You: %s\n", txt);
    strncat(chat_log, user_line, sizeof(chat_log) - strlen(chat_log) - 1);
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

    // 从 PSRAM 分配栈，避免内部堆耗尽
    StaticTask_t *task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
    StackType_t *task_stack = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
    if (!task_buf || !task_stack) {
        ESP_LOGE("CHAT", "chat_task 栈分配失败");
        chat_fetching = false;
        if (chat_spinner && lv_obj_is_valid(chat_spinner)) { lv_obj_delete(chat_spinner); chat_spinner = NULL; }
        free(msg);
        if (task_buf) heap_caps_free(task_buf);
        if (task_stack) heap_caps_free(task_stack);
    } else {
        // 把 task_buf 和 task_stack 指针通过 msg 之外的方式传递以便释放
        // 注意：StaticTask 的内存在任务删除后需手动释放，这里接受泄漏换稳定性
        // 改用动态任务避免泄漏
        heap_caps_free(task_buf);
        heap_caps_free(task_stack);
        BaseType_t ret = xTaskCreate(chat_fetch_task, "chat_task", 16384, msg, 3, NULL);
        if (ret != pdPASS) {
            ESP_LOGE("CHAT", "chat_task 创建失败");
            chat_fetching = false;
            if (chat_spinner && lv_obj_is_valid(chat_spinner)) { lv_obj_delete(chat_spinner); chat_spinner = NULL; }
            free(msg);
        }
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
        strncat(chat_log, ai_line, sizeof(chat_log) - strlen(chat_log) - 1);
    } else {
        char err_line[80];
        snprintf(err_line, sizeof(err_line), "Error: %s\n", res.data.error_msg);
        strncat(chat_log, err_line, sizeof(chat_log) - strlen(chat_log) - 1);
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

/* asr_recognize_task 暂时停用，音频通过 USB CDC 发送
static void asr_recognize_task(void *arg)
{
    uint32_t audio_len = *(uint32_t *)arg;
    free(arg);
    asr_task_result_t res;
    res.success = asr_recognize(audio_len, &res.data);
    xQueueSend(asr_result_queue, &res, 0);
    vTaskDelete(NULL);
}
*/

static void asr_btn_cb(lv_event_t *e)
{
    if (asr_processing) return;
    if (!asr_recording) {
        asr_recording = true;
        asr_record_start();
        lv_label_set_text(asr_status_label, "录音中...");
        lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0xe94560), 0);
        lv_obj_t *lbl = lv_obj_get_child(asr_btn, 0);
        if (lbl) lv_label_set_text(lbl, LV_SYMBOL_STOP " 停止录音");
    } else {
        asr_recording = false;
        asr_audio_len = asr_record_stop();
        lv_obj_t *lbl = lv_obj_get_child(asr_btn, 0);
        if (lbl) lv_label_set_text(lbl, LV_SYMBOL_AUDIO " 开始录音");
        if (asr_audio_len == 0) {
            lv_label_set_text(asr_status_label, "录音太短，请重试");
            lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x16213e), 0);
            return;
        }
        lv_label_set_text(asr_status_label, "已发送至电脑");
        lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x16213e), 0);
        /* 识别功能暂时关闭，音频已通过 USB CDC 实时发送
        asr_processing = true;
        lv_label_set_text(asr_status_label, "识别中...");
        lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x555555), 0);
        uint32_t *len_arg = malloc(sizeof(uint32_t));
        *len_arg = asr_audio_len;
        StaticTask_t *task_buf = heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        StackType_t  *task_stack = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
        if (!task_buf || !task_stack) {
            lv_label_set_text(asr_status_label, "内存不足");
            asr_processing = false;
            free(len_arg);
            if (task_buf) heap_caps_free(task_buf);
            if (task_stack) heap_caps_free(task_stack);
        } else {
            heap_caps_free(task_buf);
            heap_caps_free(task_stack);
            BaseType_t ret = xTaskCreate(asr_recognize_task, "asr_task", 16384, len_arg, 3, NULL);
            if (ret != pdPASS) {
                lv_label_set_text(asr_status_label, "内存不足");
                asr_processing = false;
                free(len_arg);
            }
        }
        */
    }
}

static void asr_timer_cb(lv_timer_t *t)
{
    if (!asr_scr || !lv_obj_is_valid(asr_scr)) { lv_timer_delete(t); return; }
    if (asr_recording) asr_record_read();
    if (!asr_result_queue) return;
    asr_task_result_t res;
    if (xQueueReceive(asr_result_queue, &res, 0) != pdTRUE) return;
    asr_processing = false;
    lv_obj_set_style_bg_color(asr_btn, lv_color_hex(0x16213e), 0);
    if (res.success && strlen(res.data.result) > 0) {
        lv_label_set_text(asr_result_label, res.data.result);
        lv_label_set_text(asr_status_label, "识别完成");
    } else {
        lv_label_set_text(asr_result_label, "识别失败");
        lv_label_set_text(asr_status_label, res.data.error_msg);
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
    lv_screen_load_anim(lv_obj_get_screen(list), LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, true);
    indev_set_group(group);
}

static void show_asr_screen(void)
{
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
    lv_label_set_text(btn_lbl, LV_SYMBOL_AUDIO " 开始录音");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xffffff), 0);
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
        snprintf(msg, sizeof(msg), LV_SYMBOL_OK " Connected!\n%s", ip);
    else
        snprintf(msg, sizeof(msg), LV_SYMBOL_CLOSE " WiFi Failed!");

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
        weather_fetching = false;
        safe_delete_loading_dialog(&weather_spinner_cont);
        if (wresp.success) {
            show_weather_screen(&wresp.data);
        } else {
            /* 构造错误信息后创建结果弹窗 */
            char err_msg[80];
            snprintf(err_msg, sizeof(err_msg), LV_SYMBOL_CLOSE " %s", wresp.data.error_msg);
            create_result_dialog(err_msg, 0xff0000);
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
        wifi_spinner_cont = create_loading_dialog("WiFi Connecting...", cancel_btn_cb);
        xTaskCreate(wifi_connect_task, "wifi_task", 4096, NULL, 3, &wifi_task_handle);
    } else if (strstr(txt, "天气")) {
        if (weather_fetching) return;
        weather_fetching = true;
        weather_spinner_cont = create_loading_dialog("Fetching Weather...", weather_cancel_btn_cb);
        xTaskCreate(weather_fetch_task, "weather_task", 16384, NULL, 3, NULL);
    } else if (strstr(txt, "聊天")) {
        show_chat_screen();
    } else if (strstr(txt, "语音")) {
        show_asr_screen();
    } else {
        lv_label_set_text_fmt(selected_label, "Selected: %s", txt);
    }
}

void my_demo(void)
{
    wifi_result_queue = xQueueCreate(2, sizeof(wifi_result_t));
    weather_result_queue = xQueueCreate(2, sizeof(weather_result_t));
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
        LV_SYMBOL_WIFI,
        LV_SYMBOL_EYE_OPEN,
        LV_SYMBOL_BATTERY_FULL,
        LV_SYMBOL_CALL,
        LV_SYMBOL_AUDIO,
        LV_SYMBOL_POWER,
    };
    static const char *labels[] = {
        "WiFi 连接",
        "天气与日期",
        "电池",
        "聊天助手",
        "语音助手",
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
