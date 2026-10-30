#ifndef LAUNCHER_DISPLAY_SESSION_H
#define LAUNCHER_DISPLAY_SESSION_H

#include "process.h"
#include "ui.h"
#include <cm0/input.h>

namespace launcher {

// Owns the display, input and UI together so a DRM handoff cannot leave them
// attached to different display generations. LVGL must outlive this session.
class DisplaySession {
public:
    DisplaySession(const launcher_process_options_t &options, launcher_registry_t *registry);
    ~DisplaySession();

    DisplaySession(const DisplaySession &)            = delete;
    DisplaySession &operator=(const DisplaySession &) = delete;

    bool initialize(launcher_orientation_t orientation);
    bool alive() const;
    launcher_ui_t *ui() const;
    launcher_orientation_t orientation() const;
    // True when the detachable CM0 TCA8418 keyboard is currently present.
    // This is kept separate from generic evdev keyboard discovery so the
    // account screen can suppress its software keyboard only for the
    // hardware keypad.
    bool tca8418_present() const;
    void set_orientation(launcher_orientation_t orientation);
    // Only a new TCA8418 connection selects landscape. Manual rotation and
    // disconnects preserve the selected direction until the next connection.
    // While suspended, only hardware/state are updated; rotation waits for resume.
    void poll_keyboard();
    void refresh();
    void refresh_ready();

    // Pair these around the foreground child process, including failed execs.
    // resume() rebuilds the display if retaining its resources is unavailable
    // or fails. retained() describes the most recent handoff for diagnostics.
    void suspend();
    bool resume();
    bool retained() const;

private:
    static void display_deleted(lv_event_t *event);
    static void input_deleted(lv_event_t *event);
    static void keyboard_deleted(lv_event_t *event);
    static void keyboard_event(lv_event_t *event);
    bool uses_drm() const;
    bool create_display();
    void open_input();
    void open_keyboard();
    void close_input();
    void clear_display();
    bool create_ui();
    void apply_orientation();

    const launcher_process_options_t options_;
    launcher_registry_t *registry_;
    lv_display_t *display_              = nullptr;
    lv_indev_t *input_                  = nullptr;
    lv_indev_t *keyboard_               = nullptr;
    lv_group_t *input_group_            = nullptr;
    launcher_ui_t *ui_                  = nullptr;
    launcher_orientation_t orientation_ = LAUNCHER_ORIENTATION_PORTRAIT;
    bool retained_                      = false;
    bool paused_                        = false;
#if defined(LAUNCHER_SIMULATOR)
    bool sdl_initialized_ = false;
#else
    bool tca8418_present_ = false;
    bool tca8418_landscape_pending_ = false;
#endif
};

}  // namespace launcher

#endif
