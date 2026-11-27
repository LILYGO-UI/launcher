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
#define COLOR_CONTROL_BACKGROUND   0xe6e8ec
#define COLOR_CONTROL_PRESSED      0xd8dbe1
#define COLOR_CONTROL_FOCUS        0x007aff

static lv_color32_t *framebuffer;
static lv_color32_t *status_snapshot;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state = LV_INDEV_STATE_RELEASED;

static lv_color32_t rgb565_pixel(uint16_t packed)
{
    lv_color32_t result = {0};
    result.red = (uint8_t)((((packed >> 11) & 31U) * 255U + 15U) / 31U);
    result.green = (uint8_t)((((packed >> 5) & 63U) * 255U + 31U) / 63U);
    result.blue = (uint8_t)(((packed & 31U) * 255U + 15U) / 31U);
    result.alpha = 255;
    return result;
}

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
    const lv_color_format_t format = lv_display_get_color_format(display);
    const uint32_t stride = lv_draw_buf_width_to_stride(width, format);
    assert(format == LV_COLOR_FORMAT_RGB565 || format == LV_COLOR_FORMAT_XRGB8888 ||
           format == LV_COLOR_FORMAT_ARGB8888);
    for (int y = area->y1; y <= area->y2; ++y) {
        const uint8_t *source = pixels + (size_t)(y - area->y1) * stride;
        lv_color32_t *destination = framebuffer + (size_t)y * display_width + area->x1;
        if (format == LV_COLOR_FORMAT_RGB565) {
            for (int x = 0; x < width; ++x) {
                uint16_t packed;
                memcpy(&packed, source + (size_t)x * 2, sizeof(packed));
                destination[x] = rgb565_pixel(packed);
            }
        } else {
            memcpy(destination, source, (size_t)width * sizeof(*destination));
        }
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

static void render_reboot_state(lv_display_t *display, const char *filename)
{
    const char *dir = getenv("AGENT_HIL_SCREENSHOT_DIR");
    char path[4096];
    int length = snprintf(path, sizeof(path), "%s/%s", dir && *dir ? dir : ".", filename);
    assert(length >= 0 && (size_t)length < sizeof(path));
    render(display, path);
}

static void assert_file_share_content_fits(lv_obj_t *screen)
{
    lv_obj_t *panel = lv_obj_find_by_name(screen, "file_share_panel");
    assert(panel);
    lv_obj_update_layout(screen);
    lv_area_t bounds;
    lv_obj_get_coords(panel, &bounds);
    const char *names[] = {"file_share_title", "file_share_path", "file_share_status", "file_share_close_button"};
    for (unsigned index = 0; index < sizeof(names) / sizeof(names[0]); ++index) {
        lv_obj_t *child = lv_obj_find_by_name(panel, names[index]);
        lv_area_t area;
        assert(child);
        lv_obj_get_coords(child, &area);
        assert(area.x1 >= bounds.x1 && area.x2 <= bounds.x2);
        assert(area.y1 >= bounds.y1 && area.y2 <= bounds.y2);
    }
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

static void assert_session_actions_fit(lv_obj_t *screen)
{
    lv_obj_t *panel = lv_obj_find_by_name(screen, "quick_panel");
    lv_obj_t *controls[] = {
        lv_obj_find_by_name(screen, "orientation_button"),
        lv_obj_find_by_name(screen, "file_share_button"),
        lv_obj_find_by_name(screen, "lock_button"),
        lv_obj_find_by_name(screen, "session_button"),
    };
    lv_obj_t *labels[] = {
        lv_obj_find_by_name(screen, "orientation_label"),
        lv_obj_find_by_name(screen, "file_share_label"),
        lv_obj_find_by_name(screen, "lock_label"),
        lv_obj_find_by_name(screen, "session_label"),
    };
    lv_obj_t *handle = lv_obj_find_by_name(screen, "quick_panel_handle");
    assert(panel && handle);
    lv_area_t panel_area, handle_area, control_areas[4];
    lv_obj_get_coords(panel, &panel_area);
    lv_obj_get_coords(handle, &handle_area);
    assert(handle_area.y2 <= panel_area.y2);
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        assert(controls[i] && labels[i]);
        assert(!lv_obj_has_flag(controls[i], LV_OBJ_FLAG_HIDDEN));
        lv_area_t *control_area = &control_areas[i];
        lv_area_t label_area;
        lv_obj_get_coords(controls[i], control_area);
        lv_obj_get_coords(labels[i], &label_area);
        assert(lv_area_get_width(control_area) == lv_area_get_width(&control_areas[0]));
        assert(lv_area_get_height(control_area) == lv_area_get_height(&control_areas[0]));
        assert(control_area->x1 >= panel_area.x1 && control_area->x2 <= panel_area.x2);
        /* Cards live in a scrolling content viewport.  On compact displays
         * lower cards may extend beneath the fixed handle until scrolled into
         * view; keep the panel bounds and non-overlap checks here and verify
         * focused outlines against the viewport separately. */
        assert(control_area->y1 >= panel_area.y1);
        assert(label_area.x1 >= control_area->x1 && label_area.x2 <= control_area->x2);
        assert(label_area.y1 >= control_area->y1 && label_area.y2 <= control_area->y2);
        for (size_t j = 0; j < i; ++j) {
            const lv_area_t *other_area = &control_areas[j];
            assert(control_area->x2 < other_area->x1 || other_area->x2 < control_area->x1 ||
                   control_area->y2 < other_area->y1 || other_area->y2 < control_area->y1);
        }
    }
    assert(strcmp(lv_label_get_text(labels[1]), "File sharing") == 0);
    assert(strcmp(lv_label_get_text(labels[2]), "Lock screen") == 0);
}

static bool has_non_label_child(lv_obj_t *parent, lv_obj_t *label)
{
    const uint32_t child_count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < child_count; ++i) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);
        if (child != label && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) return true;
    }
    return false;
}

static void assert_session_action_icons(lv_obj_t *screen)
{
    lv_obj_t *lock = lv_obj_find_by_name(screen, "lock_button");
    lv_obj_t *logout = lv_obj_find_by_name(screen, "session_button");
    lv_obj_t *share = lv_obj_find_by_name(screen, "file_share_button");
    lv_obj_t *lock_label = lv_obj_find_by_name(screen, "lock_label");
    lv_obj_t *logout_label = lv_obj_find_by_name(screen, "session_label");
    lv_obj_t *lock_icon = lv_obj_find_by_name(screen, "lock_icon");
    lv_obj_t *logout_icon = lv_obj_find_by_name(screen, "session_icon");
    lv_obj_t *share_label = lv_obj_find_by_name(screen, "file_share_label");
    lv_obj_t *share_icon = lv_obj_find_by_name(screen, "file_share_icon");
    assert(lock && logout && share && lock_label && logout_label && share_label && lock_icon && logout_icon && share_icon);
    assert(lv_label_get_text(lock_icon) && lv_label_get_text(lock_icon)[0] != '\0');
    assert(lv_label_get_text(logout_icon) && lv_label_get_text(logout_icon)[0] != '\0');
    assert(lv_label_get_text(share_icon) && lv_label_get_text(share_icon)[0] != '\0');
    /* Lock and Sign out are represented by an icon plus their existing text.
     * Keep the check structural so it remains valid if the icon glyph or
     * drawing primitive changes with the bundled font set. */
    assert(has_non_label_child(lock, lock_label));
    assert(has_non_label_child(logout, logout_label));
    assert(has_non_label_child(share, share_label));
}

