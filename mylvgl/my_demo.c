#include "my_demo.h"
#include "lvgl.h"
#include "lcd.h"

static lv_obj_t *selected_label;
static lv_obj_t *list;
static lv_group_t *group;

static void list_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);
    const char *txt = lv_label_get_text(label);
    lv_label_set_text_fmt(selected_label, "Selected: %s", txt);
}

void my_demo(void)
{
    // 背景
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x1a1a2e), 0);

    // 标题
    lv_obj_t *title = lv_label_create(lv_screen_active());
    lv_label_set_text(title, "ESP32-S3 Menu");
    lv_obj_set_style_text_color(title, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // 分割线
    lv_obj_t *line = lv_obj_create(lv_screen_active());
    lv_obj_set_size(line, LCD_W - 10, 2);
    lv_obj_set_style_bg_color(line, lv_color_hex(0xe94560), 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    lv_obj_set_style_pad_all(line, 0, 0);
    lv_obj_align(line, LV_ALIGN_TOP_MID, 0, 30);

    // 列表
    list = lv_list_create(lv_screen_active());
    lv_obj_set_size(list, LCD_W - 10, LCD_H - 80);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 40);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_radius(list, 4, 0);

    // 列表项
    const char *items[] = {
        LV_SYMBOL_WIFI    " WiFi Settings",
        LV_SYMBOL_BELL    " Notifications",
        LV_SYMBOL_BATTERY_FULL " Battery",
        LV_SYMBOL_SETTINGS " System Settings",
        LV_SYMBOL_LOOP    " Update Firmware",
        LV_SYMBOL_POWER   " Restart",
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

    // 底部提示
    lv_obj_t *hint = lv_label_create(lv_screen_active());
    lv_label_set_text(hint, LV_SYMBOL_UP"/"LV_SYMBOL_DOWN" Scroll  "LV_SYMBOL_OK" Select");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -20);

    // 选中状态显示
    selected_label = lv_label_create(lv_screen_active());
    lv_label_set_text(selected_label, "Selected: none");
    lv_obj_set_style_text_color(selected_label, lv_color_hex(0xe94560), 0);
    lv_obj_align(selected_label, LV_ALIGN_BOTTOM_MID, 0, -5);
}

lv_group_t *my_demo_get_group(void)
{
    return group;
}
