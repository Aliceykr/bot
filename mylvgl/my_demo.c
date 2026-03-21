#include "my_demo.h"
#include "lvgl.h"
#include "lcd.h"

static void btn_event_cb(lv_event_t *e)
{
    static int count = 0;
    lv_obj_t *label = lv_event_get_user_data(e);
    count++;
    lv_label_set_text_fmt(label, "Clicked: %d", count);
}

void my_demo(void)
{
    // 背景色
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x1a1a2e), 0);

    // 标题
    lv_obj_t *title = lv_label_create(lv_screen_active());
    lv_label_set_text(title, "ESP32-S3 LVGL Demo");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    // 分割线
    lv_obj_t *line = lv_obj_create(lv_screen_active());
    lv_obj_set_size(line, LCD_W - 20, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 35);

    // 进度条
    lv_obj_t *bar = lv_bar_create(lv_screen_active());
    lv_obj_set_size(bar, LCD_W - 40, 20);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 50);
    lv_bar_set_value(bar, 75, LV_ANIM_ON);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0xe94560), LV_PART_INDICATOR);

    // 进度条标签
    lv_obj_t *bar_label = lv_label_create(lv_screen_active());
    lv_label_set_text(bar_label, "CPU Load: 75%");
    lv_obj_set_style_text_color(bar_label, lv_color_hex(0xffffff), 0);
    lv_obj_align_to(bar_label, bar, LV_ALIGN_OUT_BOTTOM_MID, 0, 5);

    // 圆弧
    lv_obj_t *arc = lv_arc_create(lv_screen_active());
    lv_obj_set_size(arc, 100, 100);
    lv_arc_set_value(arc, 60);
    lv_obj_align(arc, LV_ALIGN_LEFT_MID, 10, 20);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0xe94560), LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(0x16213e), LV_PART_MAIN);

    // 圆弧标签
    lv_obj_t *arc_label = lv_label_create(lv_screen_active());
    lv_label_set_text(arc_label, "60%");
    lv_obj_set_style_text_color(arc_label, lv_color_hex(0xffffff), 0);
    lv_obj_align_to(arc_label, arc, LV_ALIGN_CENTER, 0, 0);

    // 按钮
    lv_obj_t *click_label = lv_label_create(lv_screen_active());
    lv_label_set_text(click_label, "Clicked: 0");
    lv_obj_set_style_text_color(click_label, lv_color_hex(0xffffff), 0);
    lv_obj_align(click_label, LV_ALIGN_RIGHT_MID, -10, 20);

    lv_obj_t *btn = lv_button_create(lv_screen_active());
    lv_obj_set_size(btn, 100, 40);
    lv_obj_align_to(btn, click_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 10);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xe94560), 0);
    lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, click_label);
    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "Click Me");
    lv_obj_center(btn_label);

    // 底部状态栏
    lv_obj_t *status = lv_label_create(lv_screen_active());
    lv_label_set_text(status, "ILI9341 240x320 | LVGL v9");
    lv_obj_set_style_text_color(status, lv_color_hex(0x888888), 0);
    lv_obj_align(status, LV_ALIGN_BOTTOM_MID, 0, -5);
}
