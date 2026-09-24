#include "registry.h"
#include "ui.h"

#include <cm0/status_bar.h>
#include <cm0/typography.h>

#include <lvgl.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH                      LAUNCHER_DEFAULT_WIDTH
#define HEIGHT                     LAUNCHER_DEFAULT_HEIGHT
#define PARTIAL_ROWS               80
#define STATUS_COMPARE_X           380
#define STATUS_COMPARE_RIGHT       534
#define COLOR_PAGE_BACKGROUND      0xf2f2f7
#define COLOR_CONTAINER_BACKGROUND 0xffffff
#define COLOR_CONTAINER_TITLE      0x1a1a1a
#define COLOR_TEXT                 0x000000

static lv_color32_t *framebuffer;
static lv_color32_t *status_snapshot;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state = LV_INDEV_STATE_RELEASED;

static void read_pointer(lv_indev_t *input, lv_indev_data_t *data)
{
    (void)input;
    data->point = pointer_point;
    data->state = pointer_state;
}

static void flush_display(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    int width                  = area->x2 - area->x1 + 1;
    int display_width          = lv_display_get_horizontal_resolution(display);
    const lv_color32_t *source = (const lv_color32_t *)pixels;
    for (int y = area->y1; y <= area->y2; ++y) {
        memcpy(framebuffer + (size_t)y * display_width + area->x1, source + (size_t)(y - area->y1) * width,
               (size_t)width * sizeof(*source));
    }
    lv_display_flush_ready(display);
}

