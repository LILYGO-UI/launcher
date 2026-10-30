#include "display_session.h"
#include "app_identity.h"
#include "keyboard_device.h"
#include "logging.h"

#include <stdlib.h>
#include <string.h>

#if defined(LAUNCHER_SIMULATOR)
#include <SDL.h>
#endif

namespace launcher {

#if defined(LAUNCHER_SIMULATOR)
static void fit_simulator_window(lv_display_t *display)
{
    SDL_Window *window = lv_sdl_window_get_window(display);
    int display_index  = window ? SDL_GetWindowDisplayIndex(window) : -1;
    SDL_Rect usable_bounds;
    if (display_index < 0 || SDL_GetDisplayUsableBounds(display_index, &usable_bounds) != 0) return;

    const int width             = lv_display_get_horizontal_resolution(display);
    const int height            = lv_display_get_vertical_resolution(display);
    const float usable_fraction = 0.90f;
    const float horizontal_zoom = usable_bounds.w * usable_fraction / (float)width;
    const float vertical_zoom   = usable_bounds.h * usable_fraction / (float)height;
    const float zoom            = horizontal_zoom < vertical_zoom ? horizontal_zoom : vertical_zoom;
    if (zoom >= 1.0f) return;

    lv_sdl_window_set_zoom(display, zoom);
    lv_display_set_resolution(display, width, height);
    const int window_width  = (int)(width * zoom);
    const int window_height = (int)(height * zoom);
    SDL_SetWindowPosition(window, usable_bounds.x + (usable_bounds.w - window_width) / 2,
                          usable_bounds.y + (usable_bounds.h - window_height) / 2);
    logging::info("simulator scaled to %.0f%% to fit the display", zoom * 100.0f);
}
#endif

DisplaySession::DisplaySession(const launcher_process_options_t &options, launcher_registry_t *registry)
    : options_(options), registry_(registry)
{
}

DisplaySession::~DisplaySession()
{
    close_input();
    clear_display();
    if (input_group_) {
        lv_group_set_default(nullptr);
        lv_group_delete(input_group_);
        input_group_ = nullptr;
    }
    if (paused_) lv_timer_enable(true);
#if defined(LAUNCHER_SIMULATOR)
    if (sdl_initialized_) lv_sdl_quit();
#endif
}

void DisplaySession::display_deleted(lv_event_t *event)
{
    auto *session     = static_cast<DisplaySession *>(lv_event_get_user_data(event));
    session->display_ = nullptr;
}

void DisplaySession::input_deleted(lv_event_t *event)
{
    auto *session   = static_cast<DisplaySession *>(lv_event_get_user_data(event));
    session->input_ = nullptr;
}

void DisplaySession::keyboard_deleted(lv_event_t *event)
{
    auto *session      = static_cast<DisplaySession *>(lv_event_get_user_data(event));
    session->keyboard_ = nullptr;
}

void DisplaySession::keyboard_event(lv_event_t *event)
{
    auto *session = static_cast<DisplaySession *>(lv_event_get_user_data(event));
    if (!session || !session->keyboard_) return;
    if (launcher_ui_handle_keyboard_input(session->ui_, session->keyboard_))
        lv_indev_stop_processing(session->keyboard_);
}

bool DisplaySession::uses_drm() const
{
    return strcmp(options_.display_backend, "drm") == 0;
}

bool DisplaySession::alive() const
{
    return display_ != nullptr;
}

launcher_ui_t *DisplaySession::ui() const
{
    return ui_;
}

launcher_orientation_t DisplaySession::orientation() const
{
    return lv_display_get_horizontal_resolution(display_) > lv_display_get_vertical_resolution(display_)
               ? LAUNCHER_ORIENTATION_LANDSCAPE
               : LAUNCHER_ORIENTATION_PORTRAIT;
}

bool DisplaySession::tca8418_present() const
{
#if defined(LAUNCHER_SIMULATOR)
    return false;
#else
    return tca8418_present_;
#endif
}

void DisplaySession::apply_orientation()
{
#if defined(LAUNCHER_SIMULATOR)
    const bool landscape = orientation_ == LAUNCHER_ORIENTATION_LANDSCAPE;
    lv_display_set_resolution(display_, landscape ? options_.height : options_.width,
                              landscape ? options_.width : options_.height);
#else
    const lv_display_rotation_t rotation =
        orientation_ == LAUNCHER_ORIENTATION_LANDSCAPE ? LV_DISPLAY_ROTATION_90 : LV_DISPLAY_ROTATION_0;
    if (lv_display_get_rotation(display_) != rotation) lv_display_set_rotation(display_, rotation);
#endif
}

void DisplaySession::set_orientation(launcher_orientation_t orientation)
{
    orientation_ = orientation;
    apply_orientation();
    launcher_ui_apply_orientation(ui_, orientation_);
    refresh();
}

void DisplaySession::poll_keyboard()
{
#if !defined(LAUNCHER_SIMULATOR)
    if (!display_ && !paused_) return;

    // Power loss resets the chip while its old input node can remain alive.
    // Let the kernel reinitialize it before looking for a usable keyboard.
    const bool present = update_tca8418_keyboard();
    if (present && !tca8418_present_) tca8418_landscape_pending_ = true;
    tca8418_present_ = present;
    if (paused_) return;

    // Retry discovery after startup or an evdev deletion so newly registered
    // keyboards can also navigate the launcher.
    if (!keyboard_) {
        open_keyboard();
        if (keyboard_) launcher_ui_rebind_input(ui_);
    }

    if (tca8418_landscape_pending_ && orientation() != LAUNCHER_ORIENTATION_LANDSCAPE) {
        set_orientation(LAUNCHER_ORIENTATION_LANDSCAPE);
        logging::info("TCA8418 connected; display orientation changed to landscape");
    }
    tca8418_landscape_pending_ = false;
#endif
}

bool DisplaySession::create_display()
{
    if (strcmp(options_.display_backend, "sdl") == 0) {
#if defined(LAUNCHER_SIMULATOR)
        SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
        display_ = lv_sdl_window_create(options_.width, options_.height);
        if (display_) {
            sdl_initialized_ = true;
            lv_sdl_window_set_title(display_, LILYGO_UI_LAUNCHER_APP_NAME);
            lv_sdl_window_set_resizeable(display_, true);
        }
#else
        logging::error("SDL backend is not included in this build");
#endif
    } else if (strcmp(options_.display_backend, "fbdev") == 0) {
#if !defined(LAUNCHER_SIMULATOR)
        display_ = lv_linux_fbdev_create();
        if (display_ && lv_linux_fbdev_set_file(display_, options_.fbdev_path) != LV_RESULT_OK) {
            logging::error("cannot initialize framebuffer '%s'", options_.fbdev_path);
            clear_display();
        }
#else
        logging::error("fbdev backend is only included in device builds");
#endif
    } else if (uses_drm()) {
#if !defined(LAUNCHER_SIMULATOR)
        display_ = lv_linux_drm_create();
        if (display_ && lv_linux_drm_set_file(display_, options_.drm_path, options_.drm_connector_id) != LV_RESULT_OK) {
            logging::error("cannot initialize DRM device '%s' connector %lld", options_.drm_path,
                           (long long)options_.drm_connector_id);
            clear_display();
        }
#else
        logging::error("DRM backend is only included in device builds");
#endif
    } else {
        logging::error("unknown display backend '%s'", options_.display_backend);
    }
    if (!display_) return false;
    lv_display_add_event_cb(display_, display_deleted, LV_EVENT_DELETE, this);
    apply_orientation();
    return true;
}

void DisplaySession::open_input()
{
    input_ = cm0_input_create_pointer(options_.input_path);
    if (!input_) logging::warn("cannot initialize input device '%s'", options_.input_path);
    open_keyboard();
    if (input_) lv_indev_add_event_cb(input_, input_deleted, LV_EVENT_DELETE, this);
}

void DisplaySession::open_keyboard()
{
    // AppKit discovers keyboard event nodes independently from the configured
    // pointer path. The pointer path usually names the touchscreen, while the
    // TCA8418 matrix has its own event node and may be hot-swapped.
    keyboard_ = cm0_input_create_keyboard(nullptr);
    if (keyboard_ && input_group_) {
        lv_indev_set_group(keyboard_, input_group_);
        lv_indev_add_event_cb(keyboard_, keyboard_event, LV_EVENT_KEY, this);
        lv_indev_add_event_cb(keyboard_, keyboard_deleted, LV_EVENT_DELETE, this);
    }
}

void DisplaySession::close_input()
{
    if (input_) lv_indev_delete(input_);
    input_ = nullptr;
    if (keyboard_) lv_indev_delete(keyboard_);
    keyboard_ = nullptr;
}

void DisplaySession::clear_display()
{
    // UI callbacks must be detached while their display still exists.
    launcher_ui_destroy(ui_);
    ui_ = nullptr;
    if (display_) lv_display_delete(display_);
    display_ = nullptr;
}

bool DisplaySession::create_ui()
{
    ui_ = launcher_ui_create(registry_);
    if (!ui_) logging::error("cannot allocate UI");
    return ui_ != nullptr;
}

bool DisplaySession::initialize(launcher_orientation_t orientation)
{
    orientation_ = orientation;
    if (!create_display()) return false;
    orientation_ = this->orientation();
#if defined(LAUNCHER_SIMULATOR)
    fit_simulator_window(display_);
#endif
    logging::info("initialized %s display at %dx%d (renderer=%s)", options_.display_backend,
                  (int)lv_display_get_horizontal_resolution(display_),
                  (int)lv_display_get_vertical_resolution(display_),
                  !uses_drm()            ? "software"
                  : LV_USE_DRAW_NANOVG   ? "nanovg"
                  : LV_USE_DRAW_OPENGLES ? "opengles"
                  : "software");
    input_group_ = lv_group_create();
    lv_group_set_default(input_group_);
    open_input();
    if (!create_ui()) return false;
    poll_keyboard();
    return true;
}

void DisplaySession::refresh()
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(display_);
}

