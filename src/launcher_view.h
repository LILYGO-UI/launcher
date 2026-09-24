#ifndef LAUNCHER_VIEW_H
#define LAUNCHER_VIEW_H

#include <lvgl.h>

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *app_grid;
    lv_obj_t *page_indicator;
    lv_obj_t *page_bottom_inset;
    lv_obj_t *quick_scrim;
    lv_obj_t *quick_panel;
    lv_obj_t *orientation_button;
    lv_obj_t *orientation_label;
    lv_obj_t *shutdown_scrim;
    lv_obj_t *shutdown_panel;
    lv_obj_t *status_bar;
    lv_obj_t *home_indicator;
    const lv_font_t *font_22;
    const lv_font_t *font_28;
    const lv_font_t *font_36;
} launcher_view_t;

bool launcher_view_create(launcher_view_t *view);
void launcher_view_destroy(launcher_view_t *view);
lv_obj_t *launcher_view_create_app_tile(const launcher_view_t *view, lv_obj_t *parent, const char *label,
                                        const char *icon_path, const char *fallback_icon, lv_color_t icon_background,
                                        int32_t width, int32_t height);
void launcher_view_layout_app_tile(lv_obj_t *tile, int32_t width, int32_t height);
void launcher_view_layout_quick_panel(launcher_view_t *view, int32_t height);

#endif
