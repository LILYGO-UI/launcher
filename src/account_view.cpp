#include "account_view.h"

#include "launcher_theme.h"

#include <cm0/typography.h>

#include <algorithm>
#include <cstring>
#include <new>
#include <vector>

constexpr size_t kUsernameMax = 63;
constexpr size_t kPasswordMax = 127;
constexpr int32_t kGap = 10;

struct launcher_account_view {
    struct input_binding {
        lv_indev_t *input;
        lv_group_t *group;
    };
    lv_obj_t *modal = nullptr;
    lv_obj_t *card = nullptr;
    lv_obj_t *brand = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *subtitle = nullptr;
    lv_obj_t *fields = nullptr;
    lv_obj_t *mode_button = nullptr;
    lv_obj_t *mode_label = nullptr;
    lv_obj_t *username = nullptr;
    lv_obj_t *password = nullptr;
    lv_obj_t *confirm = nullptr;
    lv_obj_t *keyboard = nullptr;
    lv_obj_t *submit = nullptr;
    lv_obj_t *submit_label = nullptr;
    lv_obj_t *error = nullptr;
    const lv_font_t *font_14 = nullptr;
    const lv_font_t *font_22 = nullptr;
    const lv_font_t *font_28 = nullptr;
    const lv_font_t *font_36 = nullptr;
    lv_group_t *group = nullptr;
    lv_group_t *previous_group = nullptr;
    lv_obj_t *previous_focus = nullptr;
    std::vector<input_binding> input_bindings;
    bool setup_available = false;
    bool create_mode = false;
    bool first_use = false;
    bool setup_continuation = false;
    bool locked = false;
    bool hardware_keyboard_present = false;
    bool software_keyboard_visible = true;
    bool busy = false;
    bool pending = false;
    char pending_username[kUsernameMax + 1]{};
    char pending_password[kPasswordMax + 1]{};
    char locked_username[kUsernameMax + 1]{};
};

