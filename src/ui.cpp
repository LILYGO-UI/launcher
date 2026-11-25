#include "ui.h"

#include "launcher_theme.h"
#include "launcher_view.h"

#include <cm0/status_bar.h>
#include <cm0/typography.h>

#include <new>
#include <algorithm>
#include <array>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct launcher_ui {
    explicit launcher_ui(launcher_registry_t *value) : registry(value)
    {
    }

    launcher_registry_t *registry                = nullptr;
    launcher_app_t *pending_app                  = nullptr;
    launcher_orientation_t orientation           = LAUNCHER_ORIENTATION_PORTRAIT;
    bool orientation_request_pending             = false;
    launcher_orientation_t requested_orientation = LAUNCHER_ORIENTATION_PORTRAIT;
    launcher_view_t view{};
    lv_obj_t *previous_screen = nullptr;
    lv_obj_t *pages           = nullptr;
    lv_indev_t *input         = nullptr;
    lv_point_t pull_start{};
    bool pull_tracking        = false;
    bool quick_panel_open     = false;
    lv_obj_t *quick_panel_previous_focus = nullptr;
    lv_group_t *quick_panel_group = nullptr;
    lv_group_t *quick_panel_previous_group = nullptr;
    bool input_enabled        = true;
    bool account_actions_enabled = true;
    bool lock_request         = false;
    bool logout_request       = false;
    int brightness_request    = -1;
    int volume_request        = -1;
    bool power_request        = false;
    bool reboot_request       = false;
    bool power_confirmation_open = false;
    bool power_confirmation_reboot = false;
    bool file_share_open      = false;
    bool file_share_approval_visible = false;
    bool file_share_decision_pending = false;
    bool file_share_decision_accepted = false;
    lv_group_t *file_share_group = nullptr;
    lv_group_t *file_share_previous_group = nullptr;
    lv_obj_t *file_share_previous_focus = nullptr;
    bool power_busy           = false;
    char session_user[128]{};
    char file_share_directory[LAUNCHER_PATH_MAX]{};
    bool shutdown_visible     = false;
    bool relayout_in_progress = false;
    int32_t layout_width      = 0;
    int32_t layout_height     = 0;
    int32_t columns           = 0;
    int32_t cell_height       = 0;
    size_t apps_per_page      = LAUNCHER_APPS_PER_PAGE;
    size_t active_page        = 0;
    char refresh_focused_app_id[LAUNCHER_ID_MAX]{};
    char refresh_anchor_app_id[LAUNCHER_ID_MAX]{};
    char refresh_pending_app_id[LAUNCHER_ID_MAX]{};
    size_t refresh_active_page     = 0;
    bool registry_refresh_prepared = false;
    uint32_t keyboard_key          = 0;
    uint32_t keyboard_action_tick  = 0;
    bool keyboard_pressed          = false;
    bool keyboard_repeating        = false;
};

enum {
    APP_CELL_SIZE               = 150,
    APP_CELL_MIN_HEIGHT         = 96,
    APP_ROW_LIMIT               = 6,
    APP_COLUMN_LIMIT            = 6,
    PAGE_INDICATOR_HEIGHT       = 40,
    PAGE_INDICATOR_HIT_SIZE     = 40,
    PAGE_INDICATOR_MARK_SIZE    = 8,
    PAGE_INDICATOR_ACTIVE_WIDTH = 24,
    HOME_INDICATOR_HEIGHT       = 38,
    QUICK_PANEL_PULL_START_ZONE = 96,
    QUICK_PANEL_PULL_DISTANCE   = 64,
    KEYBOARD_REPEAT_DELAY_MS   = 450,
    KEYBOARD_REPEAT_INTERVAL_MS = 150,
};

struct layout_metrics_t {
    int32_t width;
    int32_t height;
    int32_t columns;
    int32_t rows;
    int32_t cell_height;
    int32_t bottom_inset;
    size_t apps_per_page;
};

static const char *icon_text(const char *icon)
{
    if (strcmp(icon, "settings") == 0) return LV_SYMBOL_SETTINGS;
    if (strcmp(icon, "play") == 0) return LV_SYMBOL_PLAY;
    if (strcmp(icon, "image") == 0 || strcmp(icon, "camera") == 0) return LV_SYMBOL_IMAGE;
    if (strcmp(icon, "list") == 0) return LV_SYMBOL_LIST;
    if (strcmp(icon, "home") == 0) return LV_SYMBOL_HOME;
    if (strcmp(icon, "calculator") == 0) return "=";
    if (strcmp(icon, "clock") == 0) return LV_SYMBOL_BELL;
    if (strcmp(icon, "artboard") == 0) return LV_SYMBOL_EDIT;
    if (strcmp(icon, "battery") == 0) return LV_SYMBOL_BATTERY_FULL;
    if (strcmp(icon, "led") == 0) return LV_SYMBOL_CHARGE;
    if (strcmp(icon, "gnss") == 0) return LV_SYMBOL_GPS;
    if (strcmp(icon, "hdmi") == 0 || strcmp(icon, "video") == 0) return LV_SYMBOL_VIDEO;
    if (strcmp(icon, "weather") == 0) return LV_SYMBOL_TINT;
    if (strcmp(icon, "cellular") == 0 || strcmp(icon, "lora") == 0) return LV_SYMBOL_WIFI;
    if (strcmp(icon, "ble") == 0) return LV_SYMBOL_BLUETOOTH;
    if (strcmp(icon, "music") == 0) return LV_SYMBOL_AUDIO;
    if (strcmp(icon, "live-call") == 0) return LV_SYMBOL_CALL;
    return LV_SYMBOL_FILE;
}

static lv_color_t icon_color(const char *icon)
{
    if (strcmp(icon, "settings") == 0 || strcmp(icon, "hdmi") == 0) return lv_color_hex(launcher::theme::kIconSystem);
    if (strcmp(icon, "play") == 0) return lv_color_hex(launcher::theme::kIconPrimary);
    if (strcmp(icon, "image") == 0) return lv_color_hex(launcher::theme::kIconMedia);
    if (strcmp(icon, "list") == 0 || strcmp(icon, "gnss") == 0 || strcmp(icon, "ble") == 0)
        return lv_color_hex(launcher::theme::kIconConnectivity);
    if (strcmp(icon, "home") == 0) return lv_color_hex(launcher::theme::kIconHome);
    if (strcmp(icon, "calculator") == 0 || strcmp(icon, "weather") == 0)
        return lv_color_hex(launcher::theme::kIconUtility);
    if (strcmp(icon, "clock") == 0 || strcmp(icon, "camera") == 0) return lv_color_hex(launcher::theme::kIconCapture);
    if (strcmp(icon, "artboard") == 0 || strcmp(icon, "music") == 0)
        return lv_color_hex(launcher::theme::kIconCreative);
    if (strcmp(icon, "battery") == 0 || strcmp(icon, "cellular") == 0) return lv_color_hex(launcher::theme::kIconPower);
    if (strcmp(icon, "led") == 0) return lv_color_hex(launcher::theme::kIconEnergy);
    if (strcmp(icon, "video") == 0) return lv_color_hex(launcher::theme::kIconVideo);
    if (strcmp(icon, "live-call") == 0) return lv_color_hex(launcher::theme::kIconCommunication);
    if (strcmp(icon, "lora") == 0) return lv_color_hex(launcher::theme::kIconRadio);
    return lv_color_hex(launcher::theme::kIconFallback);
}

static const char *loadable_icon_path(const launcher_app_t *app)
{
    if (!app->icon_path[0]) return nullptr;
    lv_image_header_t header{};
    return lv_image_decoder_get_info(app->icon_path, &header) == LV_RESULT_OK ? app->icon_path : nullptr;
}

static int32_t absolute_distance(int32_t first, int32_t second)
{
    return first >= second ? first - second : second - first;
}

static void animate_object_y(void *object, int32_t value)
{
    lv_obj_set_y(static_cast<lv_obj_t *>(object), value);
}

static void animate_object_opacity(void *object, int32_t value)
{
    lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), (lv_opa_t)value, 0);
}

static void start_animation(lv_obj_t *object, lv_anim_exec_xcb_t callback, int32_t from, int32_t to)
{
    lv_anim_delete(object, callback);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, object);
    lv_anim_set_exec_cb(&animation, callback);
    lv_anim_set_values(&animation, from, to);
    lv_anim_set_duration(&animation, 180);
    lv_anim_start(&animation);
}

static void capture_quick_panel_input(launcher_ui_t *ui)
{
    if (!ui->quick_panel_group) return;
    for (lv_indev_t *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_group(input) == ui->quick_panel_previous_group)
            lv_indev_set_group(input, ui->quick_panel_group);
    }
}

