#include "launcher_view.h"

#include <cm0/status_bar.h>
#include <cm0/typography.h>

#include <string.h>

namespace {

void make_clear(lv_obj_t *object) {
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(object, 0, 0);
  lv_obj_set_style_shadow_width(object, 0, 0);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_set_style_radius(object, 0, 0);
}

lv_obj_t *create_main_layout(lv_obj_t *screen) {
  lv_obj_t *layout = lv_obj_create(screen);
  lv_obj_set_name_static(layout, "main_layout");
  lv_obj_set_size(layout, LV_PCT(100), LV_PCT(100));
  lv_obj_center(layout);
  lv_obj_remove_flag(layout, LV_OBJ_FLAG_SCROLLABLE);
  make_clear(layout);
  lv_obj_set_style_pad_row(layout, 0, 0);
  lv_obj_set_style_pad_column(layout, 0, 0);
  lv_obj_set_flex_flow(layout, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(layout, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  return layout;
}

lv_obj_t *create_status_bar(lv_obj_t *layout) {
  lv_obj_t *slot = lv_obj_create(layout);
  lv_obj_set_name_static(slot, "system_status_bar_slot");
  lv_obj_remove_style_all(slot);
  lv_obj_set_size(slot, LV_PCT(100), CM0_STATUS_BAR_HEIGHT);
  lv_obj_remove_flag(slot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
  return cm0_status_bar_create(slot);
}

lv_obj_t *create_app_grid(lv_obj_t *layout) {
  lv_obj_t *grid = lv_obj_create(layout);
  lv_obj_set_name_static(grid, "app_grid");
  lv_obj_set_size(grid, LV_PCT(100), 900);
  lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  make_clear(grid);
  return grid;
}

lv_obj_t *create_page_indicator(lv_obj_t *layout) {
  lv_obj_t *spacer = lv_obj_create(layout);
  lv_obj_set_size(spacer, LV_PCT(100), 1);
  lv_obj_set_flex_grow(spacer, 1);
  lv_obj_remove_flag(spacer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
  make_clear(spacer);

  lv_obj_t *indicator = lv_obj_create(layout);
  lv_obj_set_name_static(indicator, "page_indicator");
  lv_obj_set_size(indicator, LV_SIZE_CONTENT, 40);
  lv_obj_set_flex_flow(indicator, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(indicator, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(indicator, LV_OBJ_FLAG_SCROLLABLE);
  make_clear(indicator);
  lv_obj_set_style_pad_column(indicator, 10, 0);
  return indicator;
}

lv_obj_t *create_page_bottom_inset(lv_obj_t *layout) {
  lv_obj_t *inset = lv_obj_create(layout);
  lv_obj_set_name_static(inset, "page_bottom_inset");
  lv_obj_set_size(inset, 1, 52);
  lv_obj_remove_flag(inset, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(inset, LV_OBJ_FLAG_SCROLLABLE);
  make_clear(inset);
  return inset;
}

void create_home_indicator(lv_obj_t *screen) {
  lv_obj_t *slot = lv_obj_create(screen);
  lv_obj_remove_style_all(slot);
  lv_obj_set_size(slot, LV_PCT(100), 38);
  lv_obj_align(slot, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_add_flag(slot, LV_OBJ_FLAG_FLOATING);
  lv_obj_remove_flag(slot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
  cm0_home_indicator_create(slot);
}

lv_obj_t *create_quick_scrim(lv_obj_t *screen) {
  lv_obj_t *scrim = lv_obj_create(screen);
  lv_obj_set_name_static(scrim, "quick_scrim");
  lv_obj_set_size(scrim, LV_PCT(100), LV_PCT(100));
  lv_obj_center(scrim);
  lv_obj_add_flag(scrim, LV_OBJ_FLAG_FLOATING);
  lv_obj_add_flag(scrim, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_opa(scrim, LV_OPA_10, 0);
  lv_obj_set_style_opa(scrim, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(scrim, 0, 0);
  lv_obj_set_style_pad_all(scrim, 0, 0);
  lv_obj_set_style_radius(scrim, 0, 0);
  return scrim;
}

lv_obj_t *create_quick_panel(lv_obj_t *screen,
                             lv_obj_t **orientation_button,
                             lv_obj_t **orientation_label) {
  lv_obj_t *panel = lv_obj_create(screen);
  lv_obj_set_name_static(panel, "quick_panel");
  lv_obj_set_size(panel, LV_PCT(100), 260);
  lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, -260);
  lv_obj_add_flag(panel, LV_OBJ_FLAG_FLOATING);
  lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(panel, lv_color_hex(0xf7f7f8), 0);
  lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(panel, 0, 0);
  lv_obj_set_style_pad_top(panel, 24, 0);
  lv_obj_set_style_pad_bottom(panel, 12, 0);
  lv_obj_set_style_pad_left(panel, 28, 0);
  lv_obj_set_style_pad_right(panel, 28, 0);
  lv_obj_set_style_pad_row(panel, 16, 0);
  lv_obj_set_style_radius(panel, 8, 0);
  lv_obj_set_style_shadow_width(panel, 18, 0);
  lv_obj_set_style_shadow_opa(panel, LV_OPA_20, 0);
  lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);

  lv_obj_t *title = lv_label_create(panel);
  lv_label_set_text(title, "Quick controls");
  lv_obj_set_style_text_font(title, CM0_FONT_UI_22, 0);

  lv_obj_t *button = lv_button_create(panel);
  lv_obj_set_name_static(button, "orientation_button");
  lv_obj_set_size(button, 220, 104);
  lv_obj_set_style_bg_color(button, lv_color_hex(0xe6e8ec), 0);
  lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
  lv_obj_set_style_text_color(button, lv_color_hex(0x18181a), 0);
  lv_obj_set_style_border_width(button, 0, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_set_style_pad_top(button, 0, 0);
  lv_obj_set_style_pad_bottom(button, 0, 0);
  lv_obj_set_style_pad_left(button, 24, 0);
  lv_obj_set_style_pad_right(button, 0, 0);
  lv_obj_set_style_pad_column(button, 26, 0);
  lv_obj_set_style_radius(button, 8, 0);
  lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_bg_color(button, lv_color_hex(0xd8dbe1), LV_STATE_PRESSED);

  lv_obj_t *icon = lv_label_create(button);
  lv_label_set_text(icon, LV_SYMBOL_REFRESH);
  lv_obj_set_style_text_color(icon, lv_color_hex(0x19766f), 0);
  lv_obj_set_style_text_font(icon, CM0_FONT_UI_28, 0);

  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, "Landscape");
  lv_obj_set_style_text_font(label, CM0_FONT_UI_22, 0);

  lv_obj_t *footer = lv_obj_create(panel);
  lv_obj_set_size(footer, LV_PCT(100), 1);
  lv_obj_set_flex_grow(footer, 1);
  lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_remove_flag(footer, LV_OBJ_FLAG_SCROLLABLE);
  make_clear(footer);

  lv_obj_t *handle = lv_obj_create(footer);
  lv_obj_set_size(handle, 72, 6);
  lv_obj_remove_flag(handle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(handle, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(handle, 6, 0);
  lv_obj_set_style_bg_color(handle, lv_color_hex(0x8e8e93), 0);
  lv_obj_set_style_bg_opa(handle, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(handle, 0, 0);

  *orientation_button = button;
  *orientation_label = label;
  return panel;
}

} // namespace

bool launcher_view_create(launcher_view_t *view) {
  if (!view)
    return false;
  memset(view, 0, sizeof(*view));

  view->screen = lv_obj_create(nullptr);
  if (!view->screen)
    return false;
  lv_obj_set_name_static(view->screen, "launcher");
  lv_obj_remove_flag(view->screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(view->screen, lv_color_hex(0xf7fafc), 0);
  lv_obj_set_style_bg_grad_color(view->screen, lv_color_hex(0xe3e8f0), 0);
  lv_obj_set_style_bg_grad_dir(view->screen, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(view->screen, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(view->screen, 0, 0);
  lv_obj_set_style_pad_all(view->screen, 0, 0);
  lv_obj_set_style_radius(view->screen, 36, 0);
  lv_obj_set_style_clip_corner(view->screen, true, 0);
  lv_obj_set_style_text_color(view->screen, lv_color_hex(0x18181a), 0);

  lv_obj_t *layout = create_main_layout(view->screen);
  view->status_bar = create_status_bar(layout);
  view->app_grid = create_app_grid(layout);
  view->page_indicator = create_page_indicator(layout);
  view->page_bottom_inset = create_page_bottom_inset(layout);
  create_home_indicator(view->screen);
  view->quick_scrim = create_quick_scrim(view->screen);
  view->quick_panel = create_quick_panel(
      view->screen, &view->orientation_button, &view->orientation_label);

  return view->status_bar && view->app_grid && view->page_indicator &&
         view->page_bottom_inset && view->quick_scrim && view->quick_panel &&
         view->orientation_button && view->orientation_label;
}

lv_obj_t *launcher_view_create_app_tile(lv_obj_t *parent, const char *label,
                                        const char *icon_path,
                                        const char *fallback_icon,
                                        lv_color_t icon_background) {
  lv_obj_t *tile = lv_button_create(parent);
  lv_obj_set_size(tile, 150, 150);
  lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(tile, 0, 0);
  lv_obj_set_style_shadow_width(tile, 0, 0);
  lv_obj_set_style_pad_all(tile, 0, 0);
  lv_obj_set_style_radius(tile, 0, 0);
  lv_obj_set_style_bg_color(tile, lv_color_hex(0x000000), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(tile, LV_OPA_20, LV_STATE_PRESSED);

  lv_obj_t *icon_box = lv_obj_create(tile);
  lv_obj_set_size(icon_box, 84, 84);
  lv_obj_align(icon_box, LV_ALIGN_TOP_MID, 0, 15);
  lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(icon_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(icon_box, icon_background, 0);
  lv_obj_set_style_bg_opa(icon_box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(icon_box, 0, 0);
  lv_obj_set_style_pad_all(icon_box, 0, 0);
  lv_obj_set_style_radius(icon_box, 20, 0);
  lv_obj_set_style_clip_corner(icon_box, true, 0);

  if (icon_path && icon_path[0]) {
    lv_obj_set_style_bg_opa(icon_box, LV_OPA_TRANSP, 0);
    lv_obj_t *icon_image = lv_image_create(icon_box);
    lv_obj_set_size(icon_image, 84, 84);
    lv_image_set_inner_align(icon_image, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(icon_image, icon_path);
    lv_obj_center(icon_image);
  } else {
    lv_obj_t *icon_label = lv_label_create(icon_box);
    lv_label_set_text(icon_label, fallback_icon);
    lv_obj_center(icon_label);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(icon_label, CM0_FONT_UI_28, 0);
  }

  lv_obj_t *name = lv_label_create(tile);
  lv_label_set_text(name, label);
  lv_obj_set_size(name, 150, 30);
  lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 109);
  lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(name, lv_color_hex(0x18181a), 0);
  lv_obj_set_style_text_font(name, CM0_FONT_UI_22, 0);
  return tile;
}
