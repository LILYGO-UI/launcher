#include "registry.h"
#include "ui.h"

#include <cm0/status_bar.h>
#include <cm0/typography.h>

#include <lvgl.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH LAUNCHER_DEFAULT_WIDTH
#define HEIGHT LAUNCHER_DEFAULT_HEIGHT
#define PARTIAL_ROWS 80
#define STATUS_COMPARE_X 380
#define STATUS_COMPARE_RIGHT 534

static lv_color32_t *framebuffer;
static lv_color32_t *status_snapshot;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state = LV_INDEV_STATE_RELEASED;

static void read_pointer(lv_indev_t *input, lv_indev_data_t *data) {
  (void)input;
  data->point = pointer_point;
  data->state = pointer_state;
}

static void flush_display(lv_display_t *display, const lv_area_t *area,
                          uint8_t *pixels) {
  int width = area->x2 - area->x1 + 1;
  int display_width = lv_display_get_horizontal_resolution(display);
  const lv_color32_t *source = (const lv_color32_t *)pixels;
  for (int y = area->y1; y <= area->y2; ++y) {
    memcpy(framebuffer + (size_t)y * display_width + area->x1,
           source + (size_t)(y - area->y1) * width,
           (size_t)width * sizeof(*source));
  }
  lv_display_flush_ready(display);
}

static void write_ppm(const char *path, int width, int height) {
  FILE *file = fopen(path, "wb");
  assert(file);
  fprintf(file, "P6\n%d %d\n255\n", width, height);
  for (size_t i = 0; i < (size_t)width * height; ++i) {
    unsigned char rgb[3] = {
        framebuffer[i].red,
        framebuffer[i].green,
        framebuffer[i].blue,
    };
    assert(fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb));
  }
  fclose(file);
}

static void render(lv_display_t *display, const char *path) {
  lv_tick_inc(20);
  lv_refr_now(display);
  write_ppm(path, lv_display_get_horizontal_resolution(display),
            lv_display_get_vertical_resolution(display));
}

static void snapshot_status_region(void) {
  const size_t row_pixels = STATUS_COMPARE_RIGHT - STATUS_COMPARE_X + 1;
  for (int y = 0; y < CM0_STATUS_BAR_HEIGHT; ++y) {
    memcpy(status_snapshot + (size_t)y * row_pixels,
           framebuffer + (size_t)y * WIDTH + STATUS_COMPARE_X,
           row_pixels * sizeof(*framebuffer));
  }
}

static void assert_status_region_unchanged(void) {
  const size_t row_pixels = STATUS_COMPARE_RIGHT - STATUS_COMPARE_X + 1;
  size_t mismatch_count = 0;
  int first_x = -1;
  int first_y = -1;
  int last_x = -1;
  int last_y = -1;
  lv_color32_t first_before = {0};
  lv_color32_t first_after = {0};
  for (int y = 0; y < CM0_STATUS_BAR_HEIGHT; ++y) {
    for (size_t x = 0; x < row_pixels; ++x) {
      const lv_color32_t before =
          status_snapshot[(size_t)y * row_pixels + x];
      const lv_color32_t after =
          framebuffer[(size_t)y * WIDTH + STATUS_COMPARE_X + x];
      if (memcmp(&before, &after, sizeof(before)) != 0) {
        if (first_x < 0) {
          first_x = STATUS_COMPARE_X + (int)x;
          first_y = y;
          first_before = before;
          first_after = after;
        }
        last_x = STATUS_COMPARE_X + (int)x;
        last_y = y;
        ++mismatch_count;
      }
    }
  }
  if (mismatch_count)
    fprintf(stderr,
            "status region changed from (%d, %d) to (%d, %d): %zu pixels "
            "differ, first rgba %u,%u,%u,%u -> %u,%u,%u,%u\n",
            first_x, first_y, last_x, last_y, mismatch_count,
            first_before.red, first_before.green, first_before.blue,
            first_before.alpha, first_after.red, first_after.green,
            first_after.blue, first_after.alpha);
  assert(mismatch_count == 0);
}