static std::array<lv_obj_t *, 10> quick_control_objects(launcher_ui_t *ui)
{
    return {ui->view.orientation_button, ui->view.file_share_button, ui->view.lock_button,
            ui->view.session_button, ui->view.power_button, ui->view.reboot_button,
            ui->view.brightness_slider, ui->view.volume_slider, ui->view.power_cancel_button,
            ui->view.power_confirm_button};
}

static void quick_control_focused(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (!ui || !ui->view.quick_controls) return;
    lv_obj_t *control = lv_event_get_target_obj(event);
    lv_obj_t *viewport = ui->view.quick_controls;
    // Native focus scrolling considers only the widget bounds. Cancel its
    // animation and reveal the complete outline inside the scroll viewport.
    lv_obj_stop_scroll_anim(viewport);
    lv_obj_update_layout(viewport);
    const int32_t extent = lv_obj_get_style_outline_pad(control, LV_PART_MAIN) +
                           lv_obj_get_style_outline_width(control, LV_PART_MAIN);
    lv_area_t visible, area;
    lv_obj_get_coords(viewport, &visible);
    lv_obj_get_coords(control, &area);
    int32_t delta = 0;
    if (area.y1 - extent < visible.y1) delta = visible.y1 + extent - area.y1;
    else if (area.y2 + extent > visible.y2) delta = visible.y2 - extent - area.y2;
    lv_obj_scroll_to_y(viewport, lv_obj_get_scroll_y(viewport) - delta, LV_ANIM_OFF);
}

static void populate_quick_panel_group(launcher_ui_t *ui)
{
    if (!ui->quick_panel_group) return;
    lv_group_remove_all_objs(ui->quick_panel_group);
    const auto controls = quick_control_objects(ui);
    const size_t begin = ui->power_confirmation_open ? 8 : 0;
    const size_t end = ui->power_confirmation_open ? 10 : 8;
    for (size_t i = begin; i < end; ++i) {
        if (!lv_obj_has_flag(controls[i], LV_OBJ_FLAG_HIDDEN) && !lv_obj_has_state(controls[i], LV_STATE_DISABLED))
            lv_group_add_obj(ui->quick_panel_group, controls[i]);
    }
}

static void close_power_confirmation(launcher_ui_t *ui)
{
    if (!ui->power_confirmation_open) return;
    ui->power_confirmation_open = false;
    ui->power_request = false;
    ui->reboot_request = false;
    ui->power_busy = false;
    lv_obj_add_flag(ui->view.power_confirm_scrim, LV_OBJ_FLAG_HIDDEN);
    populate_quick_panel_group(ui);
    if (ui->quick_panel_group) {
        lv_group_focus_obj(ui->power_confirmation_reboot ? ui->view.reboot_button : ui->view.power_button);
    }
}

static void restore_quick_panel_focus(launcher_ui_t *ui)
{
    if (!ui->quick_panel_group) return;
    for (lv_indev_t *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_group(input) == ui->quick_panel_group)
            lv_indev_set_group(input, ui->quick_panel_previous_group);
    }
    for (lv_obj_t *control : quick_control_objects(ui)) {
        if (!control) continue;
        if (ui->quick_panel_previous_group)
            lv_group_add_obj(ui->quick_panel_previous_group, control);
        else
            lv_group_remove_obj(control);
    }
    lv_group_delete(ui->quick_panel_group);
    ui->quick_panel_group = nullptr;
    ui->quick_panel_previous_group = nullptr;
    if (ui->quick_panel_previous_focus && lv_obj_is_valid(ui->quick_panel_previous_focus))
        lv_group_focus_obj(ui->quick_panel_previous_focus);
    ui->quick_panel_previous_focus = nullptr;
}

static void play_quick_panel(launcher_ui_t *ui, bool open)
{
    if (!ui || !ui->view.screen || ui->quick_panel_open == open) return;
    if (open) {
        ui->quick_panel_group = lv_group_create();
        if (!ui->quick_panel_group) return;
        ui->quick_panel_previous_group = static_cast<lv_group_t *>(lv_obj_get_group(ui->view.orientation_button));
        lv_group_t *group = ui->quick_panel_previous_group;
        ui->quick_panel_previous_focus = group ? lv_group_get_focused(group) : nullptr;
        // Native Tab navigation always follows the input's group, even after
        // lv_indev_stop_processing(). Keep only the modal cards in that group.
        populate_quick_panel_group(ui);
        capture_quick_panel_input(ui);
        if (lv_obj_t *background_focus = group ? lv_group_get_focused(group) : nullptr)
            lv_obj_remove_state(background_focus, static_cast<lv_state_t>(LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY));
        lv_group_focus_obj(ui->view.orientation_button);
    } else {
        close_power_confirmation(ui);
        restore_quick_panel_focus(ui);
    }
    ui->quick_panel_open       = open;
    const int32_t panel_height = lv_obj_get_height(ui->view.quick_panel);
    if (open)
        lv_obj_remove_flag(ui->view.quick_scrim, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(ui->view.quick_scrim, LV_OBJ_FLAG_HIDDEN);
    start_animation(ui->view.quick_scrim, animate_object_opacity,
                    lv_obj_get_style_opa(ui->view.quick_scrim, LV_PART_MAIN), open ? LV_OPA_COVER : LV_OPA_TRANSP);
    start_animation(ui->view.quick_panel, animate_object_y, lv_obj_get_y(ui->view.quick_panel),
                    open ? 0 : -panel_height);
}

static void pull_down_input_event(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (!ui || !ui->input) return;
    if ((!ui->input_enabled || ui->power_confirmation_open || ui->file_share_open || ui->shutdown_visible) &&
        lv_event_get_code(event) != LV_EVENT_DELETE) return;

    switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED:
            lv_indev_get_point(ui->input, &ui->pull_start);
            ui->pull_tracking = ui->quick_panel_open || ui->pull_start.y <= QUICK_PANEL_PULL_START_ZONE;
            if (ui->quick_panel_open) {
                // A vertical slider drag belongs to that control, even when
                // the panel itself has no scroll overflow.
                for (lv_obj_t *slider : {ui->view.brightness_slider, ui->view.volume_slider}) {
                    lv_area_t area;
                    lv_obj_get_coords(slider, &area);
                    if (ui->pull_start.x >= area.x1 && ui->pull_start.x <= area.x2 &&
                        ui->pull_start.y >= area.y1 && ui->pull_start.y <= area.y2)
                        ui->pull_tracking = false;
                }
            }
            if (ui->quick_panel_open && ui->view.quick_controls &&
                lv_obj_get_scroll_top(ui->view.quick_controls) + lv_obj_get_scroll_bottom(ui->view.quick_controls) > 0) {
                lv_area_t controls_area;
                lv_obj_get_coords(ui->view.quick_controls, &controls_area);
                // On short displays this gesture scrolls the cards instead of
                // dismissing the panel. The handle still supports dismissal.
                if (ui->pull_start.x >= controls_area.x1 && ui->pull_start.x <= controls_area.x2 &&
                    ui->pull_start.y >= controls_area.y1 && ui->pull_start.y <= controls_area.y2)
                    ui->pull_tracking = false;
            }
            break;
        case LV_EVENT_RELEASED: {
            if (!ui->pull_tracking) break;
            lv_point_t current;
            lv_indev_get_point(ui->input, &current);
            const int32_t vertical   = current.y - ui->pull_start.y;
            const int32_t horizontal = absolute_distance(current.x, ui->pull_start.x);
            if (!ui->quick_panel_open && vertical >= QUICK_PANEL_PULL_DISTANCE && vertical > horizontal)
                play_quick_panel(ui, true);
            else if (ui->quick_panel_open && -vertical >= QUICK_PANEL_PULL_DISTANCE && -vertical > horizontal)
                play_quick_panel(ui, false);
            ui->pull_tracking = false;
            break;
        }
        case LV_EVENT_DELETE:
            ui->input         = nullptr;
            ui->pull_tracking = false;
            break;
        default:
            break;
    }
}

static void attach_pull_down_input(launcher_ui_t *ui)
{
    for (lv_indev_t *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER) continue;
        ui->input = input;
        lv_indev_add_event_cb(input, pull_down_input_event, LV_EVENT_PRESSED, ui);
        lv_indev_add_event_cb(input, pull_down_input_event, LV_EVENT_RELEASED, ui);
        lv_indev_add_event_cb(input, pull_down_input_event, LV_EVENT_DELETE, ui);
        break;
    }
}

static void scrim_clicked(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (ui) play_quick_panel(ui, false);
}

static void orientation_clicked(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (!ui || !ui->input_enabled || !ui->quick_panel_open || ui->power_confirmation_open || ui->shutdown_visible) return;
    ui->requested_orientation = ui->orientation == LAUNCHER_ORIENTATION_LANDSCAPE ? LAUNCHER_ORIENTATION_PORTRAIT
                                                                                  : LAUNCHER_ORIENTATION_LANDSCAPE;
    ui->orientation_request_pending = true;
    play_quick_panel(ui, false);
}

