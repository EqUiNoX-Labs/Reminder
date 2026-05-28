#include "reminder_ui.h"

#include "audio_player.h"
#include "board_config.h"
#include "esp_lvgl_port.h"
#include "fonts/lv_font_clock_mono.h"
#include "reminder_storage.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "reminder_ui";

#define UI_BG_COLOR              0x0d0d10
#define UI_SURFACE_COLOR         0x18181c
#define UI_SURFACE_RAISED        0x212128
#define UI_SURFACE_PRESSED       0x2c2c35
#define UI_BORDER_COLOR          0x2e2e38
#define UI_TEXT_PRIMARY          0xf4f4f5
#define UI_TEXT_SECONDARY        0x9ca3af
#define UI_TEXT_MUTED            0x6b7280
#define UI_ACCENT_COLOR          0x6366f1
#define UI_ACCENT_PRESSED        0x4f46e5
#define UI_ACCENT_SOFT           0x818cf8
#define UI_ON_ACCENT             0xffffff
#define UI_DANGER_COLOR          0xef4444
#define UI_DANGER_PRESSED        0xdc2626
#define UI_ON_DANGER             0xffffff
#define UI_RADIUS_SM             8
#define UI_RADIUS_MD             12
#define UI_SETTINGS_HOLD_MS      3000
#define UI_SETTINGS_HOTSPOT_W    72
#define UI_SETTINGS_HOTSPOT_H    72
#define UI_REM_ROW_Y_BASE        78
#define UI_REM_ROW_HEIGHT        58
#define UI_REM_ROW_SPACING       62
#define UI_REM_ADD_GAP           14
#define UI_REM_ROW_PAD           12
#define UI_REM_MSG_Y             8
#define UI_REM_TIME_Y            36
#define UI_REM_TEXT_WIDTH        210
#define UI_REM_REMOVE_BTN_W      36
#define UI_REM_REMOVE_BTN_H      28
#define UI_REM_EDIT_KB_H         132
#define UI_REM_EDIT_KB_MARGIN    6
#define UI_REM_EDIT_TOP_PAD      4
#define UI_REM_EDIT_ROW_H        44
#define UI_ALARM_TASK_STACK      8192
#define UI_ALARM_TASK_PRIO       4

#define UI_FONT_CLOCK_DISPLAY    (&lv_font_clock_mono_58)
#define UI_FONT_TIME_LARGE       (&lv_font_clock_mono_28)
#define UI_FONT_TIME_SMALL       (&lv_font_clock_mono_20)

typedef enum {
    UI_PICK_HOUR_UP,
    UI_PICK_HOUR_DOWN,
    UI_PICK_MINUTE_UP,
    UI_PICK_MINUTE_DOWN,
    UI_PICK_AMPM_UP,
    UI_PICK_AMPM_DOWN,
} ui_pick_action_t;

typedef enum {
    UI_PICK_TARGET_BOOT,
    UI_PICK_TARGET_SETTINGS_CLOCK,
    UI_PICK_TARGET_WIZARD_ADD,
    UI_PICK_TARGET_WIZARD_EDIT,
} ui_pick_target_t;

typedef enum {
    UI_WIZARD_NONE,
    UI_WIZARD_ADD,
    UI_WIZARD_EDIT,
} ui_wizard_mode_t;

typedef struct {
    lv_display_t *display;
    lv_obj_t *scr_init;
    lv_obj_t *bar_init;
    lv_obj_t *lbl_init_status;
    lv_obj_t *scr_set_time;
    lv_obj_t *lbl_pick_hour;
    lv_obj_t *lbl_pick_minute;
    lv_obj_t *lbl_pick_ampm;
    lv_obj_t *scr_clock;
    lv_obj_t *lbl_clock;
    lv_obj_t *lbl_clock_ampm;
    lv_obj_t *scr_alarm;
    lv_obj_t *lbl_alarm_message;
    lv_obj_t *scr_settings;
    lv_obj_t *settings_scroll;
    lv_obj_t *lbl_settings_clock;
    lv_obj_t *scr_time_edit;
    lv_obj_t *lbl_time_edit_title;
    lv_obj_t *lbl_time_edit_save;
    lv_obj_t *lbl_edit_pick_hour;
    lv_obj_t *lbl_edit_pick_minute;
    lv_obj_t *lbl_edit_pick_ampm;
    lv_obj_t *scr_reminder_edit;
    lv_obj_t *reminder_edit_scroll;
    lv_obj_t *reminder_edit_time_row;
    lv_obj_t *ta_reminder_text;
    lv_obj_t *kb_reminder_text;
    lv_obj_t *lbl_reminder_edit_time;
    lv_obj_t *btn_reminder_edit_time;
    lv_obj_t *reminder_rows[REMINDER_COUNT];
    lv_obj_t *reminder_lbls[REMINDER_COUNT];
    lv_obj_t *reminder_time_lbls[REMINDER_COUNT];
    lv_obj_t *reminder_remove_btns[REMINDER_COUNT];
    lv_obj_t *btn_add_reminder;
    lv_timer_t *clock_timer;
    lv_timer_t *settings_hold_timer;
    reminder_settings_t settings;
    bool reminder_fired[REMINDER_COUNT];
    int pick_hour12;
    int pick_minute;
    bool pick_is_pm;
    ui_pick_target_t pick_target;
    int pick_reminder_idx;
    int set_hour;
    int set_minute;
    int set_second;
    int64_t set_epoch_ms;
    SemaphoreHandle_t time_set_sem;
} reminder_ui_t;

static reminder_ui_t s_ui;
static lv_obj_t *s_active_scr;

static volatile bool s_alarm_stop_requested;
static bool s_alarm_active;
static TaskHandle_t s_alarm_task;
static char s_alarm_message[REMINDER_TEXT_LEN];

static int s_edit_draft_hour12;
static int s_edit_draft_minute;
static bool s_edit_draft_is_pm;
static int s_edit_reminder_idx;
static ui_wizard_mode_t s_wizard_mode;
static uint8_t s_wizard_draft_hour24;
static uint8_t s_wizard_draft_minute;

static void ui_set_time_btn_cb(lv_event_t *event);
static void ui_pick_btn_cb(lv_event_t *event);
static void ui_update_pick_labels(void);
static void ui_update_time_edit_labels(void);
static void ui_update_settings_clock_label(void);
static void ui_update_reminder_list(void);
static void ui_reminder_add_cb(lv_event_t *event);
static void ui_reminder_remove_cb(lv_event_t *event);
static void ui_load_pickers_from_target(void);
static void ui_apply_pickers_to_target(void);
static void ui_open_time_editor(ui_pick_target_t target, int reminder_idx);
static void ui_open_wizard_message_screen(void);
static void ui_build_reminder_edit_screen(void);
static void ui_reminder_edit_do_save(void);
static void ui_reminder_kb_action_cb(lv_event_t *event);
static void ui_reminder_ta_event_cb(lv_event_t *event);
static void ui_reminder_edit_show_keyboard(void);
static void ui_reminder_edit_hide_keyboard(void);
static void ui_add_centered_time_pickers(lv_obj_t *scr, int mid_y,
                                         lv_obj_t **lbl_hour, lv_obj_t **lbl_minute, lv_obj_t **lbl_ampm);
static void ui_settings_save_cb(lv_event_t *event);
static void ui_settings_back_cb(lv_event_t *event);
static void ui_time_edit_cancel_cb(lv_event_t *event);
static void ui_time_edit_save_cb(lv_event_t *event);
static void ui_settings_clock_row_cb(lv_event_t *event);
static void ui_settings_hotspot_cb(lv_event_t *event);
static void ui_settings_hold_timer_cb(lv_timer_t *timer);
static void ui_reminder_row_cb(lv_event_t *event);
static void ui_get_now(int *hour24, int *minute, int *second);
static void ui_check_reminders(int hour24, int minute, int second);
static void ui_build_alarm_screen(void);
static void ui_alarm_stop_cb(lv_event_t *event);
static void ui_alarm_task(void *arg);
static void ui_start_alarm(int reminder_idx);
static bool ui_alarm_wait_ms(uint32_t ms);
static void ui_alarm_finish(void);

