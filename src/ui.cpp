#include "ui.h"

#include "launcher_view.h"

#include <cm0/typography.h>

#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct launcher_ui {
  explicit launcher_ui(launcher_registry_t *value) : registry(value) {}

  launcher_registry_t *registry = nullptr;
  launcher_app_t *pending_app = nullptr;
  launcher_orientation_t orientation = LAUNCHER_ORIENTATION_PORTRAIT;
  bool orientation_request_pending = false;
  launcher_orientation_t requested_orientation = LAUNCHER_ORIENTATION_PORTRAIT;
  launcher_view_t view{};
  lv_obj_t *pages = nullptr;
  lv_indev_t *input = nullptr;
  lv_point_t pull_start{};
  bool pull_tracking = false;
  bool quick_panel_open = false;
};

enum {
  APP_CELL_SIZE = 150,
  PAGE_INDICATOR_GAP = 10,
  PAGE_INDICATOR_HEIGHT = 40,
  PAGE_INDICATOR_MARK_SIZE = 8,
  PAGE_INDICATOR_ACTIVE_WIDTH = 24,
  QUICK_PANEL_PULL_START_ZONE = 96,
  QUICK_PANEL_PULL_DISTANCE = 64,
};

static bool is_landscape(const launcher_ui_t *ui) {
  return ui->orientation == LAUNCHER_ORIENTATION_LANDSCAPE;
}

static const char *icon_text(const char *icon) {
  if (strcmp(icon, "settings") == 0)
    return LV_SYMBOL_SETTINGS;
  if (strcmp(icon, "play") == 0)
    return LV_SYMBOL_PLAY;
  if (strcmp(icon, "image") == 0 || strcmp(icon, "camera") == 0)
    return LV_SYMBOL_IMAGE;
  if (strcmp(icon, "list") == 0)
    return LV_SYMBOL_LIST;
  if (strcmp(icon, "home") == 0)
    return LV_SYMBOL_HOME;
  if (strcmp(icon, "calculator") == 0)
    return "=";
  if (strcmp(icon, "clock") == 0)
    return LV_SYMBOL_BELL;
  if (strcmp(icon, "artboard") == 0)
    return LV_SYMBOL_EDIT;
  if (strcmp(icon, "battery") == 0)
    return LV_SYMBOL_BATTERY_FULL;
  if (strcmp(icon, "led") == 0)
    return LV_SYMBOL_CHARGE;
  if (strcmp(icon, "gnss") == 0)
    return LV_SYMBOL_GPS;
  if (strcmp(icon, "hdmi") == 0 || strcmp(icon, "video") == 0)
    return LV_SYMBOL_VIDEO;
  if (strcmp(icon, "weather") == 0)
    return LV_SYMBOL_TINT;
  if (strcmp(icon, "cellular") == 0 || strcmp(icon, "lora") == 0)
    return LV_SYMBOL_WIFI;
  if (strcmp(icon, "ble") == 0)
    return LV_SYMBOL_BLUETOOTH;
  if (strcmp(icon, "music") == 0)
    return LV_SYMBOL_AUDIO;
  if (strcmp(icon, "live-call") == 0)
    return LV_SYMBOL_CALL;
  return LV_SYMBOL_FILE;
}

