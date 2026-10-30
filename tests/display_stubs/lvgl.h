#ifndef LAUNCHER_TEST_LVGL_H
#define LAUNCHER_TEST_LVGL_H

#include <stdint.h>

#define LV_USE_DRAW_NANOVG   0
#define LV_USE_DRAW_OPENGLES LV_LINUX_DRM_USE_EGL

struct lv_display_t;
struct lv_indev_t;
struct lv_obj_t;
struct lv_event_t;
struct lv_group_t;
using lv_event_cb_t = void (*)(lv_event_t *);
enum lv_result_t { LV_RESULT_INVALID, LV_RESULT_OK };
enum lv_display_rotation_t { LV_DISPLAY_ROTATION_0, LV_DISPLAY_ROTATION_90 };
enum lv_indev_state_t { LV_INDEV_STATE_RELEASED, LV_INDEV_STATE_PRESSED };
enum { LV_EVENT_DELETE = 1, LV_EVENT_KEY = 2, LV_EVENT_PRESSED = 3, LV_INDEV_TYPE_POINTER = 1,
       LV_INDEV_TYPE_KEYPAD = 2, LV_KEY_ESC = 27 };

void *lv_event_get_user_data(lv_event_t *event);
lv_display_t *lv_linux_fbdev_create();
lv_display_t *lv_linux_drm_create();
lv_result_t lv_linux_fbdev_set_file(lv_display_t *display, const char *path);
lv_result_t lv_linux_drm_set_file(lv_display_t *display, const char *path, int64_t connector);
#if LV_LINUX_DRM_USE_EGL && defined(LILYGO_UI_HAS_DRM_HANDOFF)
lv_result_t lv_linux_drm_suspend(lv_display_t *display);
lv_result_t lv_linux_drm_resume(lv_display_t *display);
#endif
void lv_display_add_event_cb(lv_display_t *display, lv_event_cb_t callback, int code, void *data);
void lv_display_delete(lv_display_t *display);
lv_display_rotation_t lv_display_get_rotation(lv_display_t *display);
void lv_display_set_rotation(lv_display_t *display, lv_display_rotation_t rotation);
int32_t lv_display_get_horizontal_resolution(lv_display_t *display);
int32_t lv_display_get_vertical_resolution(lv_display_t *display);
lv_indev_t *lv_evdev_create(int type, const char *path);
void lv_indev_add_event_cb(lv_indev_t *input, lv_event_cb_t callback, int code, void *data);
void lv_indev_delete(lv_indev_t *input);
void lv_indev_set_group(lv_indev_t *input, lv_group_t *group);
lv_indev_state_t lv_indev_get_state(const lv_indev_t *input);
uint32_t lv_indev_get_key(const lv_indev_t *input);
void lv_indev_stop_processing(lv_indev_t *input);
lv_group_t *lv_group_create();
void lv_group_set_default(lv_group_t *group);
void lv_group_delete(lv_group_t *group);
lv_obj_t *lv_screen_active();
void lv_obj_invalidate(lv_obj_t *object);
void lv_refr_now(lv_display_t *display);
void lv_timer_enable(bool enable);

#endif