static int ui_hour12_to_24(int hour12, bool is_pm)
{
    if (hour12 <= 0) {
        hour12 = 12;
    } else if (hour12 > 12) {
        hour12 = 12;
    }

    if (is_pm) {
        return (hour12 == 12) ? 12 : hour12 + 12;
    }
    return (hour12 == 12) ? 0 : hour12;
}

static void ui_hour24_to_12(int hour24, int *hour12, bool *is_pm)
{
    *is_pm = hour24 >= 12;
    *hour12 = hour24 % 12;
    if (*hour12 == 0) {
        *hour12 = 12;
    }
}

static void ui_apply_time_font(lv_obj_t *lbl, bool large)
{
    lv_obj_set_style_text_font(lbl, large ? UI_FONT_TIME_LARGE : UI_FONT_TIME_SMALL, 0);
}

static void ui_make_noninteractive(lv_obj_t *obj)
{
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void ui_style_step_btn(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_SURFACE_RAISED), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_SURFACE_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn, lv_color_hex(UI_BORDER_COLOR), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, UI_RADIUS_SM, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 6);
}

static void ui_style_accent_btn(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_ACCENT_COLOR), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_ACCENT_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, UI_RADIUS_MD, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
}

static void ui_style_danger_btn(lv_obj_t *btn)
{
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_DANGER_COLOR), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(UI_DANGER_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, UI_RADIUS_MD, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
}

static void ui_style_card(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_SURFACE_COLOR), 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_SURFACE_RAISED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_BORDER_COLOR), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, UI_RADIUS_MD, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
}

static void ui_style_input(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_SURFACE_RAISED), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_BORDER_COLOR), 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, UI_RADIUS_SM, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void ui_style_textarea_cursor(lv_obj_t *ta)
{
    /* Default LVGL light-theme cursor is dark and invisible on our dark inputs. */
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_ACCENT_SOFT), LV_PART_CURSOR);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR);
    lv_obj_set_style_pad_left(ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_pad_right(ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_ACCENT_SOFT), LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(ta, 2, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR | LV_STATE_FOCUSED);
    lv_textarea_set_cursor_click_pos(ta, true);
    lv_obj_remove_flag(ta, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
}

static void ui_reminder_ta_event_cb(lv_event_t *event)
{
    lv_obj_t *ta = lv_event_get_target(event);
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED || code == LV_EVENT_FOCUSED) {
        lv_obj_add_state(ta, LV_STATE_FOCUSED);
        lv_keyboard_set_textarea(s_ui.kb_reminder_text, ta);
    }
}

static void ui_style_scroll(lv_obj_t *obj)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(UI_BG_COLOR), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
}

static void ui_style_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(UI_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *ui_create_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_style_screen(scr);
    return scr;
}

static void ui_load_screen(lv_obj_t *scr)
{
    if (s_ui.settings_hold_timer != NULL) {
        lv_timer_delete(s_ui.settings_hold_timer);
        s_ui.settings_hold_timer = NULL;
    }

    lv_screen_load(scr);
    s_active_scr = scr;
    lv_obj_invalidate(scr);
}

static void ui_get_now(int *hour24, int *minute, int *second)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const int64_t elapsed_s = (now_ms - s_ui.set_epoch_ms) / 1000;
    int total_s = s_ui.set_hour * 3600 + s_ui.set_minute * 60 + s_ui.set_second + (int)elapsed_s;
    total_s = ((total_s % 86400) + 86400) % 86400;

    if (hour24) {
        *hour24 = total_s / 3600;
    }
    if (minute) {
        *minute = (total_s / 60) % 60;
    }
    if (second) {
        *second = total_s % 60;
    }
}

static void ui_sync_runtime_from_settings(void)
{
    s_ui.set_hour = s_ui.settings.hour24;
    s_ui.set_minute = s_ui.settings.minute;
    s_ui.set_second = s_ui.settings.second;
    s_ui.set_epoch_ms = s_ui.settings.epoch_ms;
}

static void ui_sync_settings_from_runtime(void)
{
    s_ui.settings.hour24 = (uint8_t)s_ui.set_hour;
    s_ui.settings.minute = (uint8_t)s_ui.set_minute;
    s_ui.settings.second = (uint8_t)s_ui.set_second;
    s_ui.settings.epoch_ms = s_ui.set_epoch_ms;
}

static lv_obj_t *ui_create_pick_btn(lv_obj_t *parent, const char *text, int x, int y,
                                      ui_pick_action_t action, int w, int h, lv_align_t align)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_align(btn, align, x, y);
    ui_style_step_btn(btn);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl);
    ui_make_noninteractive(lbl);

    lv_obj_add_event_cb(btn, ui_pick_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);
    return btn;
}

static lv_obj_t *ui_create_pick_value_on(lv_obj_t *parent, int x, int y, int w, int h,
                                         bool large, lv_align_t align)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, w, h);
    lv_obj_align(box, align, x, y);
    ui_style_input(box);
    ui_make_noninteractive(box);

    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT_PRIMARY), 0);
    ui_apply_time_font(lbl, large);
    lv_obj_center(lbl);
    ui_make_noninteractive(lbl);
    return lbl;
}

static void ui_update_pick_labels(void)
{
    char buf[8];

    snprintf(buf, sizeof(buf), "%02d", s_ui.pick_hour12);
    lv_label_set_text(s_ui.lbl_pick_hour, buf);

    snprintf(buf, sizeof(buf), "%02d", s_ui.pick_minute);
    lv_label_set_text(s_ui.lbl_pick_minute, buf);

    lv_label_set_text(s_ui.lbl_pick_ampm, s_ui.pick_is_pm ? "PM" : "AM");
}

static void ui_update_time_edit_labels(void)
{
    char buf[8];

    snprintf(buf, sizeof(buf), "%02d", s_ui.pick_hour12);
    lv_label_set_text(s_ui.lbl_edit_pick_hour, buf);

    snprintf(buf, sizeof(buf), "%02d", s_ui.pick_minute);
    lv_label_set_text(s_ui.lbl_edit_pick_minute, buf);

    lv_label_set_text(s_ui.lbl_edit_pick_ampm, s_ui.pick_is_pm ? "PM" : "AM");
}

static void ui_update_settings_clock_label(void)
{
    if (s_ui.lbl_settings_clock == NULL || s_active_scr != s_ui.scr_settings) {
        return;
    }

    int hour24;
    int minute;
    int second;
    ui_get_now(&hour24, &minute, &second);

    bool is_pm;
    int hour12;
    ui_hour24_to_12(hour24, &hour12, &is_pm);

    char buf[20];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d %s", hour12, minute, second, is_pm ? "PM" : "AM");
    lv_label_set_text(s_ui.lbl_settings_clock, buf);
}

static void ui_format_time_12h(uint8_t hour24, uint8_t minute, char *buf, size_t buf_size)
{
    bool is_pm;
    int hour12;
    ui_hour24_to_12(hour24, &hour12, &is_pm);
    snprintf(buf, buf_size, "%02d:%02d %s", hour12, minute, is_pm ? "PM" : "AM");
}

static void ui_format_reminder_time_label(int idx, char *buf, size_t buf_size)
{
    const reminder_entry_t *r = &s_ui.settings.reminders[idx];
    ui_format_time_12h(r->hour24, r->minute, buf, buf_size);
}