void DisplaySession::refresh_ready()
{
    refresh();
    logging::info("UI ready at %dx%d", (int)lv_display_get_horizontal_resolution(display_),
                  (int)lv_display_get_vertical_resolution(display_));
}

void DisplaySession::suspend()
{
    orientation_ = orientation();
    lv_refr_now(display_);
    lv_timer_enable(false);
    paused_   = true;
    retained_ = false;
#if defined(LAUNCHER_SIMULATOR)
    SDL_Window *window = lv_sdl_window_get_window(display_);
    if (window) SDL_HideWindow(window);
#else
    if (uses_drm()) {
#if LV_LINUX_DRM_USE_EGL && defined(LILYGO_UI_HAS_DRM_HANDOFF)
        const char *recreate = getenv("LILYGO_UI_DRM_RECREATE");
        retained_ = (!recreate || strcmp(recreate, "1") != 0) && lv_linux_drm_suspend(display_) == LV_RESULT_OK;
#endif
        close_input();
        if (!retained_) clear_display();
    }
#endif
}

bool DisplaySession::resume()
{
#if defined(LAUNCHER_SIMULATOR)
    SDL_Window *window = lv_sdl_window_get_window(display_);
    if (window) {
        SDL_ShowWindow(window);
        SDL_RaiseWindow(window);
    }
#else
    if (uses_drm()) {
#if LV_LINUX_DRM_USE_EGL && defined(LILYGO_UI_HAS_DRM_HANDOFF)
        if (retained_ && lv_linux_drm_resume(display_) != LV_RESULT_OK) {
            logging::warn("cannot resume DRM display; rebuilding it");
            clear_display();
            retained_ = false;
        }
#endif
        if (!retained_ && !create_display()) {
            logging::error("cannot restore %s display", options_.display_backend);
            return false;
        }
        open_input();
        if (retained_)
            launcher_ui_rebind_input(ui_);
        else if (!create_ui())
            return false;

        // Prime both software scanout buffers after recreation. EGL retains a
        // complete texture and only needs the final refresh below.
#if !LV_LINUX_DRM_USE_EGL
        refresh();
#endif
    }
#endif
    lv_timer_enable(true);
    paused_ = false;
    poll_keyboard();
    refresh_ready();
    return true;
}

bool DisplaySession::retained() const
{
    return retained_;
}

}  // namespace launcher