static void assert_quick_control_style(lv_obj_t *orientation, lv_obj_t *share, lv_obj_t *lock, lv_obj_t *logout)
{
    lv_obj_t *controls[] = {orientation, share, lock, logout};
    for (size_t i = 0; i < sizeof(controls) / sizeof(controls[0]); ++i) {
        lv_obj_t *control = controls[i];
        assert(lv_color_eq(lv_obj_get_style_bg_color(control, LV_PART_MAIN), lv_color_hex(COLOR_CONTROL_BACKGROUND)));
        assert(lv_obj_get_style_bg_opa(control, LV_PART_MAIN) == LV_OPA_COVER);
        assert(lv_color_eq(lv_obj_get_style_text_color(control, LV_PART_MAIN), lv_color_hex(COLOR_TEXT)));
        assert(lv_obj_get_style_border_width(control, LV_PART_MAIN) == 0);
        assert(lv_obj_get_style_shadow_width(control, LV_PART_MAIN) == 0);
        assert(lv_obj_get_style_radius(control, LV_PART_MAIN) == 24);
    }
}

static void mark_deleted(lv_event_t *event)
{
    bool *deleted = lv_event_get_user_data(event);
    *deleted      = true;
}

typedef struct {
    uint32_t key;
    lv_indev_state_t state;
    bool drain_again;
} keyboard_sample_t;

static void read_keyboard(lv_indev_t *input, lv_indev_data_t *data)
{
    keyboard_sample_t *sample = lv_indev_get_user_data(input);
    data->key                = sample->key;
    data->state              = sample->state;
    data->continue_reading   = sample->drain_again;
    sample->drain_again      = false;
}

static void keyboard_event(lv_event_t *event)
{
    launcher_ui_t *ui = lv_event_get_user_data(event);
    lv_indev_t *input = lv_event_get_target(event);
    if (launcher_ui_handle_keyboard_input(ui, input)) lv_indev_stop_processing(input);
}

static void sample_keyboard(lv_indev_t *input, uint32_t key, lv_indev_state_t state, uint32_t elapsed)
{
    keyboard_sample_t *sample = lv_indev_get_user_data(input);
    sample->key               = key;
    sample->state             = state;
    /* evdev returns one key event, then drains SYN_REPORT/EAGAIN in the same
     * LVGL read. Both samples retain the same key and pressed state. */
    sample->drain_again = true;
    lv_tick_inc(elapsed);
    lv_indev_read(input);
}

static void settle_focus_render(lv_display_t *display)
{
    /* Allow both focus transitions and automatic scrolling to finish before
     * inspecting the outline, including a scroll started by a layout pass. */
    for (int i = 0; i < 2; ++i) {
        lv_tick_inc(300);
        lv_timer_handler();
        lv_obj_update_layout(lv_screen_active());
    }
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display);
}

static void assert_focused_outline_visible(lv_display_t *display, lv_obj_t *control, lv_obj_t *viewport)
{
    assert(lv_obj_has_state(control, LV_STATE_FOCUS_KEY));
    settle_focus_render(display);
    const int32_t outline_width = lv_obj_get_style_outline_width(control, LV_PART_MAIN);
    const int32_t outline_pad = lv_obj_get_style_outline_pad(control, LV_PART_MAIN);
    assert(outline_width > 0);
    assert(lv_obj_get_style_outline_opa(control, LV_PART_MAIN) > LV_OPA_TRANSP);
    lv_area_t control_area, viewport_area;
    lv_obj_get_coords(control, &control_area);
    lv_obj_get_coords(viewport, &viewport_area);
    const int32_t extent = outline_pad + outline_width;
    if (control_area.x1 - extent < viewport_area.x1 || control_area.x2 + extent > viewport_area.x2 ||
        control_area.y1 - extent < viewport_area.y1 || control_area.y2 + extent > viewport_area.y2) {
        fprintf(stderr,
                "quick-control focus outline clipped: display=%dx%d control=%s "
                "control=(%d,%d)-(%d,%d) viewport=(%d,%d)-(%d,%d) extent=%d scroll=(%d,%d)\n",
                lv_display_get_horizontal_resolution(display), lv_display_get_vertical_resolution(display),
                lv_obj_get_name(control), control_area.x1, control_area.y1, control_area.x2, control_area.y2,
                viewport_area.x1, viewport_area.y1, viewport_area.x2, viewport_area.y2, extent,
                lv_obj_get_scroll_x(viewport), lv_obj_get_scroll_y(viewport));
        assert(0 && "quick-control focus outline clipped");
    }
    assert(control_area.x1 - extent >= viewport_area.x1);
    assert(control_area.x2 + extent <= viewport_area.x2);
    assert(control_area.y1 - extent >= viewport_area.y1);
    assert(control_area.y2 + extent <= viewport_area.y2);

    /* Sample the middle of each straight outline edge, clear of the rounded
     * corners. Geometry alone cannot detect a parent or corner mask hiding
     * an otherwise correctly positioned focus ring. */
    const int32_t offset = outline_pad + (outline_width + 1) / 2;
    const int32_t center_x = (control_area.x1 + control_area.x2) / 2;
    const int32_t center_y = (control_area.y1 + control_area.y2) / 2;
    const lv_point_t samples[] = {
        {control_area.x1 - offset, center_y},
        {control_area.x2 + offset, center_y},
        {center_x, control_area.y1 - offset},
        {center_x, control_area.y2 + offset},
    };
    const int32_t display_width = lv_display_get_horizontal_resolution(display);
    const int32_t display_height = lv_display_get_vertical_resolution(display);
    const lv_color_t outline_color = lv_obj_get_style_outline_color(control, LV_PART_MAIN);
    assert(lv_color_eq(outline_color, lv_color_hex(COLOR_CONTROL_FOCUS)));
    const lv_color32_t expected = lv_display_get_color_format(display) == LV_COLOR_FORMAT_RGB565
        ? rgb565_pixel(lv_color_to_u16(outline_color)) : lv_color_to_32(outline_color, LV_OPA_COVER);
    static const char *edge_names[] = {"left", "right", "top", "bottom"};
    for (size_t i = 0; i < 4; ++i) {
        assert(samples[i].x >= 0 && samples[i].x < display_width);
        assert(samples[i].y >= 0 && samples[i].y < display_height);
        const lv_color32_t pixel = framebuffer[(size_t)samples[i].y * display_width + samples[i].x];
        const bool matches = pixel.red == expected.red && pixel.green == expected.green &&
                             pixel.blue == expected.blue;
        if (!matches)
            fprintf(stderr,
                    "quick-control focus outline missing: display=%dx%d control=%s edge=%s "
                    "sample=(%d,%d) rgb=(%u,%u,%u) expected=(%u,%u,%u) opacity=%u\n",
                    display_width, display_height, lv_obj_get_name(control), edge_names[i],
                    samples[i].x, samples[i].y, pixel.red, pixel.green, pixel.blue,
                    expected.red, expected.green, expected.blue,
                    lv_obj_get_style_outline_opa(control, LV_PART_MAIN));
        assert(matches);
    }
}

