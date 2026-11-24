/* SPDX-License-Identifier: Apache-2.0 */

#include "gateway_ui.h"

#include "ble_gateway.h"
#include "ble_protocol.h"
#include "esp_lvgl_port.h"
#include "gateway_display.h"

LV_FONT_DECLARE(gateway_font_16);

enum
{
    SCREEN_WIDTH = 240,
    ANGLE_STEP = 5,
    SERVO_MIDPOINT = 90,
    COLOR_PAPER = 0xF2F6F9,
    COLOR_INK = 0x142B3D,
    COLOR_MUTED = 0x4B6679,
    COLOR_BORDER = 0xCAD8E1,
    COLOR_BLUE = 0x2058C9,
    COLOR_HEADER = 0x142E44,
    COLOR_NEUTRAL = 0xE6EDF3,
    COLOR_OK = 0x106657,
    COLOR_OK_BG = 0xE4F0ED,
    COLOR_WARN = 0x854812,
    COLOR_WARN_BG = 0xFAECD9,
};

/* LVGL 事件和定时器都在同一任务中维护界面对象。 */
static ble_gateway_status_t s_status;
static uint16_t s_target_angle = 120;
static lv_obj_t *s_link_badge;
static lv_obj_t *s_link_dot;
static lv_obj_t *s_link_label;
static lv_obj_t *s_angle_arc;
static lv_obj_t *s_angle_value;
static lv_obj_t *s_reply_row;
static lv_obj_t *s_reply_dot;
static lv_obj_t *s_reply_label;
static lv_obj_t *s_reply_value;
static lv_obj_t *s_decrease;
static lv_obj_t *s_center;
static lv_obj_t *s_increase;
static lv_obj_t *s_send;
static lv_obj_t *s_send_label;
static lv_obj_t *s_ota_label;

static void set_fill(lv_obj_t *object, uint32_t color)
{
    lv_obj_set_style_bg_color(object, lv_color_hex(color), LV_PART_MAIN);
}

