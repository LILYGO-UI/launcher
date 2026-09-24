#ifndef LAUNCHER_DISPLAY_SESSION_H
#define LAUNCHER_DISPLAY_SESSION_H

#include "process.h"
#include "ui.h"

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
    void set_orientation(launcher_orientation_t orientation);
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
    bool uses_drm() const;
    bool create_display();
    void open_input();
    void close_input();
    void clear_display();
    bool create_ui();
    void apply_orientation();

    const launcher_process_options_t options_;
    launcher_registry_t *registry_;
    lv_display_t *display_              = nullptr;
    lv_indev_t *input_                  = nullptr;
    launcher_ui_t *ui_                  = nullptr;
    launcher_orientation_t orientation_ = LAUNCHER_ORIENTATION_PORTRAIT;
    bool retained_                      = false;
    bool paused_                        = false;
#if defined(LAUNCHER_SIMULATOR)
    bool sdl_initialized_ = false;
#endif
};

}  // namespace launcher

#endif