static void test_quick_control_tab_focus(launcher_ui_t *ui, lv_group_t *group, lv_display_t *display)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *viewport = lv_obj_find_by_name(screen, "quick_controls");
    lv_obj_t *orientation = lv_obj_find_by_name(screen, "orientation_button");
    lv_obj_t *share = lv_obj_find_by_name(screen, "file_share_button");
    lv_obj_t *lock = lv_obj_find_by_name(screen, "lock_button");
    lv_obj_t *logout = lv_obj_find_by_name(screen, "session_button");
    lv_obj_t *power = lv_obj_find_by_name(screen, "power_button");
    lv_obj_t *reboot = lv_obj_find_by_name(screen, "reboot_button");
    lv_obj_t *brightness = lv_obj_find_by_name(screen, "brightness_slider");
    lv_obj_t *volume = lv_obj_find_by_name(screen, "volume_slider");
    assert(viewport && orientation && share && lock && logout && power && reboot && brightness && volume);
    assert(!lv_obj_has_flag(lv_obj_find_by_name(screen, "quick_scrim"), LV_OBJ_FLAG_HIDDEN));
    lv_group_t *controls_group = lv_obj_get_group(orientation);
    assert(controls_group && controls_group != group);
    keyboard_sample_t sample = {0, LV_INDEV_STATE_RELEASED, false};
    lv_indev_t *keyboard = lv_indev_create();
    assert(keyboard);
    lv_indev_set_type(keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_group(keyboard, group);
    lv_indev_set_user_data(keyboard, &sample);
    lv_indev_set_read_cb(keyboard, read_keyboard);
    lv_indev_add_event_cb(keyboard, keyboard_event, LV_EVENT_KEY, ui);
    lv_timer_pause(lv_indev_get_read_timer(keyboard));
    lv_group_focus_obj(orientation);
    /* Complete both cycles: Tab from the last card and Shift+Tab from the
     * first card must wrap within the open panel instead of reaching apps. */
    const uint32_t keys[] = {
        LV_KEY_NEXT, LV_KEY_NEXT, LV_KEY_NEXT, LV_KEY_NEXT, LV_KEY_NEXT, LV_KEY_NEXT, LV_KEY_NEXT, LV_KEY_NEXT,
        LV_KEY_PREV, LV_KEY_PREV, LV_KEY_PREV, LV_KEY_PREV, LV_KEY_PREV, LV_KEY_PREV, LV_KEY_PREV, LV_KEY_PREV,
    };
    lv_obj_t *expected[] = {
        share, lock, logout, power, reboot, brightness, volume, orientation,
        volume, brightness, reboot, power, logout, lock, share, orientation,
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        sample_keyboard(keyboard, keys[i], LV_INDEV_STATE_PRESSED, 0);
        sample_keyboard(keyboard, keys[i], LV_INDEV_STATE_RELEASED, 0);
        assert(lv_indev_get_group(keyboard) == controls_group);
        assert(lv_group_get_focused(controls_group) == expected[i]);
        assert_focused_outline_visible(display, expected[i], viewport);
    }
    /* Native key repeat must stay inside the panel through multiple wraps. */
    for (int i = 0; i < 12; ++i) {
        sample_keyboard(keyboard, LV_KEY_NEXT, LV_INDEV_STATE_PRESSED, 150);
        lv_obj_t *focused = lv_group_get_focused(lv_indev_get_group(keyboard));
        assert(focused == orientation || focused == share || focused == lock || focused == logout ||
               focused == power || focused == reboot || focused == brightness || focused == volume);
    }
    sample_keyboard(keyboard, LV_KEY_NEXT, LV_INDEV_STATE_RELEASED, 0);
    lv_group_focus_obj(orientation);

    /* Display/input reconnection may bind the keyboard back to the home
     * group while the panel remains open. Rebinding must recapture it. */
    lv_indev_set_group(keyboard, group);
    launcher_ui_rebind_input(ui);
    assert(lv_indev_get_group(keyboard) == controls_group);
    sample_keyboard(keyboard, LV_KEY_NEXT, LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, LV_KEY_NEXT, LV_INDEV_STATE_RELEASED, 0);
    assert(lv_group_get_focused(controls_group) == share);
    sample_keyboard(keyboard, LV_KEY_PREV, LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, LV_KEY_PREV, LV_INDEV_STATE_RELEASED, 0);
    assert(lv_group_get_focused(controls_group) == orientation);
    settle_focus_render(display);
    lv_indev_delete(keyboard);
}