static void ui_update_reminder_list(void)
{
    const int count = s_ui.settings.reminder_count;
    char time_buf[16];

    for (int i = 0; i < REMINDER_COUNT; i++) {
        if (i < count) {
            const reminder_entry_t *r = &s_ui.settings.reminders[i];

            lv_obj_remove_flag(s_ui.reminder_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_ui.reminder_lbls[i], r->text);
            ui_format_reminder_time_label(i, time_buf, sizeof(time_buf));
            lv_label_set_text(s_ui.reminder_time_lbls[i], time_buf);
            lv_obj_set_y(s_ui.reminder_rows[i], UI_REM_ROW_Y_BASE + (i * UI_REM_ROW_SPACING));
        } else {
            lv_obj_add_flag(s_ui.reminder_rows[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_ui.btn_add_reminder != NULL) {
        if (count < REMINDER_COUNT) {
            lv_obj_remove_flag(s_ui.btn_add_reminder, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_y(s_ui.btn_add_reminder, UI_REM_ROW_Y_BASE + (count * UI_REM_ROW_SPACING) + UI_REM_ADD_GAP);
        } else {
            lv_obj_add_flag(s_ui.btn_add_reminder, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ui_load_pickers_from_target(void)
{
    if (s_ui.pick_target == UI_PICK_TARGET_WIZARD_EDIT) {
        const reminder_entry_t *r = &s_ui.settings.reminders[s_ui.pick_reminder_idx];
        ui_hour24_to_12(r->hour24, &s_ui.pick_hour12, &s_ui.pick_is_pm);
        s_ui.pick_minute = r->minute;
    } else if (s_ui.pick_target == UI_PICK_TARGET_WIZARD_ADD) {
        s_ui.pick_hour12 = 12;
        s_ui.pick_minute = 0;
        s_ui.pick_is_pm = false;
    } else {
        int hour24;
        ui_get_now(&hour24, &s_ui.pick_minute, NULL);
        ui_hour24_to_12(hour24, &s_ui.pick_hour12, &s_ui.pick_is_pm);
    }
}

static void ui_apply_pickers_to_target(void)
{
    const int hour24 = ui_hour12_to_24(s_ui.pick_hour12, s_ui.pick_is_pm);

    if (s_ui.pick_target == UI_PICK_TARGET_SETTINGS_CLOCK) {
        s_ui.set_hour = hour24;
        s_ui.set_minute = s_ui.pick_minute;
        s_ui.set_second = 0;
        s_ui.set_epoch_ms = esp_timer_get_time() / 1000;
        ui_sync_settings_from_runtime();
    }
}

static void ui_pick_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const ui_pick_action_t action = (ui_pick_action_t)(intptr_t)lv_event_get_user_data(event);

    switch (action) {
    case UI_PICK_HOUR_UP:
        s_ui.pick_hour12 = (s_ui.pick_hour12 % 12) + 1;
        break;
    case UI_PICK_HOUR_DOWN:
        s_ui.pick_hour12 = (s_ui.pick_hour12 == 1) ? 12 : s_ui.pick_hour12 - 1;
        break;
    case UI_PICK_MINUTE_UP:
        s_ui.pick_minute = (s_ui.pick_minute + 1) % 60;
        break;
    case UI_PICK_MINUTE_DOWN:
        s_ui.pick_minute = (s_ui.pick_minute == 0) ? 59 : s_ui.pick_minute - 1;
        break;
    case UI_PICK_AMPM_UP:
        s_ui.pick_is_pm = true;
        break;
    case UI_PICK_AMPM_DOWN:
        s_ui.pick_is_pm = false;
        break;
    }

    if (s_ui.pick_target == UI_PICK_TARGET_BOOT) {
        ui_update_pick_labels();
    } else {
        ui_update_time_edit_labels();
    }
}

static int ui_y_center_between(lv_obj_t *top, lv_obj_t *bottom, int pad)
{
    lv_obj_t *scr = lv_obj_get_parent(top);
    lv_obj_update_layout(scr);

    lv_area_t top_a;
    lv_area_t bot_a;
    lv_obj_get_coords(top, &top_a);
    lv_obj_get_coords(bottom, &bot_a);

    const int gap_top = top_a.y2 + pad;
    const int gap_bottom = bot_a.y1 - pad;
    const int mid_y = (gap_top + gap_bottom) / 2;
    return mid_y - (ILI9341_V_RES / 2);
}

static void ui_build_init_screen(void)
{
    s_ui.scr_init = ui_create_screen();

    lv_obj_t *title = lv_label_create(s_ui.scr_init);
    lv_label_set_text(title, "Reminder");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);
    ui_make_noninteractive(title);

    lv_obj_t *subtitle = lv_label_create(s_ui.scr_init);
    lv_label_set_text(subtitle, "Initializing");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(subtitle, &lv_font_montserrat_16, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 64);
    ui_make_noninteractive(subtitle);

    s_ui.bar_init = lv_bar_create(s_ui.scr_init);
    lv_obj_set_size(s_ui.bar_init, 260, 18);
    lv_obj_align(s_ui.bar_init, LV_ALIGN_CENTER, 0, 0);
    lv_bar_set_range(s_ui.bar_init, 0, 100);
    lv_bar_set_value(s_ui.bar_init, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_ui.bar_init, lv_color_hex(UI_SURFACE_RAISED), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.bar_init, lv_color_hex(UI_ACCENT_COLOR), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ui.bar_init, UI_RADIUS_SM, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.bar_init, UI_RADIUS_SM, LV_PART_INDICATOR);

    s_ui.lbl_init_status = lv_label_create(s_ui.scr_init);
    lv_label_set_text(s_ui.lbl_init_status, "Starting...");
    lv_obj_set_style_text_color(s_ui.lbl_init_status, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(s_ui.lbl_init_status, &lv_font_montserrat_16, 0);
    lv_obj_align(s_ui.lbl_init_status, LV_ALIGN_CENTER, 0, 36);
    ui_make_noninteractive(s_ui.lbl_init_status);
}

static void ui_build_set_time_screen(void)
{
    s_ui.scr_set_time = ui_create_screen();
    s_ui.pick_target = UI_PICK_TARGET_BOOT;
    s_ui.pick_hour12 = 12;
    s_ui.pick_minute = 0;
    s_ui.pick_is_pm = false;

    lv_obj_t *title = lv_label_create(s_ui.scr_set_time);
    lv_label_set_text(title, "Set current time");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 12);
    ui_make_noninteractive(title);

    lv_obj_t *btn = lv_button_create(s_ui.scr_set_time);
    lv_obj_set_size(btn, 140, 40);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -10);
    ui_style_accent_btn(btn);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "Set time");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_center(btn_lbl);
    ui_make_noninteractive(btn_lbl);
    lv_obj_add_event_cb(btn, ui_set_time_btn_cb, LV_EVENT_CLICKED, NULL);

    const int mid_y = ui_y_center_between(title, btn, 12);

    ui_add_centered_time_pickers(s_ui.scr_set_time, mid_y,
                                 &s_ui.lbl_pick_hour, &s_ui.lbl_pick_minute, &s_ui.lbl_pick_ampm);
    ui_update_pick_labels();
}

static void ui_build_time_edit_screen(void)
{
    s_ui.scr_time_edit = ui_create_screen();

    s_ui.lbl_time_edit_title = lv_label_create(s_ui.scr_time_edit);
    lv_label_set_text(s_ui.lbl_time_edit_title, "Set time");
    lv_obj_set_style_text_color(s_ui.lbl_time_edit_title, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_ui.lbl_time_edit_title, &lv_font_montserrat_28, 0);
    lv_obj_align(s_ui.lbl_time_edit_title, LV_ALIGN_TOP_MID, 0, 12);
    ui_make_noninteractive(s_ui.lbl_time_edit_title);

    lv_obj_t *btn_cancel = lv_button_create(s_ui.scr_time_edit);
    lv_obj_set_size(btn_cancel, 120, 40);
    lv_obj_align(btn_cancel, LV_ALIGN_BOTTOM_LEFT, 16, -10);
    ui_style_step_btn(btn_cancel);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_center(lbl_cancel);
    ui_make_noninteractive(lbl_cancel);
    lv_obj_add_event_cb(btn_cancel, ui_time_edit_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_save = lv_button_create(s_ui.scr_time_edit);
    lv_obj_set_size(btn_save, 120, 40);
    lv_obj_align(btn_save, LV_ALIGN_BOTTOM_RIGHT, -16, -10);
    ui_style_accent_btn(btn_save);
    lv_obj_t *lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, "Save");
    lv_obj_set_style_text_color(lbl_save, lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_center(lbl_save);
    ui_make_noninteractive(lbl_save);
    s_ui.lbl_time_edit_save = lbl_save;
    lv_obj_add_event_cb(btn_save, ui_time_edit_save_cb, LV_EVENT_CLICKED, NULL);

    const int mid_y = ui_y_center_between(s_ui.lbl_time_edit_title, btn_cancel, 12);
    ui_add_centered_time_pickers(s_ui.scr_time_edit, mid_y,
                                 &s_ui.lbl_edit_pick_hour, &s_ui.lbl_edit_pick_minute, &s_ui.lbl_edit_pick_ampm);
}

static void ui_add_centered_time_pickers(lv_obj_t *scr, int mid_y,
                                         lv_obj_t **lbl_hour, lv_obj_t **lbl_minute, lv_obj_t **lbl_ampm)
{
    const int step_y = 38;

    ui_create_pick_btn(scr, "+", -80, mid_y - step_y, UI_PICK_HOUR_UP, 52, 34, LV_ALIGN_CENTER);
    *lbl_hour = ui_create_pick_value_on(scr, -80, mid_y, 68, 44, true, LV_ALIGN_CENTER);
    ui_create_pick_btn(scr, "-", -80, mid_y + step_y, UI_PICK_HOUR_DOWN, 52, 34, LV_ALIGN_CENTER);

    ui_create_pick_btn(scr, "+", 0, mid_y - step_y, UI_PICK_MINUTE_UP, 52, 34, LV_ALIGN_CENTER);
    *lbl_minute = ui_create_pick_value_on(scr, 0, mid_y, 68, 44, true, LV_ALIGN_CENTER);
    ui_create_pick_btn(scr, "-", 0, mid_y + step_y, UI_PICK_MINUTE_DOWN, 52, 34, LV_ALIGN_CENTER);

    ui_create_pick_btn(scr, "PM", 80, mid_y - step_y, UI_PICK_AMPM_UP, 52, 34, LV_ALIGN_CENTER);
    *lbl_ampm = ui_create_pick_value_on(scr, 80, mid_y, 68, 44, true, LV_ALIGN_CENTER);
    ui_create_pick_btn(scr, "AM", 80, mid_y + step_y, UI_PICK_AMPM_DOWN, 52, 34, LV_ALIGN_CENTER);

    lv_obj_t *colon = lv_label_create(scr);
    lv_label_set_text(colon, ":");
    lv_obj_set_style_text_color(colon, lv_color_hex(UI_TEXT_PRIMARY), 0);
    ui_apply_time_font(colon, true);
    lv_obj_align(colon, LV_ALIGN_CENTER, -40, mid_y);
    ui_make_noninteractive(colon);
}

static void ui_open_time_editor(ui_pick_target_t target, int reminder_idx)
{
    s_ui.pick_target = target;
    s_ui.pick_reminder_idx = reminder_idx;
    ui_load_pickers_from_target();

    s_edit_draft_hour12 = s_ui.pick_hour12;
    s_edit_draft_minute = s_ui.pick_minute;
    s_edit_draft_is_pm = s_ui.pick_is_pm;

    if (target == UI_PICK_TARGET_SETTINGS_CLOCK) {
        lv_label_set_text(s_ui.lbl_time_edit_title, "Set clock");
        lv_label_set_text(s_ui.lbl_time_edit_save, "Save");
    } else {
        lv_label_set_text(s_ui.lbl_time_edit_title, "When?");
        lv_label_set_text(s_ui.lbl_time_edit_save, "Next");
    }

    ui_update_time_edit_labels();
    ui_load_screen(s_ui.scr_time_edit);
}

static void ui_return_from_time_editor(void)
{
    if (s_ui.pick_target == UI_PICK_TARGET_WIZARD_ADD ||
        s_ui.pick_target == UI_PICK_TARGET_WIZARD_EDIT) {
        s_wizard_mode = UI_WIZARD_NONE;
    }

    reminder_ui_show_settings();
}

static void ui_time_edit_cancel_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    s_ui.pick_hour12 = s_edit_draft_hour12;
    s_ui.pick_minute = s_edit_draft_minute;
    s_ui.pick_is_pm = s_edit_draft_is_pm;
    ui_return_from_time_editor();
}

static void ui_time_edit_save_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    if (s_ui.pick_target == UI_PICK_TARGET_WIZARD_ADD ||
        s_ui.pick_target == UI_PICK_TARGET_WIZARD_EDIT) {
        s_wizard_draft_hour24 = (uint8_t)ui_hour12_to_24(s_ui.pick_hour12, s_ui.pick_is_pm);
        s_wizard_draft_minute = (uint8_t)s_ui.pick_minute;
        if (s_ui.pick_target == UI_PICK_TARGET_WIZARD_EDIT) {
            s_edit_reminder_idx = s_ui.pick_reminder_idx;
        }
        ui_open_wizard_message_screen();
        return;
    }

    ui_apply_pickers_to_target();
    if (s_ui.pick_target == UI_PICK_TARGET_SETTINGS_CLOCK) {
        ui_sync_settings_from_runtime();
    }
    ui_return_from_time_editor();
}

static void ui_settings_clock_row_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    ui_open_time_editor(UI_PICK_TARGET_SETTINGS_CLOCK, 0);
}

static void ui_layout_reminder_edit_screen(void)
{
    if (s_ui.reminder_edit_time_row != NULL) {
        lv_obj_add_flag(s_ui.reminder_edit_time_row, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_open_wizard_message_screen(void)
{
    if (s_wizard_mode == UI_WIZARD_ADD) {
        s_edit_reminder_idx = -1;
        char default_text[REMINDER_TEXT_LEN];
        reminder_entry_format_default_text(s_wizard_draft_hour24, s_wizard_draft_minute,
                                           default_text, sizeof(default_text));
        lv_textarea_set_text(s_ui.ta_reminder_text, default_text);
    } else {
        lv_textarea_set_text(s_ui.ta_reminder_text, s_ui.settings.reminders[s_edit_reminder_idx].text);
    }

    lv_textarea_set_cursor_pos(s_ui.ta_reminder_text, LV_TEXTAREA_CURSOR_LAST);
    ui_layout_reminder_edit_screen();
    ui_reminder_edit_show_keyboard();
    lv_obj_add_state(s_ui.ta_reminder_text, LV_STATE_FOCUSED);
    if (s_ui.reminder_edit_scroll != NULL) {
        lv_obj_scroll_to_y(s_ui.reminder_edit_scroll, 0, LV_ANIM_OFF);
    }
    ui_load_screen(s_ui.scr_reminder_edit);
}

#define UI_KB_BTN(w)             (LV_BUTTONMATRIX_CTRL_POPOVER | (w))

static const char *const ui_reminder_kb_map[] = {
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", ".", "\n",
    "z", "x", "c", "v", "b", "n", "m", ",", " ", LV_SYMBOL_OK, "",
};

static const lv_buttonmatrix_ctrl_t ui_reminder_kb_ctrl[] = {
    /* row 1: q-p + backspace (11) */
    UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1),
    UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1),
    UI_KB_BTN(2),
    /* row 2: a-l + . (10) */
    UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1),
    UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1),
    /* row 3: z-m + , + space + ok (10) */
    UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1),
    UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(1), UI_KB_BTN(6), UI_KB_BTN(1),
};

