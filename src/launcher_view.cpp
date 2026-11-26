#include "launcher_view.h"

#include "launcher_theme.h"

#include <cm0/status_bar.h>
#include <cm0/typography.h>

#include <algorithm>
#include <stdio.h>
#include <string.h>

namespace {

constexpr int32_t kAppIconSize       = 84;
constexpr int32_t kAppLabelHeight    = 30;
constexpr int32_t kAppFocusWidth     = 3;
constexpr int32_t kControlFocusWidth = 3;

/* Font Awesome glyphs supplied by the AppKit symbol face.  LVGL's built-in
 * symbol list does not include lock or sign-out, so keep these local to the
 * launcher quick controls rather than adding public symbol aliases. */
constexpr char kLockSymbol[]    = "\xEF\x80\xA3"; // U+F023, lock
constexpr char kSignOutSymbol[] = "\xEF\x8B\xB5"; // U+F2F5, sign-out-alt
constexpr char kShareSymbol[]   = "\xEF\x87\xA0"; // U+F1E0, share-alt
constexpr char kRestartSymbol[] = "\xEF\x8B\xB1"; // U+F2F1, sync-alt

/* The GLES and NanoVG draw units do not consume LV_DRAW_TASK_TYPE_BLUR.  In
 * those modes a backdrop blur task would be handed to the software unit
 * against the renderer's dummy draw buffer, so keep the renderer-independent
 * translucent glass treatment and enable the native blur only for software. */
#ifndef LILYGO_UI_USE_OPENGLES
#define LILYGO_UI_USE_OPENGLES 0
#endif
#ifndef LILYGO_UI_USE_NANOVG
#define LILYGO_UI_USE_NANOVG 0
#endif

void make_clear(lv_obj_t *object)
{
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
}

void style_quick_control(lv_obj_t *button)
{
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(button, lv_color_hex(launcher::theme::kControlBackground), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(button, lv_color_hex(launcher::theme::kText), 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_radius(button, 24, 0);
    const int32_t focus_width = lv_display_dpx(lv_obj_get_display(button), kControlFocusWidth);
    lv_obj_set_style_outline_width(button, focus_width, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(button, focus_width, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(button, lv_color_hex(launcher::theme::kControlFocus), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(button, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(button, lv_color_hex(launcher::theme::kControlPressed), LV_STATE_PRESSED);
}

void make_decoration(lv_obj_t *object)
{
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

void control_group_ext_draw_size(lv_event_t *event)
{
    lv_obj_t *group = lv_event_get_target_obj(event);
    // OVERFLOW_VISIBLE extends clipping only to the group's own draw area.
    // Include the child focus outline so edge cards retain all four sides.
    lv_event_set_ext_draw_size(event, 2 * lv_display_dpx(lv_obj_get_display(group), kControlFocusWidth));
}

lv_obj_t *create_control_group(lv_obj_t *parent, const char *name)
{
    lv_obj_t *group = lv_obj_create(parent);
    lv_obj_set_name_static(group, name);
    make_clear(group);
    make_decoration(group);
    lv_obj_add_flag(group, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_add_event_cb(group, control_group_ext_draw_size, LV_EVENT_REFR_EXT_DRAW_SIZE, nullptr);
    lv_obj_refresh_ext_draw_size(group);
    lv_obj_set_flex_flow(group, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(group, 12, 0);
    lv_obj_set_style_pad_row(group, 12, 0);
    return group;
}

void create_sun(lv_obj_t *slider)
{
    // Draw the sun with primitives: no dependency on a private icon glyph.
    lv_obj_t *sun = lv_obj_create(slider);
    make_clear(sun);
    make_decoration(sun);
    lv_obj_set_size(sun, 30, 30);
    lv_obj_align(sun, LV_ALIGN_BOTTOM_MID, 0, -19);
    lv_obj_t *center = lv_obj_create(sun);
    make_clear(center);
    make_decoration(center);
    lv_obj_set_size(center, 14, 14);
    lv_obj_center(center);
    lv_obj_set_style_radius(center, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(center, 2, 0);
    lv_obj_set_style_border_color(center, lv_color_hex(launcher::theme::kControlIcon), 0);
    static const lv_point_precise_t rays[8][2] = {
        {{15, 0}, {15, 4}}, {{23, 7}, {26, 4}}, {{26, 15}, {30, 15}}, {{23, 23}, {26, 26}},
        {{15, 26}, {15, 30}}, {{4, 26}, {7, 23}}, {{0, 15}, {4, 15}}, {{4, 4}, {7, 7}}
    };
    for (const auto &ray : rays) {
        lv_obj_t *line = lv_line_create(sun);
        make_decoration(line);
        lv_line_set_points(line, ray, 2);
        lv_obj_set_style_line_color(line, lv_color_hex(launcher::theme::kControlIcon), 0);
        lv_obj_set_style_line_width(line, 2, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
    }
}

void create_level_card(lv_obj_t *parent, const char *card_name, const char *slider_name,
                       const char *value_name, const char *title, bool brightness,
                       const lv_font_t *font_22, const lv_font_t *font_28,
                       lv_obj_t **slider_out, lv_obj_t **label_out)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_name_static(card, card_name);
    make_clear(card);
    make_decoration(card);
    lv_obj_set_size(card, 220, 264);
    lv_obj_set_style_bg_color(card, lv_color_hex(launcher::theme::kControlBackground), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 24, 0);

    lv_obj_t *slider = lv_slider_create(card);
    lv_obj_set_name_static(slider, slider_name);
    lv_obj_set_size(slider, 132, 176);
    lv_obj_align(slider, LV_ALIGN_TOP_MID, 0, 18);
    lv_slider_set_range(slider, brightness ? 1 : 0, 100);
    lv_slider_set_value(slider, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_remove_flag(slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_pad_all(slider, 0, 0);
    lv_obj_set_style_radius(slider, 28, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(slider, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(launcher::theme::kControlTrack), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(slider, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0xffffff), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_INDICATOR);
    lv_obj_set_style_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
    const int32_t focus_width = lv_display_dpx(lv_obj_get_display(slider), kControlFocusWidth);
    lv_obj_set_style_outline_width(slider, focus_width, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(slider, focus_width, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(slider, lv_color_hex(launcher::theme::kControlFocus), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(slider, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    if (brightness) {
        create_sun(slider);
    } else {
        lv_obj_t *icon = lv_label_create(slider);
        make_decoration(icon);
        lv_label_set_text(icon, LV_SYMBOL_VOLUME_MAX);
        lv_obj_set_style_text_font(icon, font_28, 0);
        lv_obj_set_style_text_color(icon, lv_color_hex(launcher::theme::kControlIcon), 0);
        lv_obj_align(icon, LV_ALIGN_BOTTOM_MID, 0, -18);
    }
    lv_obj_t *name = lv_label_create(card);
    make_decoration(name);
    lv_label_set_text(name, title);
    lv_obj_set_style_text_font(name, font_22, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(launcher::theme::kText), 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -37);

    lv_obj_t *value = lv_label_create(card);
    lv_obj_set_name_static(value, value_name);
    make_decoration(value);
    lv_label_set_text(value, "--");
    lv_obj_set_style_text_font(value, lilygo_ui_font_get(14), 0);
    lv_obj_set_style_text_color(value, lv_color_hex(launcher::theme::kMutedText), 0);
    lv_obj_align(value, LV_ALIGN_BOTTOM_MID, 0, -14);
    *slider_out = slider;
    *label_out = value;
}

lv_obj_t *create_main_layout(lv_obj_t *screen)
{
    lv_obj_t *layout = lv_obj_create(screen);
    lv_obj_set_name_static(layout, "main_layout");
    lv_obj_set_size(layout, LV_PCT(100), LV_PCT(100));
    lv_obj_center(layout);
    lv_obj_remove_flag(layout, LV_OBJ_FLAG_SCROLLABLE);
    make_clear(layout);
    lv_obj_set_style_pad_row(layout, 0, 0);
    lv_obj_set_style_pad_column(layout, 0, 0);
    lv_obj_set_flex_flow(layout, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(layout, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return layout;
}

lv_obj_t *create_status_bar(lv_obj_t *layout)
{
    lv_obj_t *slot = lv_obj_create(layout);
    lv_obj_set_name_static(slot, "system_status_bar_slot");
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, LV_PCT(100), CM0_STATUS_BAR_HEIGHT);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    return cm0_status_bar_create(slot);
}

lv_obj_t *create_app_grid(lv_obj_t *layout)
{
    lv_obj_t *grid = lv_obj_create(layout);
    lv_obj_set_name_static(grid, "app_grid");
    lv_obj_set_size(grid, LV_PCT(100), 1);
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    make_clear(grid);
    return grid;
}

lv_obj_t *create_page_indicator(lv_obj_t *layout)
{
    lv_obj_t *indicator = lv_obj_create(layout);
    lv_obj_set_name_static(indicator, "page_indicator");
    lv_obj_set_size(indicator, LV_SIZE_CONTENT, 40);
    lv_obj_set_flex_flow(indicator, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
    make_clear(indicator);
    lv_obj_set_style_pad_column(indicator, 0, 0);
    return indicator;
}

lv_obj_t *create_page_bottom_inset(lv_obj_t *layout)
{
    lv_obj_t *inset = lv_obj_create(layout);
    lv_obj_set_name_static(inset, "page_bottom_inset");
    lv_obj_set_size(inset, 1, 52);
    lv_obj_remove_flag(inset, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(inset, LV_OBJ_FLAG_SCROLLABLE);
    make_clear(inset);
    return inset;
}

lv_obj_t *create_home_indicator(lv_obj_t *screen)
{
    lv_obj_t *slot = lv_obj_create(screen);
    lv_obj_set_name_static(slot, "home_indicator_slot");
    lv_obj_remove_style_all(slot);
    lv_obj_set_size(slot, LV_PCT(100), 38);
    lv_obj_align(slot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(slot, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    cm0_home_indicator_create(slot);
    return slot;
}

lv_obj_t *create_quick_scrim(lv_obj_t *screen)
{
    lv_obj_t *scrim = lv_obj_create(screen);
    lv_obj_set_name_static(scrim, "quick_scrim");
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_center(scrim);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(launcher::theme::kScrim), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_10, 0);
    lv_obj_set_style_opa(scrim, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    return scrim;
}

lv_obj_t *create_quick_panel(launcher_view_t *view)
{
    lv_obj_t *screen = view->screen;
    const lv_font_t *font_22 = view->font_22;
    const lv_font_t *font_28 = view->font_28;
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_name_static(panel, "quick_panel");
    lv_obj_set_size(panel, LV_PCT(100), 260);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, -260);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(launcher::theme::kContainerBackground), 0);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
#if !LILYGO_UI_USE_OPENGLES && !LILYGO_UI_USE_NANOVG
    lv_obj_set_style_blur_backdrop(panel, true, 0);
    lv_obj_set_style_blur_radius(panel, 16, 0);
    lv_obj_set_style_blur_quality(panel, LV_BLUR_QUALITY_SPEED, 0);
#endif
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_top(panel, 24, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_set_style_pad_left(panel, 28, 0);
    lv_obj_set_style_pad_right(panel, 28, 0);
    lv_obj_set_style_pad_row(panel, 16, 0);
    lv_obj_set_style_radius(panel, 32, 0);
    lv_obj_set_style_shadow_width(panel, 18, 0);
    lv_obj_set_style_shadow_color(panel, lv_color_hex(launcher::theme::kPanelShadow), 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_20, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *title = lv_label_create(panel);
    lv_obj_set_name_static(title, "quick_panel_title");
    lv_label_set_text(title, "Quick controls");
    lv_obj_set_style_text_font(title, font_22, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(launcher::theme::kContainerTitle), 0);

    lv_obj_t *controls = lv_obj_create(panel);
    lv_obj_set_name_static(controls, "quick_controls");
    make_clear(controls);
    // Keep a full focus ring clear of the scroll viewport on every edge.
    // LVGL scrolls the card bounds, while the ring extends by its outline
    // width and pad beyond those bounds.
    lv_obj_set_style_pad_all(controls, 2 * lv_display_dpx(lv_obj_get_display(controls), kControlFocusWidth), 0);
    lv_obj_set_width(controls, LV_PCT(100));
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(controls, 12, 0);
    lv_obj_set_style_pad_row(controls, 12, 0);
    lv_obj_set_scroll_dir(controls, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(controls, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *actions = create_control_group(controls, "quick_actions");
    lv_obj_t *button = lv_button_create(actions);
    lv_obj_set_name_static(button, "orientation_button");
    lv_obj_set_size(button, 220, 104);
    style_quick_control(button);
    lv_obj_set_style_pad_top(button, 0, 0);
    lv_obj_set_style_pad_bottom(button, 0, 0);
    lv_obj_set_style_pad_left(button, 24, 0);
    lv_obj_set_style_pad_right(button, 0, 0);
    lv_obj_set_style_pad_column(button, 26, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *icon = lv_label_create(button);
    make_decoration(icon);
    lv_label_set_text(icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(icon, lv_color_hex(launcher::theme::kControlAccent), 0);
    lv_obj_set_style_text_font(icon, font_28, 0);

    lv_obj_t *label = lv_label_create(button);
    make_decoration(label);
    lv_obj_set_name_static(label, "orientation_label");
    lv_label_set_text(label, "Landscape");
    lv_obj_set_style_text_font(label, font_22, 0);

    lv_obj_t *file_share = lv_button_create(actions);
    lv_obj_set_name_static(file_share, "file_share_button");
    lv_obj_set_size(file_share, 220, 104);
    lv_obj_add_flag(file_share, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_min_width(file_share, 0, 0);
    style_quick_control(file_share);
    lv_obj_set_style_pad_all(file_share, 0, 0);
    lv_obj_set_style_pad_row(file_share, 6, 0);
    lv_obj_set_flex_flow(file_share, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(file_share, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *file_share_icon = lv_label_create(file_share);
    make_decoration(file_share_icon);
    lv_obj_set_name_static(file_share_icon, "file_share_icon");
    lv_label_set_text(file_share_icon, kShareSymbol);
    lv_obj_set_style_text_font(file_share_icon, font_28, 0);
    lv_obj_set_style_text_color(file_share_icon, lv_color_hex(launcher::theme::kControlAccent), 0);
    lv_obj_t *file_share_text = lv_label_create(file_share);
    make_decoration(file_share_text);
    lv_obj_set_name_static(file_share_text, "file_share_label");
    lv_label_set_text(file_share_text, "File sharing");
    lv_obj_set_width(file_share_text, LV_PCT(100));
    lv_obj_set_height(file_share_text, lv_font_get_line_height(font_22));
    lv_label_set_long_mode(file_share_text, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(file_share_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(file_share_text, font_22, 0);

    lv_obj_t *lock = lv_button_create(actions);
    lv_obj_set_name_static(lock, "lock_button");
    lv_obj_set_size(lock, 220, 104);
    lv_obj_add_flag(lock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_min_width(lock, 0, 0);
    style_quick_control(lock);
    lv_obj_set_style_pad_all(lock, 0, 0);
    lv_obj_set_style_pad_row(lock, 6, 0);
    lv_obj_set_flex_flow(lock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(lock, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *lock_icon = lv_label_create(lock);
    make_decoration(lock_icon);
    lv_obj_set_name_static(lock_icon, "lock_icon");
    lv_label_set_text(lock_icon, kLockSymbol);
    lv_obj_set_style_text_font(lock_icon, font_28, 0);
    lv_obj_set_style_text_color(lock_icon, lv_color_hex(launcher::theme::kControlIcon), 0);
    lv_obj_t *lock_text = lv_label_create(lock);
    make_decoration(lock_text);
    lv_obj_set_name_static(lock_text, "lock_label");
    lv_label_set_text(lock_text, "Lock screen");
    lv_obj_set_width(lock_text, LV_PCT(100));
    lv_obj_set_height(lock_text, lv_font_get_line_height(font_22));
    lv_label_set_long_mode(lock_text, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(lock_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(lock_text, font_22, 0);

    lv_obj_t *session = lv_button_create(actions);
    lv_obj_set_name_static(session, "session_button");
    lv_obj_set_size(session, 220, 104);
    lv_obj_add_flag(session, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_min_width(session, 0, 0);
    style_quick_control(session);
    lv_obj_set_style_pad_all(session, 0, 0);
    lv_obj_set_style_pad_row(session, 6, 0);
    lv_obj_set_flex_flow(session, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(session, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *session_icon = lv_label_create(session);
    make_decoration(session_icon);
    lv_obj_set_name_static(session_icon, "session_icon");
    lv_label_set_text(session_icon, kSignOutSymbol);
    lv_obj_set_style_text_font(session_icon, font_28, 0);
    lv_obj_set_style_text_color(session_icon, lv_color_hex(launcher::theme::kControlIcon), 0);
    lv_obj_t *session_text = lv_label_create(session);
    make_decoration(session_text);
    lv_obj_set_name_static(session_text, "session_label");
    lv_label_set_text(session_text, "Sign out");
    lv_obj_set_width(session_text, LV_PCT(100));
    lv_obj_set_height(session_text, lv_font_get_line_height(font_22));
    lv_label_set_long_mode(session_text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(session_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(session_text, font_22, 0);

    lv_obj_t *power_group = create_control_group(controls, "quick_power");
    lv_obj_set_flex_flow(power_group, LV_FLEX_FLOW_ROW);
    lv_obj_t *power = lv_button_create(power_group);
    lv_obj_set_name_static(power, "power_button");
    lv_obj_set_size(power, 220, 104);
    style_quick_control(power);
    lv_obj_set_style_pad_all(power, 0, 0);
    lv_obj_set_style_pad_column(power, 14, 0);
    lv_obj_set_flex_flow(power, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(power, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *power_icon = lv_label_create(power);
    make_decoration(power_icon);
    lv_label_set_text(power_icon, LV_SYMBOL_POWER);
    lv_obj_set_style_text_font(power_icon, font_28, 0);
    lv_obj_set_style_text_color(power_icon, lv_color_hex(launcher::theme::kDestructive), 0);
    lv_obj_t *power_label = lv_label_create(power);
    make_decoration(power_label);
    lv_label_set_text(power_label, "Power off");
    lv_obj_set_style_text_font(power_label, font_22, 0);

    lv_obj_t *reboot = lv_button_create(power_group);
    lv_obj_set_name_static(reboot, "reboot_button");
    lv_obj_set_size(reboot, 220, 104);
    style_quick_control(reboot);
    lv_obj_set_style_pad_all(reboot, 0, 0);
    lv_obj_set_style_pad_column(reboot, 14, 0);
    lv_obj_set_flex_flow(reboot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(reboot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *reboot_icon = lv_label_create(reboot);
    make_decoration(reboot_icon);
    lv_label_set_text(reboot_icon, kRestartSymbol);
    lv_obj_set_style_text_font(reboot_icon, font_28, 0);
    lv_obj_set_style_text_color(reboot_icon, lv_color_hex(launcher::theme::kDestructive), 0);
    lv_obj_t *reboot_label = lv_label_create(reboot);
    make_decoration(reboot_label);
    lv_label_set_text(reboot_label, "Restart");
    lv_obj_set_style_text_font(reboot_label, font_22, 0);

    lv_obj_t *levels = create_control_group(controls, "quick_levels");
    create_level_card(levels, "brightness_card", "brightness_slider", "brightness_label", "Brightness", true,
                      font_22, font_28, &view->brightness_slider, &view->brightness_label);
    create_level_card(levels, "volume_card", "volume_slider", "volume_label", "Volume", false,
                      font_22, font_28, &view->volume_slider, &view->volume_label);
    launcher_view_set_control_level(view->brightness_slider, view->brightness_label, false, 0);
    launcher_view_set_control_level(view->volume_slider, view->volume_label, false, 0);

    lv_obj_t *footer = lv_obj_create(panel);
    make_clear(footer);
    lv_obj_set_size(footer, LV_PCT(100), 6);
    lv_obj_set_flex_grow(footer, 1);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *handle = lv_obj_create(footer);
    lv_obj_set_name_static(handle, "quick_panel_handle");
    lv_obj_set_size(handle, 72, 6);
    lv_obj_remove_flag(handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(handle, 6, 0);
    lv_obj_set_style_bg_color(handle, lv_color_hex(launcher::theme::kIndicatorInactive), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(handle, 0, 0);

    view->orientation_button = button;
    view->orientation_label  = label;
    view->quick_controls = controls;
    view->file_share_button = file_share;
    view->file_share_label = file_share_text;
    view->lock_button = lock;
    view->session_button = session;
    view->session_label = session_text;
    view->power_button = power;
    view->reboot_button = reboot;
    return panel;
}

void layout_power_confirmation(lv_obj_t *scrim)
{
    const int32_t width = lv_obj_get_width(scrim);
    const int32_t height = lv_obj_get_height(scrim);
    const bool compact = height < 320 || width < 400;
    lv_obj_t *card = lv_obj_get_child(scrim, 0);
    if (!card) return;
    const int32_t card_width = std::min(440, std::max(1, width - (compact ? 24 : 64)));
    lv_obj_set_size(card, card_width, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, std::max(1, height - 16), 0);
    lv_obj_set_style_pad_all(card, compact ? 16 : 28, 0);
    lv_obj_set_style_pad_row(card, compact ? 8 : 16, 0);
    lv_obj_t *title = lv_obj_get_child(card, 0);
    lv_obj_t *hint = lv_obj_get_child(card, 1);
    lv_obj_t *buttons = lv_obj_get_child(card, 2);
    lv_obj_set_style_text_font(title, lilygo_ui_font_get(compact ? 22 : 28), 0);
    lv_obj_set_style_text_font(hint, lilygo_ui_font_get(compact ? 14 : 22), 0);
    const int32_t button_gap = compact ? 10 : 12;
    const int32_t button_width = (card_width - (compact ? 32 : 56) - button_gap) / 2;
    lv_obj_set_style_pad_column(buttons, button_gap, 0);
    lv_obj_set_height(buttons, compact ? 48 : 58);
    for (uint32_t i = 0; i < 2; ++i) {
        lv_obj_t *button = lv_obj_get_child(buttons, i);
        lv_obj_set_size(button, button_width, compact ? 48 : 58);
        lv_obj_set_style_radius(button, compact ? 16 : 20, 0);
        lv_obj_set_style_text_font(button, lilygo_ui_font_get(compact ? 14 : 22), 0);
    }
    lv_obj_center(card);
}

void power_confirmation_resized(lv_event_t *event)
{
    layout_power_confirmation(static_cast<lv_obj_t *>(lv_event_get_target(event)));
}

lv_obj_t *create_power_confirmation(launcher_view_t *view)
{
    lv_obj_t *scrim = lv_obj_create(view->screen);
    lv_obj_set_name_static(scrim, "power_confirm_scrim");
    make_clear(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_center(scrim);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(launcher::theme::kScrim), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);

    lv_obj_t *card = lv_obj_create(scrim);
    lv_obj_set_name_static(card, "power_confirm_panel");
    make_clear(card);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(card, lv_color_hex(launcher::theme::kContainerBackground), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_name_static(title, "power_confirm_title");
    lv_label_set_text(title, "Power off?");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(launcher::theme::kText), 0);
    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_name_static(hint, "power_confirm_hint");
    lv_label_set_text(hint, "This will turn off your device.");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(launcher::theme::kMutedText), 0);

    lv_obj_t *buttons = create_control_group(card, "power_confirm_actions");
    lv_obj_set_width(buttons, LV_PCT(100));
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_t *cancel = lv_button_create(buttons);
    lv_obj_set_name_static(cancel, "power_cancel_button");
    style_quick_control(cancel);
    lv_obj_set_style_pad_all(cancel, 0, 0);
    lv_obj_t *cancel_text = lv_label_create(cancel);
    make_decoration(cancel_text);
    lv_label_set_text(cancel_text, "Cancel");
    lv_obj_center(cancel_text);

    lv_obj_t *confirm = lv_button_create(buttons);
    lv_obj_set_name_static(confirm, "power_confirm_button");
    style_quick_control(confirm);
    lv_obj_set_style_pad_all(confirm, 0, 0);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(launcher::theme::kDestructive), 0);
    lv_obj_set_style_bg_color(confirm, lv_color_hex(0xd92d23), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(confirm, lv_color_hex(0xffffff), 0);
    lv_obj_t *confirm_text = lv_label_create(confirm);
    make_decoration(confirm_text);
    lv_obj_set_name_static(confirm_text, "power_confirm_label");
    lv_label_set_text(confirm_text, "Power off");
    lv_obj_center(confirm_text);

    view->power_confirm_title = title;
    view->power_confirm_hint = hint;
    view->power_cancel_button = cancel;
    view->power_confirm_button = confirm;
    view->power_confirm_label = confirm_text;
    lv_obj_add_event_cb(scrim, power_confirmation_resized, LV_EVENT_SIZE_CHANGED, nullptr);
    return scrim;
}

lv_obj_t *create_file_share_overlay(launcher_view_t *view)
{
    lv_obj_t *scrim = lv_obj_create(view->screen);
    lv_obj_set_name_static(scrim, "file_share_scrim");
    make_clear(scrim);
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_center(scrim);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(launcher::theme::kScrim), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_50, 0);

    lv_obj_t *card = lv_obj_create(scrim);
    lv_obj_set_name_static(card, "file_share_panel");
    make_clear(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(launcher::theme::kContainerBackground), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 28, 0);
    lv_obj_set_style_shadow_width(card, 18, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(launcher::theme::kPanelShadow), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(card, 28, 0);
    lv_obj_set_style_pad_row(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_name_static(title, "file_share_title");
    lv_label_set_text(title, "File sharing");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_font(title, view->font_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(launcher::theme::kContainerTitle), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *path_caption = lv_label_create(card);
    lv_obj_set_name_static(path_caption, "file_share_path_caption");
    lv_label_set_text(path_caption, "Shared folder");
    lv_obj_set_width(path_caption, LV_PCT(100));
    lv_obj_set_style_text_font(path_caption, lilygo_ui_font_get(14), 0);
    lv_obj_set_style_text_color(path_caption, lv_color_hex(launcher::theme::kMutedText), 0);
    lv_obj_set_style_text_align(path_caption, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *path = lv_label_create(card);
    lv_obj_set_name_static(path, "file_share_path");
    lv_label_set_text(path, "$HOME/Downloads");
    lv_obj_set_width(path, LV_PCT(100));
    lv_label_set_long_mode(path, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(path, view->font_22, 0);
    lv_obj_set_style_text_color(path, lv_color_hex(launcher::theme::kText), 0);
    lv_obj_set_style_text_align(path, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *status = lv_label_create(card);
    lv_obj_set_name_static(status, "file_share_status");
    lv_label_set_text(status, "Ready to receive files from your browser.");
    lv_obj_set_width(status, LV_PCT(100));
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(status, view->font_22, 0);
    lv_obj_set_style_text_color(status, lv_color_hex(launcher::theme::kMutedText), 0);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *approve = lv_button_create(card);
    lv_obj_set_name_static(approve, "file_share_approve_button");
    style_quick_control(approve);
    lv_obj_set_style_pad_all(approve, 0, 0);
    lv_obj_set_size(approve, 220, 54);
    lv_obj_t *approve_text = lv_label_create(approve);
    make_decoration(approve_text);
    lv_label_set_text(approve_text, "Receive file");
    lv_obj_set_style_text_font(approve_text, view->font_22, 0);
    lv_obj_center(approve_text);
    lv_obj_add_flag(approve, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *reject = lv_button_create(card);
    lv_obj_set_name_static(reject, "file_share_reject_button");
    style_quick_control(reject);
    lv_obj_set_style_pad_all(reject, 0, 0);
    lv_obj_set_size(reject, 220, 54);
    lv_obj_t *reject_text = lv_label_create(reject);
    make_decoration(reject_text);
    lv_label_set_text(reject_text, "Decline");
    lv_obj_set_style_text_font(reject_text, view->font_22, 0);
    lv_obj_center(reject_text);
    lv_obj_add_flag(reject, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *close = lv_button_create(card);
    lv_obj_set_name_static(close, "file_share_close_button");
    style_quick_control(close);
    lv_obj_set_style_pad_all(close, 0, 0);
    lv_obj_set_size(close, 220, 58);
    lv_obj_t *close_text = lv_label_create(close);
    make_decoration(close_text);
    lv_label_set_text(close_text, "Close");
    lv_obj_set_style_text_font(close_text, view->font_22, 0);
    lv_obj_center(close_text);

    view->file_share_scrim = scrim;
    view->file_share_panel = card;
    view->file_share_path = path;
    view->file_share_status = status;
    view->file_share_approve_button = approve;
    view->file_share_reject_button = reject;
    view->file_share_close_button = close;
    return scrim;
}

void layout_file_share_overlay(launcher_view_t *view, int32_t width, int32_t height)
{
    if (!view || !view->file_share_scrim || !view->file_share_panel || width <= 0 || height <= 0) return;
    const bool compact = width < 400 || height < 320;
    const int32_t card_width = std::min(440, std::max(1, width - (compact ? 24 : 64)));
    const int32_t card_height = std::min(height - 16, compact ? 320 : 420);
    lv_obj_set_size(view->file_share_panel, card_width, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(view->file_share_panel, std::max(1, card_height), 0);
    lv_obj_set_style_pad_all(view->file_share_panel, compact ? 12 : 28, 0);
    lv_obj_set_style_pad_row(view->file_share_panel, compact ? 6 : 12, 0);
    lv_obj_t *title = lv_obj_find_by_name(view->file_share_panel, "file_share_title");
    lv_obj_t *caption = lv_obj_find_by_name(view->file_share_panel, "file_share_path_caption");
    if (title) lv_obj_set_style_text_font(title, compact ? view->font_22 : view->font_28, 0);
    if (caption) {
        if (compact) lv_obj_add_flag(caption, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(caption, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_font(view->file_share_path, compact ? lilygo_ui_font_get(14) : view->font_22, 0);
    lv_obj_set_style_text_font(view->file_share_status, compact ? lilygo_ui_font_get(14) : view->font_22, 0);
    lv_obj_set_size(view->file_share_approve_button, compact ? 150 : 220, compact ? 42 : 54);
    lv_obj_set_size(view->file_share_reject_button, compact ? 150 : 220, compact ? 42 : 54);
    lv_obj_set_size(view->file_share_close_button, compact ? 180 : 220, compact ? 42 : 58);
    lv_obj_center(view->file_share_panel);
}

lv_obj_t *create_shutdown_overlay(lv_obj_t *screen, const lv_font_t *font_22, const lv_font_t *font_36,
                                   lv_obj_t **panel)
{
    lv_obj_t *scrim = lv_obj_create(screen);
    lv_obj_set_name_static(scrim, "shutdown_scrim");
    lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
    lv_obj_center(scrim);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_FLOATING);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scrim, lv_color_hex(launcher::theme::kScrim), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_60, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_pad_all(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);

    lv_obj_t *card = lv_obj_create(scrim);
    lv_obj_set_name_static(card, "shutdown_panel");
    lv_obj_set_size(card, LV_PCT(86), 250);
    lv_obj_center(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(launcher::theme::kContainerBackground), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_shadow_width(card, 18, 0);
    lv_obj_set_style_shadow_color(card, lv_color_hex(launcher::theme::kPanelShadow), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_pad_left(card, 24, 0);
    lv_obj_set_style_pad_right(card, 24, 0);
    lv_obj_set_style_pad_top(card, 32, 0);
    lv_obj_set_style_pad_bottom(card, 24, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, 18, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_name_static(title, "shutdown_title");
    lv_label_set_text(title, "正在关机");
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(launcher::theme::kContainerTitle), 0);
    lv_obj_set_style_text_font(title, font_36, 0);

    lv_obj_t *hint = lv_label_create(card);
    lv_obj_set_name_static(hint, "shutdown_hint");
    lv_label_set_text(hint, "请继续按住电源键");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(launcher::theme::kMutedText), 0);
    lv_obj_set_style_text_font(hint, font_22, 0);

    *panel = card;
    return scrim;
}

}  // namespace

bool launcher_view_create(launcher_view_t *view)
{
    if (!view) return false;
    memset(view, 0, sizeof(*view));

    view->font_22 = lilygo_ui_font_get(22);
    view->font_28 = lilygo_ui_font_get(28);
    view->font_36 = lilygo_ui_font_get(36);
    if (!view->font_22 || !view->font_28 || !view->font_36) {
        memset(view, 0, sizeof(*view));
        return false;
    }

    view->screen = lv_obj_create(nullptr);
    if (!view->screen) return false;
    lv_obj_set_name_static(view->screen, "launcher");
    lv_obj_remove_flag(view->screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(view->screen, lv_color_hex(launcher::theme::kPageBackground), 0);
    lv_obj_set_style_bg_grad_dir(view->screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(view->screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(view->screen, 0, 0);
    lv_obj_set_style_pad_all(view->screen, 0, 0);
    lv_obj_set_style_radius(view->screen, 36, 0);
    lv_obj_set_style_clip_corner(view->screen, true, 0);
    lv_obj_set_style_text_color(view->screen, lv_color_hex(launcher::theme::kText), 0);

    lv_obj_t *layout        = create_main_layout(view->screen);
    view->status_bar        = create_status_bar(layout);
    view->app_grid          = create_app_grid(layout);
    view->page_indicator    = create_page_indicator(layout);
    view->page_bottom_inset = create_page_bottom_inset(layout);
    view->home_indicator    = create_home_indicator(view->screen);
    view->brightness_overlay = lv_obj_create(view->screen);
    if (view->brightness_overlay) {
        lv_obj_set_name_static(view->brightness_overlay, "software_brightness_overlay");
        make_clear(view->brightness_overlay);
        lv_obj_set_size(view->brightness_overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_center(view->brightness_overlay);
        lv_obj_remove_flag(view->brightness_overlay, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(view->brightness_overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(view->brightness_overlay, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_style_bg_color(view->brightness_overlay, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(view->brightness_overlay, LV_OPA_TRANSP, 0);
    }
    view->quick_scrim       = create_quick_scrim(view->screen);
    view->quick_panel       = create_quick_panel(view);
    view->file_share_scrim  = create_file_share_overlay(view);
    view->power_confirm_scrim = create_power_confirmation(view);
    view->shutdown_scrim    = create_shutdown_overlay(view->screen, view->font_22, view->font_36, &view->shutdown_panel);

    return view->status_bar && view->app_grid && view->page_indicator && view->page_bottom_inset && view->quick_scrim &&
           view->quick_panel && view->orientation_button && view->orientation_label && view->quick_controls &&
           view->file_share_button && view->file_share_label &&
           view->file_share_scrim && view->file_share_panel && view->file_share_path && view->file_share_status &&
           view->file_share_approve_button && view->file_share_reject_button && view->file_share_close_button &&
           view->lock_button && view->session_button &&
           view->session_label && view->home_indicator && view->brightness_overlay && view->power_button &&
           view->reboot_button &&
           view->brightness_slider && view->brightness_label && view->volume_slider && view->volume_label &&
           view->power_confirm_scrim && view->power_confirm_button && view->power_cancel_button &&
           view->power_confirm_title && view->power_confirm_hint && view->power_confirm_label &&
           view->shutdown_scrim && view->shutdown_panel;
}

void launcher_view_destroy(launcher_view_t *view)
{
    if (!view) return;
    if (view->screen && lv_obj_is_valid(view->screen)) lv_obj_delete(view->screen);
    memset(view, 0, sizeof(*view));
}

void launcher_view_layout_app_tile(lv_obj_t *tile, int32_t width, int32_t height)
{
    if (!tile || width <= 0 || height <= 0) return;

    const int32_t top = height >= 140 ? 15 : 4;
    const int32_t gap = height >= 140 ? 10 : 2;
    int32_t icon_size = height - kAppLabelHeight - top - gap;
    if (icon_size > kAppIconSize) icon_size = kAppIconSize;
    if (icon_size < 40) icon_size = 40;

    lv_obj_set_size(tile, width, height);
    lv_obj_t *icon_box = lv_obj_get_child(tile, 0);
    lv_obj_t *name     = lv_obj_get_child(tile, 1);
    if (!icon_box || !name) return;

    lv_obj_set_size(icon_box, icon_size, icon_size);
    lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_style_radius(icon_box, icon_size / 4, 0);
    lv_obj_t *icon = lv_obj_get_child(icon_box, 0);
    if (icon && lv_obj_has_class(icon, &lv_image_class)) lv_obj_set_size(icon, LV_PCT(100), LV_PCT(100));

    lv_obj_set_size(name, width, kAppLabelHeight);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, top + icon_size + gap);
}

int32_t launcher_view_layout_quick_panel(launcher_view_t *view, int32_t width, int32_t height)
{
    if (!view || !view->quick_panel || !view->quick_controls || width <= 0 || height <= 0) return 0;

    const bool compact = width > height || height < 260;
    const int32_t pad_top = compact ? 12 : 24;
    const int32_t pad_bottom = compact ? 8 : 12;
    const int32_t row_gap = compact ? 8 : 16;
    constexpr int32_t card_gap = 12;
    const int32_t controls_pad_x = lv_obj_get_style_pad_left(view->quick_controls, LV_PART_MAIN) +
                                   lv_obj_get_style_pad_right(view->quick_controls, LV_PART_MAIN);
    const int32_t controls_pad_y = lv_obj_get_style_pad_top(view->quick_controls, LV_PART_MAIN) +
                                   lv_obj_get_style_pad_bottom(view->quick_controls, LV_PART_MAIN);
    const int32_t panel_pad_x = width < 400 ? 20 : 28;
    const int32_t available_width = std::max(1, width - 2 * panel_pad_x - controls_pad_x);
    const int32_t card_width = std::min(220, available_width);
    const int32_t columns = std::clamp((available_width + card_gap) / (card_width + card_gap), 1, 2);
    const int32_t count = 1 + (lv_obj_has_flag(view->file_share_button, LV_OBJ_FLAG_HIDDEN) ? 0 : 1) +
                          (lv_obj_has_flag(view->lock_button, LV_OBJ_FLAG_HIDDEN) ? 0 : 1) +
                          (lv_obj_has_flag(view->session_button, LV_OBJ_FLAG_HIDDEN) ? 0 : 1);
    const int32_t rows = (count + columns - 1) / columns;
    const int32_t chrome_height = pad_top + pad_bottom + lv_font_get_line_height(view->font_22) +
                                  2 * row_gap + 6;
    const int32_t available_height = std::max(1, height - chrome_height);
    const int32_t card_height = height < 260 ? 44 : (compact ? 84 : 104);
    const int32_t actions_width = columns * card_width + (columns - 1) * card_gap;
    const int32_t actions_height = rows * card_height + (rows - 1) * card_gap;
    // Keep the two power actions together, even when whole groups wrap.
    // On wide panels share the space after the action grid with the two sliders.
    const int32_t remaining = available_width - actions_width - 2 * card_gap;
    const int32_t pair_width = remaining >= 2 * (2 * 144 + card_gap)
                                   ? std::min(452, remaining / 2) : std::min(452, available_width);
    const int32_t level_width = (pair_width - card_gap) / 2;
    const int32_t levels_height = height < 260 ? 208 : (compact ? 232 : 264);
    int32_t controls_height = controls_pad_y, used = 0, row_height = 0;
    const int32_t groups[][2] = {{actions_width, actions_height}, {pair_width, card_height}, {pair_width, levels_height}};
    for (const auto &group : groups) {
        if (used && used + card_gap + group[0] > available_width) {
            controls_height += row_height + card_gap;
            used = row_height = 0;
        }
        used += (used ? card_gap : 0) + group[0];
        row_height = std::max(row_height, group[1]);
    }
    controls_height += row_height;
    const int32_t panel_height = std::min(height, std::max(compact ? 190 : 260,
                                                          chrome_height + controls_height));

    lv_obj_set_height(view->quick_panel, panel_height);
    lv_obj_set_style_pad_top(view->quick_panel, pad_top, 0);
    lv_obj_set_style_pad_bottom(view->quick_panel, pad_bottom, 0);
    lv_obj_set_style_pad_left(view->quick_panel, panel_pad_x, 0);
    lv_obj_set_style_pad_right(view->quick_panel, panel_pad_x, 0);
    lv_obj_set_style_pad_row(view->quick_panel, row_gap, 0);
    lv_obj_set_height(view->quick_controls, std::min(controls_height, available_height));
    lv_obj_scroll_to_y(view->quick_controls, 0, LV_ANIM_OFF);
    lv_obj_t *buttons[] = {view->orientation_button, view->file_share_button, view->lock_button, view->session_button};
    for (lv_obj_t *button : buttons) lv_obj_set_size(button, card_width, card_height);
    /* The smallest supported panel is only 44 px tall.  Keep the action
     * labels inside that compact card and restore the icon as soon as the
     * panel has enough room for the iOS-style icon-over-label hierarchy. */
    const bool show_action_icons = card_height >= 84;
    for (lv_obj_t *button : {view->file_share_button, view->lock_button, view->session_button}) {
        lv_obj_t *icon = lv_obj_get_child(button, 0);
        if (icon) lv_obj_set_flag(icon, LV_OBJ_FLAG_HIDDEN, !show_action_icons);
    }
    lv_obj_set_size(lv_obj_get_parent(view->orientation_button), actions_width, actions_height);
    lv_obj_t *power_group = lv_obj_get_parent(view->power_button);
    lv_obj_set_size(power_group, pair_width, card_height);
    for (lv_obj_t *button : {view->power_button, view->reboot_button}) {
        lv_obj_set_size(button, level_width, card_height);
        lv_obj_set_flag(lv_obj_get_child(button, 0), LV_OBJ_FLAG_HIDDEN, level_width < 144);
    }
    lv_obj_t *sliders[] = {view->brightness_slider, view->volume_slider};
    for (lv_obj_t *slider : sliders) {
        lv_obj_t *card = lv_obj_get_parent(slider);
        lv_obj_set_size(card, level_width, levels_height);
        lv_obj_set_size(slider, std::min(104, level_width - 36), levels_height - 88);
        lv_obj_t *title = lv_obj_get_child(card, 1);
        lv_obj_set_style_text_font(title, lilygo_ui_font_get(level_width < 150 ? 14 : 22), 0);
    }
    lv_obj_set_size(lv_obj_get_parent(lv_obj_get_parent(view->brightness_slider)), pair_width, levels_height);
    if (view->power_confirm_scrim) {
        lv_obj_update_layout(view->power_confirm_scrim);
        layout_power_confirmation(view->power_confirm_scrim);
    }
    layout_file_share_overlay(view, width, height);
    return panel_height;
}

void launcher_view_set_control_level(lv_obj_t *slider, lv_obj_t *label, bool available, int percent)
{
    if (!slider || !label) return;
    if (available) {
        lv_obj_remove_state(slider, LV_STATE_DISABLED);
        const int value = std::clamp(percent, static_cast<int>(lv_slider_get_min_value(slider)), 100);
        lv_slider_set_value(slider, value, LV_ANIM_OFF);
        char text[16];
        snprintf(text, sizeof(text), "%d%%", value);
        lv_label_set_text(label, text);
    } else {
        lv_obj_add_state(slider, LV_STATE_DISABLED);
        lv_slider_set_value(slider, 0, LV_ANIM_OFF);
        lv_label_set_text(label, "--");
    }
    lv_obj_set_style_opa(lv_obj_get_parent(slider), available ? LV_OPA_COVER : LV_OPA_60, 0);
}

void launcher_view_set_software_brightness(launcher_view_t *view, bool enabled, int percent)
{
    if (!view || !view->brightness_overlay) return;
    const int level = std::clamp(percent, 1, 100);
    const lv_opa_t opacity = enabled
        ? static_cast<lv_opa_t>(std::clamp(100 - level, 0, 100) * 230 / 100)
        : static_cast<lv_opa_t>(LV_OPA_TRANSP);
    lv_obj_set_style_bg_opa(view->brightness_overlay, opacity, 0);
    lv_obj_invalidate(view->brightness_overlay);
}

lv_obj_t *launcher_view_create_app_tile(const launcher_view_t *view, lv_obj_t *parent, const char *label,
                                        const char *icon_path, const char *fallback_icon, lv_color_t icon_background,
                                        int32_t width, int32_t height)
{
    if (!view || !view->font_22 || !view->font_28) return nullptr;
    lv_obj_t *tile = lv_button_create(parent);
    lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_set_style_radius(tile, 0, 0);
    /* Keep the keyboard focus frame inside the tile.  App tiles touch the
     * page edges, so the default positive outline padding is clipped by the
     * tileview viewport on the first/last row. */
    const int32_t focus_width = lv_display_dpx(lv_obj_get_display(tile), kAppFocusWidth);
    lv_obj_set_style_outline_width(tile, focus_width, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_pad(tile, -focus_width, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_color(tile, lv_color_hex(launcher::theme::kControlFocus), LV_STATE_FOCUS_KEY);
    lv_obj_set_style_outline_opa(tile, LV_OPA_COVER, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_bg_color(tile, lv_color_hex(launcher::theme::kTilePressedOverlay), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(tile, LV_OPA_20, LV_STATE_PRESSED);

    lv_obj_t *icon_box = lv_obj_create(tile);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(icon_box, icon_background, 0);
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_set_style_clip_corner(icon_box, true, 0);

    if (icon_path && icon_path[0]) {
        lv_obj_set_style_bg_opa(icon_box, LV_OPA_TRANSP, 0);
        lv_obj_t *icon_image = lv_image_create(icon_box);
        lv_image_set_inner_align(icon_image, LV_IMAGE_ALIGN_CONTAIN);
        lv_image_set_src(icon_image, icon_path);
        lv_obj_center(icon_image);
    } else {
        lv_obj_t *icon_label = lv_label_create(icon_box);
        lv_label_set_text(icon_label, fallback_icon);
        lv_obj_center(icon_label);
        lv_obj_set_style_text_color(icon_label, lv_color_hex(launcher::theme::kIconForeground), 0);
        lv_obj_set_style_text_font(icon_label, view->font_28, 0);
    }

    lv_obj_t *name = lv_label_create(tile);
    lv_label_set_text(name, label);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(name, lv_color_hex(launcher::theme::kText), 0);
    lv_obj_set_style_text_font(name, view->font_22, 0);
    launcher_view_layout_app_tile(tile, width, height);
    return tile;
}