static void write_ppm(const char *path, int width, int height)
{
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

static void render(lv_display_t *display, const char *path)
{
    lv_obj_invalidate(lv_screen_active());
    lv_tick_inc(20);
    lv_refr_now(display);
    write_ppm(path, lv_display_get_horizontal_resolution(display), lv_display_get_vertical_resolution(display));
}

static void snapshot_status_region(void)
{
    const size_t row_pixels = STATUS_COMPARE_RIGHT - STATUS_COMPARE_X + 1;
    for (int y = 0; y < CM0_STATUS_BAR_HEIGHT; ++y) {
        memcpy(status_snapshot + (size_t)y * row_pixels, framebuffer + (size_t)y * WIDTH + STATUS_COMPARE_X,
               row_pixels * sizeof(*framebuffer));
    }
}

static void assert_status_region_unchanged(void)
{
    const size_t row_pixels   = STATUS_COMPARE_RIGHT - STATUS_COMPARE_X + 1;
    size_t mismatch_count     = 0;
    int first_x               = -1;
    int first_y               = -1;
    int last_x                = -1;
    int last_y                = -1;
    lv_color32_t first_before = {0};
    lv_color32_t first_after  = {0};
    for (int y = 0; y < CM0_STATUS_BAR_HEIGHT; ++y) {
        for (size_t x = 0; x < row_pixels; ++x) {
            const lv_color32_t before = status_snapshot[(size_t)y * row_pixels + x];
            const lv_color32_t after  = framebuffer[(size_t)y * WIDTH + STATUS_COMPARE_X + x];
            if (memcmp(&before, &after, sizeof(before)) != 0) {
                if (first_x < 0) {
                    first_x      = STATUS_COMPARE_X + (int)x;
                    first_y      = y;
                    first_before = before;
                    first_after  = after;
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
                first_x, first_y, last_x, last_y, mismatch_count, first_before.red, first_before.green,
                first_before.blue, first_before.alpha, first_after.red, first_after.green, first_after.blue,
                first_after.alpha);
    assert(mismatch_count == 0);
}

static lv_obj_t *find_by_class(lv_obj_t *parent, const lv_obj_class_t *class_p)
{
    if (lv_obj_has_class(parent, class_p)) return parent;
    uint32_t child_count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *match = find_by_class(lv_obj_get_child(parent, (int32_t)i), class_p);
        if (match) return match;
    }
    return NULL;
}

static lv_obj_t *find_app_tile(lv_obj_t *pages, const launcher_app_t *app)
{
    uint32_t page_count = lv_obj_get_child_count(pages);
    for (uint32_t page_index = 0; page_index < page_count; ++page_index) {
        lv_obj_t *page       = lv_obj_get_child(pages, (int32_t)page_index);
        uint32_t child_count = lv_obj_get_child_count(page);
        for (uint32_t child_index = 0; child_index < child_count; ++child_index) {
            lv_obj_t *child = lv_obj_get_child(page, (int32_t)child_index);
            if (lv_obj_get_user_data(child) == app) return child;
        }
    }
    return NULL;
}

static void select_page(lv_obj_t *pages, uint32_t page_index)
{
    lv_tileview_set_tile_by_index(pages, page_index, 0, LV_ANIM_OFF);
    assert(lv_obj_send_event(pages, LV_EVENT_VALUE_CHANGED, NULL) == LV_RESULT_OK);
}

static uint32_t active_page_index(lv_obj_t *pages)
{
    lv_obj_t *active = lv_tileview_get_tile_active(pages);
    assert(active);
    return lv_obj_get_index(active);
}

static void mark_deleted(lv_event_t *event)
{
    bool *deleted = lv_event_get_user_data(event);
    *deleted      = true;
}

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr,
                "usage: %s PORTRAIT.ppm QUICK_CONTROLS.ppm LANDSCAPE.ppm "
                "COMPACT.ppm COMPACT_QUICK_CONTROLS.ppm\n",
                argv[0]);
        return 2;
    }
    framebuffer = calloc((size_t)WIDTH * HEIGHT, sizeof(*framebuffer));
    status_snapshot =
        calloc((size_t)(STATUS_COMPARE_RIGHT - STATUS_COMPARE_X + 1) * CM0_STATUS_BAR_HEIGHT, sizeof(*status_snapshot));
    lv_color32_t *draw_buffer = calloc((size_t)WIDTH * PARTIAL_ROWS, sizeof(*draw_buffer));
    assert(framebuffer && status_snapshot && draw_buffer);

    launcher_registry_t registry;
    launcher_registry_init(&registry);
    assert(launcher_registry_scan(&registry, LAUNCHER_TEST_APP_DIR) >= 1);
    launcher_registry_sort(&registry);
    char fixture_icon_path[LAUNCHER_PATH_MAX];
    snprintf(fixture_icon_path, sizeof(fixture_icon_path), "%s", registry.apps[0].icon_path);

    static const char *first_page_names[] = {
        "Squareline", "Calculator", "2048 Game", "Camera",  "Music", "Lora",     "Settings", "Led",      "Face_detect",
        "Clock",      "Artboard",   "Ble",       "Battery", "Video", "LiveCall", "Weather",  "KeyBoard", "Gnss",
    };
    static const char *first_page_icons[] = {
        "home",  "calculator", "play", "camera",  "music", "lora",      "settings", "led",  "image",
        "clock", "artboard",   "ble",  "battery", "video", "live-call", "weather",  "list", "gnss",
    };
    launcher_app_t fixture = registry.apps[0];
    for (size_t i = 0; i < 37; ++i) {
        registry.apps[i]              = fixture;
        registry.apps[i].icon_path[0] = '\0';
        snprintf(registry.apps[i].manifest.id, sizeof(registry.apps[i].manifest.id), "cc.lilygo.ui.Render%zu", i);
        if (i < 18) {
            snprintf(registry.apps[i].manifest.name, sizeof(registry.apps[i].manifest.name), "%s", first_page_names[i]);
            snprintf(registry.apps[i].manifest.icon, sizeof(registry.apps[i].manifest.icon), "%s", first_page_icons[i]);
        } else {
            snprintf(registry.apps[i].manifest.name, sizeof(registry.apps[i].manifest.name), "App %zu", i + 1);
        }
        registry.apps[i].manifest.order = (int)i;
    }
    snprintf(registry.apps[0].icon_path, sizeof(registry.apps[0].icon_path), "%s", fixture_icon_path);
    snprintf(registry.apps[1].manifest.icon, sizeof(registry.apps[1].manifest.icon), "%s", "missing.png");
    registry.count = 37;

    lv_init();
    lv_font_glyph_dsc_t glyph;
    assert(lv_font_get_glyph_dsc(lilygo_ui_font_get(22), &glyph, 'A', 0));
    assert(lv_font_get_glyph_dsc(lilygo_ui_font_get(22), &glyph, 0x8bbe, 0));
    lv_display_t *display = lv_display_create(WIDTH, HEIGHT);
    assert(display);
    lv_display_set_buffers(display, draw_buffer, NULL, WIDTH * PARTIAL_ROWS * sizeof(*draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);

    lv_indev_t *input = lv_indev_create();
    assert(input);
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, display);
    lv_indev_set_read_cb(input, read_pointer);

    lv_group_t *focus_group = lv_group_create();
    assert(focus_group);
    lv_group_set_default(focus_group);
    const uint32_t input_event_count = lv_indev_get_event_count(input);
    lv_obj_t *previous_screen        = lv_screen_active();
    launcher_ui_t *ui                = launcher_ui_create(&registry);
    assert(ui);
    assert(lv_indev_get_event_count(input) == input_event_count + 3);
    lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_get_width(lv_screen_active()) == 568);
    assert(lv_obj_get_height(lv_screen_active()) == 1232);

    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *shutdown_scrim = lv_obj_find_by_name(screen, "shutdown_scrim");
    lv_obj_t *shutdown_panel = lv_obj_find_by_name(screen, "shutdown_panel");
    lv_obj_t *shutdown_title = lv_obj_find_by_name(screen, "shutdown_title");
    lv_obj_t *shutdown_hint  = lv_obj_find_by_name(screen, "shutdown_hint");
    assert(shutdown_scrim && shutdown_panel && shutdown_title && shutdown_hint);
    assert(lv_obj_has_flag(shutdown_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(shutdown_title), "正在关机") == 0);
    assert(strcmp(lv_label_get_text(shutdown_hint), "请继续按住电源键") == 0);
    assert(lv_color_eq(lv_obj_get_style_bg_color(screen, LV_PART_MAIN), lv_color_hex(COLOR_PAGE_BACKGROUND)));
    assert(lv_obj_get_style_bg_opa(screen, LV_PART_MAIN) == LV_OPA_COVER);
    assert(lv_obj_get_style_bg_grad_dir(screen, LV_PART_MAIN) == LV_GRAD_DIR_NONE);
    lv_obj_t *status_bar = lv_obj_find_by_name(screen, "status_bar");
    assert(status_bar);
    lv_obj_t *time_container = lv_obj_get_child(status_bar, 0);
    lv_obj_t *levels         = lv_obj_get_child(status_bar, 2);
    lv_obj_t *clock          = lv_obj_get_child(time_container, 0);
    lv_obj_t *keyboard       = lv_obj_find_by_name(levels, "cm0_status_bar_keyboard");
    lv_obj_t *ethernet       = lv_obj_find_by_name(levels, "cm0_status_bar_ethernet");
    lv_obj_t *wifi           = lv_obj_find_by_name(levels, "cm0_status_bar_wifi");
    lv_obj_t *battery        = lv_obj_find_by_name(levels, "cm0_status_bar_battery");
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
    assert(lv_obj_get_height(pages) == 1044);
    size_t expected_page_count = (registry.count + LAUNCHER_APPS_PER_PAGE - 1) / LAUNCHER_APPS_PER_PAGE;
    assert(lv_obj_get_child_count(pages) == expected_page_count);
    for (size_t i = 0; i < expected_page_count; ++i) {
        lv_obj_t *page       = lv_obj_get_child(pages, (int32_t)i);
        size_t remaining     = registry.count - i * LAUNCHER_APPS_PER_PAGE;
        size_t expected_apps = remaining < LAUNCHER_APPS_PER_PAGE ? remaining : LAUNCHER_APPS_PER_PAGE;
        assert(lv_obj_get_child_count(page) == expected_apps);
        if (expected_apps > 0 && i == 0) {
            lv_obj_t *card  = lv_obj_get_child(page, 0);
            lv_obj_t *icon  = lv_obj_get_child(card, 0);
            lv_obj_t *label = lv_obj_get_child(card, 1);
            lv_obj_get_coords(card, &area);
            if (area.x1 != 29 || area.y1 != CM0_STATUS_BAR_HEIGHT) {
                lv_area_t status_area;
                lv_area_t slot_area;
                lv_area_t grid_area;
                lv_obj_get_coords(status_bar, &status_area);
                lv_obj_get_coords(lv_obj_get_parent(status_bar), &slot_area);
                lv_obj_get_coords(lv_obj_get_parent(pages), &grid_area);
                fprintf(stderr, "portrait first card at (%d, %d), expected (29, %d)\n", area.x1, area.y1,
                        CM0_STATUS_BAR_HEIGHT);
                fprintf(stderr,
                        "status=(%d,%d)-(%d,%d) slot=(%d,%d)-(%d,%d) "
                        "grid=(%d,%d)-(%d,%d)\n",
                        status_area.x1, status_area.y1, status_area.x2, status_area.y2, slot_area.x1, slot_area.y1,
                        slot_area.x2, slot_area.y2, grid_area.x1, grid_area.y1, grid_area.x2, grid_area.y2);
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
            lv_obj_t *fallback_box  = lv_obj_get_child(fallback_card, 0);
            assert(lv_obj_get_class(lv_obj_get_child(fallback_box, 0)) == &lv_label_class);
        }
    }

    lv_obj_t *first_card      = lv_obj_get_child(lv_obj_get_child(pages, 0), 0);
    lv_obj_t *first_card_name = lv_obj_get_child(first_card, 1);
    assert(lv_color_eq(lv_obj_get_style_text_color(first_card_name, LV_PART_MAIN), lv_color_hex(COLOR_TEXT)));
    assert(lv_obj_send_event(first_card, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(launcher_ui_take_launch_request(ui) == &registry.apps[0]);
    assert(!launcher_ui_take_launch_request(ui));

    lv_obj_t *indicator = lv_obj_find_by_name(screen, "page_indicator");
    assert(indicator);
    assert(lv_obj_get_child_count(indicator) == 3);
    for (size_t i = 0; i < 3; ++i) {
        lv_obj_t *button = lv_obj_get_child(indicator, (int32_t)i);
        lv_obj_t *mark   = lv_obj_get_child(button, 0);
        assert(lv_obj_get_width(button) == 40);
        assert(lv_obj_get_height(button) == 40);
        assert(lv_obj_get_width(mark) == (i == 0 ? 24 : 8));
        assert(lv_obj_get_height(mark) == 8);
    }

    render(display, argv[1]);
    snapshot_status_region();

    select_page(pages, 1);
    lv_obj_t *focused_card = find_app_tile(pages, &registry.apps[19]);
    assert(focused_card);
    lv_group_focus_obj(focused_card);
    assert(lv_group_get_focused(focus_group) == focused_card);
    assert(active_page_index(pages) == 1);
    assert(lv_obj_send_event(focused_card, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);

    char focused_app_id[LAUNCHER_ID_MAX];
    snprintf(focused_app_id, sizeof(focused_app_id), "%s", registry.apps[19].manifest.id);
    launcher_ui_prepare_registry_refresh(ui);
    launcher_app_t reordered_app = registry.apps[18];
    registry.apps[18]            = registry.apps[19];
    registry.apps[19]            = reordered_app;
    launcher_ui_refresh_home(ui);
    pages        = find_by_class(screen, &lv_tileview_class);
    focused_card = lv_group_get_focused(focus_group);
    assert(pages);
    assert(focused_card);
    assert(strcmp(((launcher_app_t *)lv_obj_get_user_data(focused_card))->manifest.id, focused_app_id) == 0);
    const launcher_app_t *pending_app = launcher_ui_take_launch_request(ui);
    assert(pending_app == &registry.apps[18]);
    assert(strcmp(pending_app->manifest.id, focused_app_id) == 0);
    assert(lv_obj_send_event(focused_card, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);

    launcher_ui_prepare_registry_refresh(ui);
    reordered_app     = registry.apps[18];
    registry.apps[18] = registry.apps[19];
    registry.apps[19] = reordered_app;
    launcher_ui_refresh_home(ui);
    pages        = find_by_class(screen, &lv_tileview_class);
    focused_card = lv_group_get_focused(focus_group);
    assert(pages);
    assert(focused_card);
    assert(lv_obj_get_user_data(focused_card) == &registry.apps[19]);
    assert(active_page_index(pages) == 1);

    bool original_pages_deleted = false;
    lv_obj_add_event_cb(pages, mark_deleted, LV_EVENT_DELETE, &original_pages_deleted);
    lv_display_set_resolution(display, 550, 1200);
    lv_obj_update_layout(screen);
    assert(lv_obj_get_width(screen) == 550);
    assert(lv_obj_get_height(screen) == 1200);
    assert(find_by_class(screen, &lv_tileview_class) == pages);
    assert(!original_pages_deleted);
    assert(lv_obj_get_height(pages) == 1012);
    assert(active_page_index(pages) == 1);
    assert(lv_obj_get_scroll_x(pages) == 550);
    assert(lv_group_get_focused(focus_group) == focused_card);

    lv_display_set_resolution(display, 568, 420);
    lv_obj_update_layout(screen);
    assert(original_pages_deleted);
    pages = find_by_class(screen, &lv_tileview_class);
    assert(pages);
    assert(lv_obj_get_width(pages) == 568);
    assert(lv_obj_get_height(pages) == 246);
    assert(lv_obj_get_child_count(pages) == 7);
    assert(active_page_index(pages) == 3);
    assert(lv_obj_get_scroll_x(pages) == 3 * 568);
    lv_obj_t *compact_active_page = lv_tileview_get_tile_active(pages);
    assert(lv_obj_get_child_count(compact_active_page) == 6);
    assert(lv_obj_get_user_data(lv_obj_get_child(compact_active_page, 0)) == &registry.apps[18]);
    focused_card = lv_group_get_focused(focus_group);
    assert(focused_card);
    assert(lv_obj_get_user_data(focused_card) == &registry.apps[19]);
    assert(lv_obj_get_parent(focused_card) == compact_active_page);
    assert(lv_obj_get_height(focused_card) == 123);
    assert(launcher_ui_take_launch_request(ui) == &registry.apps[19]);
    assert(!launcher_ui_take_launch_request(ui));
    indicator = lv_obj_find_by_name(screen, "page_indicator");
    assert(indicator);
    assert(lv_obj_get_child_count(indicator) == 7);
    assert(lv_obj_get_width(lv_obj_get_child(lv_obj_get_child(indicator, 3), 0)) == 24);

    bool compact_pages_deleted = false;
    lv_obj_add_event_cb(pages, mark_deleted, LV_EVENT_DELETE, &compact_pages_deleted);
    lv_display_set_resolution(display, 320, 240);
    lv_obj_update_layout(screen);
    assert(compact_pages_deleted);
    pages = find_by_class(screen, &lv_tileview_class);
    assert(pages);
    assert(lv_obj_get_width(screen) == 320);
    assert(lv_obj_get_height(screen) == 240);
    assert(lv_obj_get_width(pages) == 320);
    assert(lv_obj_get_height(pages) == 96);
    assert(lv_obj_get_child_count(pages) == 19);
    assert(active_page_index(pages) == 9);
    assert(lv_obj_get_scroll_x(pages) == 9 * 320);
    lv_obj_t *tiny_active_page = lv_tileview_get_tile_active(pages);
    assert(lv_obj_get_child_count(tiny_active_page) == 2);
    assert(lv_obj_get_user_data(lv_obj_get_child(tiny_active_page, 0)) == &registry.apps[18]);
    focused_card = lv_group_get_focused(focus_group);
    assert(focused_card);
    assert(lv_obj_get_user_data(focused_card) == &registry.apps[19]);
    assert(lv_obj_get_parent(focused_card) == tiny_active_page);
    assert(lv_obj_get_width(focused_card) == 150);
    assert(lv_obj_get_height(focused_card) == 96);
    lv_obj_t *tiny_icon  = lv_obj_get_child(focused_card, 0);
    lv_obj_t *tiny_label = lv_obj_get_child(focused_card, 1);
    lv_area_t tiny_card_area;
    lv_area_t tiny_icon_area;
    lv_area_t tiny_label_area;
    lv_obj_get_coords(focused_card, &tiny_card_area);
    lv_obj_get_coords(tiny_icon, &tiny_icon_area);
    lv_obj_get_coords(tiny_label, &tiny_label_area);
    assert(tiny_icon_area.y1 >= tiny_card_area.y1);
    assert(tiny_icon_area.y2 < tiny_label_area.y1);
    assert(tiny_label_area.y2 <= tiny_card_area.y2);
    lv_obj_t *home_indicator = lv_obj_find_by_name(screen, "home_indicator_slot");
    assert(home_indicator);
    assert(lv_obj_has_flag(home_indicator, LV_OBJ_FLAG_HIDDEN));
    indicator = lv_obj_find_by_name(screen, "page_indicator");
    assert(indicator);
    assert(lv_obj_get_child_count(indicator) == 19);
    assert(lv_obj_get_width(indicator) <= 320);
    assert(lv_obj_has_flag(indicator, LV_OBJ_FLAG_SCROLLABLE));
    lv_obj_t *tiny_active_mark = lv_obj_get_child(lv_obj_get_child(indicator, 9), 0);
    assert(lv_obj_get_width(lv_obj_get_child(indicator, 0)) == 40);
    assert(lv_obj_get_width(lv_obj_get_child(indicator, 9)) == 40);
    assert(lv_obj_get_width(tiny_active_mark) == 24);
    lv_area_t indicator_area;
    lv_area_t active_mark_area;
    lv_obj_get_coords(indicator, &indicator_area);
    lv_obj_get_coords(tiny_active_mark, &active_mark_area);
    assert(active_mark_area.x1 >= indicator_area.x1);
    assert(active_mark_area.x2 <= indicator_area.x2);
    lv_obj_t *tiny_orientation_label  = lv_obj_find_by_name(screen, "orientation_label");
    lv_obj_t *tiny_orientation_button = lv_obj_find_by_name(screen, "orientation_button");
    lv_obj_t *tiny_quick_panel        = lv_obj_find_by_name(screen, "quick_panel");
    lv_obj_t *tiny_quick_scrim        = lv_obj_find_by_name(screen, "quick_scrim");
    lv_obj_t *tiny_quick_handle       = lv_obj_find_by_name(screen, "quick_panel_handle");
    assert(tiny_orientation_label);
    assert(tiny_orientation_button);
    assert(tiny_quick_panel);
    assert(tiny_quick_scrim);
    assert(tiny_quick_handle);
    assert(strcmp(lv_label_get_text(tiny_orientation_label), "Portrait") == 0);
    pointer_point = (lv_point_t){160, 20};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    pointer_point = (lv_point_t){160, 120};
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    assert(!lv_obj_has_flag(tiny_quick_scrim, LV_OBJ_FLAG_HIDDEN));
    lv_tick_inc(200);
    lv_timer_handler();
    lv_obj_update_layout(screen);
    lv_area_t tiny_panel_area;
    lv_area_t tiny_orientation_button_area;
    lv_area_t tiny_quick_handle_area;
    lv_obj_get_coords(tiny_quick_panel, &tiny_panel_area);
    lv_obj_get_coords(tiny_orientation_button, &tiny_orientation_button_area);
    lv_obj_get_coords(tiny_quick_handle, &tiny_quick_handle_area);
    assert(tiny_panel_area.y1 >= 0);
    assert(tiny_panel_area.y2 < 240);
    assert(tiny_orientation_button_area.x1 >= tiny_panel_area.x1);
    assert(tiny_orientation_button_area.x2 <= tiny_panel_area.x2);
    assert(tiny_orientation_button_area.y1 >= tiny_panel_area.y1);
    assert(tiny_orientation_button_area.y2 <= tiny_panel_area.y2);
    assert(tiny_quick_handle_area.x1 >= tiny_panel_area.x1);
    assert(tiny_quick_handle_area.x2 <= tiny_panel_area.x2);
    assert(tiny_quick_handle_area.y1 >= tiny_panel_area.y1);
    assert(tiny_quick_handle_area.y2 <= tiny_panel_area.y2);
    memset(framebuffer, 0, (size_t)WIDTH * HEIGHT * sizeof(*framebuffer));
    render(display, argv[5]);
    assert(lv_obj_send_event(tiny_orientation_button, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    launcher_orientation_t tiny_requested_orientation;
    assert(launcher_ui_take_orientation_request(ui, &tiny_requested_orientation));
    assert(tiny_requested_orientation == LAUNCHER_ORIENTATION_PORTRAIT);
    lv_tick_inc(200);
    lv_timer_handler();
    assert(lv_obj_has_flag(tiny_quick_scrim, LV_OBJ_FLAG_HIDDEN));
    memset(framebuffer, 0, (size_t)WIDTH * HEIGHT * sizeof(*framebuffer));
    render(display, argv[4]);

    bool tiny_pages_deleted = false;
    lv_obj_add_event_cb(pages, mark_deleted, LV_EVENT_DELETE, &tiny_pages_deleted);
    lv_display_set_resolution(display, WIDTH, HEIGHT);
    lv_obj_update_layout(screen);
    assert(tiny_pages_deleted);
    pages = find_by_class(screen, &lv_tileview_class);
    assert(pages);
    assert(lv_obj_get_width(pages) == WIDTH);
    assert(lv_obj_get_height(pages) == 1044);
    assert(lv_obj_get_child_count(pages) == expected_page_count);
    assert(active_page_index(pages) == 1);
    assert(lv_obj_get_scroll_x(pages) == WIDTH);
    focused_card = lv_group_get_focused(focus_group);
    assert(focused_card);
    assert(lv_obj_get_user_data(focused_card) == &registry.apps[19]);
    assert(lv_obj_get_parent(focused_card) == lv_obj_get_child(pages, 1));

    select_page(pages, 0);
    first_card = find_app_tile(pages, &registry.apps[0]);
    assert(first_card);
    lv_group_focus_obj(first_card);

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
    assert(lv_color_eq(lv_obj_get_style_bg_color(quick_panel, LV_PART_MAIN), lv_color_hex(COLOR_CONTAINER_BACKGROUND)));
    assert(lv_obj_get_style_bg_opa(quick_panel, LV_PART_MAIN) == LV_OPA_COVER);
    lv_obj_t *quick_panel_title = lv_obj_find_by_name(quick_panel, "quick_panel_title");
    assert(quick_panel_title);
    assert(
        lv_color_eq(lv_obj_get_style_text_color(quick_panel_title, LV_PART_MAIN), lv_color_hex(COLOR_CONTAINER_TITLE)));
    lv_obj_t *orientation_label = lv_obj_find_by_name(quick_panel, "orientation_label");
    assert(orientation_label);
    assert(lv_color_eq(lv_obj_get_style_text_color(orientation_label, LV_PART_MAIN), lv_color_hex(COLOR_TEXT)));
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    lv_tick_inc(200);
    lv_timer_handler();
    render(display, argv[2]);
    assert(lv_obj_send_event(quick_scrim, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    pointer_point = (lv_point_t){284, 20};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    pointer_point = (lv_point_t){286, 120};
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t *orientation_button = lv_obj_find_by_name(quick_panel, "orientation_button");
    assert(orientation_button);
    assert(lv_obj_send_event(orientation_button, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    launcher_orientation_t requested_orientation;
    assert(launcher_ui_take_orientation_request(ui, &requested_orientation));
    assert(requested_orientation == LAUNCHER_ORIENTATION_LANDSCAPE);
    assert(!launcher_ui_take_orientation_request(ui, &requested_orientation));

    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
    launcher_ui_apply_orientation(ui, LAUNCHER_ORIENTATION_LANDSCAPE);
    lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_get_width(lv_screen_active()) == 1232);
    assert(lv_obj_get_height(lv_screen_active()) == 568);

    screen     = lv_screen_active();
    status_bar = lv_obj_find_by_name(screen, "status_bar");
    assert(status_bar);
    assert(lv_obj_get_width(status_bar) == 1232);
    assert(lv_obj_get_height(status_bar) == CM0_STATUS_BAR_HEIGHT);

    pages = find_by_class(screen, &lv_tileview_class);
    assert(pages);
    assert(lv_obj_get_x(pages) == 0);
    assert(lv_obj_get_y(pages) == 0);
    assert(lv_obj_get_width(pages) == 1232);
    assert(lv_obj_get_height(pages) == 394);
    assert(lv_obj_get_child_count(pages) == expected_page_count);
    lv_obj_t *landscape_page = lv_obj_get_child(pages, 0);
    lv_obj_t *landscape_card = lv_obj_get_child(landscape_page, 0);
    assert(lv_obj_get_height(landscape_card) == 131);
    lv_obj_get_coords(landscape_card, &area);
    if (area.x1 != 47 || area.y1 != CM0_STATUS_BAR_HEIGHT)
        fprintf(stderr, "landscape first card at (%d, %d), expected (47, %d)\n", area.x1, area.y1,
                CM0_STATUS_BAR_HEIGHT);
    assert(area.x1 == 47 && area.y1 == CM0_STATUS_BAR_HEIGHT);

    indicator = lv_obj_find_by_name(screen, "page_indicator");
    assert(indicator);
    assert(lv_obj_get_child_count(indicator) == 3);

    memset(framebuffer, 0, (size_t)WIDTH * HEIGHT * sizeof(*framebuffer));
    render(display, argv[3]);

    /* LVGL rotates raw input points into the landscape coordinate space. */
    pointer_point = (lv_point_t){20, 615};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    pointer_point = (lv_point_t){120, 615};
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    lv_tick_inc(200);
    lv_timer_handler();
    lv_obj_update_layout(screen);
    assert(lv_obj_get_y(quick_panel) == 0);
    assert(lv_obj_send_event(quick_scrim, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    lv_tick_inc(60);
    lv_timer_handler();
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);
    lv_obj_update_layout(screen);
    lv_tick_inc(200);
    lv_timer_handler();
    assert(lv_obj_get_height(quick_panel) == 260);
    assert(lv_obj_get_y(quick_panel) == -260);

    /* Retaining the UI across DRM handoff must not lose gestures when the
     * exclusive evdev input is closed and recreated. Deleting mid-gesture also
     * discards the old press instead of carrying it into the new input. */
    launcher_ui_rebind_input(NULL);
    launcher_ui_rebind_input(ui);
    assert(lv_indev_get_event_count(input) == input_event_count + 3);
    pointer_point = (lv_point_t){284, 20};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    lv_indev_delete(input);
    launcher_ui_rebind_input(ui);
    assert(lv_screen_active() == screen);
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));

    input = lv_indev_create();
    assert(input);
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, display);
    lv_indev_set_read_cb(input, read_pointer);
    assert(lv_indev_get_event_count(input) == input_event_count);
    launcher_ui_rebind_input(ui);
    launcher_ui_rebind_input(ui);
    assert(lv_indev_get_event_count(input) == input_event_count + 3);
    pointer_point = (lv_point_t){286, 120};
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    pointer_point = (lv_point_t){284, 20};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    pointer_point = (lv_point_t){286, 120};
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    lv_tick_inc(200);
    lv_timer_handler();
    lv_obj_update_layout(screen);
    assert(lv_obj_get_y(quick_panel) == 0);
    assert(lv_obj_send_event(quick_scrim, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);

    lv_obj_t *launcher_screen = screen;
    launcher_ui_destroy(ui);
    assert(lv_screen_active() == previous_screen);
    assert(!lv_obj_is_valid(launcher_screen));
    assert(lv_indev_get_event_count(input) == input_event_count);
    lv_indev_delete(input);
    lv_display_delete(display);

    display = lv_display_create(WIDTH, HEIGHT);
    assert(display);
    lv_display_set_buffers(display, draw_buffer, NULL, WIDTH * PARTIAL_ROWS * sizeof(*draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    assert(lv_display_get_theme(display));
    previous_screen = lv_screen_active();
    ui              = launcher_ui_create(&registry);
    assert(ui);
    launcher_screen = lv_screen_active();
    assert(lv_display_get_theme(display));
    assert(lv_obj_find_by_name(lv_screen_active(), "app_grid"));
    lv_obj_t *restored_terminal = lv_obj_find_by_name(lv_screen_active(), "cm0_status_bar_battery_terminal");
    assert(restored_terminal);
    assert(lv_obj_get_style_bg_opa(restored_terminal, LV_PART_MAIN) == LV_OPA_COVER);
    memset(framebuffer, 0, (size_t)WIDTH * HEIGHT * sizeof(*framebuffer));
    lv_obj_invalidate(lv_screen_active());
    lv_tick_inc(20);
    lv_refr_now(display);
    assert_status_region_unchanged();
    launcher_ui_show_shutdown(ui);
    lv_obj_t *restored_shutdown = lv_obj_find_by_name(lv_screen_active(), "shutdown_scrim");
    assert(restored_shutdown && !lv_obj_has_flag(restored_shutdown, LV_OBJ_FLAG_HIDDEN));
    launcher_ui_hide_shutdown(ui);
    assert(lv_obj_has_flag(restored_shutdown, LV_OBJ_FLAG_HIDDEN));
    launcher_ui_destroy(ui);
    assert(lv_screen_active() == previous_screen);
    assert(!lv_obj_is_valid(launcher_screen));
    lv_display_delete(display);

    display = lv_display_create(WIDTH, HEIGHT);
    assert(display);
    lv_display_set_buffers(display, draw_buffer, NULL, WIDTH * PARTIAL_ROWS * sizeof(*draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    input = lv_indev_create();
    assert(input);
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(input, display);
    lv_indev_set_read_cb(input, read_pointer);
    const uint32_t display_first_input_events = lv_indev_get_event_count(input);
    ui                                        = launcher_ui_create(&registry);
    assert(ui);
    launcher_screen = lv_screen_active();
    assert(lv_indev_get_event_count(input) == display_first_input_events + 3);
    lv_display_delete(display);
    assert(!lv_obj_is_valid(launcher_screen));
    launcher_ui_destroy(ui);
    assert(lv_indev_get_event_count(input) == display_first_input_events);
    lv_indev_delete(input);

    lv_group_set_default(NULL);
    lv_group_delete(focus_group);

    lilygo_ui_fonts_deinit();
    lv_deinit();
    launcher_registry_destroy(&registry);
    free(draw_buffer);
    free(status_snapshot);
    free(framebuffer);
    return 0;
}