static void ui_style_reminder_keyboard(lv_obj_t *kb)
{
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_SURFACE_COLOR), 0);
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_SURFACE_RAISED), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(kb, lv_color_hex(UI_SURFACE_PRESSED), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(kb, lv_color_hex(UI_TEXT_PRIMARY), LV_PART_ITEMS);
    lv_obj_set_style_text_opa(kb, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_text_color(kb, lv_color_hex(UI_TEXT_PRIMARY), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_opa(kb, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_16, LV_PART_ITEMS);
    lv_obj_set_style_border_color(kb, lv_color_hex(UI_BORDER_COLOR), LV_PART_ITEMS);
    lv_obj_set_style_border_width(kb, 1, LV_PART_ITEMS);
    lv_obj_set_style_radius(kb, UI_RADIUS_SM, LV_PART_ITEMS);
    lv_obj_set_style_pad_row(kb, 6, LV_PART_ITEMS);
    lv_obj_set_style_pad_column(kb, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_all(kb, UI_REM_EDIT_KB_MARGIN, LV_PART_MAIN);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, ui_reminder_kb_map, ui_reminder_kb_ctrl);
    lv_keyboard_set_mode(kb, LV_KEYBOARD_MODE_USER_1);
    lv_obj_add_event_cb(kb, ui_reminder_kb_action_cb, LV_EVENT_READY, NULL);
}

static void ui_reminder_kb_action_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_READY) {
        ui_reminder_edit_do_save();
    }
}

