#include "launcher_view.h"

#include "launcher_theme.h"

#include <cm0/status_bar.h>
#include <cm0/typography.h>

#include <string.h>

namespace {

constexpr int32_t kAppIconSize    = 84;
constexpr int32_t kAppLabelHeight = 30;

void make_clear(lv_obj_t *object)
{
    lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_set_style_radius(object, 0, 0);
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

lv_obj_t *create_quick_panel(lv_obj_t *screen, const lv_font_t *font_22, const lv_font_t *font_28,
                             lv_obj_t **orientation_button, lv_obj_t **orientation_label)
{
    lv_obj_t *panel = lv_obj_create(screen);
    lv_obj_set_name_static(panel, "quick_panel");
    lv_obj_set_size(panel, LV_PCT(100), 260);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, -260);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(panel, lv_color_hex(launcher::theme::kContainerBackground), 0);
    lv_obj_set_style_bg_grad_dir(panel, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_top(panel, 24, 0);
    lv_obj_set_style_pad_bottom(panel, 12, 0);
    lv_obj_set_style_pad_left(panel, 28, 0);
    lv_obj_set_style_pad_right(panel, 28, 0);
    lv_obj_set_style_pad_row(panel, 16, 0);
    lv_obj_set_style_radius(panel, 8, 0);
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

    lv_obj_t *button = lv_button_create(panel);
    lv_obj_set_name_static(button, "orientation_button");
    lv_obj_set_size(button, 220, 104);
    lv_obj_set_style_bg_color(button, lv_color_hex(launcher::theme::kControlBackground), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(button, lv_color_hex(launcher::theme::kText), 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_top(button, 0, 0);
    lv_obj_set_style_pad_bottom(button, 0, 0);
    lv_obj_set_style_pad_left(button, 24, 0);
    lv_obj_set_style_pad_right(button, 0, 0);
    lv_obj_set_style_pad_column(button, 26, 0);
    lv_obj_set_style_radius(button, 8, 0);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(button, lv_color_hex(launcher::theme::kControlPressed), LV_STATE_PRESSED);

    lv_obj_t *icon = lv_label_create(button);
    lv_label_set_text(icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(icon, lv_color_hex(launcher::theme::kControlAccent), 0);
    lv_obj_set_style_text_font(icon, font_28, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_obj_set_name_static(label, "orientation_label");
    lv_label_set_text(label, "Landscape");
    lv_obj_set_style_text_font(label, font_22, 0);

    lv_obj_t *footer = lv_obj_create(panel);
    lv_obj_set_size(footer, LV_PCT(100), 1);
    lv_obj_set_flex_grow(footer, 1);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
    make_clear(footer);

    lv_obj_t *handle = lv_obj_create(footer);
    lv_obj_set_name_static(handle, "quick_panel_handle");
    lv_obj_set_size(handle, 72, 6);
    lv_obj_remove_flag(handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(handle, 6, 0);
    lv_obj_set_style_bg_color(handle, lv_color_hex(launcher::theme::kIndicatorInactive), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(handle, 0, 0);

    *orientation_button = button;
    *orientation_label  = label;
    return panel;
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
    view->quick_scrim       = create_quick_scrim(view->screen);
    view->quick_panel       = create_quick_panel(view->screen, view->font_22, view->font_28, &view->orientation_button,
                                                 &view->orientation_label);
    view->shutdown_scrim    = create_shutdown_overlay(view->screen, view->font_22, view->font_36, &view->shutdown_panel);

    return view->status_bar && view->app_grid && view->page_indicator && view->page_bottom_inset && view->quick_scrim &&
           view->quick_panel && view->orientation_button && view->orientation_label && view->home_indicator &&
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

void launcher_view_layout_quick_panel(launcher_view_t *view, int32_t height)
{
    if (!view || !view->quick_panel || !view->orientation_button || height <= 0) return;

    const bool compact = height < 260;
    lv_obj_set_height(view->quick_panel, height);
    lv_obj_set_style_pad_top(view->quick_panel, compact ? 12 : 24, 0);
    lv_obj_set_style_pad_bottom(view->quick_panel, compact ? 8 : 12, 0);
    lv_obj_set_style_pad_row(view->quick_panel, compact ? 8 : 16, 0);
    lv_obj_set_height(view->orientation_button, compact ? 84 : 104);
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