namespace {
static void clear_sensitive(char *buffer, size_t size)
{
    if (!buffer || !size) return;
    volatile char *p = buffer;
    while (size--) *p++ = 0;
}

static bool group_exists(lv_group_t *group)
{
    if (!group) return false;
    for (uint32_t index = 0; index < lv_group_get_count(); ++index) {
        if (lv_group_by_index(index) == group) return true;
    }
    return false;
}

static void capture_input(launcher_account_view_t *view)
{
    for (lv_indev_t *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        const lv_indev_type_t type = lv_indev_get_type(input);
        if (type != LV_INDEV_TYPE_KEYPAD && type != LV_INDEV_TYPE_ENCODER) continue;
        lv_group_t *original = lv_indev_get_group(input);
        if (original == view->group) continue;
        auto binding = std::find_if(view->input_bindings.begin(), view->input_bindings.end(),
                                   [input](const launcher_account_view_t::input_binding &entry) {
                                       return entry.input == input;
                                   });
        if (binding != view->input_bindings.end()) binding->group = original;
        else view->input_bindings.push_back({input, original});
        lv_indev_reset(input, nullptr);
        lv_indev_set_group(input, view->group);
    }
}

static void restore_input(launcher_account_view_t *view)
{
    lv_group_t *previous = group_exists(view->previous_group) ? view->previous_group : nullptr;
    if (lv_group_get_default() == view->group) lv_group_set_default(previous);
    for (lv_indev_t *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
        if (lv_indev_get_group(input) != view->group) continue;
        lv_group_t *target = previous;
        for (const auto &binding : view->input_bindings) {
            if (binding.input != input) continue;
            target = group_exists(binding.group) ? binding.group : nullptr;
            break;
        }
        lv_indev_reset(input, nullptr);
        lv_indev_set_group(input, target);
    }
    if (view->previous_focus && lv_obj_is_valid(view->previous_focus) &&
        lv_obj_get_group(view->previous_focus) == previous)
        lv_group_focus_obj(view->previous_focus);
    if (group_exists(view->group)) lv_group_delete(view->group);
    view->group = nullptr;
}

static lv_obj_t *label(lv_obj_t *parent, const char *name, const char *text, const lv_font_t *font,
                       lv_color_t color)
{
    lv_obj_t *object = lv_label_create(parent);
    if (!object) return nullptr;
    if (name) lv_obj_set_name_static(object, name);
    lv_label_set_text(object, text ? text : "");
    lv_obj_set_style_text_font(object, font, 0);
    lv_obj_set_style_text_color(object, color, 0);
    return object;
}

static void style_field(lv_obj_t *field, const lv_font_t *font)
{
    lv_obj_set_width(field, LV_PCT(100));
    lv_obj_set_height(field, 52);
    lv_obj_set_style_min_height(field, 52, 0);
    lv_obj_set_style_text_font(field, font, 0);
    lv_obj_set_style_text_color(field, lv_color_hex(launcher::theme::kText), 0);
    lv_obj_set_style_text_color(field, lv_color_hex(launcher::theme::kAccountPlaceholder), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_bg_color(field, lv_color_hex(launcher::theme::kAccountFieldBackground), 0);
    lv_obj_set_style_bg_opa(field, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(field, 1, 0);
    lv_obj_set_style_border_color(field, lv_color_hex(launcher::theme::kAccountFieldBorder), 0);
    lv_obj_set_style_border_width(field, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(field, lv_color_hex(launcher::theme::kAccountAccent), LV_STATE_FOCUSED);
    /* Keyboard focus uses LV_STATE_FOCUS_KEY. Match the touch-focused field
     * border so every account input has one consistent focus treatment. */
    lv_obj_set_style_border_width(field, 2, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_color(field, lv_color_hex(launcher::theme::kAccountAccent), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(field, lv_color_hex(launcher::theme::kContainerBackground), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_width(field, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(field, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(field, LV_OPA_TRANSP, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(field, lv_color_hex(launcher::theme::kContainerBackground), LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(field, lv_color_hex(launcher::theme::kControlPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(field, lv_color_hex(launcher::theme::kControlPressed), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(field, lv_color_hex(launcher::theme::kMutedText), LV_STATE_DISABLED);
    lv_obj_set_style_radius(field, 12, 0);
    lv_obj_set_style_pad_left(field, 16, 0);
    lv_obj_set_style_pad_right(field, 16, 0);
    /* One-line textareas size their child label from the top edge. Symmetric
     * vertical padding keeps both entered text and the placeholder centered
     * inside the 52 px touch target. */
    lv_obj_set_style_pad_top(field, 11, 0);
    lv_obj_set_style_pad_bottom(field, 11, 0);
    lv_textarea_set_one_line(field, true);
    lv_textarea_set_cursor_click_pos(field, true);
    lv_textarea_set_max_length(field, kPasswordMax);
}

static void set_keyboard_target(launcher_account_view_t *view, lv_obj_t *field)
{
    if (view && view->keyboard && field) lv_keyboard_set_textarea(view->keyboard, field);
}

static void relayout(launcher_account_view_t *view);

static void set_software_keyboard_visible(launcher_account_view_t *view, bool visible)
{
    if (!view || view->hardware_keyboard_present) return;
    if (view->software_keyboard_visible == visible) return;
    view->software_keyboard_visible = visible;
    relayout(view);
}

static void dismiss_keyboard_event(lv_event_t *event)
{
    set_software_keyboard_visible(static_cast<launcher_account_view_t *>(lv_event_get_user_data(event)), false);
}

static void field_event(lv_event_t *event)
{
    auto *view = static_cast<launcher_account_view_t *>(lv_event_get_user_data(event));
    if (!view || view->busy) return;
    lv_obj_t *field = lv_event_get_current_target_obj(event);
    if (field && (!view->locked || field == view->password)) {
        set_software_keyboard_visible(view, true);
        set_keyboard_target(view, field);
        /* Keep the active field visible when the keyboard reduces the card to
         * a scrollable viewport on compact displays. */
        lv_obj_scroll_to_view(field, LV_ANIM_ON);
    }
}

static const char *subtitle_text(const launcher_account_view_t *view)
{
    if (view->locked) return "Enter your password to unlock";
    if (view->first_use) return view->setup_continuation ? "2 of 5 / Create your account to continue setup" :
                                                           "Choose your username and password to get started";
    if (view->create_mode) return "Create an account for this device";
    return "Sign in to continue";
}

static void set_mode(launcher_account_view_t *view, bool create)
{
    if (!view) return;
    view->create_mode = view->first_use || (!view->locked && view->setup_available && create);
    if (view->create_mode)
        lv_obj_clear_flag(view->confirm, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(view->confirm, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(view->title, view->locked ? "Screen locked" : view->first_use ? "First-time setup" :
                                                        view->create_mode ? "Create account" : "Sign in");
    lv_label_set_text(view->subtitle, subtitle_text(view));
    lv_label_set_text(view->mode_label, view->create_mode ? "Use existing account" : "Create first account");
    lv_label_set_text(view->submit_label, view->locked ? "Unlock" : view->first_use ? (view->setup_continuation ? "Continue" : "Finish setup") :
                                                             view->create_mode ? "Create account" : "Sign in");
    lv_textarea_set_text(view->password, "");
    lv_textarea_set_text(view->confirm, "");
    if (view->error) lv_label_set_text(view->error, "");
    set_keyboard_target(view, view->locked ? view->password : view->username);
}

static void mode_clicked(lv_event_t *event)
{
    auto *view = static_cast<launcher_account_view_t *>(lv_event_get_user_data(event));
    if (!view || view->busy || view->first_use || !view->setup_available) return;
    set_software_keyboard_visible(view, false);
    set_mode(view, !view->create_mode);
}

static void submit_clicked(lv_event_t *event)
{
    auto *view = static_cast<launcher_account_view_t *>(lv_event_get_user_data(event));
    if (!view || view->busy || view->pending) return;
    set_software_keyboard_visible(view, false);
    const char *username = view->locked ? view->locked_username : lv_textarea_get_text(view->username);
    const char *password = lv_textarea_get_text(view->password);
    const char *confirm = lv_textarea_get_text(view->confirm);
    if (!username || !username[0]) {
        launcher_account_view_set_error(view, "Enter a username");
        return;
    }
    if (!password || !password[0]) {
        launcher_account_view_set_error(view, "Enter a password");
        return;
    }
    if (view->create_mode && std::strcmp(password, confirm ? confirm : "") != 0) {
        launcher_account_view_set_error(view, "Passwords do not match");
        launcher_account_view_clear_passwords(view);
        return;
    }
    std::strncpy(view->pending_username, username, kUsernameMax);
    view->pending_username[kUsernameMax] = '\0';
    std::strncpy(view->pending_password, password, kPasswordMax);
    view->pending_password[kPasswordMax] = '\0';
    view->pending = true;
    /* Remove secrets from widgets as soon as submit is pressed. */
    launcher_account_view_clear_passwords(view);
    if (view->error) lv_label_set_text(view->error, "");
}

static void relayout(launcher_account_view_t *view)
{
    if (!view || !view->modal || !view->card || !view->keyboard) return;
    const int32_t width = lv_obj_get_width(view->modal);
    const int32_t height = lv_obj_get_height(view->modal);
    /* Keep compact diagnostic windows and wide landscape displays on the same
     * stacked form so every field retains a readable touch target. */
    const bool landscape = width >= 600 && width > height;
    const bool show_keyboard = !view->hardware_keyboard_present && view->software_keyboard_visible;
    const int32_t keyboard_height = show_keyboard ? std::max<int32_t>(72, height * 36 / 100) : 0;
    if (show_keyboard) {
        lv_obj_clear_flag(view->keyboard, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_get_group(view->keyboard) != view->group) lv_group_add_obj(view->group, view->keyboard);
    } else {
        lv_obj_add_flag(view->keyboard, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_get_group(view->keyboard) == view->group) lv_group_remove_obj(view->keyboard);
    }
    lv_obj_set_size(view->keyboard, LV_PCT(100), keyboard_height);
    lv_obj_align(view->keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    /* Keep the keyboard readable in the compact diagnostic viewport while
     * giving the full-height device the airy spacing of an iOS keyboard. */
    const int32_t keyboard_padding = height < 400 ? 3 : 8;
    const int32_t keyboard_gap = height < 400 ? 2 : 6;
    lv_obj_set_style_pad_all(view->keyboard, keyboard_padding, 0);
    lv_obj_set_style_pad_row(view->keyboard, keyboard_gap, 0);
    lv_obj_set_style_pad_column(view->keyboard, keyboard_gap, 0);
    const int32_t available_height = height - keyboard_height;
    const int32_t card_padding = landscape ? 20 : 24;
    const int32_t card_gap = landscape ? 8 : kGap + 2;
    lv_obj_set_style_pad_all(view->card, card_padding, 0);
    lv_obj_set_style_pad_row(view->card, card_gap, 0);
    /* Keep the form readable in landscape as well: fields remain stacked so
     * Username and Password never compete for one narrow horizontal row. */
    lv_obj_set_flex_flow(view->fields, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->fields, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(view->fields, card_gap, 0);
    lv_obj_set_style_pad_column(view->fields, 0, 0);
    for (lv_obj_t *field : {view->username, view->password, view->confirm}) {
        lv_obj_set_width(field, LV_PCT(100));
        lv_obj_set_style_flex_grow(field, 0, 0);
    }
    /* A hardware keypad leaves the whole landscape canvas available. Keep a
     * content-sized card centered in that space instead of stretching it from
     * the top edge to the bottom edge. */
    const bool centered_landscape = landscape && !show_keyboard;
    const int32_t card_height = centered_landscape
        ? std::min<int32_t>(std::max<int32_t>(124, available_height - 32), 480)
        : std::max<int32_t>(124, available_height - 24);
    const int32_t max_card_width = landscape ? 560 : 440;
    const int32_t card_width = std::min<int32_t>(std::max<int32_t>(200, width - 24), max_card_width);
    lv_obj_set_size(view->card, card_width, card_height);
    if (centered_landscape) {
        lv_obj_set_style_pad_all(view->card, card_padding, 0);
        lv_obj_set_flex_align(view->card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(view->card, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_set_flex_align(view->card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_align(view->card, LV_ALIGN_TOP_MID, 0, 12);
    }
}

} // namespace

static launcher_account_view_t *create_account_view(bool setup_available, const char *last_user,
                                                    bool start_setup, bool locked, bool first_use)
{
    if (locked && (!last_user || !last_user[0] || std::strlen(last_user) > kUsernameMax)) return nullptr;
    auto *view = new (std::nothrow) launcher_account_view_t{};
    if (!view) return nullptr;
    view->setup_available = setup_available;
    view->first_use = first_use;
    view->create_mode = first_use || (setup_available && start_setup);
    view->locked = locked;
    if (locked) std::strcpy(view->locked_username, last_user);
    view->font_14 = lilygo_ui_font_get(14);
    view->font_22 = lilygo_ui_font_get(22);
    view->font_28 = lilygo_ui_font_get(28);
    view->font_36 = lilygo_ui_font_get(36);
    if (!view->font_14 || !view->font_22 || !view->font_28 || !view->font_36) {
        delete view;
        return nullptr;
    }

    view->modal = lv_obj_create(lv_layer_top());
    if (!view->modal) {
        delete view;
        return nullptr;
    }
    view->previous_group = lv_group_get_default();
    view->previous_focus = view->previous_group ? lv_group_get_focused(view->previous_group) : nullptr;
    view->group = lv_group_create();
    if (!view->group) {
        lv_obj_delete(view->modal);
        delete view;
        return nullptr;
    }
    lv_group_set_default(view->group);
    lv_obj_set_name_static(view->modal, "account_modal");
    lv_obj_set_size(view->modal, LV_PCT(100), LV_PCT(100));
    lv_obj_center(view->modal);
    lv_obj_add_flag(view->modal, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(view->modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(view->modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(view->modal, lv_color_hex(launcher::theme::kPageBackground), 0);
    lv_obj_set_style_bg_opa(view->modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->modal, 0, 0);
    lv_obj_set_style_pad_all(view->modal, 0, 0);
    lv_obj_add_event_cb(view->modal, dismiss_keyboard_event, LV_EVENT_CLICKED, view);

    view->card = lv_obj_create(view->modal);
    lv_obj_set_name_static(view->card, "account_card");
    lv_obj_add_flag(view->card, LV_OBJ_FLAG_FLOATING);
    /* The compact diagnostic viewport can be shorter than the complete form;
     * keep the submit control reachable by scrolling the card above the
     * on-screen keyboard. */
    lv_obj_add_flag(view->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(view->card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(view->card, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(view->card, lv_color_hex(launcher::theme::kContainerBackground), 0);
    lv_obj_set_style_bg_opa(view->card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->card, 0, 0);
    lv_obj_set_style_radius(view->card, 22, 0);
    lv_obj_set_style_shadow_width(view->card, 28, 0);
    lv_obj_set_style_shadow_color(view->card, lv_color_hex(launcher::theme::kPanelShadow), 0);
    lv_obj_set_style_shadow_opa(view->card, LV_OPA_20, 0);
    lv_obj_set_style_shadow_offset_y(view->card, 8, 0);
    lv_obj_set_style_pad_all(view->card, 24, 0);
    lv_obj_set_style_pad_row(view->card, kGap + 2, 0);
    lv_obj_set_flex_flow(view->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(view->card, dismiss_keyboard_event, LV_EVENT_CLICKED, view);

    view->brand = label(view->card, "account_brand", "LILYGO UI", view->font_14,
                        lv_color_hex(launcher::theme::kAccountAccent));
    lv_obj_set_width(view->brand, LV_PCT(100));
    lv_obj_set_style_text_align(view->brand, LV_TEXT_ALIGN_CENTER, 0);

    view->title = label(view->card, "account_title", locked ? "Screen locked" : view->first_use ? "First-time setup" :
                                                                    view->create_mode ? "Create account" : "Sign in", view->font_36,
                        lv_color_hex(launcher::theme::kContainerTitle));
    lv_obj_set_width(view->title, LV_PCT(100));
    lv_obj_set_style_text_align(view->title, LV_TEXT_ALIGN_CENTER, 0);

    view->subtitle = label(view->card, "account_subtitle", subtitle_text(view), view->font_14,
                           lv_color_hex(launcher::theme::kMutedText));
    lv_obj_set_width(view->subtitle, LV_PCT(100));
    lv_obj_set_style_text_align(view->subtitle, LV_TEXT_ALIGN_CENTER, 0);

    view->fields = lv_obj_create(view->card);
    lv_obj_set_name_static(view->fields, "account_fields");
    lv_obj_set_width(view->fields, LV_PCT(100));
    lv_obj_set_height(view->fields, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(view->fields, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->fields, 0, 0);
    lv_obj_set_style_pad_all(view->fields, 0, 0);
    /* Focus outlines extend beyond the textarea bounds. Do not clip those
     * outer pixels at the transparent fields container. */
    lv_obj_add_flag(view->fields, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_flex_flow(view->fields, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(view->fields, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(view->fields, dismiss_keyboard_event, LV_EVENT_CLICKED, view);

    view->username = lv_textarea_create(view->fields);
    lv_obj_set_name_static(view->username, "account_username");
    style_field(view->username, view->font_22);
    lv_textarea_set_max_length(view->username, locked ? kUsernameMax : 32);
    lv_textarea_set_placeholder_text(view->username, last_user && last_user[0] ? last_user : "Username");
    if (last_user && last_user[0] && (!view->create_mode || view->first_use))
        lv_textarea_set_text(view->username, last_user);
    lv_obj_add_event_cb(view->username, field_event, LV_EVENT_FOCUSED, view);
    lv_obj_add_event_cb(view->username, field_event, LV_EVENT_CLICKED, view);
    if (locked) {
        lv_obj_add_state(view->username, LV_STATE_DISABLED);
        lv_obj_remove_flag(view->username, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(view->username, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_group_remove_obj(view->username);
    }

    view->password = lv_textarea_create(view->fields);
    lv_obj_set_name_static(view->password, "account_password");
    style_field(view->password, view->font_22);
    lv_textarea_set_placeholder_text(view->password, "Password");
    lv_textarea_set_password_mode(view->password, true);
    lv_obj_add_event_cb(view->password, field_event, LV_EVENT_FOCUSED, view);
    lv_obj_add_event_cb(view->password, field_event, LV_EVENT_CLICKED, view);

    view->confirm = lv_textarea_create(view->fields);
    lv_obj_set_name_static(view->confirm, "account_confirm_password");
    style_field(view->confirm, view->font_22);
    lv_textarea_set_placeholder_text(view->confirm, "Confirm password");
    lv_textarea_set_password_mode(view->confirm, true);
    lv_obj_add_event_cb(view->confirm, field_event, LV_EVENT_FOCUSED, view);
    lv_obj_add_event_cb(view->confirm, field_event, LV_EVENT_CLICKED, view);
    if (!view->create_mode) lv_obj_add_flag(view->confirm, LV_OBJ_FLAG_HIDDEN);

    view->error = label(view->card, "account_error", "", view->font_14,
                        lv_color_hex(launcher::theme::kAccountError));
    lv_obj_set_width(view->error, LV_PCT(100));
    lv_obj_set_style_min_height(view->error, 20, 0);
    lv_label_set_long_mode(view->error, LV_LABEL_LONG_MODE_WRAP);

    view->submit = lv_button_create(view->card);
    lv_obj_set_name_static(view->submit, "account_submit");
    lv_obj_set_width(view->submit, LV_PCT(100));
    lv_obj_set_height(view->submit, 52);
    lv_obj_set_style_min_height(view->submit, 52, 0);
    lv_obj_set_style_bg_color(view->submit, lv_color_hex(launcher::theme::kAccountAccent), 0);
    lv_obj_set_style_bg_color(view->submit, lv_color_hex(launcher::theme::kAccountAccentPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(view->submit, lv_color_hex(launcher::theme::kAccountFieldBorder), LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(view->submit, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->submit, 0, 0);
    lv_obj_set_style_radius(view->submit, 12, 0);
    view->submit_label = label(view->submit, "account_submit_label", locked ? "Unlock" : view->first_use ? (view->setup_continuation ? "Continue" : "Finish setup") :
                                                                                   view->create_mode ? "Create account" : "Sign in",
                               view->font_22, lv_color_hex(launcher::theme::kIconForeground));
    lv_obj_center(view->submit_label);
    lv_obj_add_event_cb(view->submit, submit_clicked, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(view->submit, dismiss_keyboard_event, LV_EVENT_CLICKED, view);

    view->mode_button = lv_button_create(view->card);
    lv_obj_set_name_static(view->mode_button, "account_mode_toggle");
    lv_obj_set_width(view->mode_button, LV_PCT(100));
    lv_obj_set_height(view->mode_button, 42);
    lv_obj_set_style_min_height(view->mode_button, 42, 0);
    lv_obj_set_style_bg_opa(view->mode_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(view->mode_button, lv_color_hex(launcher::theme::kAccountFieldBackground), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(view->mode_button, 0, 0);
    lv_obj_set_style_radius(view->mode_button, 10, 0);
    view->mode_label = label(view->mode_button, "account_mode_label",
                             view->create_mode ? "Use existing account" : "Create first account", view->font_14,
                             lv_color_hex(launcher::theme::kAccountAccent));
    lv_obj_center(view->mode_label);
    lv_obj_add_event_cb(view->mode_button, mode_clicked, LV_EVENT_CLICKED, view);
    lv_obj_add_event_cb(view->mode_button, dismiss_keyboard_event, LV_EVENT_CLICKED, view);
    if (!setup_available || view->first_use) lv_obj_add_flag(view->mode_button, LV_OBJ_FLAG_HIDDEN);

    view->keyboard = lv_keyboard_create(view->modal);
    lv_obj_set_name_static(view->keyboard, "account_keyboard");
    lv_keyboard_set_popovers(view->keyboard, false);
    lv_keyboard_set_textarea(view->keyboard, locked ? view->password : view->username);
    lv_obj_set_style_text_font(view->keyboard, view->font_22, 0);
    lv_obj_set_style_bg_color(view->keyboard, lv_color_hex(launcher::theme::kAccountKeyboard), 0);
    lv_obj_set_style_bg_opa(view->keyboard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->keyboard, 0, 0);
    lv_obj_set_style_pad_all(view->keyboard, 8, 0);
    lv_obj_set_style_pad_row(view->keyboard, 6, 0);
    lv_obj_set_style_pad_column(view->keyboard, 6, 0);
    lv_obj_set_style_bg_color(view->keyboard, lv_color_hex(launcher::theme::kContainerBackground), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(view->keyboard, lv_color_hex(launcher::theme::kControlPressed), LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_radius(view->keyboard, 8, LV_PART_ITEMS);
    lv_obj_set_style_text_color(view->keyboard, lv_color_hex(launcher::theme::kText), LV_PART_ITEMS);
    lv_obj_add_event_cb(view->modal, [](lv_event_t *event) {
        relayout(static_cast<launcher_account_view_t *>(lv_event_get_user_data(event)));
    }, LV_EVENT_SIZE_CHANGED, view);
    relayout(view);
    lv_obj_move_foreground(view->modal);
    /* Background launcher rebuilds must continue to use their own group. */
    lv_group_set_default(view->previous_group);
    capture_input(view);
    lv_group_focus_obj(locked ? view->password : view->username);
    return view;
}

extern "C" launcher_account_view_t *launcher_account_view_create(bool setup_available, const char *last_user,
                                                                  bool start_setup)
{
    return create_account_view(setup_available, last_user, start_setup, false, false);
}

extern "C" launcher_account_view_t *launcher_account_view_create_setup(const char *username)
{
    return create_account_view(true, username, true, false, true);
}

extern "C" launcher_account_view_t *launcher_account_view_create_locked(const char *username)
{
    return create_account_view(false, username, false, true, false);
}

extern "C" void launcher_account_view_capture_input(launcher_account_view_t *view)
{
    if (view && view->group) capture_input(view);
}

extern "C" void launcher_account_view_set_hardware_keyboard_present(launcher_account_view_t *view, bool present)
{
    if (!view || view->hardware_keyboard_present == present) return;
    view->hardware_keyboard_present = present;
    view->software_keyboard_visible = !present;
    relayout(view);
    if (!present) set_keyboard_target(view, view->locked ? view->password : view->username);
}

extern "C" void launcher_account_view_destroy(launcher_account_view_t *view)
{
    if (!view) return;
    clear_sensitive(view->pending_password, sizeof(view->pending_password));
    if (view->modal && lv_obj_is_valid(view->modal)) lv_obj_delete(view->modal);
    restore_input(view);
    delete view;
}

extern "C" bool launcher_account_view_take_request(launcher_account_view_t *view, char *username,
                                                    size_t username_capacity, char *password,
                                                    size_t password_capacity, bool *create)
{
    if (!view || !view->pending || !username || !password || !create || username_capacity <= std::strlen(view->pending_username) ||
        password_capacity <= std::strlen(view->pending_password))
        return false;
    std::strcpy(username, view->pending_username);
    std::strcpy(password, view->pending_password);
    *create = view->first_use || view->create_mode;
    clear_sensitive(view->pending_password, sizeof(view->pending_password));
    clear_sensitive(view->pending_username, sizeof(view->pending_username));
    view->pending = false;
    return true;
}

extern "C" void launcher_account_view_set_busy(launcher_account_view_t *view, bool busy)
{
    if (!view) return;
    view->busy = busy;
    const lv_state_t state = LV_STATE_DISABLED;
    for (lv_obj_t *control : {view->password, view->confirm, view->submit, view->mode_button, view->keyboard}) {
        if (busy) lv_obj_add_state(control, state);
        else lv_obj_remove_state(control, state);
    }
    if (busy || view->locked) lv_obj_add_state(view->username, state);
    else lv_obj_remove_state(view->username, state);
    lv_label_set_text(view->submit_label, busy ? (view->locked ? "Unlocking..." : view->create_mode ? "Setting up..." : "Signing in...") :
        view->locked ? "Unlock" : view->first_use ? (view->setup_continuation ? "Continue" : "Finish setup") : view->create_mode ? "Create account" : "Sign in");
}

extern "C" void launcher_account_view_set_error(launcher_account_view_t *view, const char *message)
{
    if (!view || !view->error) return;
    lv_label_set_text(view->error, message ? message : "");
}

extern "C" void launcher_account_view_set_setup_username(launcher_account_view_t *view, const char *username)
{
    if (view && view->first_use && username && username[0]) lv_textarea_set_text(view->username, username);
}

extern "C" void launcher_account_view_clear_passwords(launcher_account_view_t *view)
{
    if (!view) return;
    if (view->password) lv_textarea_set_text(view->password, "");
    if (view->confirm) lv_textarea_set_text(view->confirm, "");
}

extern "C" void launcher_account_view_set_setup_continuation(launcher_account_view_t *view, bool continuation)
{
    if (!view || !view->first_use) return;
    view->setup_continuation = continuation;
    lv_label_set_text(view->subtitle, subtitle_text(view));
    launcher_account_view_set_busy(view, view->busy);
}