static void ui_reminder_edit_layout_content(bool kb_visible)
{
    if (s_ui.reminder_edit_scroll == NULL || s_ui.ta_reminder_text == NULL) {
        return;
    }

    const int scroll_h = kb_visible
                             ? (ILI9341_V_RES - UI_REM_EDIT_TOP_PAD - UI_REM_EDIT_KB_H -
                                UI_REM_EDIT_KB_MARGIN - 4)
                             : (ILI9341_V_RES - UI_REM_EDIT_TOP_PAD - 4);
    const int scroll_w = ILI9341_H_RES - 16;

    if (lv_obj_get_width(s_ui.reminder_edit_scroll) != scroll_w ||
        lv_obj_get_height(s_ui.reminder_edit_scroll) != scroll_h) {
        lv_obj_set_size(s_ui.reminder_edit_scroll, scroll_w, scroll_h);
        lv_obj_set_size(s_ui.ta_reminder_text, scroll_w, scroll_h);
    }

    lv_obj_align(s_ui.reminder_edit_scroll, LV_ALIGN_TOP_MID, 0, UI_REM_EDIT_TOP_PAD);
    lv_obj_align(s_ui.ta_reminder_text, LV_ALIGN_TOP_LEFT, 0, 0);
}

static void ui_reminder_edit_show_keyboard(void)
{
    if (s_ui.kb_reminder_text == NULL || s_ui.ta_reminder_text == NULL) {
        return;
    }

    lv_keyboard_set_textarea(s_ui.kb_reminder_text, s_ui.ta_reminder_text);
    lv_obj_remove_flag(s_ui.kb_reminder_text, LV_OBJ_FLAG_HIDDEN);
    ui_reminder_edit_layout_content(true);
}

static void ui_reminder_edit_hide_keyboard(void)
{
    if (s_ui.kb_reminder_text == NULL) {
        return;
    }

    lv_keyboard_set_textarea(s_ui.kb_reminder_text, NULL);
    lv_obj_add_flag(s_ui.kb_reminder_text, LV_OBJ_FLAG_HIDDEN);
    ui_reminder_edit_layout_content(false);
}

static void ui_reminder_edit_do_save(void)
{
    const char *text = lv_textarea_get_text(s_ui.ta_reminder_text);
    ui_reminder_edit_hide_keyboard();

    if (s_wizard_mode == UI_WIZARD_ADD) {
        if (s_ui.settings.reminder_count >= REMINDER_COUNT) {
            s_wizard_mode = UI_WIZARD_NONE;
            reminder_ui_show_settings();
            return;
        }

        const int idx = s_ui.settings.reminder_count;
        reminder_entry_t *r = &s_ui.settings.reminders[idx];
        r->hour24 = s_wizard_draft_hour24;
        r->minute = s_wizard_draft_minute;
        if (text == NULL || text[0] == '\0') {
            reminder_entry_set_default_text(r);
        } else {
            strncpy(r->text, text, sizeof(r->text) - 1);
            r->text[sizeof(r->text) - 1] = '\0';
        }
        s_ui.settings.reminder_count++;
        s_wizard_mode = UI_WIZARD_NONE;
        ui_update_reminder_list();
        reminder_ui_show_settings();
        return;
    }

    if (s_wizard_mode == UI_WIZARD_EDIT && s_edit_reminder_idx >= 0) {
        reminder_entry_t *r = &s_ui.settings.reminders[s_edit_reminder_idx];
        r->hour24 = s_wizard_draft_hour24;
        r->minute = s_wizard_draft_minute;
        if (text == NULL || text[0] == '\0') {
            reminder_entry_set_default_text(r);
        } else {
            strncpy(r->text, text, sizeof(r->text) - 1);
            r->text[sizeof(r->text) - 1] = '\0';
        }
        s_wizard_mode = UI_WIZARD_NONE;
        ui_update_reminder_list();
        reminder_ui_show_settings();
    }
}