static lv_obj_t *create_surface(lv_obj_t *parent, int x, int y, int width, int height, int radius)
{
    lv_obj_t *surface = lv_obj_create(parent);
    lv_obj_remove_style_all(surface);
    lv_obj_remove_flag(surface, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(surface, x, y);
    lv_obj_set_size(surface, width, height);
    lv_obj_set_style_radius(surface, radius, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(surface, LV_OPA_COVER, LV_PART_MAIN);
    return surface;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *create_button(lv_obj_t *screen, const char *text, int x, int y, int width,
                               bool primary, lv_event_cb_t callback)
{
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_remove_style_all(button);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 44);
    lv_obj_set_style_radius(button, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    set_fill(button, primary ? COLOR_BLUE : 0xFFFFFF);
    lv_obj_set_style_bg_color(button, lv_color_hex(primary ? 0x143F99 : 0xE2EBF2),
                              LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xE3EBF1), LV_STATE_DISABLED);
    lv_obj_set_style_border_width(button, primary ? 0 : 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(button, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_border_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = create_label(button, text, &gateway_font_16, primary ? 0xFFFFFF : COLOR_INK);
    lv_obj_center(label);
    return button;
}

static void refresh_controls(void)
{
    const bool waiting = s_status.angle_state == BLE_GATEWAY_ANGLE_WAITING;
    const bool send_disabled = waiting || s_status.link != BLE_GATEWAY_READY;
    lv_obj_set_state(s_decrease, LV_STATE_DISABLED, waiting || s_target_angle == 0);
    lv_obj_set_state(s_center, LV_STATE_DISABLED, waiting);
    lv_obj_set_state(s_increase, LV_STATE_DISABLED,
                     waiting || s_target_angle == BLE_LINK_SERVO_MAX_ANGLE);
    lv_obj_set_state(s_send, LV_STATE_DISABLED, send_disabled);
    lv_label_set_text(s_send_label, waiting ? "下发中" : "下发角度");
    lv_obj_set_style_text_color(s_send_label, lv_color_hex(send_disabled ? COLOR_MUTED : 0xFFFFFF),
                                LV_PART_MAIN);
}

static void refresh_status(void)
{
    const bool online = s_status.link == BLE_GATEWAY_READY;
    const char *link_text = online                                    ? "C3 在线"
                            : s_status.link == BLE_GATEWAY_CONNECTING ? "连接 C3"
                                                                      : "扫描 C3";
    const uint32_t link_ink = online ? 0xD3F6ED : 0xFFE4BC;
    set_fill(s_link_badge, online ? 0x234052 : 0x4A3B32);
    set_fill(s_link_dot, link_ink);
    lv_obj_set_style_text_font(s_link_label, s_status.ota_mode ? LV_FONT_DEFAULT : &gateway_font_16,
                               LV_PART_MAIN);
    lv_label_set_text(s_link_label, s_status.ota_mode ? "OTA" : link_text);
    lv_label_set_text(s_ota_label, s_status.ota_mode ? "Exit OTA" : "OTA");
    lv_obj_set_style_text_color(s_link_label, lv_color_hex(link_ink), LV_PART_MAIN);

    const char *reply_text = "等待 C3 连接";
    uint32_t reply_bg = COLOR_NEUTRAL;
    uint32_t reply_ink = COLOR_MUTED;
    bool show_previous = true;
    if (online)
    {
        switch (s_status.angle_state)
        {
            case BLE_GATEWAY_ANGLE_WAITING:
                reply_text = "等待回执";
                break;
            case BLE_GATEWAY_ANGLE_TIMEOUT:
            case BLE_GATEWAY_ANGLE_FAILED:
                reply_text =
                    s_status.angle_state == BLE_GATEWAY_ANGLE_TIMEOUT ? "回执超时" : "下发失败";
                reply_bg = COLOR_WARN_BG;
                reply_ink = COLOR_WARN;
                break;
            case BLE_GATEWAY_ANGLE_IDLE:
                reply_text = s_status.has_ack_angle ? "C3 设置回执" : "暂无回执";
                reply_bg = s_status.has_ack_angle ? COLOR_OK_BG : COLOR_NEUTRAL;
                reply_ink = s_status.has_ack_angle ? COLOR_OK : COLOR_MUTED;
                show_previous = false;
                break;
        }
    }
    lv_obj_set_style_text_font(s_reply_label, s_status.ota_mode ? LV_FONT_DEFAULT : &gateway_font_16,
                               LV_PART_MAIN);
    lv_label_set_text(s_reply_label, s_status.ota_mode ? "ESP BLE OTA" : reply_text);
    if (s_status.has_ack_angle)
    {
        lv_label_set_text_fmt(s_reply_value, "%s%u°", show_previous ? "上次 " : "",
                              (unsigned int)s_status.last_ack_angle);
    }
    else
    {
        lv_label_set_text(s_reply_value, "—");
    }
    set_fill(s_reply_row, reply_bg);
    set_fill(s_reply_dot, reply_ink);
    lv_obj_set_style_text_color(s_reply_label, lv_color_hex(reply_ink), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_reply_value, lv_color_hex(reply_ink), LV_PART_MAIN);
    refresh_controls();
}

static void read_gateway_status(lv_timer_t *timer)
{
    (void)timer;
    if (ble_gateway_take_status(&s_status))
    {
        refresh_status();
    }
}

static void refresh_angle(void)
{
    lv_arc_set_value(s_angle_arc, s_target_angle);
    lv_label_set_text_fmt(s_angle_value, "%u°", (unsigned int)s_target_angle);
}

static void change_target_angle(lv_event_t *event)
{
    const lv_obj_t *button = lv_event_get_target_obj(event);
    if (button == s_center)
    {
        s_target_angle = SERVO_MIDPOINT;
    }
    else
    {
        const int delta = button == s_decrease ? -ANGLE_STEP : ANGLE_STEP;
        s_target_angle =
            (uint16_t)LV_CLAMP(0, (int)s_target_angle + delta, (int)BLE_LINK_SERVO_MAX_ANGLE);
    }
    refresh_angle();
    refresh_controls();
}

static void send_target_angle(lv_event_t *event)
{
    (void)event;
    read_gateway_status(NULL);
    if (s_status.link == BLE_GATEWAY_READY && s_status.angle_state != BLE_GATEWAY_ANGLE_WAITING)
    {
        s_status.angle_state = ble_gateway_request_angle(s_status.connection_id, s_target_angle)
                                   ? BLE_GATEWAY_ANGLE_WAITING
                                   : BLE_GATEWAY_ANGLE_FAILED;
        refresh_status();
    }
}

static void toggle_ota(lv_event_t *event)
{
    (void)event;
    ble_gateway_toggle_ota();
}

static void create_dial(lv_obj_t *screen)
{
    lv_obj_t *title = create_label(screen, "目标角度", &gateway_font_16, COLOR_INK);
    lv_obj_set_pos(title, 12, 50);

    /* LVGL 的 180° 在左、270° 在上、360° 在右，映射为 0 至 180° 的舵机目标。 */
    lv_obj_t *scale = lv_scale_create(screen);
    lv_obj_remove_style_all(scale);
    lv_obj_set_pos(scale, 39, 84);
    lv_obj_set_size(scale, 162, 162);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_OUTER);
    lv_scale_set_angle_range(scale, 180);
    lv_scale_set_rotation(scale, 180);
    lv_scale_set_total_tick_count(scale, 13);
    lv_scale_set_major_tick_every(scale, 3);
    lv_scale_set_label_show(scale, false);
    lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 1, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x7C94A5), LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, lv_color_hex(0x7C94A5), LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_length(scale, 4, LV_PART_INDICATOR);

    s_angle_arc = lv_arc_create(screen);
    lv_obj_remove_style_all(s_angle_arc);
    lv_obj_remove_flag(s_angle_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(s_angle_arc, 41, 86);
    lv_obj_set_size(s_angle_arc, 158, 158);
    lv_arc_set_bg_angles(s_angle_arc, 180, 360);
    lv_arc_set_range(s_angle_arc, 0, BLE_LINK_SERVO_MAX_ANGLE);
    lv_obj_set_style_arc_width(s_angle_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_angle_arc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_angle_arc, lv_color_hex(COLOR_BORDER), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_angle_arc, lv_color_hex(COLOR_BLUE), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_angle_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_angle_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_angle_arc, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_angle_arc, lv_color_hex(COLOR_BLUE), LV_PART_KNOB);
    lv_obj_set_style_radius(s_angle_arc, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_angle_arc, 1, LV_PART_KNOB);
    lv_obj_set_style_border_width(s_angle_arc, 1, LV_PART_KNOB);
    lv_obj_set_style_border_color(s_angle_arc, lv_color_hex(COLOR_PAPER), LV_PART_KNOB);
    lv_obj_set_style_border_opa(s_angle_arc, LV_OPA_COVER, LV_PART_KNOB);

    lv_obj_set_pos(create_label(screen, "0°", LV_FONT_DEFAULT, COLOR_MUTED), 35, 166);
    lv_obj_align(create_label(screen, "90°", LV_FONT_DEFAULT, COLOR_MUTED), LV_ALIGN_TOP_MID, 0,
                 68);
    lv_obj_set_pos(create_label(screen, "180°", LV_FONT_DEFAULT, COLOR_MUTED), 180, 166);

    s_angle_value = create_label(screen, "", &lv_font_montserrat_48, COLOR_INK);
    lv_obj_set_width(s_angle_value, SCREEN_WIDTH);
    lv_obj_set_pos(s_angle_value, 0, 105);
    lv_obj_set_style_text_align(s_angle_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void create_screen(lv_obj_t *screen)
{
    lv_obj_remove_style_all(screen);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    set_fill(screen, COLOR_PAPER);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *header = create_surface(screen, 0, 0, SCREEN_WIDTH, 42, 0);
    set_fill(header, COLOR_HEADER);
    lv_obj_t *title = create_label(header, "S3 网关", &gateway_font_16, 0xF6FAFF);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 12, 0);
    s_link_badge = create_surface(header, 140, 9, 88, 24, 4);
    s_link_dot = create_surface(s_link_badge, 7, 9, 6, 6, LV_RADIUS_CIRCLE);
    s_link_label = create_label(s_link_badge, "", &gateway_font_16, 0xD3F6ED);
    lv_obj_align(s_link_label, LV_ALIGN_LEFT_MID, 18, 0);

    create_dial(screen);

    s_reply_row = create_surface(screen, 12, 179, 216, 32, 4);
    s_reply_dot = create_surface(s_reply_row, 9, 13, 6, 6, LV_RADIUS_CIRCLE);
    s_reply_label = create_label(s_reply_row, "", &gateway_font_16, COLOR_MUTED);
    lv_obj_align(s_reply_label, LV_ALIGN_LEFT_MID, 22, 0);
    s_reply_value = create_label(s_reply_row, "", &gateway_font_16, COLOR_MUTED);
    lv_obj_align(s_reply_value, LV_ALIGN_RIGHT_MID, -9, 0);

    s_decrease = create_button(screen, "-5°", 12, 219, 68, false, change_target_angle);
    s_center = create_button(screen, "回中", 86, 219, 68, false, change_target_angle);
    s_increase = create_button(screen, "+5°", 160, 219, 68, false, change_target_angle);
    s_send = create_button(screen, "下发角度", 12, 270, 142, true, send_target_angle);
    s_send_label = lv_obj_get_child(s_send, 0);
    lv_obj_t *ota_button = create_button(screen, "OTA", 160, 270, 68, false, toggle_ota);
    s_ota_label = lv_obj_get_child(ota_button, 0);
    lv_obj_set_style_text_font(s_ota_label, LV_FONT_DEFAULT, LV_PART_MAIN);
    refresh_angle();
    refresh_status();
}

esp_err_t gateway_ui_init(void)
{
    lv_display_t *display;
    esp_err_t result = gateway_display_init(&display);
    if (result != ESP_OK)
    {
        return result;
    }
    if (!lvgl_port_lock(0))
    {
        return ESP_ERR_TIMEOUT;
    }

    ble_gateway_take_status(&s_status);
    create_screen(lv_display_get_screen_active(display));
    lv_timer_create(read_gateway_status, 40, NULL);
    lvgl_port_unlock();
    return ESP_OK;
}