static lv_obj_t *find_by_class(lv_obj_t *parent,
                               const lv_obj_class_t *class_p) {
  if (lv_obj_has_class(parent, class_p))
    return parent;
  uint32_t child_count = lv_obj_get_child_count(parent);
  for (uint32_t i = 0; i < child_count; ++i) {
    lv_obj_t *match =
        find_by_class(lv_obj_get_child(parent, (int32_t)i), class_p);
    if (match)
      return match;
  }
  return NULL;
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s PORTRAIT.ppm QUICK_CONTROLS.ppm LANDSCAPE.ppm\n",
            argv[0]);
    return 2;
  }
  framebuffer = calloc((size_t)WIDTH * HEIGHT, sizeof(*framebuffer));
  status_snapshot =
      calloc((size_t)(STATUS_COMPARE_RIGHT - STATUS_COMPARE_X + 1) *
                 CM0_STATUS_BAR_HEIGHT,
             sizeof(*status_snapshot));
  lv_color32_t *draw_buffer =
      calloc((size_t)WIDTH * PARTIAL_ROWS, sizeof(*draw_buffer));
  assert(framebuffer && status_snapshot && draw_buffer);

  launcher_registry_t registry;
  launcher_registry_init(&registry);
  assert(launcher_registry_scan(&registry, LAUNCHER_TEST_APP_DIR) >= 1);
  launcher_registry_sort(&registry);
  char fixture_icon_path[LAUNCHER_PATH_MAX];
  snprintf(fixture_icon_path, sizeof(fixture_icon_path), "%s",
           registry.apps[0].icon_path);

  static const char *first_page_names[] = {
      "Squareline", "Calculator", "2048 Game",   "Camera",  "Music",    "Lora",
      "Settings",   "Led",        "Face_detect", "Clock",   "Artboard", "Ble",
      "Battery",    "Video",      "LiveCall",    "Weather", "KeyBoard", "Gnss",
  };
  static const char *first_page_icons[] = {
      "home",     "calculator", "play",      "camera",  "music",    "lora",
      "settings", "led",        "image",     "clock",   "artboard", "ble",
      "battery",  "video",      "live-call", "weather", "list",     "gnss",
  };
  launcher_app_t fixture = registry.apps[0];
  for (size_t i = 0; i < 37; ++i) {
    registry.apps[i] = fixture;
    registry.apps[i].icon_path[0] = '\0';
    snprintf(registry.apps[i].manifest.id, sizeof(registry.apps[i].manifest.id),
             "cc.lilygo.ui.Render%zu", i);
    if (i < 18) {
      snprintf(registry.apps[i].manifest.name,
               sizeof(registry.apps[i].manifest.name), "%s",
               first_page_names[i]);
      snprintf(registry.apps[i].manifest.icon,
               sizeof(registry.apps[i].manifest.icon), "%s",
               first_page_icons[i]);
    } else {
      snprintf(registry.apps[i].manifest.name,
               sizeof(registry.apps[i].manifest.name), "App %zu", i + 1);
    }
    registry.apps[i].manifest.order = (int)i;
  }
  snprintf(registry.apps[0].icon_path, sizeof(registry.apps[0].icon_path),
           "%s", fixture_icon_path);
  snprintf(registry.apps[1].manifest.icon,
           sizeof(registry.apps[1].manifest.icon), "%s", "missing.png");
  registry.count = 37;

  lv_init();
  lv_font_glyph_dsc_t glyph;
  assert(lv_font_get_glyph_dsc(CM0_FONT_UI_22, &glyph, 'A', 0));
  assert(lv_font_get_glyph_dsc(CM0_FONT_UI_22, &glyph, 0x8bbe, 0));
  lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
  assert(display);
  lv_display_set_buffers(display, draw_buffer, NULL,
                         WIDTH * PARTIAL_ROWS * sizeof(*draw_buffer),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, flush_display);

  lv_indev_t *input = lv_indev_create();
  assert(input);
  lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(input, display);
  lv_indev_set_read_cb(input, read_pointer);

  launcher_ui_t *ui = launcher_ui_create(&registry);
  assert(ui);
  lv_obj_update_layout(lv_screen_active());
  assert(lv_obj_get_width(lv_screen_active()) == 568);
  assert(lv_obj_get_height(lv_screen_active()) == 1232);

  lv_obj_t *screen = lv_screen_active();
  lv_obj_t *status_bar = lv_obj_find_by_name(screen, "status_bar");
  assert(status_bar);
  lv_obj_t *time_container = lv_obj_get_child(status_bar, 0);
  lv_obj_t *levels = lv_obj_get_child(status_bar, 2);
  lv_obj_t *clock = lv_obj_get_child(time_container, 0);
  lv_obj_t *keyboard =
      lv_obj_find_by_name(levels, "cm0_status_bar_keyboard");
  lv_obj_t *ethernet =
      lv_obj_find_by_name(levels, "cm0_status_bar_ethernet");
  lv_obj_t *wifi = lv_obj_find_by_name(levels, "cm0_status_bar_wifi");
  lv_obj_t *battery =
      lv_obj_find_by_name(levels, "cm0_status_bar_battery");
  lv_area_t area;
  assert(lv_obj_get_width(status_bar) == 568);
  assert(lv_obj_get_height(status_bar) == CM0_STATUS_BAR_HEIGHT);
  assert(keyboard);
  assert(ethernet);
  assert(lv_obj_get_width(wifi) == 21);
  assert(lv_obj_get_width(battery) == 45);
  assert(lv_obj_get_class(clock) == &lv_label_class);

  lv_obj_t *pages = find_by_class(screen, &lv_tileview_class);
  assert(pages);
  assert(lv_obj_get_x(pages) == 0);
  assert(lv_obj_get_y(pages) == 0);
  assert(lv_obj_get_width(pages) == 568);
  assert(lv_obj_get_height(pages) == 900);
  size_t expected_page_count =
      (registry.count + LAUNCHER_APPS_PER_PAGE - 1) / LAUNCHER_APPS_PER_PAGE;
  assert(lv_obj_get_child_count(pages) == expected_page_count);
  for (size_t i = 0; i < expected_page_count; ++i) {
    lv_obj_t *page = lv_obj_get_child(pages, (int32_t)i);
    size_t remaining = registry.count - i * LAUNCHER_APPS_PER_PAGE;
    size_t expected_apps =
        remaining < LAUNCHER_APPS_PER_PAGE ? remaining : LAUNCHER_APPS_PER_PAGE;
    assert(lv_obj_get_child_count(page) == expected_apps);
    if (expected_apps > 0 && i == 0) {
      lv_obj_t *card = lv_obj_get_child(page, 0);
      lv_obj_t *icon = lv_obj_get_child(card, 0);
      lv_obj_t *label = lv_obj_get_child(card, 1);
      lv_obj_get_coords(card, &area);
      if (area.x1 != 29 || area.y1 != CM0_STATUS_BAR_HEIGHT) {
        lv_area_t status_area;
        lv_area_t slot_area;
        lv_area_t grid_area;
        lv_obj_get_coords(status_bar, &status_area);
        lv_obj_get_coords(lv_obj_get_parent(status_bar), &slot_area);
        lv_obj_get_coords(lv_obj_get_parent(pages), &grid_area);
        fprintf(stderr, "portrait first card at (%d, %d), expected (29, %d)\n",
                area.x1, area.y1, CM0_STATUS_BAR_HEIGHT);
        fprintf(stderr,
                "status=(%d,%d)-(%d,%d) slot=(%d,%d)-(%d,%d) "
                "grid=(%d,%d)-(%d,%d)\n",
                status_area.x1, status_area.y1, status_area.x2,
                status_area.y2, slot_area.x1, slot_area.y1, slot_area.x2,
                slot_area.y2, grid_area.x1, grid_area.y1, grid_area.x2,
                grid_area.y2);
      }
      assert(area.x1 == 29 && area.y1 == CM0_STATUS_BAR_HEIGHT);
      assert(lv_obj_get_x(icon) == 33);
      assert(lv_obj_get_y(icon) == 15);
      assert(lv_obj_get_width(icon) == 84);
      assert(lv_obj_get_height(icon) == 84);
      assert(lv_obj_get_class(lv_obj_get_child(icon, 0)) == &lv_image_class);
      assert(lv_obj_get_x(label) == 0);
      assert(lv_obj_get_y(label) == 109);

      lv_obj_t *fallback_card = lv_obj_get_child(page, 1);
      lv_obj_t *fallback_box = lv_obj_get_child(fallback_card, 0);
      assert(lv_obj_get_class(lv_obj_get_child(fallback_box, 0)) ==
             &lv_label_class);
    }
  }

  lv_obj_t *first_card = lv_obj_get_child(lv_obj_get_child(pages, 0), 0);
  assert(lv_obj_send_event(first_card, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
  assert(launcher_ui_take_launch_request(ui) == &registry.apps[0]);
  assert(!launcher_ui_take_launch_request(ui));

  lv_obj_t *indicator = lv_obj_find_by_name(screen, "page_indicator");
  assert(indicator);
  assert(lv_obj_get_child_count(indicator) == 3);
  for (size_t i = 0; i < 3; ++i) {
    lv_obj_t *button = lv_obj_get_child(indicator, (int32_t)i);
    lv_obj_t *mark = lv_obj_get_child(button, 0);
    assert(lv_obj_get_width(mark) == (i == 0 ? 24 : 8));
    assert(lv_obj_get_height(mark) == 8);
  }

  render(display, argv[1]);
  snapshot_status_region();

  assert(lv_obj_get_child_count(status_bar) == 3);
  pointer_point = (lv_point_t){284, 20};
  pointer_state = LV_INDEV_STATE_PRESSED;
  lv_indev_read(input);
  pointer_point = (lv_point_t){286, 120};
  pointer_state = LV_INDEV_STATE_RELEASED;
  lv_indev_read(input);
  lv_obj_t *quick_scrim = lv_obj_find_by_name(screen, "quick_scrim");
  lv_obj_t *quick_panel = lv_obj_find_by_name(screen, "quick_panel");
  assert(quick_scrim);
  assert(quick_panel);
  assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
  lv_tick_inc(200);
  lv_timer_handler();
  render(display, argv[2]);
  assert(lv_obj_send_event(quick_scrim, LV_EVENT_CLICKED, NULL) ==
         LV_RESULT_OK);
  assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
  pointer_point = (lv_point_t){284, 20};
  pointer_state = LV_INDEV_STATE_PRESSED;
  lv_indev_read(input);
  pointer_point = (lv_point_t){286, 120};
  pointer_state = LV_INDEV_STATE_RELEASED;
  lv_indev_read(input);
  assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
  lv_obj_t *orientation_button =
      lv_obj_find_by_name(quick_panel, "orientation_button");
  assert(orientation_button);
  assert(lv_obj_send_event(orientation_button, LV_EVENT_CLICKED, NULL) ==
         LV_RESULT_OK);
  launcher_orientation_t requested_orientation;
  assert(launcher_ui_take_orientation_request(ui, &requested_orientation));
  assert(requested_orientation == LAUNCHER_ORIENTATION_LANDSCAPE);
  assert(!launcher_ui_take_orientation_request(ui, &requested_orientation));

  lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
  launcher_ui_apply_orientation(ui, LAUNCHER_ORIENTATION_LANDSCAPE);
  lv_obj_update_layout(lv_screen_active());
  assert(lv_obj_get_width(lv_screen_active()) == 1232);
  assert(lv_obj_get_height(lv_screen_active()) == 568);

  screen = lv_screen_active();
  status_bar = lv_obj_find_by_name(screen, "status_bar");
  assert(status_bar);
  assert(lv_obj_get_width(status_bar) == 1232);
  assert(lv_obj_get_height(status_bar) == CM0_STATUS_BAR_HEIGHT);

  pages = find_by_class(screen, &lv_tileview_class);
  assert(pages);
  assert(lv_obj_get_x(pages) == 0);
  assert(lv_obj_get_y(pages) == 0);
  assert(lv_obj_get_width(pages) == 1232);
  assert(lv_obj_get_height(pages) == 450);
  assert(lv_obj_get_child_count(pages) == expected_page_count);
  lv_obj_t *landscape_page = lv_obj_get_child(pages, 0);
  lv_obj_t *landscape_card = lv_obj_get_child(landscape_page, 0);
  lv_obj_get_coords(landscape_card, &area);
  if (area.x1 != 47 || area.y1 != CM0_STATUS_BAR_HEIGHT)
    fprintf(stderr, "landscape first card at (%d, %d), expected (47, %d)\n",
            area.x1, area.y1, CM0_STATUS_BAR_HEIGHT);
  assert(area.x1 == 47 && area.y1 == CM0_STATUS_BAR_HEIGHT);

  indicator = lv_obj_find_by_name(screen, "page_indicator");
  assert(indicator);
  assert(lv_obj_get_child_count(indicator) == 3);

  memset(framebuffer, 0, (size_t)WIDTH * HEIGHT * sizeof(*framebuffer));
  render(display, argv[3]);

  launcher_ui_destroy(ui);
  lv_indev_delete(input);
  lv_display_delete(display);

  display = lv_display_create(WIDTH, HEIGHT);
  assert(display);
  lv_display_set_buffers(display, draw_buffer, NULL,
                         WIDTH * PARTIAL_ROWS * sizeof(*draw_buffer),
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, flush_display);
  assert(lv_display_get_theme(display));
  ui = launcher_ui_create(&registry);
  assert(ui);
  assert(lv_display_get_theme(display));
  assert(lv_obj_find_by_name(lv_screen_active(), "app_grid"));
  lv_obj_t *restored_terminal = lv_obj_find_by_name(
      lv_screen_active(), "cm0_status_bar_battery_terminal");
  assert(restored_terminal);
  assert(lv_obj_get_style_bg_opa(restored_terminal, LV_PART_MAIN) ==
         LV_OPA_COVER);
  memset(framebuffer, 0, (size_t)WIDTH * HEIGHT * sizeof(*framebuffer));
  lv_obj_invalidate(lv_screen_active());
  lv_tick_inc(20);
  lv_refr_now(display);
  assert_status_region_unchanged();
  launcher_ui_destroy(ui);
  lv_display_delete(display);

  lv_deinit();
  launcher_registry_destroy(&registry);
  free(draw_buffer);
  free(status_snapshot);
  free(framebuffer);
  return 0;
}