static void test_keyboard_navigation(launcher_ui_t *ui, launcher_registry_t *registry, lv_group_t *group,
                                     lv_obj_t *pages)
{
    keyboard_sample_t sample = {0, LV_INDEV_STATE_RELEASED, false};
    lv_indev_t *keyboard     = lv_indev_create();
    assert(keyboard);
    lv_indev_set_type(keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_group(keyboard, group);
    lv_indev_set_user_data(keyboard, &sample);
    lv_indev_set_read_cb(keyboard, read_keyboard);
    lv_indev_add_event_cb(keyboard, keyboard_event, LV_EVENT_KEY, ui);
    lv_timer_pause(lv_indev_get_read_timer(keyboard));

    select_page(pages, 0);
    lv_group_focus_obj(find_app_tile(pages, &registry->apps[0]));
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_PRESSED, 0);
    assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[1]));
    for (int i = 0; i < 8; ++i) {
        sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_PRESSED, 30);
        assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[1]));
    }
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_RELEASED, 0);
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_PRESSED, 0);
    assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[2]));
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_RELEASED, 0);

    /* A held direction waits 450 ms before repeating, then moves once per
     * 150 ms. Additional polls at either threshold must not repeat again. */
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_PRESSED, 0);
    assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[3]));
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_PRESSED, 449);
    assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[3]));
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_PRESSED, 1);
    assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[4]));
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_PRESSED, 149);
    assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[4]));
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_PRESSED, 1);
    assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[5]));
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_RELEASED, 0);

    /* Enter is edge-triggered even after the long-press threshold, and LVGL's
     * synthesized release click must not create a second launch request. */
    assert(!launcher_ui_take_launch_request(ui));
    sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_PRESSED, 0);
    assert(launcher_ui_take_launch_request(ui) == &registry->apps[5]);
    for (int i = 0; i < 8; ++i) {
        sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_PRESSED, 150);
        assert(!launcher_ui_take_launch_request(ui));
    }
    sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_RELEASED, 0);
    assert(!launcher_ui_take_launch_request(ui));
    sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_PRESSED, 0);
    assert(launcher_ui_take_launch_request(ui) == &registry->apps[5]);
    sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_RELEASED, 0);
    assert(!launcher_ui_take_launch_request(ui));

    /* Crossing a page boundary changes focus but must retain the held-key
     * state, so a short press stops at the first app on the next page. */
    lv_group_focus_obj(find_app_tile(pages, &registry->apps[17]));
    select_page(pages, 0);
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_PRESSED, 0);
    assert(active_page_index(pages) == 1);
    assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[18]));
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_PRESSED, 100);
    assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[18]));
    sample_keyboard(keyboard, LV_KEY_RIGHT, LV_INDEV_STATE_RELEASED, 0);

    /* Space toggles Quick controls once per press, without activating the
     * focused app or panel control when pressed, held, or released. */
    launcher_ui_set_session_user(ui, "lilygo");
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *quick_scrim = lv_obj_find_by_name(screen, "quick_scrim");
    lv_obj_t *orientation_button = lv_obj_find_by_name(screen, "orientation_button");
    lv_obj_t *file_share_button = lv_obj_find_by_name(screen, "file_share_button");
    lv_obj_t *lock_button = lv_obj_find_by_name(screen, "lock_button");
    lv_obj_t *previous_focus = find_app_tile(pages, &registry->apps[18]);
    launcher_orientation_t requested_orientation;
    assert(quick_scrim && orientation_button && file_share_button && lock_button && previous_focus);
    lv_group_focus_obj(previous_focus);
    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_PRESSED, 0);
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(lv_group_get_focused(lv_obj_get_group(orientation_button)) == orientation_button);
    for (int i = 0; i < 8; ++i) {
        sample_keyboard(keyboard, ' ', LV_INDEV_STATE_PRESSED, 150);
        assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
        assert(lv_group_get_focused(lv_obj_get_group(orientation_button)) == orientation_button);
        assert(!launcher_ui_take_orientation_request(ui, &requested_orientation));
        assert(!launcher_ui_take_launch_request(ui));
    }
    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_RELEASED, 0);
    assert(!launcher_ui_take_orientation_request(ui, &requested_orientation));
    assert(!launcher_ui_take_launch_request(ui));
    sample_keyboard(keyboard, LV_KEY_DOWN, LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, LV_KEY_DOWN, LV_INDEV_STATE_RELEASED, 0);
    assert(lv_group_get_focused(lv_obj_get_group(orientation_button)) == file_share_button);
    sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_RELEASED, 0);
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    lv_obj_t *file_share_scrim = lv_obj_find_by_name(screen, "file_share_scrim");
    assert(file_share_scrim && !lv_obj_has_flag(file_share_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(launcher_ui_handle_key(ui, LV_KEY_ESC));
    assert(lv_obj_has_flag(file_share_scrim, LV_OBJ_FLAG_HIDDEN));

    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_RELEASED, 0);
    sample_keyboard(keyboard, LV_KEY_DOWN, LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, LV_KEY_DOWN, LV_INDEV_STATE_RELEASED, 0);
    sample_keyboard(keyboard, LV_KEY_DOWN, LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, LV_KEY_DOWN, LV_INDEV_STATE_RELEASED, 0);
    assert(lv_group_get_focused(lv_obj_get_group(orientation_button)) == lock_button);
    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_PRESSED, 0);
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(lv_indev_get_group(keyboard) == group);
    assert(lv_obj_get_group(orientation_button) == group);
    assert(lv_group_get_focused(group) == previous_focus);
    assert(!launcher_ui_take_lock_request(ui));
    for (int i = 0; i < 8; ++i) {
        sample_keyboard(keyboard, ' ', LV_INDEV_STATE_PRESSED, 150);
        assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
        assert(lv_group_get_focused(group) == previous_focus);
        assert(!launcher_ui_take_lock_request(ui));
        assert(!launcher_ui_take_launch_request(ui));
    }
    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_RELEASED, 0);
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(lv_group_get_focused(group) == previous_focus);
    assert(!launcher_ui_take_lock_request(ui));
    assert(!launcher_ui_take_launch_request(ui));

    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_PRESSED, 0);
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(lv_group_get_focused(lv_obj_get_group(orientation_button)) == orientation_button);
    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_RELEASED, 0);
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(!launcher_ui_take_orientation_request(ui, &requested_orientation));
    assert(!launcher_ui_take_launch_request(ui));
    sample_keyboard(keyboard, LV_KEY_ESC, LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, LV_KEY_ESC, LV_INDEV_STATE_RELEASED, 0);
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(lv_indev_get_group(keyboard) == group);
    assert(lv_group_get_focused(group) == previous_focus);

    /* Hidden account controls and unavailable system controls must be skipped
     * in both directions, leaving the orientation card as the only action. */
    launcher_ui_set_session_user(ui, NULL);
    launcher_ui_set_system_controls(ui, false, 0, false, 0, false, false);
    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_RELEASED, 0);
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    const uint32_t single_control_keys[] = {LV_KEY_NEXT, LV_KEY_NEXT, LV_KEY_PREV, LV_KEY_PREV};
    for (size_t i = 0; i < sizeof(single_control_keys) / sizeof(single_control_keys[0]); ++i) {
        sample_keyboard(keyboard, single_control_keys[i], LV_INDEV_STATE_PRESSED, 0);
        sample_keyboard(keyboard, single_control_keys[i], LV_INDEV_STATE_RELEASED, 0);
        assert(lv_group_get_focused(lv_indev_get_group(keyboard)) == orientation_button);
    }
    sample_keyboard(keyboard, LV_KEY_ESC, LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, LV_KEY_ESC, LV_INDEV_STATE_RELEASED, 0);
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(lv_indev_get_group(keyboard) == group);
    assert(lv_group_get_focused(group) == previous_focus);
    sample_keyboard(keyboard, LV_KEY_NEXT, LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, LV_KEY_NEXT, LV_INDEV_STATE_RELEASED, 0);
    assert(lv_group_get_focused(group) == find_app_tile(pages, &registry->apps[19]));

    launcher_ui_set_input_enabled(ui, false);
    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_PRESSED, 0);
    assert(!launcher_ui_handle_key(ui, ' '));
    assert(!launcher_ui_handle_keyboard_input(ui, keyboard));
    sample_keyboard(keyboard, ' ', LV_INDEV_STATE_RELEASED, 0);
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(!launcher_ui_take_orientation_request(ui, &requested_orientation));
    assert(!launcher_ui_take_launch_request(ui));
    launcher_ui_set_input_enabled(ui, true);
    launcher_ui_set_session_user(ui, NULL);
    launcher_ui_set_system_controls(ui, true, 65, true, 40, true, true);
    lv_indev_delete(keyboard);
}

static void press_key(lv_indev_t *keyboard, uint32_t key)
{
    sample_keyboard(keyboard, key, LV_INDEV_STATE_PRESSED, 0);
    sample_keyboard(keyboard, key, LV_INDEV_STATE_RELEASED, 0);
}

