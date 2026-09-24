#include "ui.h"

#include "launcher_theme.h"
#include "launcher_view.h"

#include <cm0/status_bar.h>
#include <cm0/typography.h>

#include <new>
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
};

struct layout_metrics_t {
    int32_t width;
    int32_t height;
    int32_t columns;
    int32_t rows;
    int32_t cell_height;
    int32_t bottom_inset;
    int32_t panel_height;
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

static void play_quick_panel(launcher_ui_t *ui, bool open)
{
    if (!ui || !ui->view.screen || ui->quick_panel_open == open) return;
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

    switch (lv_event_get_code(event)) {
        case LV_EVENT_PRESSED:
            lv_indev_get_point(ui->input, &ui->pull_start);
            ui->pull_tracking = ui->quick_panel_open || ui->pull_start.y <= QUICK_PANEL_PULL_START_ZONE;
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
    if (!ui) return;
    ui->requested_orientation = ui->orientation == LAUNCHER_ORIENTATION_LANDSCAPE ? LAUNCHER_ORIENTATION_PORTRAIT
                                                                                  : LAUNCHER_ORIENTATION_LANDSCAPE;
    ui->orientation_request_pending = true;
    play_quick_panel(ui, false);
}

static void app_clicked(lv_event_t *event)
{
    auto *ui  = static_cast<launcher_ui_t *>(lv_event_get_user_data(event));
    auto *app = static_cast<launcher_app_t *>(lv_obj_get_user_data(lv_event_get_current_target_obj(event)));
    if (ui && app) ui->pending_app = app;
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
    lv_group_t *group = lv_group_get_default();
    lv_obj_t *focused = group ? lv_group_get_focused(group) : nullptr;
    if (!focused || !ui->pages) return false;

    const uint32_t page_count = lv_obj_get_child_count(ui->pages);
    for (uint32_t page_index = 0; page_index < page_count; ++page_index) {
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

    metrics.panel_height = landscape ? 190 : 260;
    if (metrics.panel_height > metrics.height) metrics.panel_height = metrics.height;
    return metrics;
}

static void apply_chrome_layout(launcher_ui_t *ui, const layout_metrics_t &metrics)
{
    lv_obj_set_height(ui->view.page_bottom_inset, metrics.bottom_inset);
    if (metrics.bottom_inset >= HOME_INDICATOR_HEIGHT)
        lv_obj_remove_flag(ui->view.home_indicator, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(ui->view.home_indicator, LV_OBJ_FLAG_HIDDEN);
    launcher_view_layout_quick_panel(&ui->view, metrics.panel_height);
    if (!ui->quick_panel_open) {
        lv_anim_delete(ui->view.quick_panel, animate_object_y);
        lv_obj_set_y(ui->view.quick_panel, -metrics.panel_height);
    }
    lv_label_set_text(ui->view.orientation_label,
                      ui->orientation == LAUNCHER_ORIENTATION_LANDSCAPE ? "Portrait" : "Landscape");
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
    lv_obj_add_event_cb(ui->view.orientation_button, orientation_clicked, LV_EVENT_CLICKED, ui);
    attach_pull_down_input(ui);
    return ui;
}

void launcher_ui_destroy(launcher_ui_t *ui)
{
    if (!ui) return;
    if (ui->input) {
        lv_indev_remove_event_cb_with_user_data(ui->input, pull_down_input_event, ui);
        ui->input = nullptr;
    }
    if (ui->view.screen) lv_obj_remove_event_cb_with_user_data(ui->view.screen, screen_size_changed, ui);
    if (ui->view.quick_scrim) {
        lv_anim_delete(ui->view.quick_scrim, animate_object_opacity);
        lv_obj_remove_event_cb_with_user_data(ui->view.quick_scrim, scrim_clicked, ui);
    }
    if (ui->view.quick_panel) lv_anim_delete(ui->view.quick_panel, animate_object_y);
    if (ui->view.orientation_button)
        lv_obj_remove_event_cb_with_user_data(ui->view.orientation_button, orientation_clicked, ui);
    if (ui->view.screen && ui->previous_screen && lv_obj_is_valid(ui->previous_screen) &&
        lv_screen_active() == ui->view.screen)
        lv_screen_load(ui->previous_screen);
    launcher_view_destroy(&ui->view);
    ui->pages = nullptr;
    delete ui;
}

void launcher_ui_rebind_input(launcher_ui_t *ui)
{
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
    ui->orientation                 = orientation;
    ui->orientation_request_pending = false;
    ui->quick_panel_open            = false;
    lv_anim_delete(ui->view.quick_scrim, animate_object_opacity);
    lv_anim_delete(ui->view.quick_panel, animate_object_y);
    lv_obj_add_flag(ui->view.quick_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(ui->view.quick_scrim, LV_OPA_TRANSP, 0);
    relayout(ui);
}

void launcher_ui_show_shutdown(launcher_ui_t *ui)
{
    if (!ui || !ui->view.shutdown_scrim || ui->shutdown_visible) return;
    ui->shutdown_visible = true;
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