static void ui_build_reminder_edit_screen(void)
{
    s_ui.scr_reminder_edit = ui_create_screen();

    s_ui.kb_reminder_text = lv_keyboard_create(s_ui.scr_reminder_edit);
    lv_obj_set_size(s_ui.kb_reminder_text, ILI9341_H_RES, UI_REM_EDIT_KB_H);
    lv_obj_align(s_ui.kb_reminder_text, LV_ALIGN_BOTTOM_MID, 0, -UI_REM_EDIT_KB_MARGIN);
    ui_style_reminder_keyboard(s_ui.kb_reminder_text);
    lv_obj_add_flag(s_ui.kb_reminder_text, LV_OBJ_FLAG_HIDDEN);

    s_ui.reminder_edit_scroll = lv_obj_create(s_ui.scr_reminder_edit);
    ui_style_scroll(s_ui.reminder_edit_scroll);
    lv_obj_set_scrollbar_mode(s_ui.reminder_edit_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(s_ui.reminder_edit_scroll, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.ta_reminder_text = lv_textarea_create(s_ui.reminder_edit_scroll);
    lv_textarea_set_max_length(s_ui.ta_reminder_text, REMINDER_TEXT_LEN - 1);
    lv_textarea_set_one_line(s_ui.ta_reminder_text, false);
    ui_style_input(s_ui.ta_reminder_text);
    ui_style_textarea_cursor(s_ui.ta_reminder_text);
    lv_obj_set_style_text_color(s_ui.ta_reminder_text, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_ui.ta_reminder_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_all(s_ui.ta_reminder_text, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(s_ui.ta_reminder_text, ui_reminder_ta_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_ui.ta_reminder_text, ui_reminder_ta_event_cb, LV_EVENT_FOCUSED, NULL);

    s_ui.reminder_edit_time_row = lv_obj_create(s_ui.reminder_edit_scroll);
    lv_obj_set_size(s_ui.reminder_edit_time_row, 288, UI_REM_EDIT_ROW_H);
    lv_obj_align(s_ui.reminder_edit_time_row, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(s_ui.reminder_edit_time_row);
    lv_obj_set_style_pad_all(s_ui.reminder_edit_time_row, 0, 0);
    lv_obj_remove_flag(s_ui.reminder_edit_time_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_flag(s_ui.reminder_edit_time_row, LV_OBJ_FLAG_HIDDEN);

    s_ui.lbl_reminder_edit_time = lv_label_create(s_ui.reminder_edit_time_row);
    lv_obj_set_style_text_color(s_ui.lbl_reminder_edit_time, lv_color_hex(UI_ACCENT_SOFT), 0);
    lv_obj_set_style_text_font(s_ui.lbl_reminder_edit_time, &lv_font_montserrat_16, 0);
    lv_obj_align(s_ui.lbl_reminder_edit_time, LV_ALIGN_LEFT_MID, 12, 0);
    ui_make_noninteractive(s_ui.lbl_reminder_edit_time);

    s_ui.btn_reminder_edit_time = lv_button_create(s_ui.reminder_edit_time_row);
    lv_obj_set_size(s_ui.btn_reminder_edit_time, 100, 32);
    lv_obj_align(s_ui.btn_reminder_edit_time, LV_ALIGN_RIGHT_MID, -6, 0);
    ui_style_step_btn(s_ui.btn_reminder_edit_time);
    lv_obj_t *lbl_time = lv_label_create(s_ui.btn_reminder_edit_time);
    lv_label_set_text(lbl_time, "Set time");
    lv_obj_set_style_text_color(lbl_time, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_time);
    ui_make_noninteractive(lbl_time);

    ui_layout_reminder_edit_screen();
}

static void ui_reminder_row_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const int idx = (int)(intptr_t)lv_event_get_user_data(event);
    if (idx < 0 || idx >= s_ui.settings.reminder_count) {
        return;
    }
    if (lv_event_get_target(event) != s_ui.reminder_rows[idx]) {
        return;
    }

    s_wizard_mode = UI_WIZARD_EDIT;
    ui_open_time_editor(UI_PICK_TARGET_WIZARD_EDIT, idx);
}

static void ui_reminder_add_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    if (s_ui.settings.reminder_count >= REMINDER_COUNT) {
        return;
    }

    s_wizard_mode = UI_WIZARD_ADD;
    ui_open_time_editor(UI_PICK_TARGET_WIZARD_ADD, -1);
}

static void ui_reminder_remove_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const int idx = (int)(intptr_t)lv_event_get_user_data(event);
    const int count = s_ui.settings.reminder_count;
    if (idx < 0 || idx >= count) {
        return;
    }

    for (int i = idx; i < count - 1; i++) {
        s_ui.settings.reminders[i] = s_ui.settings.reminders[i + 1];
        s_ui.reminder_fired[i] = s_ui.reminder_fired[i + 1];
    }
    s_ui.settings.reminder_count--;
    s_ui.reminder_fired[count - 1] = false;
    ui_update_reminder_list();
}

static void ui_settings_save_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    ui_sync_settings_from_runtime();
    reminder_storage_save(&s_ui.settings);
    memset(s_ui.reminder_fired, 0, sizeof(s_ui.reminder_fired));
    reminder_ui_show_clock();
}

static void ui_settings_back_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    reminder_ui_show_clock();
}

static void ui_settings_hold_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_ui.settings_hold_timer = NULL;
    reminder_ui_show_settings();
}

static void ui_settings_hotspot_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        if (s_ui.settings_hold_timer != NULL) {
            lv_timer_delete(s_ui.settings_hold_timer);
        }
        s_ui.settings_hold_timer = lv_timer_create(ui_settings_hold_timer_cb, UI_SETTINGS_HOLD_MS, NULL);
        lv_timer_set_repeat_count(s_ui.settings_hold_timer, 1);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_ui.settings_hold_timer != NULL) {
            lv_timer_delete(s_ui.settings_hold_timer);
            s_ui.settings_hold_timer = NULL;
        }
    }
}