static lv_color_t icon_color(const char *icon) {
  if (strcmp(icon, "settings") == 0 || strcmp(icon, "hdmi") == 0)
    return lv_color_hex(0x596573);
  if (strcmp(icon, "play") == 0)
    return lv_color_hex(0x19766f);
  if (strcmp(icon, "image") == 0)
    return lv_color_hex(0x9b5f3e);
  if (strcmp(icon, "list") == 0 || strcmp(icon, "gnss") == 0 ||
      strcmp(icon, "ble") == 0)
    return lv_color_hex(0x416e93);
  if (strcmp(icon, "home") == 0)
    return lv_color_hex(0x8e6a3c);
  if (strcmp(icon, "calculator") == 0 || strcmp(icon, "weather") == 0)
    return lv_color_hex(0x277078);
  if (strcmp(icon, "clock") == 0 || strcmp(icon, "camera") == 0)
    return lv_color_hex(0xb2534b);
  if (strcmp(icon, "artboard") == 0 || strcmp(icon, "music") == 0)
    return lv_color_hex(0x7c4d9e);
  if (strcmp(icon, "battery") == 0 || strcmp(icon, "cellular") == 0)
    return lv_color_hex(0x3d8b5f);
  if (strcmp(icon, "led") == 0)
    return lv_color_hex(0xd39128);
  if (strcmp(icon, "video") == 0)
    return lv_color_hex(0x2f6f75);
  if (strcmp(icon, "live-call") == 0)
    return lv_color_hex(0x31745a);
  if (strcmp(icon, "lora") == 0)
    return lv_color_hex(0x9a6a2f);
  return lv_color_hex(0x7b5269);
}

static const char *loadable_icon_path(const launcher_app_t *app) {
  if (!app->icon_path[0])
    return nullptr;
  lv_image_header_t header{};
  return lv_image_decoder_get_info(app->icon_path, &header) == LV_RESULT_OK
             ? app->icon_path
             : nullptr;
}

static int32_t absolute_distance(int32_t first, int32_t second) {
  return first >= second ? first - second : second - first;
}

static void animate_object_y(void *object, int32_t value) {
  lv_obj_set_y(static_cast<lv_obj_t *>(object), value);
}

static void animate_object_opacity(void *object, int32_t value) {
  lv_obj_set_style_opa(static_cast<lv_obj_t *>(object), (lv_opa_t)value, 0);
}

static void start_animation(lv_obj_t *object, lv_anim_exec_xcb_t callback,
                            int32_t from, int32_t to) {
  lv_anim_delete(object, callback);
  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, object);
  lv_anim_set_exec_cb(&animation, callback);
  lv_anim_set_values(&animation, from, to);
  lv_anim_set_duration(&animation, 180);
  lv_anim_start(&animation);
}

