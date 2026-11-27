#ifndef LAUNCHER_UI_H
#define LAUNCHER_UI_H

#include "registry.h"

#include <lvgl.h>
#include <stdbool.h>
#include <stdint.h>

#define LAUNCHER_DEFAULT_WIDTH  568
#define LAUNCHER_DEFAULT_HEIGHT 1232
#define LAUNCHER_APPS_PER_PAGE  18

typedef struct launcher_ui launcher_ui_t;

typedef enum {
    LAUNCHER_ORIENTATION_PORTRAIT  = 0,
    LAUNCHER_ORIENTATION_LANDSCAPE = 1,
} launcher_orientation_t;

typedef enum {
    LAUNCHER_POWER_NONE = 0,
    LAUNCHER_POWER_OFF,
    LAUNCHER_POWER_REBOOT,
} launcher_power_action_t;

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
/* Update the quick panel account actions. Passing NULL hides lock and sign out. */
void launcher_ui_set_session_user(launcher_ui_t *ui, const char *name);
/* Local appliance sessions retain file sharing but have no account service. */
void launcher_ui_set_account_actions_enabled(launcher_ui_t *ui, bool enabled);
bool launcher_ui_take_lock_request(launcher_ui_t *ui);
bool launcher_ui_take_logout_request(launcher_ui_t *ui);
void launcher_ui_set_file_share_directory(launcher_ui_t *ui, const char *path);
void launcher_ui_set_file_share_status(launcher_ui_t *ui, const char *status);
void launcher_ui_set_file_share_approval(launcher_ui_t *ui, const char *filename, uint64_t size, bool pending);
bool launcher_ui_take_file_share_decision(launcher_ui_t *ui, bool *accepted);
bool launcher_ui_file_share_open(const launcher_ui_t *ui);
/* Synchronize device capabilities and levels without interrupting a drag. */
void launcher_ui_set_system_controls(launcher_ui_t *ui, bool brightness_available, int brightness_percent,
                                     bool volume_available, int volume_percent, bool power_available, bool reboot_available);
void launcher_ui_set_software_brightness(launcher_ui_t *ui, bool enabled, int percent);
bool launcher_ui_take_brightness_request(launcher_ui_t *ui, int *percent);
bool launcher_ui_take_volume_request(launcher_ui_t *ui, int *percent);
launcher_power_action_t launcher_ui_take_power_request(launcher_ui_t *ui);
void launcher_ui_set_power_result(launcher_ui_t *ui, bool success, bool simulated);
/* Block home actions and discard queued requests while an account screen is active. */
void launcher_ui_set_input_enabled(launcher_ui_t *ui, bool enabled);
/* Handle a logical LVGL key or character. Returns true when launcher consumes it. */
bool launcher_ui_handle_key(launcher_ui_t *ui, uint32_t key);
/* Process an LV_EVENT_KEY sample with press edges and paced direction repeats. */
bool launcher_ui_handle_keyboard_input(launcher_ui_t *ui, lv_indev_t *keyboard);
void launcher_ui_show_shutdown(launcher_ui_t *ui);
void launcher_ui_hide_shutdown(launcher_ui_t *ui);
bool launcher_ui_take_orientation_request(launcher_ui_t *ui, launcher_orientation_t *orientation);
const launcher_app_t *launcher_ui_take_launch_request(launcher_ui_t *ui);

#ifdef __cplusplus
}
#endif

#endif
