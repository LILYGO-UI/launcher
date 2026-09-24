#ifndef LAUNCHER_UI_H
#define LAUNCHER_UI_H

#include "registry.h"

#include <lvgl.h>
#include <stdbool.h>

#define LAUNCHER_DEFAULT_WIDTH  568
#define LAUNCHER_DEFAULT_HEIGHT 1232
#define LAUNCHER_APPS_PER_PAGE  18

typedef struct launcher_ui launcher_ui_t;

typedef enum {
    LAUNCHER_ORIENTATION_PORTRAIT  = 0,
    LAUNCHER_ORIENTATION_LANDSCAPE = 1,
} launcher_orientation_t;

#ifdef __cplusplus
extern "C" {
#endif

launcher_ui_t *launcher_ui_create(launcher_registry_t *registry);
void launcher_ui_destroy(launcher_ui_t *ui);
/* Reattach gestures after the old input was deleted and a new one created. */
void launcher_ui_rebind_input(launcher_ui_t *ui);
bool launcher_ui_is_home(const launcher_ui_t *ui);
void launcher_ui_prepare_registry_refresh(launcher_ui_t *ui);
void launcher_ui_refresh_home(launcher_ui_t *ui);
void launcher_ui_apply_orientation(launcher_ui_t *ui, launcher_orientation_t orientation);
void launcher_ui_show_shutdown(launcher_ui_t *ui);
void launcher_ui_hide_shutdown(launcher_ui_t *ui);
bool launcher_ui_take_orientation_request(launcher_ui_t *ui, launcher_orientation_t *orientation);
const launcher_app_t *launcher_ui_take_launch_request(launcher_ui_t *ui);

#ifdef __cplusplus
}
#endif

#endif