static void test_system_controls(launcher_ui_t *ui, lv_group_t *group, lv_display_t *display, lv_indev_t *pointer)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *quick_scrim = lv_obj_find_by_name(screen, "quick_scrim");
    lv_obj_t *orientation = lv_obj_find_by_name(screen, "orientation_button");
    lv_obj_t *brightness = lv_obj_find_by_name(screen, "brightness_slider");
    lv_obj_t *brightness_label = lv_obj_find_by_name(screen, "brightness_label");
    lv_obj_t *volume = lv_obj_find_by_name(screen, "volume_slider");
    lv_obj_t *volume_label = lv_obj_find_by_name(screen, "volume_label");
    lv_obj_t *power = lv_obj_find_by_name(screen, "power_button");
    lv_obj_t *reboot = lv_obj_find_by_name(screen, "reboot_button");
    lv_obj_t *confirmation = lv_obj_find_by_name(screen, "power_confirm_scrim");
    lv_obj_t *confirm = lv_obj_find_by_name(screen, "power_confirm_button");
    lv_obj_t *cancel = lv_obj_find_by_name(screen, "power_cancel_button");
    lv_obj_t *hint = lv_obj_find_by_name(screen, "power_confirm_hint");
    lv_obj_t *title = lv_obj_find_by_name(screen, "power_confirm_title");
    assert(quick_scrim && orientation && brightness && brightness_label && volume && volume_label && power);
    assert(confirmation && confirm && cancel && hint && title && reboot);
    assert(lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));

    keyboard_sample_t sample = {0, LV_INDEV_STATE_RELEASED, false};
    lv_indev_t *keyboard = lv_indev_create();
    assert(keyboard);
    lv_indev_set_type(keyboard, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_group(keyboard, group);
    lv_indev_set_user_data(keyboard, &sample);
    lv_indev_set_read_cb(keyboard, read_keyboard);
    lv_indev_add_event_cb(keyboard, keyboard_event, LV_EVENT_KEY, ui);
    lv_timer_pause(lv_indev_get_read_timer(keyboard));
    int requested = -1;

    launcher_ui_set_system_controls(ui, true, 65, true, 40, true, true);
    press_key(keyboard, ' ');
    settle_focus_render(display);
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(brightness_label), "65%") == 0);
    assert(strcmp(lv_label_get_text(volume_label), "40%") == 0);
    assert(!launcher_ui_take_brightness_request(ui, &requested));
    assert(!launcher_ui_take_volume_request(ui, &requested));

    /* Level arrows adjust in five-percent steps without moving focus; Enter
     * and the native release click must neither change nor submit a level. */
    lv_group_focus_obj(brightness);
    press_key(keyboard, LV_KEY_UP);
    assert(lv_group_get_focused(lv_indev_get_group(keyboard)) == brightness);
    assert(lv_slider_get_value(brightness) == 70);
    assert(launcher_ui_take_brightness_request(ui, &requested) && requested == 70);
    assert(!launcher_ui_take_brightness_request(ui, &requested));
    press_key(keyboard, LV_KEY_LEFT);
    assert(launcher_ui_take_brightness_request(ui, &requested) && requested == 65);
    press_key(keyboard, LV_KEY_ENTER);
    assert(lv_slider_get_value(brightness) == 65);
    assert(!launcher_ui_take_brightness_request(ui, &requested));
    press_key(keyboard, LV_KEY_NEXT);
    assert(lv_group_get_focused(lv_indev_get_group(keyboard)) == volume);
    press_key(keyboard, LV_KEY_RIGHT);
    assert(launcher_ui_take_volume_request(ui, &requested) && requested == 45);
    press_key(keyboard, LV_KEY_DOWN);
    assert(launcher_ui_take_volume_request(ui, &requested) && requested == 40);

    /* The backlight cannot be turned completely off; muted audio can reach
     * zero. Hardware samples outside the supported range are also clamped. */
    launcher_ui_set_system_controls(ui, true, -20, true, 140, true, true);
    assert(lv_slider_get_value(brightness) == 1);
    assert(lv_slider_get_value(volume) == 100);
    lv_group_focus_obj(brightness);
    press_key(keyboard, LV_KEY_DOWN);
    assert(lv_slider_get_value(brightness) == 1);
    if (launcher_ui_take_brightness_request(ui, &requested)) assert(requested == 1);
    launcher_ui_set_system_controls(ui, true, 98, true, 2, true, true);
    press_key(keyboard, LV_KEY_UP);
    assert(launcher_ui_take_brightness_request(ui, &requested) && requested == 100);
    lv_group_focus_obj(volume);
    press_key(keyboard, LV_KEY_DOWN);
    assert(launcher_ui_take_volume_request(ui, &requested) && requested == 0);

    /* Exercise real pointer input: dragging a vertical level card must not
     * trigger the panel's upward-dismiss gesture or lose the final value. */
    launcher_ui_set_system_controls(ui, true, 25, true, 40, true, true);
    lv_group_focus_obj(brightness);
    settle_focus_render(display);
    lv_area_t slider_area;
    lv_obj_get_coords(brightness, &slider_area);
    assert(lv_area_get_height(&slider_area) > lv_area_get_width(&slider_area));
    pointer_point = (lv_point_t){(slider_area.x1 + slider_area.x2) / 2,
                                 slider_area.y2 - lv_area_get_height(&slider_area) / 4};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    pointer_point.y = slider_area.y1 + lv_area_get_height(&slider_area) / 5;
    lv_indev_read(pointer);
    assert(lv_slider_get_value(brightness) >= 75);
    const int dragged_brightness = lv_slider_get_value(brightness);
    launcher_ui_set_system_controls(ui, true, 25, true, 40, true, true);
    assert(lv_slider_get_value(brightness) == dragged_brightness);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    assert(launcher_ui_take_brightness_request(ui, &requested) && requested == dragged_brightness);
    assert(!launcher_ui_take_brightness_request(ui, &requested));
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));

    /* Missing devices stay visibly unavailable, ignore synthetic value
     * events, and are omitted from keyboard navigation. */
    launcher_ui_set_system_controls(ui, false, 50, false, 50, false, false);
    assert(lv_obj_has_state(brightness, LV_STATE_DISABLED));
    assert(lv_obj_has_state(volume, LV_STATE_DISABLED));
    assert(lv_obj_has_state(power, LV_STATE_DISABLED));
    assert(lv_obj_has_state(reboot, LV_STATE_DISABLED));
    assert(lv_obj_send_event(reboot, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(strcmp(lv_label_get_text(brightness_label), "--") == 0);
    assert(strcmp(lv_label_get_text(volume_label), "--") == 0);
    assert(lv_obj_send_event(brightness, LV_EVENT_VALUE_CHANGED, NULL) == LV_RESULT_OK);
    assert(lv_obj_send_event(volume, LV_EVENT_VALUE_CHANGED, NULL) == LV_RESULT_OK);
    assert(lv_obj_send_event(power, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(!launcher_ui_take_brightness_request(ui, &requested));
    assert(!launcher_ui_take_volume_request(ui, &requested));
    assert(!launcher_ui_take_power_request(ui));
    assert(lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    lv_group_focus_obj(orientation);
    press_key(keyboard, LV_KEY_NEXT);
    assert(lv_group_get_focused(lv_indev_get_group(keyboard)) == orientation);

    launcher_ui_set_system_controls(ui, true, 65, true, 40, false, true);
    lv_group_focus_obj(reboot);
    press_key(keyboard, LV_KEY_ENTER);
    assert(lv_group_get_focused(lv_indev_get_group(keyboard)) == cancel);
    assert(strcmp(lv_label_get_text(title), "Reboot?") == 0);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(confirm, 0)), "Reboot") == 0);
    assert(launcher_ui_take_power_request(ui) == LAUNCHER_POWER_NONE);
    press_key(keyboard, LV_KEY_ENTER);
    assert(lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    assert(lv_group_get_focused(lv_indev_get_group(keyboard)) == reboot);
    assert(launcher_ui_take_power_request(ui) == LAUNCHER_POWER_NONE);

    launcher_ui_set_system_controls(ui, true, 65, true, 40, true, true);
    settle_focus_render(display);
    render_reboot_state(display, "quick-controls-reboot.ppm");
    assert(lv_obj_send_event(reboot, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    render_reboot_state(display, "reboot-confirmation.ppm");
    press_key(keyboard, LV_KEY_NEXT);
    press_key(keyboard, LV_KEY_ENTER);
    assert(launcher_ui_take_power_request(ui) == LAUNCHER_POWER_REBOOT);
    assert(launcher_ui_take_power_request(ui) == LAUNCHER_POWER_NONE);
    assert(strcmp(lv_label_get_text(title), "Rebooting") == 0);
    launcher_ui_set_power_result(ui, false, false);
    assert(strcmp(lv_label_get_text(title), "Couldn't reboot") == 0);
    lv_group_focus_obj(confirm);
    press_key(keyboard, LV_KEY_ENTER);
    assert(launcher_ui_take_power_request(ui) == LAUNCHER_POWER_REBOOT);
    launcher_ui_set_power_result(ui, true, true);
    assert(lv_obj_has_flag(confirm, LV_OBJ_FLAG_HIDDEN));
    assert(strstr(lv_label_get_text(hint), "won't restart"));
    render_reboot_state(display, "reboot-simulation-complete.ppm");
    press_key(keyboard, LV_KEY_ESC);

    lv_group_focus_obj(power);
    sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_PRESSED, 0);
    assert(!lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(title), "Power off?") == 0);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(confirm, 0)), "Power off") == 0);
    assert(lv_group_get_focused(lv_indev_get_group(keyboard)) == cancel);
    for (int i = 0; i < 5; ++i) {
        sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_PRESSED, 150);
        assert(!launcher_ui_take_power_request(ui));
        assert(!lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    }
    sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_RELEASED, 0);
    assert(!launcher_ui_take_power_request(ui));
    press_key(keyboard, LV_KEY_ESC);
    assert(lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(!launcher_ui_take_power_request(ui));

    lv_group_focus_obj(power);
    press_key(keyboard, LV_KEY_ENTER);
    press_key(keyboard, LV_KEY_ENTER); /* Cancel is initially selected. */
    assert(lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    assert(!launcher_ui_take_power_request(ui));
    lv_group_focus_obj(power);
    press_key(keyboard, LV_KEY_ENTER);
    press_key(keyboard, LV_KEY_NEXT);
    assert(lv_group_get_focused(lv_indev_get_group(keyboard)) == confirm);
    sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_PRESSED, 0);
    assert(launcher_ui_take_power_request(ui) == LAUNCHER_POWER_OFF);
    for (int i = 0; i < 5; ++i) {
        sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_PRESSED, 150);
        assert(!launcher_ui_take_power_request(ui));
    }
    sample_keyboard(keyboard, LV_KEY_ENTER, LV_INDEV_STATE_RELEASED, 0);
    assert(!launcher_ui_take_power_request(ui));
    launcher_ui_set_power_result(ui, false, false);
    assert(!lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_state(confirm, LV_STATE_DISABLED));
    assert(strlen(lv_label_get_text(hint)) > 0);
    lv_group_focus_obj(confirm);
    press_key(keyboard, LV_KEY_ENTER);
    assert(launcher_ui_take_power_request(ui) == LAUNCHER_POWER_OFF);
    launcher_ui_set_power_result(ui, true, true);
    assert(!lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(confirm, LV_OBJ_FLAG_HIDDEN));
    assert(strstr(lv_label_get_text(hint), "stay on"));
    press_key(keyboard, LV_KEY_ESC);

    /* Transitioning to the account screen clears requests already queued by
     * all three controls and rejects subsequent events on hidden objects. */
    lv_slider_set_value(brightness, 70, LV_ANIM_OFF);
    assert(lv_obj_send_event(brightness, LV_EVENT_VALUE_CHANGED, NULL) == LV_RESULT_OK);
    lv_slider_set_value(volume, 50, LV_ANIM_OFF);
    assert(lv_obj_send_event(volume, LV_EVENT_VALUE_CHANGED, NULL) == LV_RESULT_OK);
    lv_group_focus_obj(power);
    press_key(keyboard, LV_KEY_ENTER);
    lv_group_focus_obj(confirm);
    press_key(keyboard, LV_KEY_ENTER);
    launcher_ui_set_input_enabled(ui, false);
    assert(!launcher_ui_take_brightness_request(ui, &requested));
    assert(!launcher_ui_take_volume_request(ui, &requested));
    assert(!launcher_ui_take_power_request(ui));
    assert(lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_send_event(brightness, LV_EVENT_VALUE_CHANGED, NULL) == LV_RESULT_OK);
    assert(lv_obj_send_event(volume, LV_EVENT_VALUE_CHANGED, NULL) == LV_RESULT_OK);
    assert(lv_obj_send_event(power, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(lv_obj_send_event(confirm, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(!launcher_ui_take_brightness_request(ui, &requested));
    assert(!launcher_ui_take_volume_request(ui, &requested));
    assert(!launcher_ui_take_power_request(ui));
    assert(lv_obj_has_flag(confirmation, LV_OBJ_FLAG_HIDDEN));
    launcher_ui_set_input_enabled(ui, true);
    launcher_ui_set_system_controls(ui, true, 65, true, 40, true, true);
    settle_focus_render(display);
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    lv_indev_delete(keyboard);
}

int main(int argc, char **argv)
{
    if (argc != 6 && argc != 8) {
        fprintf(stderr,
                "usage: %s PORTRAIT.ppm QUICK_CONTROLS.ppm LANDSCAPE.ppm "
                "COMPACT.ppm COMPACT_QUICK_CONTROLS.ppm [FILE_SHARE.ppm COMPACT_FILE_SHARE.ppm]\n",
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
    launcher_ui_set_system_controls(ui, true, 65, true, 40, true, true);
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

    /* Hardware keypad navigation starts from the current page even when the
     * default group is focused on a non-app control (for example the quick
     * panel orientation button).  Verify grid movement, activation, and a
     * right-edge page transition through the public key handler. */
    assert(launcher_ui_handle_key(ui, LV_KEY_RIGHT));
    assert(lv_group_get_focused(focus_group) == first_card);
    assert(launcher_ui_handle_key(ui, LV_KEY_RIGHT));
    assert(lv_group_get_focused(focus_group) == find_app_tile(pages, &registry.apps[1]));
    assert(launcher_ui_handle_key(ui, LV_KEY_DOWN));
    assert(lv_group_get_focused(focus_group) == find_app_tile(pages, &registry.apps[4]));
    assert(launcher_ui_handle_key(ui, LV_KEY_ENTER));
    assert(launcher_ui_take_launch_request(ui) == &registry.apps[4]);
    lv_group_focus_obj(find_app_tile(pages, &registry.apps[17]));
    select_page(pages, 0);
    assert(launcher_ui_handle_key(ui, LV_KEY_RIGHT));
    pages = find_by_class(screen, &lv_tileview_class);
    assert(active_page_index(pages) == 1);
    assert(lv_group_get_focused(focus_group) == find_app_tile(pages, &registry.apps[18]));
    assert(launcher_ui_handle_key(ui, LV_KEY_ESC));

    test_keyboard_navigation(ui, &registry, focus_group, pages);
    test_system_controls(ui, focus_group, display, input);

    /* Restore the initial page and non-app focus before the existing render
     * snapshots below; navigation assertions must not change their baseline. */
    select_page(pages, 0);
    lv_group_focus_obj(lv_obj_find_by_name(screen, "orientation_button"));
    first_card = find_app_tile(pages, &registry.apps[0]);
    assert(first_card);

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
    launcher_ui_set_session_user(ui, "lilygo");
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
    assert_session_actions_fit(screen);
    test_quick_control_tab_focus(ui, focus_group, display);
    memset(framebuffer, 0, (size_t)WIDTH * HEIGHT * sizeof(*framebuffer));
    render(display, argv[5]);
    launcher_ui_set_file_share_directory(ui, "/home/lilygo/Downloads");
    launcher_ui_set_file_share_status(ui, "Waiting for a browser on the local network.");
    assert(lv_obj_send_event(lv_obj_find_by_name(screen, "file_share_button"), LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(launcher_ui_file_share_open(ui));
    lv_obj_t *approve_file = lv_obj_find_by_name(screen, "file_share_approve_button");
    lv_obj_t *reject_file = lv_obj_find_by_name(screen, "file_share_reject_button");
    assert(approve_file && reject_file);
    assert(lv_obj_has_flag(approve_file, LV_OBJ_FLAG_HIDDEN));
    launcher_ui_set_file_share_approval(ui, "annual-report.pdf", 4096, true);
    assert(!lv_obj_has_flag(approve_file, LV_OBJ_FLAG_HIDDEN));
    if (argc == 8) render(display, argv[7]);
    /* A newly displayed request defaults to Decline for hardware keyboards. */
    assert(lv_obj_has_state(reject_file, LV_STATE_FOCUSED));
    assert(launcher_ui_handle_key(ui, LV_KEY_ENTER));
    bool file_approved = true;
    assert(launcher_ui_take_file_share_decision(ui, &file_approved) && !file_approved);
    launcher_ui_set_file_share_approval(ui, "annual-report.pdf", 4096, true);
    assert(launcher_ui_handle_key(ui, LV_KEY_LEFT));
    assert(lv_obj_has_state(approve_file, LV_STATE_FOCUSED));
    assert(launcher_ui_handle_key(ui, LV_KEY_ENTER));
    assert(launcher_ui_take_file_share_decision(ui, &file_approved) && file_approved);
    launcher_ui_set_file_share_approval(ui, "next-file.txt", 64, true);
    assert(!launcher_ui_take_file_share_decision(ui, &file_approved));
    assert(lv_obj_has_state(reject_file, LV_STATE_FOCUSED));
    launcher_ui_set_file_share_approval(ui, NULL, 0, false);
    assert_file_share_content_fits(screen);
    assert(launcher_ui_handle_key(ui, LV_KEY_ESC));
    assert(!launcher_ui_file_share_open(ui));
    assert(launcher_ui_handle_key(ui, ' '));

    /* On shorter displays, scrolling the cards must not dismiss the panel. */
    lv_display_set_resolution(display, 320, 200);
    lv_obj_update_layout(screen);
    lv_obj_t *overflow_controls = lv_obj_find_by_name(screen, "quick_controls");
    lv_obj_t *overflow_lock = lv_obj_find_by_name(screen, "lock_button");
    lv_obj_t *overflow_logout = lv_obj_find_by_name(screen, "session_button");
    assert(overflow_controls && overflow_lock && overflow_logout);
    assert(lv_obj_get_width(overflow_lock) == lv_obj_get_width(tiny_orientation_button));
    assert(lv_obj_get_width(overflow_logout) == lv_obj_get_width(tiny_orientation_button));
    assert(lv_obj_get_height(overflow_lock) == lv_obj_get_height(tiny_orientation_button));
    assert(lv_obj_get_height(overflow_logout) == lv_obj_get_height(tiny_orientation_button));
    assert(lv_obj_get_scroll_bottom(overflow_controls) > 0);
    test_quick_control_tab_focus(ui, focus_group, display);
    pointer_point = (lv_point_t){160, 150};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    pointer_point = (lv_point_t){160, 65};
    lv_indev_read(input);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    assert(!lv_obj_has_flag(tiny_quick_scrim, LV_OBJ_FLAG_HIDDEN));
    lv_display_set_resolution(display, 320, 240);
    lv_obj_update_layout(screen);
    pages = find_by_class(screen, &lv_tileview_class);
    assert(pages);
    assert_session_actions_fit(screen);

    assert(lv_obj_send_event(lv_obj_find_by_name(screen, "lock_button"), LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(launcher_ui_take_lock_request(ui));
    assert(!launcher_ui_take_lock_request(ui));
    assert(!launcher_ui_take_logout_request(ui));
    assert(lv_obj_has_flag(tiny_quick_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(launcher_ui_handle_key(ui, ' '));
    assert(lv_obj_send_event(tiny_orientation_button, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    launcher_orientation_t tiny_requested_orientation;
    assert(launcher_ui_take_orientation_request(ui, &tiny_requested_orientation));
    assert(tiny_requested_orientation == LAUNCHER_ORIENTATION_PORTRAIT);
    lv_tick_inc(200);
    lv_timer_handler();
    assert(lv_obj_has_flag(tiny_quick_scrim, LV_OBJ_FLAG_HIDDEN));
    memset(framebuffer, 0, (size_t)WIDTH * HEIGHT * sizeof(*framebuffer));
    render(display, argv[4]);
    launcher_ui_set_session_user(ui, NULL);

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
    launcher_ui_set_session_user(ui, "lilygo");
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
    assert(lv_obj_get_style_bg_opa(quick_panel, LV_PART_MAIN) < LV_OPA_COVER);
    assert(lv_obj_get_style_blur_backdrop(quick_panel, LV_PART_MAIN));
    assert(lv_obj_get_style_blur_radius(quick_panel, LV_PART_MAIN) >= 1);
    lv_obj_t *quick_panel_title = lv_obj_find_by_name(quick_panel, "quick_panel_title");
    assert(quick_panel_title);
    assert(
        lv_color_eq(lv_obj_get_style_text_color(quick_panel_title, LV_PART_MAIN), lv_color_hex(COLOR_CONTAINER_TITLE)));
    lv_obj_t *orientation_label = lv_obj_find_by_name(quick_panel, "orientation_label");
    lv_obj_t *orientation_button = lv_obj_find_by_name(quick_panel, "orientation_button");
    assert(orientation_button);
    assert(orientation_label);
    assert(lv_color_eq(lv_obj_get_style_text_color(orientation_label, LV_PART_MAIN), lv_color_hex(COLOR_TEXT)));
    assert_quick_control_style(orientation_button, lv_obj_find_by_name(quick_panel, "file_share_button"),
                               lv_obj_find_by_name(quick_panel, "lock_button"),
                               lv_obj_find_by_name(quick_panel, "session_button"));
    assert_session_action_icons(screen);
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    lv_tick_inc(200);
    lv_timer_handler();
    lv_obj_update_layout(screen);
    assert_session_actions_fit(screen);
    assert(lv_obj_get_width(orientation_button) == 220);
    assert(lv_obj_get_height(orientation_button) == 104);
    test_quick_control_tab_focus(ui, focus_group, display);
    render(display, argv[2]);
    assert(lv_obj_send_event(first_card, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(launcher_ui_handle_key(ui, LV_KEY_DOWN));
    assert(lv_group_get_focused(lv_obj_get_group(orientation_button)) == lv_obj_find_by_name(screen, "file_share_button"));
    assert(launcher_ui_handle_key(ui, LV_KEY_ENTER));
    lv_obj_t *file_share_scrim = lv_obj_find_by_name(screen, "file_share_scrim");
    assert(file_share_scrim && !lv_obj_has_flag(file_share_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(launcher_ui_file_share_open(ui));
    assert_file_share_content_fits(screen);
    if (argc == 8) render(display, argv[6]);
    assert(launcher_ui_handle_key(ui, LV_KEY_ESC));
    assert(!launcher_ui_file_share_open(ui));
    assert(lv_obj_has_flag(file_share_scrim, LV_OBJ_FLAG_HIDDEN));
    assert(launcher_ui_handle_key(ui, ' '));
    assert(launcher_ui_handle_key(ui, LV_KEY_DOWN));
    assert(launcher_ui_handle_key(ui, LV_KEY_DOWN));
    assert(lv_group_get_focused(lv_obj_get_group(orientation_button)) == lv_obj_find_by_name(screen, "lock_button"));
    assert(launcher_ui_handle_key(ui, LV_KEY_ENTER));
    assert(launcher_ui_take_lock_request(ui));
    assert(!launcher_ui_take_lock_request(ui));
    assert(launcher_ui_handle_key(ui, ' '));
    assert(lv_obj_send_event(lv_obj_find_by_name(screen, "session_button"), LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(launcher_ui_take_logout_request(ui));
    assert(!launcher_ui_take_logout_request(ui));
    assert(!launcher_ui_take_launch_request(ui));
    assert(!launcher_ui_take_logout_request(ui));
    launcher_ui_set_input_enabled(ui, false);
    assert(!launcher_ui_handle_key(ui, LV_KEY_ENTER));
    assert(!launcher_ui_handle_key(ui, LV_KEY_ESC));
    assert(lv_obj_send_event(first_card, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(lv_obj_send_event(lv_obj_find_by_name(screen, "lock_button"), LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(!launcher_ui_take_launch_request(ui));
    assert(!launcher_ui_take_lock_request(ui));
    launcher_ui_set_input_enabled(ui, true);
    // A local embedded session keeps sharing available without advertising
    // account operations that require PAM. Hidden controls also ignore events.
    launcher_ui_set_account_actions_enabled(ui, false);
    assert(lv_obj_has_flag(lv_obj_find_by_name(screen, "lock_button"), LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(lv_obj_find_by_name(screen, "session_button"), LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(lv_obj_find_by_name(screen, "file_share_button"), LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_send_event(lv_obj_find_by_name(screen, "lock_button"), LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(lv_obj_send_event(lv_obj_find_by_name(screen, "session_button"), LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(!launcher_ui_take_lock_request(ui) && !launcher_ui_take_logout_request(ui));
    launcher_ui_set_session_user(ui, "local-user");
    assert(lv_obj_has_flag(lv_obj_find_by_name(screen, "lock_button"), LV_OBJ_FLAG_HIDDEN));
    launcher_ui_set_account_actions_enabled(ui, true);
    assert(!lv_obj_has_flag(lv_obj_find_by_name(screen, "lock_button"), LV_OBJ_FLAG_HIDDEN));
    launcher_ui_set_session_user(ui, NULL);
    assert(lv_obj_has_flag(lv_obj_find_by_name(screen, "lock_button"), LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(lv_obj_find_by_name(screen, "session_button"), LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(orientation_button, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_send_event(lv_obj_find_by_name(screen, "lock_button"), LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    assert(!launcher_ui_take_lock_request(ui));
    assert(launcher_ui_handle_key(ui, LV_KEY_ESC));
    assert(lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
    pointer_point = (lv_point_t){284, 20};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    pointer_point = (lv_point_t){286, 120};
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
    assert(!lv_obj_has_flag(quick_scrim, LV_OBJ_FLAG_HIDDEN));
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
    launcher_ui_set_session_user(ui, "lilygo");
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
    assert_session_actions_fit(screen);
    assert(lv_obj_get_width(orientation_button) == 220);
    assert(lv_obj_get_height(orientation_button) == 84);
    test_quick_control_tab_focus(ui, focus_group, display);

    /* Open controls remain equally sized when the display changes shape. */
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);
    lv_obj_update_layout(screen);
    assert_session_actions_fit(screen);
    assert(lv_obj_get_width(orientation_button) == 220);
    assert(lv_obj_get_height(orientation_button) == 104);
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_90);
    lv_obj_update_layout(screen);
    assert_session_actions_fit(screen);
    assert(lv_obj_get_height(orientation_button) == 84);
    launcher_ui_set_session_user(ui, NULL);
    assert(lv_obj_send_event(quick_scrim, LV_EVENT_CLICKED, NULL) == LV_RESULT_OK);
    lv_tick_inc(60);
    lv_timer_handler();
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);
    lv_obj_update_layout(screen);
    lv_tick_inc(200);
    lv_timer_handler();
    const int32_t closed_panel_height = lv_obj_get_height(quick_panel);
    assert(closed_panel_height >= 260);
    assert(lv_obj_get_y(quick_panel) == -closed_panel_height);

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
    const uint32_t home_group_count = lv_group_get_count();
    assert(launcher_ui_handle_key(ui, ' '));
    assert(lv_group_get_count() == home_group_count + 1);
    launcher_ui_destroy(ui);
    assert(lv_group_get_count() == home_group_count);
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
    assert(launcher_ui_handle_key(ui, ' '));
    assert(lv_group_get_count() == home_group_count + 1);
    lv_display_delete(display);
    assert(lv_group_get_count() == home_group_count);
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