static void ui_build_settings_screen(void)
{
    s_ui.scr_settings = ui_create_screen();

    lv_obj_t *title = lv_label_create(s_ui.scr_settings);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    ui_make_noninteractive(title);

    lv_obj_t *footer = lv_obj_create(s_ui.scr_settings);
    lv_obj_set_size(footer, ILI9341_H_RES, 44);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    ui_style_scroll(footer);
    ui_make_noninteractive(footer);

    lv_obj_t *btn_back = lv_button_create(footer);
    lv_obj_set_size(btn_back, 120, 34);
    lv_obj_align(btn_back, LV_ALIGN_LEFT_MID, 16, 0);
    ui_style_step_btn(btn_back);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_set_style_text_color(lbl_back, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_center(lbl_back);
    ui_make_noninteractive(lbl_back);
    lv_obj_add_event_cb(btn_back, ui_settings_back_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_save = lv_button_create(footer);
    lv_obj_set_size(btn_save, 120, 34);
    lv_obj_align(btn_save, LV_ALIGN_RIGHT_MID, -16, 0);
    ui_style_accent_btn(btn_save);
    lv_obj_t *lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, "Save");
    lv_obj_set_style_text_color(lbl_save, lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_center(lbl_save);
    ui_make_noninteractive(lbl_save);
    lv_obj_add_event_cb(btn_save, ui_settings_save_cb, LV_EVENT_CLICKED, NULL);

    s_ui.settings_scroll = lv_obj_create(s_ui.scr_settings);
    lv_obj_set_size(s_ui.settings_scroll, ILI9341_H_RES - 16, ILI9341_V_RES - 44 - 40);
    lv_obj_align(s_ui.settings_scroll, LV_ALIGN_TOP_MID, 0, 36);
    ui_style_scroll(s_ui.settings_scroll);
    lv_obj_set_scrollbar_mode(s_ui.settings_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(s_ui.settings_scroll, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row_clock = lv_button_create(s_ui.settings_scroll);
    lv_obj_set_size(row_clock, 288, 44);
    lv_obj_align(row_clock, LV_ALIGN_TOP_LEFT, 0, 0);
    ui_style_card(row_clock);
    lv_obj_add_event_cb(row_clock, ui_settings_clock_row_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_clock_title = lv_label_create(row_clock);
    lv_label_set_text(lbl_clock_title, "Clock");
    lv_obj_set_style_text_color(lbl_clock_title, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(lbl_clock_title, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_clock_title, LV_ALIGN_LEFT_MID, 12, 0);
    ui_make_noninteractive(lbl_clock_title);

    s_ui.lbl_settings_clock = lv_label_create(row_clock);
    lv_obj_set_style_text_color(s_ui.lbl_settings_clock, lv_color_hex(UI_ACCENT_SOFT), 0);
    lv_obj_set_style_text_font(s_ui.lbl_settings_clock, &lv_font_montserrat_16, 0);
    lv_obj_align(s_ui.lbl_settings_clock, LV_ALIGN_RIGHT_MID, -12, 0);
    ui_make_noninteractive(s_ui.lbl_settings_clock);

    lv_obj_t *lbl_rem = lv_label_create(s_ui.settings_scroll);
    lv_label_set_text(lbl_rem, "Reminders");
    lv_obj_set_style_text_color(lbl_rem, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_obj_set_style_text_font(lbl_rem, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_rem, LV_ALIGN_TOP_LEFT, 4, 52);
    ui_make_noninteractive(lbl_rem);

    for (int i = 0; i < REMINDER_COUNT; i++) {
        lv_obj_t *row = lv_button_create(s_ui.settings_scroll);
        lv_obj_set_size(row, 288, UI_REM_ROW_HEIGHT);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, UI_REM_ROW_Y_BASE + (i * UI_REM_ROW_SPACING));
        ui_style_card(row);
        lv_obj_set_style_pad_all(row, 0, 0);
        s_ui.reminder_rows[i] = row;

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT_PRIMARY), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_width(lbl, UI_REM_TEXT_WIDTH);
        lv_obj_set_height(lbl, lv_font_get_line_height(&lv_font_montserrat_14));
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
        lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, UI_REM_ROW_PAD, UI_REM_MSG_Y);
        ui_make_noninteractive(lbl);
        s_ui.reminder_lbls[i] = lbl;

        lv_obj_t *time_lbl = lv_label_create(row);
        lv_obj_set_style_text_color(time_lbl, lv_color_hex(UI_ACCENT_SOFT), 0);
        lv_obj_set_style_text_font(time_lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(time_lbl, LV_ALIGN_TOP_LEFT, UI_REM_ROW_PAD, UI_REM_TIME_Y);
        ui_make_noninteractive(time_lbl);
        s_ui.reminder_time_lbls[i] = time_lbl;

        lv_obj_t *btn_rm = lv_button_create(row);
        lv_obj_set_size(btn_rm, UI_REM_REMOVE_BTN_W, UI_REM_REMOVE_BTN_H);
        lv_obj_align(btn_rm, LV_ALIGN_RIGHT_MID, -6, 0);
        ui_style_step_btn(btn_rm);
        lv_obj_t *lbl_rm = lv_label_create(btn_rm);
        lv_label_set_text(lbl_rm, "X");
        lv_obj_set_style_text_color(lbl_rm, lv_color_hex(UI_DANGER_COLOR), 0);
        lv_obj_set_style_text_font(lbl_rm, &lv_font_montserrat_16, 0);
        lv_obj_center(lbl_rm);
        ui_make_noninteractive(lbl_rm);
        s_ui.reminder_remove_btns[i] = btn_rm;
        lv_obj_add_event_cb(btn_rm, ui_reminder_remove_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_add_event_cb(row, ui_reminder_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    s_ui.btn_add_reminder = lv_button_create(s_ui.settings_scroll);
    lv_obj_set_size(s_ui.btn_add_reminder, 288, 38);
    lv_obj_align(s_ui.btn_add_reminder, LV_ALIGN_TOP_LEFT, 0, UI_REM_ROW_Y_BASE);
    ui_style_accent_btn(s_ui.btn_add_reminder);
    lv_obj_t *lbl_add = lv_label_create(s_ui.btn_add_reminder);
    lv_label_set_text(lbl_add, "Add reminder");
    lv_obj_set_style_text_color(lbl_add, lv_color_hex(UI_ON_ACCENT), 0);
    lv_obj_set_style_text_font(lbl_add, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_add);
    ui_make_noninteractive(lbl_add);
    lv_obj_add_event_cb(s_ui.btn_add_reminder, ui_reminder_add_cb, LV_EVENT_CLICKED, NULL);

    ui_update_reminder_list();
}

static void ui_check_reminders(int hour24, int minute, int second)
{
    const int count = s_ui.settings.reminder_count;

    if (second != 0) {
        for (int i = 0; i < count; i++) {
            const reminder_entry_t *r = &s_ui.settings.reminders[i];
            if (r->hour24 != hour24 || r->minute != minute) {
                s_ui.reminder_fired[i] = false;
            }
        }
        return;
    }

    if (s_alarm_active) {
        return;
    }

    for (int i = 0; i < count; i++) {
        const reminder_entry_t *r = &s_ui.settings.reminders[i];
        if (r->hour24 != hour24 || r->minute != minute) {
            s_ui.reminder_fired[i] = false;
            continue;
        }
        if (!s_ui.reminder_fired[i]) {
            s_ui.reminder_fired[i] = true;
            ESP_LOGI(TAG, "Reminder %d triggered at %02d:%02d: %s",
                     i + 1, hour24, minute, r->text);
            ui_start_alarm(i);
            break;
        }
    }
}

static bool ui_alarm_wait_ms(uint32_t ms)
{
    const uint32_t step_ms = 100;
    uint32_t elapsed = 0;

    while (elapsed < ms) {
        if (s_alarm_stop_requested) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        elapsed += step_ms;
    }

    return !s_alarm_stop_requested;
}

static void ui_alarm_finish(void)
{
    audio_sr_stop();
    audio_tts_clear_alarm_cache();

    s_alarm_active = false;
    s_alarm_stop_requested = false;
    s_alarm_task = NULL;

    lvgl_port_lock(0);
    reminder_ui_show_clock();
    lvgl_port_unlock();
}

static void ui_alarm_task(void *arg)
{
    (void)arg;

    if (audio_play_tone(AUDIO_WAKE_TONE_HZ, AUDIO_WAKE_TONE_MS, AUDIO_WAKE_TONE_VOLUME) != ESP_OK) {
        ESP_LOGW(TAG, "Alarm alert tone failed");
    }
    if (s_alarm_stop_requested) {
        ui_alarm_finish();
        vTaskDelete(NULL);
        return;
    }

    const bool alarm_cached = (audio_tts_cache_alarm(s_alarm_message, &s_alarm_stop_requested) == ESP_OK);
    if (!alarm_cached) {
        ESP_LOGW(TAG, "Alarm message cache failed, will synthesize on demand");
    }
    if (s_alarm_stop_requested) {
        ui_alarm_finish();
        vTaskDelete(NULL);
        return;
    }

    for (int cycle = 0; cycle < ALARM_MAX_CYCLES; cycle++) {
        if (s_alarm_stop_requested) {
            break;
        }

        if (alarm_cached) {
            if (audio_tts_play_alarm(&s_alarm_stop_requested) != ESP_OK) {
                ESP_LOGW(TAG, "Alarm speech failed");
            }
        } else if (audio_tts_speak(s_alarm_message, &s_alarm_stop_requested) != ESP_OK) {
            ESP_LOGW(TAG, "Alarm speech failed");
        }
        if (s_alarm_stop_requested) {
            break;
        }

        if (!ui_alarm_wait_ms(ALARM_PAUSE_MS)) {
            break;
        }

        if (audio_play_classical_tune(&s_alarm_stop_requested) != ESP_OK) {
            ESP_LOGW(TAG, "Alarm tune failed");
        }
        if (s_alarm_stop_requested) {
            break;
        }

        if (!ui_alarm_wait_ms(ALARM_PAUSE_MS)) {
            break;
        }
    }

    ui_alarm_finish();
    vTaskDelete(NULL);
}

static void ui_start_alarm(int reminder_idx)
{
    if (s_alarm_active || s_alarm_task != NULL) {
        return;
    }
    if (reminder_idx < 0 || reminder_idx >= s_ui.settings.reminder_count) {
        return;
    }

    const reminder_entry_t *entry = &s_ui.settings.reminders[reminder_idx];
    strncpy(s_alarm_message, entry->text, sizeof(s_alarm_message) - 1);
    s_alarm_message[sizeof(s_alarm_message) - 1] = '\0';

    s_alarm_stop_requested = false;
    s_alarm_active = true;

    lvgl_port_lock(0);
    lv_label_set_text(s_ui.lbl_alarm_message, s_alarm_message);
    ui_load_screen(s_ui.scr_alarm);
    lvgl_port_unlock();

    if (audio_sr_start() != ESP_OK) {
        ESP_LOGE(TAG, "Speech recognition start failed");
        s_alarm_active = false;
        lvgl_port_lock(0);
        reminder_ui_show_clock();
        lvgl_port_unlock();
        return;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(
        ui_alarm_task,
        "alarm",
        UI_ALARM_TASK_STACK,
        NULL,
        UI_ALARM_TASK_PRIO,
        &s_alarm_task,
        0);

    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Alarm task create failed");
        s_alarm_active = false;
        s_alarm_task = NULL;
        audio_sr_stop();
        lvgl_port_lock(0);
        reminder_ui_show_clock();
        lvgl_port_unlock();
    }
}

static void ui_alarm_stop_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    s_alarm_stop_requested = true;
}

static void ui_build_alarm_screen(void)
{
    s_ui.scr_alarm = ui_create_screen();

    lv_obj_t *title = lv_label_create(s_ui.scr_alarm);
    lv_label_set_text(title, "Reminder");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_ACCENT_SOFT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);
    ui_make_noninteractive(title);

    s_ui.lbl_alarm_message = lv_label_create(s_ui.scr_alarm);
    lv_obj_set_width(s_ui.lbl_alarm_message, 288);
    lv_label_set_long_mode(s_ui.lbl_alarm_message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(s_ui.lbl_alarm_message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_ui.lbl_alarm_message, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_ui.lbl_alarm_message, &lv_font_montserrat_16, 0);
    lv_obj_align(s_ui.lbl_alarm_message, LV_ALIGN_CENTER, 0, -10);
    ui_make_noninteractive(s_ui.lbl_alarm_message);

    lv_obj_t *btn_stop = lv_button_create(s_ui.scr_alarm);
    lv_obj_set_size(btn_stop, 180, 44);
    lv_obj_align(btn_stop, LV_ALIGN_BOTTOM_MID, 0, -10);
    ui_style_danger_btn(btn_stop);
    lv_obj_add_event_cb(btn_stop, ui_alarm_stop_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_stop = lv_label_create(btn_stop);
    lv_label_set_text(lbl_stop, "Stop");
    lv_obj_set_style_text_color(lbl_stop, lv_color_hex(UI_ON_DANGER), 0);
    lv_obj_set_style_text_font(lbl_stop, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_stop);
    ui_make_noninteractive(lbl_stop);
}

static void ui_update_clock_label(void)
{
    if (s_ui.lbl_clock == NULL) {
        return;
    }

    int hour24;
    int minute;
    int second;
    ui_get_now(&hour24, &minute, &second);

    bool is_pm;
    int hour12;
    ui_hour24_to_12(hour24, &hour12, &is_pm);

    char buf[12];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hour12, minute, second);
    lv_label_set_text(s_ui.lbl_clock, buf);
    lv_label_set_text(s_ui.lbl_clock_ampm, is_pm ? "PM" : "AM");

    ui_check_reminders(hour24, minute, second);
}

static void ui_clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_update_clock_label();
    ui_update_settings_clock_label();
}

static void ui_build_clock_screen(void)
{
    s_ui.scr_clock = ui_create_screen();

    lv_obj_t *title = lv_label_create(s_ui.scr_clock);
    lv_label_set_text(title, "Time");
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
    ui_make_noninteractive(title);

    s_ui.lbl_clock = lv_label_create(s_ui.scr_clock);
    lv_label_set_text(s_ui.lbl_clock, "12:00:00");
    lv_obj_set_style_text_color(s_ui.lbl_clock, lv_color_hex(UI_TEXT_PRIMARY), 0);
    lv_obj_set_style_text_font(s_ui.lbl_clock, UI_FONT_CLOCK_DISPLAY, 0);
    lv_obj_set_style_text_letter_space(s_ui.lbl_clock, 1, 0);
    lv_obj_align(s_ui.lbl_clock, LV_ALIGN_CENTER, 0, -14);
    ui_make_noninteractive(s_ui.lbl_clock);

    s_ui.lbl_clock_ampm = lv_label_create(s_ui.scr_clock);
    lv_label_set_text(s_ui.lbl_clock_ampm, "AM");
    lv_obj_set_style_text_color(s_ui.lbl_clock_ampm, lv_color_hex(UI_ACCENT_SOFT), 0);
    lv_obj_set_style_text_font(s_ui.lbl_clock_ampm, UI_FONT_TIME_LARGE, 0);
    lv_obj_align(s_ui.lbl_clock_ampm, LV_ALIGN_CENTER, 0, 34);
    ui_make_noninteractive(s_ui.lbl_clock_ampm);

    lv_obj_t *hotspot = lv_button_create(s_ui.scr_clock);
    lv_obj_set_size(hotspot, UI_SETTINGS_HOTSPOT_W, UI_SETTINGS_HOTSPOT_H);
    lv_obj_align(hotspot, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(hotspot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hotspot, 0, 0);
    lv_obj_set_style_shadow_width(hotspot, 0, 0);
    lv_obj_add_event_cb(hotspot, ui_settings_hotspot_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(hotspot, ui_settings_hotspot_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(hotspot, ui_settings_hotspot_cb, LV_EVENT_PRESS_LOST, NULL);

    ui_update_clock_label();

    if (s_ui.clock_timer == NULL) {
        s_ui.clock_timer = lv_timer_create(ui_clock_timer_cb, 1000, NULL);
    }
}

static void ui_set_time_btn_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const int hour12 = s_ui.pick_hour12;
    s_ui.set_minute = s_ui.pick_minute;
    const bool is_pm = s_ui.pick_is_pm;
    s_ui.set_hour = ui_hour12_to_24(hour12, is_pm);
    s_ui.set_second = 0;
    s_ui.set_epoch_ms = esp_timer_get_time() / 1000;

    ui_sync_settings_from_runtime();
    reminder_storage_save(&s_ui.settings);

    ESP_LOGI(TAG, "Time set to %02d:%02d %s", hour12, s_ui.set_minute, is_pm ? "PM" : "AM");

    int hour24;
    int minute;
    int second;
    ui_get_now(&hour24, &minute, &second);
    ui_check_reminders(hour24, minute, second);

    if (s_ui.time_set_sem) {
        xSemaphoreGive(s_ui.time_set_sem);
    }
}

esp_err_t reminder_ui_init(lv_display_t *display)
{
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_ui, 0, sizeof(s_ui));
    s_ui.display = display;
    s_ui.time_set_sem = xSemaphoreCreateBinary();
    if (s_ui.time_set_sem == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (reminder_storage_load(&s_ui.settings) != ESP_OK) {
        memset(&s_ui.settings.reminders, 0, sizeof(s_ui.settings.reminders));
        s_ui.settings.reminder_count = 0;
    }
    ui_sync_runtime_from_settings();

    ui_build_init_screen();
    ui_build_set_time_screen();
    ui_build_time_edit_screen();
    ui_build_reminder_edit_screen();
    ui_build_alarm_screen();
    ui_build_clock_screen();
    ui_build_settings_screen();

    return ESP_OK;
}

void reminder_ui_show_init(void)
{
    ui_load_screen(s_ui.scr_init);
}

void reminder_ui_set_init_progress(int percent, const char *status)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    lv_bar_set_value(s_ui.bar_init, percent, LV_ANIM_ON);
    if (status != NULL) {
        lv_label_set_text(s_ui.lbl_init_status, status);
    }
    if (s_ui.display) {
        lv_refr_now(s_ui.display);
    }
}

void reminder_ui_show_set_time(void)
{
    s_ui.pick_target = UI_PICK_TARGET_BOOT;
    ui_load_screen(s_ui.scr_set_time);
}

esp_err_t reminder_ui_wait_time_set(void)
{
    if (s_ui.time_set_sem == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_ui.time_set_sem, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

void reminder_ui_show_clock(void)
{
    if (s_alarm_active) {
        return;
    }

    ui_load_screen(s_ui.scr_clock);
    ui_update_clock_label();
}

void reminder_ui_show_settings(void)
{
    if (s_alarm_active) {
        return;
    }

    ui_update_settings_clock_label();
    ui_update_reminder_list();
    ui_load_screen(s_ui.scr_settings);
}

bool reminder_ui_alarm_is_active(void)
{
    return s_alarm_active;
}

void reminder_ui_alarm_request_stop(void)
{
    if (!s_alarm_active) {
        return;
    }

    s_alarm_stop_requested = true;
}

const reminder_settings_t *reminder_ui_get_settings(void)
{
    return &s_ui.settings;
}