static void lock_clicked(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (!ui || !ui->input_enabled || !ui->account_actions_enabled || !ui->session_user[0] || !ui->quick_panel_open || ui->power_confirmation_open || ui->shutdown_visible) return;
    ui->pending_app = nullptr;
    ui->lock_request = true;
    play_quick_panel(ui, false);
}

static void logout_clicked(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (!ui || !ui->input_enabled || !ui->account_actions_enabled || !ui->session_user[0] || !ui->quick_panel_open || ui->power_confirmation_open || ui->shutdown_visible) return;
    ui->logout_request = true;
    play_quick_panel(ui, false);
}

static void capture_file_share_input(launcher_ui_t *ui)
{
    if (!ui->file_share_group) return;
    for (lv_indev_t *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_group(input) == ui->file_share_previous_group)
            lv_indev_set_group(input, ui->file_share_group);
    }
}

static void restore_file_share_focus(launcher_ui_t *ui)
{
    if (!ui->file_share_group) return;
    for (lv_indev_t *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_group(input) == ui->file_share_group)
            lv_indev_set_group(input, ui->file_share_previous_group);
    }
    for (lv_obj_t *control : {ui->view.file_share_approve_button, ui->view.file_share_reject_button,
                              ui->view.file_share_close_button}) {
        if (!control) continue;
        if (ui->file_share_previous_group) lv_group_add_obj(ui->file_share_previous_group, control);
        else lv_group_remove_obj(control);
    }
    lv_group_delete(ui->file_share_group);
    ui->file_share_group = nullptr;
    ui->file_share_previous_group = nullptr;
    if (ui->file_share_previous_focus && lv_obj_is_valid(ui->file_share_previous_focus))
        lv_group_focus_obj(ui->file_share_previous_focus);
    ui->file_share_previous_focus = nullptr;
}

static void populate_file_share_group(launcher_ui_t *ui)
{
    if (!ui->file_share_group) return;
    lv_group_remove_all_objs(ui->file_share_group);
    if (ui->file_share_approval_visible) {
        lv_group_add_obj(ui->file_share_group, ui->view.file_share_approve_button);
        lv_group_add_obj(ui->file_share_group, ui->view.file_share_reject_button);
    }
    lv_group_add_obj(ui->file_share_group, ui->view.file_share_close_button);
    lv_group_focus_obj(ui->file_share_approval_visible ? ui->view.file_share_reject_button
                                                      : ui->view.file_share_close_button);
}

static void close_file_share_panel(launcher_ui_t *ui)
{
    if (!ui || !ui->file_share_open) return;
    ui->file_share_open = false;
    ui->file_share_approval_visible = false;
    ui->file_share_decision_pending = false;
    restore_file_share_focus(ui);
    lv_obj_add_flag(ui->view.file_share_approve_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->view.file_share_reject_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->view.file_share_scrim, LV_OBJ_FLAG_HIDDEN);
}

static void open_file_share_panel(launcher_ui_t *ui)
{
    if (!ui || !ui->view.file_share_scrim || ui->power_confirmation_open || ui->file_share_open) return;
    play_quick_panel(ui, false);
    ui->file_share_group = lv_group_create();
    if (!ui->file_share_group) return;
    ui->file_share_previous_group = static_cast<lv_group_t *>(lv_obj_get_group(ui->view.file_share_close_button));
    ui->file_share_previous_focus = ui->file_share_previous_group
        ? lv_group_get_focused(ui->file_share_previous_group) : nullptr;
    ui->file_share_open = true;
    ui->file_share_approval_visible = false;
    ui->file_share_decision_pending = false;
    lv_obj_add_flag(ui->view.file_share_approve_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui->view.file_share_reject_button, LV_OBJ_FLAG_HIDDEN);
    populate_file_share_group(ui);
    capture_file_share_input(ui);
    if (ui->file_share_directory[0])
        lv_label_set_text(ui->view.file_share_path, ui->file_share_directory);
    else
        lv_label_set_text(ui->view.file_share_path, "$HOME/Downloads");
    lv_obj_remove_flag(ui->view.file_share_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->view.file_share_scrim);
    lv_obj_update_layout(ui->view.file_share_scrim);
}

static void file_share_close_clicked(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (ui) close_file_share_panel(ui);
}

static void file_share_approval_clicked(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (!ui || !ui->input_enabled || !ui->file_share_open || !ui->file_share_approval_visible ||
        ui->file_share_decision_pending) return;
    ui->file_share_decision_accepted = target == ui->view.file_share_approve_button;
    ui->file_share_decision_pending = true;
}

static void file_share_clicked(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (!ui || !ui->input_enabled || !ui->session_user[0] || !ui->quick_panel_open ||
        ui->power_confirmation_open || ui->shutdown_visible)
        return;
    ui->pending_app = nullptr;
    open_file_share_panel(ui);
}

static void app_clicked(lv_event_t *event)
{
    auto *ui  = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    auto *app = static_cast<launcher_app_t *>(lv_obj_get_user_data(lv_event_get_current_target_obj(event)));
    if (ui && ui->input_enabled && !ui->quick_panel_open && !ui->file_share_open && !ui->shutdown_visible && app)
        ui->pending_app = app;
}

static void level_changed(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    lv_obj_t *slider = lv_event_get_target_obj(event);
    if (!ui || !ui->input_enabled || !ui->quick_panel_open || ui->power_confirmation_open ||
        ui->shutdown_visible || lv_obj_has_state(slider, LV_STATE_DISABLED)) return;
    const bool brightness = slider == ui->view.brightness_slider;
    const int value = lv_slider_get_value(slider);
    if (brightness) ui->brightness_request = value;
    else ui->volume_request = value;
    lv_label_set_text_fmt(brightness ? ui->view.brightness_label : ui->view.volume_label, "%d%%", value);
}

static void open_power_confirmation(launcher_ui_t *ui, bool reboot)
{
    if (!ui || !ui->input_enabled || !ui->quick_panel_open || ui->power_confirmation_open ||
        ui->shutdown_visible) return;
    lv_obj_t *button = reboot ? ui->view.reboot_button : ui->view.power_button;
    if (!button || lv_obj_has_state(button, LV_STATE_DISABLED)) return;
    ui->pending_app = nullptr;
    ui->pull_tracking = false;
    ui->power_confirmation_open = true;
    ui->power_confirmation_reboot = reboot;
    ui->power_busy = false;
    lv_label_set_text(ui->view.power_confirm_title, reboot ? "Restart?" : "Power off?");
    lv_label_set_text(ui->view.power_confirm_hint,
                      reboot ? "This will restart your device." : "This will turn off your device.");
    lv_label_set_text(ui->view.power_confirm_label, reboot ? "Restart" : "Power off");
    lv_obj_remove_flag(ui->view.power_confirm_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_state(ui->view.power_confirm_button, LV_STATE_DISABLED);
    lv_obj_remove_state(ui->view.power_cancel_button, LV_STATE_DISABLED);
    lv_obj_remove_flag(ui->view.power_confirm_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->view.power_confirm_scrim);
    populate_quick_panel_group(ui);
    lv_group_focus_obj(ui->view.power_cancel_button);
}

static void power_clicked(lv_event_t *event)
{
    open_power_confirmation(static_cast<launcher_ui_t *>(lv_event_get_user_data(event)), false);
}

static void reboot_clicked(lv_event_t *event)
{
    open_power_confirmation(static_cast<launcher_ui_t *>(lv_event_get_user_data(event)), true);
}

static void power_cancelled(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (ui && !ui->power_busy) close_power_confirmation(ui);
}

static void power_confirmed(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (!ui || !ui->input_enabled || !ui->power_confirmation_open || ui->power_busy ||
        lv_obj_has_flag(ui->view.power_confirm_button, LV_OBJ_FLAG_HIDDEN)) return;
    const bool reboot = ui->power_confirmation_reboot;
    if (reboot) ui->reboot_request = true;
    else ui->power_request = true;
    ui->power_busy = true;
    lv_label_set_text(ui->view.power_confirm_title, reboot ? "Restarting" : "Powering off");
    lv_label_set_text(ui->view.power_confirm_hint, "Please wait...");
    lv_obj_add_state(ui->view.power_confirm_button, LV_STATE_DISABLED);
    lv_obj_add_state(ui->view.power_cancel_button, LV_STATE_DISABLED);
    populate_quick_panel_group(ui);
}

static void update_page_indicator(launcher_ui_t *ui, size_t active_page)
{
    lv_obj_t *indicator  = ui->view.page_indicator;
    const uint32_t count = lv_obj_get_child_count(indicator);
    if (count == 0) return;
    if (active_page >= count) active_page = count - 1;
    ui->active_page = active_page;
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *mark      = lv_obj_get_child(lv_obj_get_child(indicator, (int32_t)i), 0);
        const int32_t width = i == active_page ? PAGE_INDICATOR_ACTIVE_WIDTH : PAGE_INDICATOR_MARK_SIZE;
        lv_obj_set_width(mark, width);
        lv_obj_set_style_bg_color(
            mark,
            lv_color_hex(i == active_page ? launcher::theme::kIndicatorActive : launcher::theme::kIndicatorInactive),
            0);
    }
    lv_obj_update_layout(indicator);
    lv_obj_scroll_to_view(lv_obj_get_child(indicator, (int32_t)active_page), LV_ANIM_OFF);
}