static void play_quick_panel(launcher_ui_t *ui, bool open) {
  if (!ui || !ui->view.screen || ui->quick_panel_open == open)
    return;
  ui->quick_panel_open = open;
  const int32_t panel_height = lv_obj_get_height(ui->view.quick_panel);
  if (open)
    lv_obj_remove_flag(ui->view.quick_scrim, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(ui->view.quick_scrim, LV_OBJ_FLAG_HIDDEN);
  start_animation(ui->view.quick_scrim, animate_object_opacity,
                  lv_obj_get_style_opa(ui->view.quick_scrim, LV_PART_MAIN),
                  open ? LV_OPA_COVER : LV_OPA_TRANSP);
  start_animation(ui->view.quick_panel, animate_object_y,
                  lv_obj_get_y(ui->view.quick_panel),
                  open ? 0 : -panel_height);
}

static void pull_down_input_event(lv_event_t *event) {
  auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
  if (!ui || !ui->input)
    return;

  switch (lv_event_get_code(event)) {
  case LV_EVENT_PRESSED:
    lv_indev_get_point(ui->input, &ui->pull_start);
    ui->pull_tracking =
        ui->quick_panel_open || ui->pull_start.y <= QUICK_PANEL_PULL_START_ZONE;
    break;
  case LV_EVENT_RELEASED: {
    if (!ui->pull_tracking)
      break;
    lv_point_t current;
    lv_indev_get_point(ui->input, &current);
    const int32_t vertical = current.y - ui->pull_start.y;
    const int32_t horizontal = absolute_distance(current.x, ui->pull_start.x);
    if (!ui->quick_panel_open && vertical >= QUICK_PANEL_PULL_DISTANCE &&
        vertical > horizontal)
      play_quick_panel(ui, true);
    else if (ui->quick_panel_open &&
             -vertical >= QUICK_PANEL_PULL_DISTANCE && -vertical > horizontal)
      play_quick_panel(ui, false);
    ui->pull_tracking = false;
    break;
  }
  case LV_EVENT_DELETE:
    ui->input = nullptr;
    ui->pull_tracking = false;
    break;
  default:
    break;
  }
}

static void attach_pull_down_input(launcher_ui_t *ui) {
  for (lv_indev_t *input = lv_indev_get_next(nullptr); input;
       input = lv_indev_get_next(input)) {
    if (lv_indev_get_type(input) != LV_INDEV_TYPE_POINTER)
      continue;
    ui->input = input;
    lv_indev_add_event_cb(input, pull_down_input_event, LV_EVENT_PRESSED, ui);
    lv_indev_add_event_cb(input, pull_down_input_event, LV_EVENT_RELEASED, ui);
    lv_indev_add_event_cb(input, pull_down_input_event, LV_EVENT_DELETE, ui);
    break;
  }
}

static void scrim_clicked(lv_event_t *event) {
  auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
  if (ui)
    play_quick_panel(ui, false);
}

static void orientation_clicked(lv_event_t *event) {
  auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
  if (!ui)
    return;
  ui->requested_orientation =
      is_landscape(ui) ? LAUNCHER_ORIENTATION_PORTRAIT
                       : LAUNCHER_ORIENTATION_LANDSCAPE;
  ui->orientation_request_pending = true;
  play_quick_panel(ui, false);
}

static void app_clicked(lv_event_t *event) {
  auto *ui = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
  auto *app = static_cast<launcher_app_t *>(
      lv_obj_get_user_data(lv_event_get_current_target_obj(event)));
  if (ui && app)
    ui->pending_app = app;
}

static void update_page_indicator(lv_obj_t *indicator, size_t active_page) {
  const uint32_t count = lv_obj_get_child_count(indicator);
  for (uint32_t i = 0; i < count; ++i) {
    lv_obj_t *button = lv_obj_get_child(indicator, (int32_t)i);
    lv_obj_t *mark = lv_obj_get_child(button, 0);
    const int32_t width = i == active_page ? PAGE_INDICATOR_ACTIVE_WIDTH
                                           : PAGE_INDICATOR_MARK_SIZE;
    lv_obj_set_width(button, width);
    lv_obj_set_width(mark, width);
    lv_obj_set_style_bg_color(
        mark, lv_color_hex(i == active_page ? 0x18181a : 0x8e8e93), 0);
  }
}

static void page_changed(lv_event_t *event) {
  auto *indicator = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
  lv_obj_t *active =
      lv_tileview_get_tile_active(lv_event_get_current_target_obj(event));
  if (active)
    update_page_indicator(indicator, lv_obj_get_index(active));
}

static void page_dot_clicked(lv_event_t *event) {
  lv_obj_t *button = lv_event_get_current_target_obj(event);
  lv_obj_t *indicator = lv_obj_get_parent(button);
  auto *pages = static_cast<lv_obj_t *>(lv_obj_get_user_data(indicator));
  const uint32_t page = lv_obj_get_index(button);
  lv_tileview_set_tile_by_index(pages, page, 0, LV_ANIM_ON);
  update_page_indicator(indicator, page);
}

static void populate_page_indicator(lv_obj_t *indicator, lv_obj_t *pages,
                                    size_t page_count) {
  lv_obj_clean(indicator);
  lv_obj_set_user_data(indicator, pages);
  int32_t width = PAGE_INDICATOR_ACTIVE_WIDTH;
  if (page_count > 1)
    width += (int32_t)(page_count - 1) *
             (PAGE_INDICATOR_MARK_SIZE + PAGE_INDICATOR_GAP);
  lv_obj_set_width(indicator, width);

  for (size_t i = 0; i < page_count; ++i) {
    lv_obj_t *button = lv_button_create(indicator);
    lv_obj_set_size(button, PAGE_INDICATOR_MARK_SIZE, PAGE_INDICATOR_HEIGHT);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(button, page_dot_clicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *mark = lv_obj_create(button);
    lv_obj_set_size(mark, PAGE_INDICATOR_MARK_SIZE,
                    PAGE_INDICATOR_MARK_SIZE);
    lv_obj_center(mark);
    lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(mark, 0, 0);
    lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
  }
  update_page_indicator(indicator, 0);
}

static int32_t app_grid_inset(int32_t width, int32_t columns) {
  const int32_t free_space = width - columns * APP_CELL_SIZE;
  return free_space > 0 ? free_space / (columns + 1) : 0;
}

static void style_app_page(lv_obj_t *page, int32_t width, int32_t columns) {
  const int32_t inset = app_grid_inset(width, columns);
  const int32_t used = columns * APP_CELL_SIZE + (columns - 1) * inset;
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
  lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                        LV_FLEX_ALIGN_START);
  lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
}

static void rebuild_apps(launcher_ui_t *ui) {
  ui->pending_app = nullptr;
  lv_obj_clean(ui->view.app_grid);
  lv_obj_set_style_pad_all(ui->view.app_grid, 0, 0);

  ui->pages = lv_tileview_create(ui->view.app_grid);
  lv_obj_set_size(ui->pages, LV_PCT(100), LV_PCT(100));
  lv_obj_set_scrollbar_mode(ui->pages, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_border_width(ui->pages, 0, 0);
  lv_obj_set_style_radius(ui->pages, 0, 0);
  lv_obj_set_style_pad_all(ui->pages, 0, 0);
  lv_obj_set_style_bg_opa(ui->pages, LV_OPA_TRANSP, 0);

  size_t page_count =
      (ui->registry->count + LAUNCHER_APPS_PER_PAGE - 1) /
      LAUNCHER_APPS_PER_PAGE;
  if (page_count == 0)
    page_count = 1;
  const int32_t width = lv_obj_get_width(ui->view.screen);
  const int32_t columns = is_landscape(ui) ? 6 : 3;

  lv_obj_t *first_page = nullptr;
  for (size_t page_index = 0; page_index < page_count; ++page_index) {
    lv_dir_t direction = LV_DIR_NONE;
    if (page_index > 0)
      direction = static_cast<lv_dir_t>(direction | LV_DIR_LEFT);
    if (page_index + 1 < page_count)
      direction = static_cast<lv_dir_t>(direction | LV_DIR_RIGHT);
    lv_obj_t *page = lv_tileview_add_tile(ui->pages, (uint8_t)page_index, 0,
                                         direction);
    if (!first_page)
      first_page = page;
    style_app_page(page, width, columns);

    const size_t first_app = page_index * LAUNCHER_APPS_PER_PAGE;
    size_t last_app = first_app + LAUNCHER_APPS_PER_PAGE;
    if (last_app > ui->registry->count)
      last_app = ui->registry->count;
    for (size_t i = first_app; i < last_app; ++i) {
      launcher_app_t *app = &ui->registry->apps[i];
      lv_obj_t *tile = launcher_view_create_app_tile(
          page, app->manifest.name, loadable_icon_path(app),
          icon_text(app->manifest.icon),
          icon_color(app->manifest.icon));
      lv_obj_set_user_data(tile, app);
      lv_obj_add_event_cb(tile, app_clicked, LV_EVENT_CLICKED, ui);
    }
  }

  if (ui->registry->count == 0) {
    lv_obj_t *empty = lv_label_create(first_page);
    lv_label_set_text(empty, "No applications installed");
    lv_obj_set_width(empty, width - 2 * app_grid_inset(width, columns));
    lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(empty, lv_color_hex(0x6b6b70), 0);
    lv_obj_set_style_text_font(empty, CM0_FONT_UI_22, 0);
  }

  populate_page_indicator(ui->view.page_indicator, ui->pages, page_count);
  lv_obj_add_event_cb(ui->pages, page_changed, LV_EVENT_VALUE_CHANGED,
                      ui->view.page_indicator);
  lv_tileview_set_tile(ui->pages, first_page, LV_ANIM_OFF);
}

static void apply_layout(launcher_ui_t *ui) {
  const bool landscape = is_landscape(ui);
  const int32_t panel_height = landscape ? 190 : 260;
  lv_obj_set_height(ui->view.app_grid, landscape ? 450 : 900);
  lv_obj_set_height(ui->view.page_bottom_inset, landscape ? 14 : 52);
  lv_obj_set_height(ui->view.quick_panel, panel_height);
  if (!ui->quick_panel_open)
    lv_obj_set_y(ui->view.quick_panel, -panel_height);
  lv_label_set_text(ui->view.orientation_label,
                    landscape ? "Portrait" : "Landscape");
}

launcher_ui_t *launcher_ui_create(launcher_registry_t *registry) {
  auto *ui = new (std::nothrow) launcher_ui_t{registry};
  if (!ui)
    return nullptr;

  if (!launcher_view_create(&ui->view)) {
    delete ui;
    return nullptr;
  }

  lv_screen_load(ui->view.screen);
  ui->orientation =
      lv_obj_get_width(ui->view.screen) > lv_obj_get_height(ui->view.screen)
          ? LAUNCHER_ORIENTATION_LANDSCAPE
          : LAUNCHER_ORIENTATION_PORTRAIT;
  apply_layout(ui);
  rebuild_apps(ui);

  lv_obj_add_event_cb(ui->view.quick_scrim, scrim_clicked, LV_EVENT_CLICKED,
                      ui);
  lv_obj_add_event_cb(ui->view.orientation_button, orientation_clicked,
                      LV_EVENT_CLICKED, ui);
  attach_pull_down_input(ui);
  return ui;
}

void launcher_ui_destroy(launcher_ui_t *ui) {
  if (!ui)
    return;
  if (ui->input)
    lv_indev_remove_event_cb_with_user_data(ui->input, pull_down_input_event,
                                            ui);
  if (ui->view.quick_scrim)
    lv_obj_remove_event_cb_with_user_data(ui->view.quick_scrim, scrim_clicked,
                                          ui);
  if (ui->view.orientation_button)
    lv_obj_remove_event_cb_with_user_data(ui->view.orientation_button,
                                          orientation_clicked, ui);
  delete ui;
}

bool launcher_ui_is_home(const launcher_ui_t *ui) { return ui != nullptr; }

void launcher_ui_refresh_home(launcher_ui_t *ui) {
  if (ui)
    rebuild_apps(ui);
}

void launcher_ui_apply_orientation(launcher_ui_t *ui,
                                   launcher_orientation_t orientation) {
  if (!ui)
    return;
  ui->orientation = orientation;
  ui->orientation_request_pending = false;
  ui->quick_panel_open = false;
  lv_anim_delete(ui->view.quick_scrim, animate_object_opacity);
  lv_anim_delete(ui->view.quick_panel, animate_object_y);
  lv_obj_add_flag(ui->view.quick_scrim, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(ui->view.quick_scrim, LV_OPA_TRANSP, 0);
  apply_layout(ui);
  rebuild_apps(ui);
}

bool launcher_ui_take_orientation_request(
    launcher_ui_t *ui, launcher_orientation_t *orientation) {
  if (!ui || !orientation || !ui->orientation_request_pending)
    return false;
  *orientation = ui->requested_orientation;
  ui->orientation_request_pending = false;
  return true;
}

const launcher_app_t *launcher_ui_take_launch_request(launcher_ui_t *ui) {
  if (!ui)
    return nullptr;
  launcher_app_t *app = ui->pending_app;
  ui->pending_app = nullptr;
  return app;
}