static void page_changed(lv_event_t *event)
{
    auto *ui         = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    lv_obj_t *active = lv_tileview_get_tile_active(lv_event_get_current_target_obj(event));
    if (ui && active) update_page_indicator(ui, lv_obj_get_index(active));
}

static void page_dot_clicked(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (!ui || !ui->pages) return;
    lv_obj_t *button    = lv_event_get_current_target_obj(event);
    const uint32_t page = lv_obj_get_index(button);
    lv_tileview_set_tile_by_index(ui->pages, page, 0, LV_ANIM_ON);
    update_page_indicator(ui, page);
}

static void configure_page_indicator(launcher_ui_t *ui, size_t page_count)
{
    lv_obj_t *indicator           = ui->view.page_indicator;
    const int32_t content_width   = (int32_t)page_count * PAGE_INDICATOR_HIT_SIZE;
    const int32_t available_width = ui->layout_width > 0 ? ui->layout_width : 1;
    const bool scrollable         = content_width > available_width;
    lv_obj_set_width(indicator, scrollable ? available_width : content_width);
    lv_obj_set_flex_align(indicator, scrollable ? LV_FLEX_ALIGN_START : LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    if (scrollable) {
        lv_obj_add_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(indicator, LV_DIR_HOR);
        lv_obj_set_scrollbar_mode(indicator, LV_SCROLLBAR_MODE_OFF);
    } else {
        lv_obj_remove_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_scroll_to_x(indicator, 0, LV_ANIM_OFF);
    }
}

static void populate_page_indicator(launcher_ui_t *ui, size_t page_count, size_t active_page)
{
    lv_obj_t *indicator = ui->view.page_indicator;
    lv_obj_clean(indicator);
    configure_page_indicator(ui, page_count);

    for (size_t i = 0; i < page_count; ++i) {
        lv_obj_t *button = lv_button_create(indicator);
        lv_obj_set_size(button, PAGE_INDICATOR_HIT_SIZE, PAGE_INDICATOR_HEIGHT);
        lv_obj_set_style_pad_all(button, 0, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_set_style_shadow_width(button, 0, 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
        lv_obj_add_event_cb(button, page_dot_clicked, LV_EVENT_CLICKED, ui);

        lv_obj_t *mark = lv_obj_create(button);
        lv_obj_set_size(mark, PAGE_INDICATOR_MARK_SIZE, PAGE_INDICATOR_MARK_SIZE);
        lv_obj_center(mark);
        lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(mark, 0, 0);
        lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
        lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    }
    update_page_indicator(ui, active_page);
}

static int32_t app_grid_inset(int32_t width, int32_t columns)
{
    const int32_t free_space = width - columns * APP_CELL_SIZE;
    return free_space > 0 ? free_space / (columns + 1) : 0;
}

static void style_app_page(lv_obj_t *page, int32_t width, int32_t columns)
{
    const int32_t inset = app_grid_inset(width, columns);
    const int32_t used  = columns * APP_CELL_SIZE + (columns - 1) * inset;
    lv_obj_set_style_pad_top(page, 0, 0);
    lv_obj_set_style_pad_bottom(page, 0, 0);
    lv_obj_set_style_pad_left(page, inset, 0);
    lv_obj_set_style_pad_right(page, width - used - inset, 0);
    lv_obj_set_style_pad_row(page, 0, 0);
    lv_obj_set_style_pad_column(page, inset, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
}

static size_t current_page_anchor(const launcher_ui_t *ui)
{
    return ui->active_page * ui->apps_per_page;
}

static bool focused_app_index(const launcher_ui_t *ui, size_t *index)
{
    if (ui->quick_panel_open) return false;
    lv_group_t *group = lv_group_get_default();
    lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
    if (!focused || !ui->pages) return false;

    size_t active_page = ui->active_page;
    if (lv_obj_t *active = lv_tileview_get_tile_active(ui->pages)) {
        const uint32_t active_index = lv_obj_get_index(active);
        if (active_index < lv_obj_get_child_count(ui->pages)) active_page = active_index;
    }
    const uint32_t page_count = lv_obj_get_child_count(ui->pages);
    for (uint32_t page_index = 0; page_index < page_count; ++page_index) {
        if (page_index != active_page) continue;
        lv_obj_t *page             = lv_obj_get_child(ui->pages, (int32_t)page_index);
        const uint32_t child_count = lv_obj_get_child_count(page);
        for (uint32_t child_index = 0; child_index < child_count; ++child_index) {
            lv_obj_t *tile = lv_obj_get_child(page, (int32_t)child_index);
            if (tile != focused) continue;
            auto *app = static_cast<launcher_app_t *>(lv_obj_get_user_data(tile));
            for (size_t app_index = 0; app_index < ui->registry->count; ++app_index) {
                if (app == &ui->registry->apps[app_index]) {
                    *index = app_index;
                    return true;
                }
            }
        }
    }
    return false;
}

/* Find a tile by registry index.  Pages contain only application buttons in
 * normal operation; scanning user data also keeps this correct for an empty
 * state label and for the last, partially filled page. */
static lv_obj_t *app_tile_at_index(const launcher_ui_t *ui, size_t index)
{
    if (!ui || !ui->pages || !ui->registry || index >= ui->registry->count) return nullptr;
    const uint32_t page_count = lv_obj_get_child_count(ui->pages);
    for (uint32_t page_index = 0; page_index < page_count; ++page_index) {
        lv_obj_t *page             = lv_obj_get_child(ui->pages, (int32_t)page_index);
        const uint32_t child_count = lv_obj_get_child_count(page);
        for (uint32_t child_index = 0; child_index < child_count; ++child_index) {
            lv_obj_t *tile = lv_obj_get_child(page, (int32_t)child_index);
            if (lv_obj_get_user_data(tile) == &ui->registry->apps[index]) return tile;
        }
    }
    return nullptr;
}

static size_t active_page_for_navigation(const launcher_ui_t *ui)
{
    if (!ui || !ui->pages) return 0;
    lv_obj_t *active = lv_tileview_get_tile_active(ui->pages);
    if (!active) return ui->active_page;
    const uint32_t page_count = lv_obj_get_child_count(ui->pages);
    const uint32_t page_index = lv_obj_get_index(active);
    return page_index < page_count ? page_index : ui->active_page;
}

static bool focus_app_index(launcher_ui_t *ui, size_t index)
{
    lv_obj_t *tile = app_tile_at_index(ui, index);
    if (!tile) return false;

    lv_group_t *group = lv_group_get_default();
    if (!group) return false;
    /* The AppKit normally installs the default group before creating the
     * launcher.  Adding lazily also makes the public key handler work when a
     * group is attached after a screen was already built. */
    if (lv_obj_get_group(tile) != group) {
        if (lv_obj_get_group(tile)) lv_group_remove_obj(tile);
        lv_group_add_obj(group, tile);
    }

    const size_t page = ui->apps_per_page ? index / ui->apps_per_page : 0;
    if (ui->pages && page != active_page_for_navigation(ui)) {
        lv_tileview_set_tile_by_index(ui->pages, (uint32_t)page, 0, LV_ANIM_OFF);
        update_page_indicator(ui, page);
    }
    lv_group_focus_obj(tile);
    return lv_group_get_focused(group) == tile;
}

static bool navigation_target(const launcher_ui_t *ui, size_t current, uint32_t key, size_t *target)
{
    if (!ui || !target || !ui->registry || current >= ui->registry->count || ui->apps_per_page == 0 ||
        ui->columns <= 0)
        return false;

    const size_t page       = current / ui->apps_per_page;
    const size_t page_start = page * ui->apps_per_page;
    const size_t page_end   = (page_start + ui->apps_per_page < ui->registry->count)
                                ? page_start + ui->apps_per_page
                                : ui->registry->count;
    const size_t local      = current - page_start;
    const size_t row        = local / (size_t)ui->columns;

    switch (key) {
        case LV_KEY_LEFT:
            if (local > 0) {
                *target = current - 1;
                return true;
            }
            if (page > 0) {
                *target = page_start - 1;
                return true;
            }
            return false;
        case LV_KEY_RIGHT:
            if (current + 1 < page_end) {
                *target = current + 1;
                return true;
            }
            if (page_end < ui->registry->count) {
                *target = page_end;
                return true;
            }
            return false;
        case LV_KEY_UP:
            if (row > 0) {
                *target = current - (size_t)ui->columns;
                return true;
            }
            return false;
        case LV_KEY_DOWN: {
            const size_t candidate = current + (size_t)ui->columns;
            if (candidate < page_end) {
                *target = candidate;
                return true;
            }
            return false;
        }
        default:
            return false;
    }
}

static bool app_index_by_id(const launcher_ui_t *ui, const char *id, size_t *index)
{
    if (!ui || !id || !id[0] || !index) return false;
    for (size_t app_index = 0; app_index < ui->registry->count; ++app_index) {
        if (strcmp(ui->registry->apps[app_index].manifest.id, id) == 0) {
            *index = app_index;
            return true;
        }
    }
    return false;
}

static void layout_existing_pages(launcher_ui_t *ui)
{
    if (!ui->pages) return;
    const uint32_t page_count = lv_obj_get_child_count(ui->pages);
    for (uint32_t page_index = 0; page_index < page_count; ++page_index) {
        lv_obj_t *page = lv_obj_get_child(ui->pages, (int32_t)page_index);
        style_app_page(page, ui->layout_width, ui->columns);
        const uint32_t child_count = lv_obj_get_child_count(page);
        for (uint32_t child_index = 0; child_index < child_count; ++child_index) {
            lv_obj_t *child = lv_obj_get_child(page, (int32_t)child_index);
            if (lv_obj_has_class(child, &lv_button_class))
                launcher_view_layout_app_tile(child, APP_CELL_SIZE, ui->cell_height);
            else
                lv_obj_set_width(child, ui->layout_width - 2 * app_grid_inset(ui->layout_width, ui->columns));
        }
    }
    configure_page_indicator(ui, page_count);
    update_page_indicator(ui, ui->active_page);
    lv_tileview_set_tile_by_index(ui->pages, (uint32_t)ui->active_page, 0, LV_ANIM_OFF);
}

static void rebuild_apps(launcher_ui_t *ui, size_t anchor, bool restore_focus, size_t focus_index)
{
    lv_obj_clean(ui->view.app_grid);
    lv_obj_set_style_pad_all(ui->view.app_grid, 0, 0);

    ui->pages = lv_tileview_create(ui->view.app_grid);
    lv_obj_set_name_static(ui->pages, "app_pages");
    lv_obj_set_size(ui->pages, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollbar_mode(ui->pages, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_border_width(ui->pages, 0, 0);
    lv_obj_set_style_radius(ui->pages, 0, 0);
    lv_obj_set_style_pad_all(ui->pages, 0, 0);
    lv_obj_set_style_bg_opa(ui->pages, LV_OPA_TRANSP, 0);

    size_t page_count = (ui->registry->count + ui->apps_per_page - 1) / ui->apps_per_page;
    if (page_count == 0) page_count = 1;
    size_t active_page = anchor / ui->apps_per_page;
    if (active_page >= page_count) active_page = page_count - 1;

    lv_obj_t *first_page         = nullptr;
    lv_obj_t *active_page_object = nullptr;
    lv_obj_t *focus_target       = nullptr;
    for (size_t page_index = 0; page_index < page_count; ++page_index) {
        lv_dir_t direction = LV_DIR_NONE;
        if (page_index > 0) direction = static_cast<lv_dir_t>(direction | LV_DIR_LEFT);
        if (page_index + 1 < page_count) direction = static_cast<lv_dir_t>(direction | LV_DIR_RIGHT);
        lv_obj_t *page = lv_tileview_add_tile(ui->pages, (uint8_t)page_index, 0, direction);
        lv_obj_set_name_static(page, "app_page");
        if (!first_page) first_page = page;
        if (page_index == active_page) active_page_object = page;
        style_app_page(page, ui->layout_width, ui->columns);

        const size_t first_app = page_index * ui->apps_per_page;
        size_t last_app        = first_app + ui->apps_per_page;
        if (last_app > ui->registry->count) last_app = ui->registry->count;
        for (size_t i = first_app; i < last_app; ++i) {
            launcher_app_t *app = &ui->registry->apps[i];
            lv_obj_t *tile      = launcher_view_create_app_tile(
                &ui->view, page, app->manifest.name, loadable_icon_path(app), icon_text(app->manifest.icon),
                icon_color(app->manifest.icon), APP_CELL_SIZE, ui->cell_height);
            if (!tile) continue;
            lv_obj_set_name_static(tile, "app_tile");
            lv_obj_set_user_data(tile, app);
            lv_obj_add_event_cb(tile, app_clicked, LV_EVENT_CLICKED, ui);
            if (restore_focus && i == focus_index) focus_target = tile;
        }
    }

    if (ui->registry->count == 0) {
        lv_obj_t *empty = lv_label_create(first_page);
        lv_label_set_text(empty, "No applications installed");
        lv_obj_set_name_static(empty, "empty_state");
        lv_obj_set_width(empty, ui->layout_width - 2 * app_grid_inset(ui->layout_width, ui->columns));
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(empty, lv_color_hex(launcher::theme::kMutedText), 0);
        lv_obj_set_style_text_font(empty, ui->view.font_22, 0);
    }

    populate_page_indicator(ui, page_count, active_page);
    lv_obj_add_event_cb(ui->pages, page_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_tileview_set_tile(ui->pages, active_page_object ? active_page_object : first_page, LV_ANIM_OFF);
    if (focus_target) lv_group_focus_obj(focus_target);
}

static layout_metrics_t calculate_layout(const launcher_ui_t *ui)
{
    layout_metrics_t metrics{};
    metrics.width  = lv_obj_get_width(ui->view.screen);
    metrics.height = lv_obj_get_height(ui->view.screen);
    if (metrics.width < 1) metrics.width = 1;
    if (metrics.height < 1) metrics.height = 1;

    const bool landscape        = metrics.width > metrics.height;
    const int32_t desired_inset = landscape ? HOME_INDICATOR_HEIGHT : 52;
    int32_t max_inset           = metrics.height - CM0_STATUS_BAR_HEIGHT - PAGE_INDICATOR_HEIGHT - APP_CELL_MIN_HEIGHT;
    if (max_inset < 0) max_inset = 0;
    metrics.bottom_inset = desired_inset < max_inset ? desired_inset : max_inset;

    const int32_t grid_height = metrics.height - CM0_STATUS_BAR_HEIGHT - PAGE_INDICATOR_HEIGHT - metrics.bottom_inset;
    metrics.columns           = metrics.width / APP_CELL_SIZE;
    if (metrics.columns < 1) metrics.columns = 1;
    if (metrics.columns > APP_COLUMN_LIMIT) metrics.columns = APP_COLUMN_LIMIT;

    int32_t fitting_rows = grid_height / APP_CELL_MIN_HEIGHT;
    if (fitting_rows < 1) fitting_rows = 1;
    int32_t needed_rows = (LAUNCHER_APPS_PER_PAGE + metrics.columns - 1) / metrics.columns;
    metrics.rows        = fitting_rows < needed_rows ? fitting_rows : needed_rows;
    if (metrics.rows > APP_ROW_LIMIT) metrics.rows = APP_ROW_LIMIT;

    metrics.cell_height = grid_height / metrics.rows;
    if (metrics.cell_height > APP_CELL_SIZE) metrics.cell_height = APP_CELL_SIZE;
    if (metrics.cell_height < 1) metrics.cell_height = 1;
    metrics.apps_per_page = (size_t)(metrics.columns * metrics.rows);
    if (metrics.apps_per_page > LAUNCHER_APPS_PER_PAGE) metrics.apps_per_page = LAUNCHER_APPS_PER_PAGE;

    return metrics;
}

static void apply_chrome_layout(launcher_ui_t *ui, const layout_metrics_t &metrics)
{
    lv_obj_set_height(ui->view.page_bottom_inset, metrics.bottom_inset);
    if (metrics.bottom_inset >= HOME_INDICATOR_HEIGHT)
        lv_obj_remove_flag(ui->view.home_indicator, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(ui->view.home_indicator, LV_OBJ_FLAG_HIDDEN);
    const int32_t panel_height = launcher_view_layout_quick_panel(&ui->view, metrics.width, metrics.height);
    if (!ui->quick_panel_open) {
        lv_anim_delete(ui->view.quick_panel, animate_object_y);
        lv_obj_set_y(ui->view.quick_panel, -panel_height);
    }
    lv_label_set_text(ui->view.orientation_label,
                      ui->orientation == LAUNCHER_ORIENTATION_LANDSCAPE ? "Portrait" : "Landscape");
    char session_label[sizeof(ui->session_user) + 12];
    if (ui->session_user[0] && metrics.width >= 480)
        snprintf(session_label, sizeof(session_label), "Sign out: %s", ui->session_user);
    else
        snprintf(session_label, sizeof(session_label), "Sign out");
    lv_label_set_text(ui->view.session_label, session_label);
}

static void relayout(launcher_ui_t *ui)
{
    if (!ui || !ui->view.screen || ui->relayout_in_progress) return;
    ui->relayout_in_progress = true;

    const layout_metrics_t metrics = calculate_layout(ui);
    const bool capacity_changed    = ui->pages && ui->apps_per_page != metrics.apps_per_page;
    size_t focus_index             = 0;
    const bool restore_focus       = focused_app_index(ui, &focus_index);
    const size_t anchor            = restore_focus ? focus_index : current_page_anchor(ui);

    ui->layout_width  = metrics.width;
    ui->layout_height = metrics.height;
    ui->orientation   = metrics.width > metrics.height ? LAUNCHER_ORIENTATION_LANDSCAPE : LAUNCHER_ORIENTATION_PORTRAIT;
    ui->columns       = metrics.columns;
    ui->cell_height   = metrics.cell_height;
    ui->apps_per_page = metrics.apps_per_page;
    apply_chrome_layout(ui, metrics);
    lv_obj_update_layout(ui->view.screen);

    if (!ui->pages || capacity_changed)
        rebuild_apps(ui, anchor, restore_focus, focus_index);
    else
        layout_existing_pages(ui);
    lv_obj_update_layout(ui->view.screen);
    ui->relayout_in_progress = false;
}

static void screen_size_changed(lv_event_t *event)
{
    relayout(static_cast<launcher_ui_t *>(lv_event_get_user_data(event)));
}

static void screen_deleted(lv_event_t *event)
{
    auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    if (!ui) return;
    ui->pages = nullptr;
    memset(&ui->view, 0, sizeof(ui->view));
    ui->file_share_previous_focus = nullptr;
    restore_file_share_focus(ui);
    ui->quick_panel_previous_focus = nullptr;
    restore_quick_panel_focus(ui);
    ui->quick_panel_open = false;
}

launcher_ui_t *launcher_ui_create(launcher_registry_t *registry)
{
    if (!registry) return nullptr;
    auto *ui = new (std::nothrow) launcher_ui_t{registry};
    if (!ui) return nullptr;

    ui->previous_screen = lv_screen_active();
    if (!launcher_view_create(&ui->view)) {
        launcher_view_destroy(&ui->view);
        delete ui;
        return nullptr;
    }

    lv_screen_load(ui->view.screen);
    lv_obj_add_event_cb(ui->view.screen, screen_size_changed, LV_EVENT_SIZE_CHANGED, ui);
    lv_obj_add_event_cb(ui->view.screen, screen_deleted, LV_EVENT_DELETE, ui);
    relayout(ui);

    lv_obj_add_event_cb(ui->view.quick_scrim, scrim_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.file_share_scrim, file_share_close_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.file_share_close_button, file_share_close_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.file_share_approve_button, file_share_approval_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.file_share_reject_button, file_share_approval_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.orientation_button, orientation_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.file_share_button, file_share_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.lock_button, lock_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.session_button, logout_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.brightness_slider, level_changed, LV_EVENT_VALUE_CHANGED, ui);
    lv_obj_add_event_cb(ui->view.volume_slider, level_changed, LV_EVENT_VALUE_CHANGED, ui);
    for (lv_obj_t *control : {ui->view.orientation_button, ui->view.file_share_button, ui->view.lock_button,
                              ui->view.session_button,
                              ui->view.power_button, ui->view.reboot_button, ui->view.brightness_slider,
                              ui->view.volume_slider})
        lv_obj_add_event_cb(control, quick_control_focused, LV_EVENT_FOCUSED, ui);
    lv_obj_add_event_cb(ui->view.power_button, power_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.reboot_button, reboot_clicked, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.power_cancel_button, power_cancelled, LV_EVENT_CLICKED, ui);
    lv_obj_add_event_cb(ui->view.power_confirm_button, power_confirmed, LV_EVENT_CLICKED, ui);
    attach_pull_down_input(ui);
    return ui;
}

void launcher_ui_destroy(launcher_ui_t *ui)
{
    if (!ui) return;
    restore_file_share_focus(ui);
    restore_quick_panel_focus(ui);
    if (ui->input) {
        lv_indev_remove_event_cb_with_user_data(ui->input, pull_down_input_event, ui);
        ui->input = nullptr;
    }
    if (ui->view.screen) lv_obj_remove_event_cb_with_user_data(ui->view.screen, screen_size_changed, ui);
    if (ui->view.quick_scrim) {
        lv_anim_delete(ui->view.quick_scrim, animate_object_opacity);
        lv_obj_remove_event_cb_with_user_data(ui->view.quick_scrim, scrim_clicked, ui);
    }
    if (ui->view.file_share_scrim)
        lv_obj_remove_event_cb_with_user_data(ui->view.file_share_scrim, file_share_close_clicked, ui);
    if (ui->view.file_share_close_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.file_share_close_button, file_share_close_clicked, ui);
    if (ui->view.file_share_approve_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.file_share_approve_button, file_share_approval_clicked, ui);
    if (ui->view.file_share_reject_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.file_share_reject_button, file_share_approval_clicked, ui);
    if (ui->view.quick_panel) lv_anim_delete(ui->view.quick_panel, animate_object_y);
    if (ui->view.orientation_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.orientation_button, orientation_clicked, ui);
    if (ui->view.file_share_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.file_share_button, file_share_clicked, ui);
    if (ui->view.power_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.power_button, power_clicked, ui);
    if (ui->view.reboot_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.reboot_button, reboot_clicked, ui);
    if (ui->view.power_cancel_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.power_cancel_button, power_cancelled, ui);
    if (ui->view.power_confirm_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.power_confirm_button, power_confirmed, ui);
    if (ui->view.session_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.session_button, logout_clicked, ui);
    if (ui->view.lock_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.lock_button, lock_clicked, ui);
    if (ui->view.screen && ui->previous_screen && lv_obj_is_valid(ui->previous_screen) &&
        lv_screen_active() == ui->view.screen)
        lv_screen_load(ui->previous_screen);
    launcher_view_destroy(&ui->view);
    ui->pages = nullptr;
    delete ui;
}

void launcher_ui_rebind_input(launcher_ui_t *ui)
{
    if (ui) {
        ui->keyboard_pressed = false;
        capture_quick_panel_input(ui);
        capture_file_share_input(ui);
    }
    if (!ui || !ui->view.screen || ui->input) return;
    ui->pull_tracking = false;
    attach_pull_down_input(ui);
}

bool launcher_ui_is_home(const launcher_ui_t *ui)
{
    return ui && ui->view.screen;
}

void launcher_ui_prepare_registry_refresh(launcher_ui_t *ui)
{
    if (!ui || !ui->view.screen) return;

    ui->refresh_focused_app_id[0] = '\0';
    ui->refresh_anchor_app_id[0]  = '\0';
    ui->refresh_pending_app_id[0] = '\0';
    ui->refresh_active_page       = ui->active_page;

    size_t focus_index = 0;
    if (focused_app_index(ui, &focus_index))
        snprintf(ui->refresh_focused_app_id, sizeof(ui->refresh_focused_app_id), "%s",
                 ui->registry->apps[focus_index].manifest.id);

    const size_t anchor = current_page_anchor(ui);
    if (anchor < ui->registry->count)
        snprintf(ui->refresh_anchor_app_id, sizeof(ui->refresh_anchor_app_id), "%s",
                 ui->registry->apps[anchor].manifest.id);
    if (ui->pending_app) {
        for (size_t i = 0; i < ui->registry->count; ++i) {
            if (ui->pending_app != &ui->registry->apps[i]) continue;
            snprintf(ui->refresh_pending_app_id, sizeof(ui->refresh_pending_app_id), "%s",
                     ui->registry->apps[i].manifest.id);
            break;
        }
    }
    ui->registry_refresh_prepared = true;
}

void launcher_ui_refresh_home(launcher_ui_t *ui)
{
    if (!ui || !ui->view.screen) return;
    size_t focus_index = 0;
    bool restore_focus = false;
    size_t anchor      = current_page_anchor(ui);
    if (ui->registry_refresh_prepared) {
        size_t pending_index = 0;
        ui->pending_app      = app_index_by_id(ui, ui->refresh_pending_app_id, &pending_index)
                                   ? &ui->registry->apps[pending_index]
                                   : nullptr;
        restore_focus        = app_index_by_id(ui, ui->refresh_focused_app_id, &focus_index);
        if (restore_focus) {
            anchor = focus_index;
        } else if (!app_index_by_id(ui, ui->refresh_anchor_app_id, &anchor)) {
            anchor = ui->refresh_active_page * ui->apps_per_page;
        }
    } else {
        restore_focus = focused_app_index(ui, &focus_index);
        if (restore_focus) anchor = focus_index;
    }
    ui->registry_refresh_prepared = false;
    rebuild_apps(ui, anchor, restore_focus, focus_index);
    lv_obj_update_layout(ui->view.screen);
}

void launcher_ui_apply_orientation(launcher_ui_t *ui, launcher_orientation_t orientation)
{
    if (!ui || !ui->view.screen) return;
    close_file_share_panel(ui);
    close_power_confirmation(ui);
    restore_quick_panel_focus(ui);
    ui->orientation                 = orientation;
    ui->orientation_request_pending = false;
    ui->quick_panel_open            = false;
    lv_anim_delete(ui->view.quick_scrim, animate_object_opacity);
    lv_anim_delete(ui->view.quick_panel, animate_object_y);
    lv_obj_add_flag(ui->view.quick_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(ui->view.quick_scrim, LV_OPA_TRANSP, 0);
    relayout(ui);
}

void launcher_ui_set_session_user(launcher_ui_t *ui, const char *name)
{
    if (!ui || !ui->view.lock_button || !ui->view.session_button || !ui->view.file_share_button) return;
    ui->session_user[0] = '\0';
    if (name && name[0]) {
        snprintf(ui->session_user, sizeof(ui->session_user), "%s", name);
        lv_obj_clear_flag(ui->view.file_share_button, LV_OBJ_FLAG_HIDDEN);
        if (ui->account_actions_enabled) {
            lv_obj_clear_flag(ui->view.lock_button, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui->view.session_button, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        close_file_share_panel(ui);
        lv_obj_add_flag(ui->view.file_share_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->view.lock_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->view.session_button, LV_OBJ_FLAG_HIDDEN);
    }
    relayout(ui);
}

void launcher_ui_set_account_actions_enabled(launcher_ui_t *ui, bool enabled)
{
    if (!ui) return;
    ui->account_actions_enabled = enabled;
    if (!enabled) {
        ui->lock_request = false;
        ui->logout_request = false;
    }
    if (!ui->view.lock_button || !ui->view.session_button) return;
    if (enabled && ui->session_user[0]) {
        lv_obj_clear_flag(ui->view.lock_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui->view.session_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->view.lock_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->view.session_button, LV_OBJ_FLAG_HIDDEN);
    }
    relayout(ui);
}

bool launcher_ui_take_lock_request(launcher_ui_t *ui)
{
    if (!ui || !ui->lock_request) return false;
    ui->lock_request = false;
    return true;
}

bool launcher_ui_take_logout_request(launcher_ui_t *ui)
{
    if (!ui || !ui->logout_request) return false;
    ui->logout_request = false;
    return true;
}

void launcher_ui_set_system_controls(launcher_ui_t *ui, bool brightness_available, int brightness_percent,
                                     bool volume_available, int volume_percent, bool power_available,
                                     bool reboot_available)
{
    if (!ui || !ui->view.screen) return;
    const bool capability_changed = brightness_available == lv_obj_has_state(ui->view.brightness_slider, LV_STATE_DISABLED) ||
        volume_available == lv_obj_has_state(ui->view.volume_slider, LV_STATE_DISABLED) ||
        power_available == lv_obj_has_state(ui->view.power_button, LV_STATE_DISABLED) ||
        reboot_available == lv_obj_has_state(ui->view.reboot_button, LV_STATE_DISABLED);
    if (!brightness_available || (!lv_slider_is_dragged(ui->view.brightness_slider) && ui->brightness_request < 0))
        launcher_view_set_control_level(ui->view.brightness_slider, ui->view.brightness_label,
                                        brightness_available, brightness_percent);
    if (!volume_available || (!lv_slider_is_dragged(ui->view.volume_slider) && ui->volume_request < 0))
        launcher_view_set_control_level(ui->view.volume_slider, ui->view.volume_label, volume_available, volume_percent);
    if (!brightness_available) ui->brightness_request = -1;
    if (!volume_available) ui->volume_request = -1;
    if (power_available) lv_obj_remove_state(ui->view.power_button, LV_STATE_DISABLED);
    else lv_obj_add_state(ui->view.power_button, LV_STATE_DISABLED);
    if (reboot_available) lv_obj_remove_state(ui->view.reboot_button, LV_STATE_DISABLED);
    else lv_obj_add_state(ui->view.reboot_button, LV_STATE_DISABLED);
    if (capability_changed && ui->quick_panel_group && !ui->power_confirmation_open) {
        lv_obj_t *focused = lv_group_get_focused(ui->quick_panel_group);
        populate_quick_panel_group(ui);
        if (focused && !lv_obj_has_state(focused, LV_STATE_DISABLED)) lv_group_focus_obj(focused);
    }
}

void launcher_ui_set_software_brightness(launcher_ui_t *ui, bool enabled, int percent)
{
    if (!ui) return;
    launcher_view_set_software_brightness(&ui->view, enabled, percent);
}

bool launcher_ui_take_brightness_request(launcher_ui_t *ui, int *percent)
{
    if (!ui || !percent || ui->brightness_request < 0) return false;
    *percent = ui->brightness_request;
    ui->brightness_request = -1;
    return true;
}

bool launcher_ui_take_volume_request(launcher_ui_t *ui, int *percent)
{
    if (!ui || !percent || ui->volume_request < 0) return false;
    *percent = ui->volume_request;
    ui->volume_request = -1;
    return true;
}

bool launcher_ui_take_power_request(launcher_ui_t *ui)
{
    if (!ui || !ui->power_request) return false;
    ui->power_request = false;
    return true;
}

bool launcher_ui_take_reboot_request(launcher_ui_t *ui)
{
    if (!ui || !ui->reboot_request) return false;
    ui->reboot_request = false;
    return true;
}

void launcher_ui_set_file_share_directory(launcher_ui_t *ui, const char *path)
{
    if (!ui) return;
    snprintf(ui->file_share_directory, sizeof(ui->file_share_directory), "%s", path && path[0] ? path : "");
    if (!ui->view.file_share_path) return;
    if (ui->file_share_directory[0]) lv_label_set_text(ui->view.file_share_path, ui->file_share_directory);
    else lv_label_set_text(ui->view.file_share_path, "$HOME/Downloads");
}

void launcher_ui_set_file_share_status(launcher_ui_t *ui, const char *status)
{
    if (!ui || !ui->view.file_share_status) return;
    lv_label_set_text(ui->view.file_share_status, status && status[0] ? status : "Ready to receive files from your browser.");
}

void launcher_ui_set_file_share_approval(launcher_ui_t *ui, const char *filename, uint64_t size, bool pending)
{
    if (!ui || !ui->view.file_share_status || !ui->view.file_share_approve_button ||
        !ui->view.file_share_reject_button) return;
    ui->file_share_approval_visible = pending && filename && filename[0];
    ui->file_share_decision_pending = false;
    if (ui->file_share_approval_visible) {
        char message[512];
        snprintf(message, sizeof(message), "Receive \"%.255s\" (%llu bytes)?",
                 filename, static_cast<unsigned long long>(size));
        lv_label_set_text(ui->view.file_share_status, message);
        lv_obj_remove_flag(ui->view.file_share_approve_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(ui->view.file_share_reject_button, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->view.file_share_approve_button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui->view.file_share_reject_button, LV_OBJ_FLAG_HIDDEN);
    }
    populate_file_share_group(ui);
}

bool launcher_ui_take_file_share_decision(launcher_ui_t *ui, bool *accepted)
{
    if (!ui || !accepted || !ui->file_share_decision_pending) return false;
    *accepted = ui->file_share_decision_accepted;
    ui->file_share_decision_pending = false;
    return true;
}

bool launcher_ui_file_share_open(const launcher_ui_t *ui)
{
    return ui && ui->input_enabled && ui->file_share_open;
}

void launcher_ui_set_power_result(launcher_ui_t *ui, bool success, bool simulated)
{
    if (!ui || !ui->view.screen || !ui->power_confirmation_open) return;
    const bool reboot = ui->power_confirmation_reboot;
    ui->power_busy = success && !simulated;
    lv_label_set_text(ui->view.power_confirm_title,
                      success ? (simulated ? "Simulation complete" : (reboot ? "Restarting" : "Powering off"))
                              : (reboot ? "Couldn't restart" : "Couldn't power off"));
    lv_label_set_text(ui->view.power_confirm_hint, success ? (simulated ? "Your computer will stay on." : "Please wait...") :
                                                           "Please try again.");
    if (success) lv_obj_add_flag(ui->view.power_confirm_button, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_state(ui->view.power_confirm_button, LV_STATE_DISABLED);
    if (!ui->power_busy) lv_obj_remove_state(ui->view.power_cancel_button, LV_STATE_DISABLED);
    populate_quick_panel_group(ui);
    if (!ui->power_busy) lv_group_focus_obj(ui->view.power_cancel_button);
}

void launcher_ui_set_input_enabled(launcher_ui_t *ui, bool enabled)
{
    if (!ui) return;
    ui->input_enabled = enabled;
    ui->keyboard_pressed = false;
    ui->pull_tracking = false;
    if (!enabled) {
        ui->pending_app = nullptr;
        ui->orientation_request_pending = false;
        ui->lock_request = false;
        ui->logout_request = false;
        ui->brightness_request = -1;
        ui->volume_request = -1;
        ui->power_request = false;
        ui->reboot_request = false;
        close_file_share_panel(ui);
        play_quick_panel(ui, false);
    }
}

bool launcher_ui_handle_key(launcher_ui_t *ui, uint32_t key)
{
    if (!ui || !ui->view.screen || !ui->input_enabled) return false;
    if (ui->shutdown_visible) return true;

    /* Launcher owns escape while it is visible.  In particular this keeps a
     * global keyboard handler from treating Esc on the home screen as an app
     * exit request. */
    if (key == LV_KEY_ESC) {
        if (ui->file_share_open) {
            close_file_share_panel(ui);
        } else if (ui->power_confirmation_open) {
            if (!ui->power_busy) close_power_confirmation(ui);
        } else if (ui->quick_panel_open) play_quick_panel(ui, false);
        return true;
    }
    if (ui->file_share_open) {
        if (key == LV_KEY_ENTER) {
            lv_obj_t *focused = ui->file_share_group ? lv_group_get_focused(ui->file_share_group) : nullptr;
            if (focused) lv_obj_send_event(focused, LV_EVENT_CLICKED, nullptr);
        } else if (ui->file_share_group) {
            if (key == LV_KEY_LEFT || key == LV_KEY_UP || key == LV_KEY_PREV)
                lv_group_focus_prev(ui->file_share_group);
            else if (key == LV_KEY_RIGHT || key == LV_KEY_DOWN || key == LV_KEY_NEXT)
                lv_group_focus_next(ui->file_share_group);
        }
        return true;
    }
    if (key == ' ') {
        if (!ui->power_confirmation_open) play_quick_panel(ui, !ui->quick_panel_open);
        return true;
    }

    const bool directional = key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN;
    const bool tab_navigation = key == LV_KEY_NEXT || key == LV_KEY_PREV;
    if (!directional && key != LV_KEY_ENTER && !(ui->quick_panel_open && tab_navigation)) return false;
    if (ui->quick_panel_open) {
        lv_group_t *group = ui->quick_panel_group;
        lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
        if (!focused || ui->power_busy) return true;
        const bool slider = focused == ui->view.brightness_slider || focused == ui->view.volume_slider;
        if (slider && directional) {
            // LVGL's slider key handler applies a one-unit nudge after the
            // indev callback. Compensate so the user-facing step is exactly
            // five percent in each direction.
            const int step = key == LV_KEY_LEFT || key == LV_KEY_DOWN ? -4 : 4;
            lv_slider_set_value(focused, std::clamp(lv_slider_get_value(focused) + step,
                                static_cast<int>(lv_slider_get_min_value(focused)),
                                static_cast<int>(lv_slider_get_max_value(focused))), LV_ANIM_OFF);
            lv_obj_send_event(focused, LV_EVENT_VALUE_CHANGED, nullptr);
            return true;
        }
        if (key == LV_KEY_ENTER) {
            if (!slider) lv_obj_send_event(focused, LV_EVENT_CLICKED, nullptr);
        } else {
            const int step = key == LV_KEY_LEFT || key == LV_KEY_UP || key == LV_KEY_PREV ? -1 : 1;
            if (step < 0) lv_group_focus_prev(group);
            else lv_group_focus_next(group);
        }
        return true;
    }
    if (!ui->registry || ui->registry->count == 0) return true;

    size_t current = 0;
    const bool has_focus = focused_app_index(ui, &current);
    if (!has_focus) {
        const size_t page = active_page_for_navigation(ui);
        const size_t first = page * ui->apps_per_page;
        current           = first < ui->registry->count ? first : 0;
        /* A first direction key establishes focus without an extra move. */
        if (directional) {
            focus_app_index(ui, current);
            return true;
        }
    }

    if (key == LV_KEY_ENTER) {
        if (!has_focus) focus_app_index(ui, current);
        lv_obj_t *tile = app_tile_at_index(ui, current);
        if (tile) ui->pending_app = static_cast<launcher_app_t *>(lv_obj_get_user_data(tile));
        return true;
    }

    size_t target = current;
    if (navigation_target(ui, current, key, &target) && target != current) focus_app_index(ui, target);
    return true;
}

bool launcher_ui_handle_keyboard_input(launcher_ui_t *ui, lv_indev_t *keyboard)
{
    if (!ui || !ui->view.screen || !keyboard || !ui->input_enabled) return false;
    // Also capture keyboards created or reconnected while the panel is open.
    if (ui->quick_panel_group && lv_indev_get_group(keyboard) != ui->quick_panel_group)
        lv_indev_set_group(keyboard, ui->quick_panel_group);
    if (ui->file_share_group && lv_indev_get_group(keyboard) != ui->file_share_group)
        lv_indev_set_group(keyboard, ui->file_share_group);
    const uint32_t key = lv_indev_get_key(keyboard);
    const bool directional = key == LV_KEY_LEFT || key == LV_KEY_RIGHT || key == LV_KEY_UP || key == LV_KEY_DOWN;
    const bool handled = directional || key == LV_KEY_ENTER || key == LV_KEY_ESC || key == ' ';
    if (lv_indev_get_state(keyboard) == LV_INDEV_STATE_RELEASED) {
        ui->keyboard_pressed = false;
        return handled;
    }

    // LVGL sends the indev KEY event on every poll, even when evdev has no new
    // event. Only a new press moves immediately; a held direction repeats at
    // a deliberate rate, independent of polling and evdev queue draining.
    const bool new_press = !ui->keyboard_pressed || ui->keyboard_key != key;
    ui->keyboard_pressed = true;
    ui->keyboard_key = key;
    if (new_press) {
        ui->keyboard_action_tick = lv_tick_get();
        ui->keyboard_repeating = false;
    } else {
        const uint32_t delay = ui->keyboard_repeating ? KEYBOARD_REPEAT_INTERVAL_MS : KEYBOARD_REPEAT_DELAY_MS;
        if (!directional || lv_tick_elaps(ui->keyboard_action_tick) < delay) return handled;
        ui->keyboard_action_tick = lv_tick_get();
        ui->keyboard_repeating = true;
    }

    if (!handled) return false;
    const bool consumed = launcher_ui_handle_key(ui, key);
    // Enter launches on press. Suppress LVGL's later release-to-click path,
    // which would otherwise queue the same application a second time.
    if (consumed && key == LV_KEY_ENTER) lv_indev_wait_release(keyboard);
    return consumed;
}

void launcher_ui_show_shutdown(launcher_ui_t *ui)
{
    if (!ui || !ui->view.shutdown_scrim || ui->shutdown_visible) return;
    ui->shutdown_visible = true;
    ui->power_request = false;
    ui->reboot_request = false;
    close_file_share_panel(ui);
    play_quick_panel(ui, false);
    lv_obj_clear_flag(ui->view.shutdown_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ui->view.shutdown_scrim);
    lv_obj_update_layout(ui->view.shutdown_scrim);
}

void launcher_ui_hide_shutdown(launcher_ui_t *ui)
{
    if (!ui || !ui->view.shutdown_scrim || !ui->shutdown_visible) return;
    ui->shutdown_visible = false;
    lv_obj_add_flag(ui->view.shutdown_scrim, LV_OBJ_FLAG_HIDDEN);
}

bool launcher_ui_take_orientation_request(launcher_ui_t *ui, launcher_orientation_t *orientation)
{
    if (!ui || !orientation || !ui->orientation_request_pending) return false;
    *orientation                    = ui->requested_orientation;
    ui->orientation_request_pending = false;
    return true;
}

const launcher_app_t *launcher_ui_take_launch_request(launcher_ui_t *ui)
{
    if (!ui) return nullptr;
    launcher_app_t *app = ui->pending_app;
    ui->pending_app     = nullptr;
    return app;
}
