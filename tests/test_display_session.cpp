// Compile the production controller against resource-checking LVGL/UI doubles.
// No real display, input device, SDK library or application process is used.
#include "display_session.h"
#include "keyboard_device.h"
#include "logging.h"

#include <assert.h>
#include <stdlib.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

struct DeleteHook {
    lv_event_cb_t callback = nullptr;
    void *data             = nullptr;
};
struct lv_obj_t {};
struct lv_event_t {
    void *data;
};
struct lv_display_t {
    bool alive                     = true;
    bool suspended                 = false;
    lv_display_rotation_t rotation = LV_DISPLAY_ROTATION_0;
    DeleteHook hook;
};
struct lv_indev_t {
    bool alive = true;
    bool tca8418 = false;
    bool invalidated = false;
    lv_display_t *display;
    int type = LV_INDEV_TYPE_POINTER;
    lv_group_t *group = nullptr;
    DeleteHook hook;
    DeleteHook key_hook;
};
struct lv_group_t {};
struct launcher_ui {
    bool alive = true;
    lv_display_t *display;
    lv_indev_t *input;
};

#if LV_LINUX_DRM_USE_EGL && defined(LILYGO_UI_HAS_DRM_HANDOFF)
constexpr bool supports_handoff = true;
#else
constexpr bool supports_handoff = false;
#endif

namespace {
struct State {
    // Keep deleted objects until the end of each case so duplicate deletes are
    // deterministic assertions rather than undefined reads from freed memory.
    std::vector<std::unique_ptr<lv_display_t>> displays;
    std::vector<std::unique_ptr<lv_indev_t>> inputs;
    std::vector<std::unique_ptr<launcher_ui_t>> uis;
    std::vector<std::string> events;
    lv_display_t *display = nullptr;
    lv_indev_t *input     = nullptr;
    lv_indev_t *keyboard  = nullptr;
    lv_group_t *group     = nullptr;
    lv_obj_t screen;
    bool timers          = true;
    bool fail_create     = false;
    bool fail_set_file   = false;
    bool fail_ui         = false;
    bool fail_input      = false;
    bool fail_suspend    = false;
    bool fail_resume     = false;
    bool external_delete = false;
    bool ordinary_keyboard_present = false;
    bool tca8418_present = false;
    bool tca8418_configured = true;
    bool fail_keyboard_recovery = false;
    bool fail_keyboard = false;
    bool keyboard_pressed = false;
    int keyboard_scans = 0;
    int keyboard_attempts = 0;
    int keyboard_recoveries = 0;
    int display_queries  = 0;
    int rotations        = 0;
    int refreshes        = 0;
    int rebinds          = 0;
    int suspends         = 0;
    int resumes          = 0;
} state;

void notify_delete(DeleteHook hook)
{
    if (hook.callback) {
        lv_event_t event{hook.data};
        hook.callback(&event);
    }
}

void reset()
{
    assert(state.timers);
    for (const auto &display : state.displays) assert(!display->alive);
    for (const auto &input : state.inputs) assert(!input->alive);
    for (const auto &ui : state.uis) assert(!ui->alive);
    assert(!state.group);
    state = State{};
    unsetenv("LILYGO_UI_DRM_RECREATE");
}

launcher_process_options_t options(const char *backend = "drm")
{
    return {backend, "/dev/fb0", "/dev/dri/card0", 44, "/dev/input/test", "portrait", 568, 1232, nullptr};
}

void assert_order(const char *first, const char *second)
{
    auto a = std::find(state.events.begin(), state.events.end(), first);
    auto b = std::find(state.events.begin(), state.events.end(), second);
    assert(a != state.events.end() && b != state.events.end() && a < b);
}

void repeated_handoff()
{
    reset();
    {
        launcher::DisplaySession session(options(), nullptr);
        assert(session.initialize(LAUNCHER_ORIENTATION_LANDSCAPE));
        assert(session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
        auto *original_ui           = session.ui();
        const int initial_rotations = state.rotations;
        session.set_orientation(LAUNCHER_ORIENTATION_LANDSCAPE);
        assert(state.rotations == initial_rotations);
        for (int cycle = 0; cycle < 20; ++cycle) {
            auto *old_input = state.input;
            state.events.clear();
            session.suspend();
            assert(!state.timers && !old_input->alive && state.input == nullptr);
#if LV_LINUX_DRM_USE_EGL && defined(LILYGO_UI_HAS_DRM_HANDOFF)
            assert(session.retained() && session.alive() && session.ui() == original_ui);
            assert_order("drm-suspend", "input-delete");
#else
            assert(!session.retained() && !session.alive() && session.ui() == nullptr);
            assert_order("input-delete", "ui-delete");
            assert_order("ui-delete", "display-delete");
#endif
            const int before_resume = state.refreshes;
            assert(session.resume());
            assert(session.alive() && state.timers && state.input != old_input);
            assert(session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
            assert(state.refreshes - before_resume == (LV_LINUX_DRM_USE_EGL ? 1 : 2));
#if LV_LINUX_DRM_USE_EGL && defined(LILYGO_UI_HAS_DRM_HANDOFF)
            assert(session.ui() == original_ui && state.rebinds == cycle + 1);
            assert_order("drm-resume", "input-create");
            assert_order("input-create", "ui-rebind");
#else
            assert(session.ui() != original_ui && state.rebinds == 0);
#endif
        }
        assert(state.displays.size() == (supports_handoff ? 1u : 21u));
        state.events.clear();
    }
    assert_order("input-delete", "ui-delete");
    assert_order("ui-delete", "display-delete");
    reset();
}

void recreation_fallbacks()
{
    for (bool forced : {false, true}) {
        reset();
        state.fail_suspend = !forced;
        if (forced) setenv("LILYGO_UI_DRM_RECREATE", "1", 1);
        {
            launcher::DisplaySession session(options(), nullptr);
            assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
            session.suspend();
            assert(!session.retained() && !session.alive());
            assert(state.suspends == ((!forced && supports_handoff) ? 1 : 0));
            assert(session.resume() && session.alive());
            assert(state.displays.size() == 2 && state.rebinds == 0);
        }
        reset();
    }
#if LV_LINUX_DRM_USE_EGL && defined(LILYGO_UI_HAS_DRM_HANDOFF)
    {
        launcher::DisplaySession session(options(), nullptr);
        assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
        session.suspend();
        state.fail_resume = true;
        state.events.clear();
        assert(session.resume());
        assert(!session.retained() && state.displays.size() == 2 && state.rebinds == 0);
        assert_order("ui-delete", "display-delete");
        assert_order("display-delete", "display-create");
    }
    reset();
#endif
}

void failed_recreation_cleanup()
{
    for (int failure = 0; failure < 3; ++failure) {
        reset();
        setenv("LILYGO_UI_DRM_RECREATE", "1", 1);
        {
            launcher::DisplaySession session(options(), nullptr);
            assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
            session.suspend();
            state.fail_create   = failure == 0;
            state.fail_set_file = failure == 1;
            state.fail_ui       = failure == 2;
            assert(!session.resume());
            assert(!state.timers);
            if (failure < 2) assert(!session.alive());
        }
        // Failed resumes still leave the RAII owner responsible for cleanup,
        // including an input opened before UI allocation fails.
        reset();
    }
}

void fbdev_and_external_deletion()
{
    reset();
    {
        launcher::DisplaySession session(options("fbdev"), nullptr);
#if LV_USE_LINUX_FBDEV
        assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
        auto *ui    = session.ui();
        auto *input = state.input;
        session.suspend();
        assert(session.alive() && session.ui() == ui && state.input == input);
        assert(!state.timers && state.suspends == 0);
        int before_resume = state.refreshes;
        assert(session.resume());
        assert(state.refreshes == before_resume + 1 && state.rebinds == 0);
        assert(state.displays.size() == 1 && state.inputs.size() == 1);
#else
        assert(!session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
        assert(!session.alive());
        assert(state.displays.empty() && state.inputs.empty());
#endif
    }
    reset();
    for (bool display_first : {false, true}) {
        {
            launcher::DisplaySession session(options(), nullptr);
            assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
            auto *display         = state.display;
            auto *input           = state.input;
            state.external_delete = true;
            if (display_first) {
                lv_display_delete(display);
                lv_indev_delete(input);
            } else {
                lv_indev_delete(input);
                lv_display_delete(display);
            }
            assert(!session.alive());
        }
        reset();
    }
    {
        launcher::DisplaySession session(options(), nullptr);
        assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
        session.suspend();
        // Destruction while suspended must release retained resources and
        // restore the global timer state without reopening the input.
    }
    reset();
    state.fail_input = true;
    {
        launcher::DisplaySession session(options(), nullptr);
        assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
        session.suspend();
        assert(session.resume());
    }
    reset();
}

void assert_keyboard_bound()
{
    assert(state.keyboard && state.keyboard->alive);
    assert(state.keyboard->display == state.display);
    assert(state.keyboard->group && state.keyboard->group == state.group);
    assert(state.keyboard->hook.callback && state.keyboard->key_hook.callback);
}

void assert_manual_orientations(launcher::DisplaySession &session)
{
    for (auto orientation : {LAUNCHER_ORIENTATION_LANDSCAPE, LAUNCHER_ORIENTATION_PORTRAIT}) {
        session.set_orientation(orientation);
        const int rotations = state.rotations;
        const int refreshes = state.refreshes;
        for (int poll = 0; poll < 3; ++poll) {
            session.poll_keyboard();
            assert(session.orientation() == orientation);
        }
        assert(state.rotations == rotations && state.refreshes == refreshes);
    }
}

void keyboard_startup_and_manual_orientation()
{
    // A keyboard supported by AppKit need not be the detachable TCA8418.
    for (int keyboard = 0; keyboard < 3; ++keyboard) {
        reset();
        state.ordinary_keyboard_present = keyboard == 1;
        state.tca8418_present = keyboard == 2;
        {
            launcher::DisplaySession session(options(), nullptr);
            assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
            assert(session.tca8418_present() == (keyboard == 2));
            assert(session.orientation() == (keyboard == 2 ? LAUNCHER_ORIENTATION_LANDSCAPE
                                                          : LAUNCHER_ORIENTATION_PORTRAIT));
            if (keyboard) assert_keyboard_bound();
            else assert(!state.keyboard);
            auto *original_keyboard = state.keyboard;
            const int attempts = state.keyboard_attempts;
            assert_manual_orientations(session);
            assert(state.keyboard == original_keyboard && state.keyboard_recoveries == 0);
            if (keyboard) assert(state.keyboard_attempts == attempts);
        }
        reset();
    }
}

void keyboard_hotplug_and_open_retry()
{
    reset();
    {
        launcher::DisplaySession session(options(), nullptr);
        // No display means there is no input owner to poll or reopen yet.
        session.poll_keyboard();
        assert(state.keyboard_scans == 0 && state.keyboard_attempts == 0);
        assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
        auto *pointer = state.input;

        state.ordinary_keyboard_present = true;
        session.poll_keyboard();
        assert_keyboard_bound();
        assert(session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);
        state.ordinary_keyboard_present = false;
        lv_indev_delete(state.keyboard);
        session.poll_keyboard();
        assert(!state.keyboard && state.input == pointer && pointer->alive);

        // Detection must not depend on successfully opening the event device.
        state.tca8418_present = true;
        state.fail_keyboard = true;
        session.poll_keyboard();
        assert(session.tca8418_present());
        assert(!state.keyboard && session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
        const int attempts = state.keyboard_attempts;
        session.set_orientation(LAUNCHER_ORIENTATION_PORTRAIT);
        session.poll_keyboard();
        assert(state.keyboard_attempts == attempts + 1);
        assert(session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);

        // A delayed open retries and clears stale UI key state without turning
        // a continuously connected keyboard into another orientation event.
        state.fail_keyboard = false;
        state.keyboard_pressed = true;
        const int rebinds = state.rebinds;
        session.poll_keyboard();
        assert_keyboard_bound();
        assert(state.rebinds == rebinds + 1 && !state.keyboard_pressed);
        assert(state.input == pointer && session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);
        assert_manual_orientations(session);

        // Driver-side input deletion/recreation is not a physical reconnect.
        auto *old_keyboard = state.keyboard;
        lv_indev_delete(old_keyboard);
        session.poll_keyboard();
        assert_keyboard_bound();
        assert(state.keyboard != old_keyboard && state.input == pointer);
        assert(session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);

        session.set_orientation(LAUNCHER_ORIENTATION_LANDSCAPE);
        state.tca8418_present = false;
        // Detection can notice removal before the evdev driver's delete event.
        old_keyboard = state.keyboard;
        session.poll_keyboard();
        assert(!session.tca8418_present());
        assert(old_keyboard->alive && state.keyboard == old_keyboard);
        assert(session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
        lv_indev_delete(state.keyboard);
        session.poll_keyboard();
        assert(!state.keyboard && session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
        assert_manual_orientations(session);

        // Only a fresh physical connection overrides the last manual choice.
        assert(session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);
        state.tca8418_present = true;
        session.poll_keyboard();
        assert_keyboard_bound();
        assert(session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
    }
    reset();
}

void assert_paused_keyboard_poll(launcher::DisplaySession &session)
{
    assert(!state.timers);
    const int scans = state.keyboard_scans;
    const int attempts = state.keyboard_attempts;
    const int queries = state.display_queries;
    const int rotations = state.rotations;
    const int refreshes = state.refreshes;
    const int rebinds = state.rebinds;
    auto *display = state.display;
    auto *keyboard = state.keyboard;
    auto *pointer = state.input;
    auto *group = state.group;
    const auto rotation = display ? display->rotation : LV_DISPLAY_ROTATION_0;
    session.poll_keyboard();
    assert(state.keyboard_scans == scans + 1);
    assert(state.keyboard_attempts == attempts);
    assert(state.display_queries == queries && state.rotations == rotations);
    assert(state.refreshes == refreshes && state.rebinds == rebinds);
    assert(state.display == display && state.keyboard == keyboard);
    assert(state.input == pointer && state.group == group && !state.timers);
    if (display) assert(display->rotation == rotation);
}

void keyboard_orientation_across_suspend()
{
    for (const char *backend : {"drm", "fbdev"}) {
#if !LV_USE_LINUX_FBDEV
        if (std::string(backend) == "fbdev") continue;
#endif
        for (bool forced_recreation : {false, true}) {
            reset();
            if (forced_recreation) setenv("LILYGO_UI_DRM_RECREATE", "1", 1);
            state.tca8418_present = true;
            {
                launcher::DisplaySession session(options(backend), nullptr);
                assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
                assert(session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
                session.set_orientation(LAUNCHER_ORIENTATION_PORTRAIT);
                for (int cycle = 0; cycle < 2; ++cycle) {
                    session.suspend();
                    // A continuously connected keyboard cannot override the
                    // manual choice when returning from an application.
                    assert_paused_keyboard_poll(session);
                    assert(session.resume());
                    assert_keyboard_bound();
                    assert(session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);
                }

                session.suspend();
                state.tca8418_present = false;
                if (state.keyboard) lv_indev_delete(state.keyboard);
                assert_paused_keyboard_poll(session);
                assert(session.resume());
                assert(!state.keyboard && session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);

                session.suspend();
                state.tca8418_present = true;
                assert_paused_keyboard_poll(session);
                assert(session.resume());
                assert_keyboard_bound();
                assert(session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
            }
            reset();
        }
    }
}

void keyboard_reconnect_while_suspended()
{
    for (bool forced_recreation : {false, true}) {
        for (bool disconnect_again : {false, true}) {
            reset();
            if (forced_recreation) setenv("LILYGO_UI_DRM_RECREATE", "1", 1);
            state.tca8418_present = true;
            {
                launcher::DisplaySession session(options(), nullptr);
                assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
                session.set_orientation(LAUNCHER_ORIENTATION_PORTRAIT);
                session.suspend();
                assert(session.retained() == (supports_handoff && !forced_recreation));
                assert(session.alive() == session.retained());
                state.tca8418_present = false;
                assert_paused_keyboard_poll(session);
                state.tca8418_present = true;
                assert_paused_keyboard_poll(session);
                // The new connection is remembered even after another poll
                // observes it continuously present, or it is removed again.
                if (disconnect_again) state.tca8418_present = false;
                assert_paused_keyboard_poll(session);
                assert(session.resume());
                assert(session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
                if (disconnect_again) assert(!state.keyboard);
                else assert_keyboard_bound();

                // Consume the pending change once. A later manual selection
                // must survive polling and another complete app handoff.
                session.set_orientation(LAUNCHER_ORIENTATION_PORTRAIT);
                session.poll_keyboard();
                assert(session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);
                session.suspend();
                assert_paused_keyboard_poll(session);
                assert(session.resume());
                assert(session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);
            }
            reset();
        }
    }
}

void keyboard_power_loss_recovery()
{
    // Unbinding the kernel driver invalidates its event device. LVGL reports
    // deletion asynchronously when it next reads the old device, not inside
    // the detector. Keep that ownership boundary visible to the controller.
    for (bool observe_removal : {false, true}) {
        reset();
        state.tca8418_present = true;
        {
            launcher::DisplaySession session(options(), nullptr);
            assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
            auto *old_keyboard = state.keyboard;
            auto *pointer = state.input;
            auto *group = state.group;
            const int attempts = state.keyboard_attempts;
            session.set_orientation(LAUNCHER_ORIENTATION_PORTRAIT);
            state.tca8418_configured = false;
            if (observe_removal) {
                state.tca8418_present = false;
                session.poll_keyboard();
                assert(session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);
                assert(old_keyboard->alive && !old_keyboard->invalidated);
                assert(state.keyboard_recoveries == 0);
                state.tca8418_present = true;
            }

            session.poll_keyboard();
            assert(state.keyboard_recoveries == 1 && state.tca8418_configured);
            assert(state.keyboard == old_keyboard && old_keyboard->alive && old_keyboard->invalidated);
            assert(state.keyboard_attempts == attempts);
            const auto expected_orientation = observe_removal ? LAUNCHER_ORIENTATION_LANDSCAPE
                                                             : LAUNCHER_ORIENTATION_PORTRAIT;
            assert(session.orientation() == expected_orientation);

            state.keyboard_pressed = true;
            lv_indev_delete(old_keyboard);
            state.events.clear();
            const int rebinds = state.rebinds;
            session.poll_keyboard();
            assert_keyboard_bound();
            assert(state.keyboard != old_keyboard && !state.keyboard->invalidated);
            assert(state.group == group && state.input == pointer && pointer->alive);
            assert(state.keyboard_attempts == attempts + 1 && state.keyboard_recoveries == 1);
            assert(state.rebinds == rebinds + 1 && !state.keyboard_pressed);
            assert(session.orientation() == expected_orientation);
            assert_order("keyboard-update", "keyboard-create");
            assert_order("keyboard-create", "ui-rebind");
            assert_manual_orientations(session);
        }
        reset();
    }
}

void keyboard_recovery_retry_and_usb_ownership()
{
    reset();
    {
        launcher::DisplaySession session(options(), nullptr);
        assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
        state.tca8418_present = true;
        state.tca8418_configured = false;
        state.fail_keyboard_recovery = true;
        session.poll_keyboard();
        assert(!state.keyboard && state.keyboard_recoveries == 1);
        assert(session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
        session.set_orientation(LAUNCHER_ORIENTATION_PORTRAIT);
        for (int retry = 0; retry < 3; ++retry) {
            session.poll_keyboard();
            assert(!state.keyboard && state.keyboard_recoveries == retry + 2);
            assert(session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);
        }
        state.fail_keyboard_recovery = false;
        state.events.clear();
        session.poll_keyboard();
        assert_keyboard_bound();
        assert(state.tca8418_configured && state.keyboard_recoveries == 5);
        assert(session.orientation() == LAUNCHER_ORIENTATION_PORTRAIT);
        assert_order("keyboard-recover", "keyboard-create");
    }
    reset();

    state.ordinary_keyboard_present = true;
    {
        launcher::DisplaySession session(options(), nullptr);
        assert(session.initialize(LAUNCHER_ORIENTATION_PORTRAIT));
        auto *usb_keyboard = state.keyboard;
        const int attempts = state.keyboard_attempts;
        assert(usb_keyboard && !usb_keyboard->tca8418);
        state.tca8418_present = true;
        state.tca8418_configured = false;
        session.poll_keyboard();
        assert(state.keyboard_recoveries == 1 && state.tca8418_configured);
        assert(state.keyboard == usb_keyboard && usb_keyboard->alive && !usb_keyboard->invalidated);
        assert(state.keyboard_attempts == attempts);
        assert(session.orientation() == LAUNCHER_ORIENTATION_LANDSCAPE);
        assert_keyboard_bound();
        assert_manual_orientations(session);
    }
    reset();
}
}  // namespace

lv_display_t *lv_linux_drm_create()
{
    if (state.fail_create) return nullptr;
    assert(!state.display);
    state.displays.push_back(std::make_unique<lv_display_t>());
    state.display = state.displays.back().get();
    state.events.emplace_back("display-create");
    return state.display;
}
lv_display_t *lv_linux_fbdev_create()
{
    return lv_linux_drm_create();
}
lv_result_t lv_linux_drm_set_file(lv_display_t *display, const char *, int64_t)
{
    assert(display->alive);
    return state.fail_set_file ? LV_RESULT_INVALID : LV_RESULT_OK;
}
lv_result_t lv_linux_fbdev_set_file(lv_display_t *display, const char *path)
{
    return lv_linux_drm_set_file(display, path, 0);
}
void *lv_event_get_user_data(lv_event_t *event)
{
    return event->data;
}
void lv_display_add_event_cb(lv_display_t *display, lv_event_cb_t cb, int code, void *data)
{
    assert(display->alive && code == LV_EVENT_DELETE);
    display->hook = {cb, data};
}
void lv_display_delete(lv_display_t *display)
{
    assert(display && display->alive);
    for (const auto &ui : state.uis) assert(state.external_delete || !ui->alive || ui->display != display);
    state.events.emplace_back("display-delete");
    display->alive = false;
    state.display  = nullptr;
    notify_delete(display->hook);
}
lv_display_rotation_t lv_display_get_rotation(lv_display_t *display)
{
    assert(display && display->alive);
    ++state.display_queries;
    return display->rotation;
}
void lv_display_set_rotation(lv_display_t *display, lv_display_rotation_t rotation)
{
    assert(display->alive);
    display->rotation = rotation;
    ++state.rotations;
}
int32_t lv_display_get_horizontal_resolution(lv_display_t *display)
{
    return lv_display_get_rotation(display) == LV_DISPLAY_ROTATION_0 ? 568 : 1232;
}
int32_t lv_display_get_vertical_resolution(lv_display_t *display)
{
    return lv_display_get_rotation(display) == LV_DISPLAY_ROTATION_0 ? 1232 : 568;
}
lv_indev_t *lv_evdev_create(int type, const char *)
{
    assert(type == LV_INDEV_TYPE_POINTER && state.display && state.display->alive && !state.input);
    if (state.fail_input) return nullptr;
    state.inputs.push_back(std::make_unique<lv_indev_t>());
    state.input          = state.inputs.back().get();
    state.input->display = state.display;
    state.events.emplace_back("input-create");
    return state.input;
}
extern "C" lv_indev_t *cm0_input_create_pointer(const char *path)
{
    return lv_evdev_create(LV_INDEV_TYPE_POINTER, path);
}
extern "C" lv_indev_t *cm0_input_create_keyboard(bool *present)
{
    assert(state.display && state.display->alive && !state.keyboard);
    ++state.keyboard_attempts;
    const bool connected = state.ordinary_keyboard_present ||
                           (state.tca8418_present && state.tca8418_configured);
    if (present) *present = connected;
    if (!connected || state.fail_keyboard) return nullptr;
    state.inputs.push_back(std::make_unique<lv_indev_t>());
    state.keyboard = state.inputs.back().get();
    state.keyboard->display = state.display;
    state.keyboard->type = LV_INDEV_TYPE_KEYPAD;
    state.keyboard->tca8418 = !state.ordinary_keyboard_present;
    state.events.emplace_back("keyboard-create");
    return state.keyboard;
}

extern "C" bool cm0_input_keyboard_present()
{
    return state.ordinary_keyboard_present || state.tca8418_present;
}
namespace launcher {
bool update_tca8418_keyboard(const char *, const char *, const char *, int (*)(const char *, unsigned int))
{
    ++state.keyboard_scans;
    state.events.emplace_back("keyboard-update");
    if (state.tca8418_present && !state.tca8418_configured) {
        ++state.keyboard_recoveries;
        if (!state.fail_keyboard_recovery) {
            state.tca8418_configured = true;
            state.events.emplace_back("keyboard-recover");
            if (state.keyboard && state.keyboard->tca8418) state.keyboard->invalidated = true;
        }
    }
    return state.tca8418_present;
}
}
void lv_indev_add_event_cb(lv_indev_t *input, lv_event_cb_t cb, int code, void *data)
{
    assert(input->alive);
    if (code == LV_EVENT_DELETE) input->hook = {cb, data};
    else {
        assert(input->type == LV_INDEV_TYPE_KEYPAD && code == LV_EVENT_KEY);
        input->key_hook = {cb, data};
    }
}
void lv_indev_delete(lv_indev_t *input)
{
    assert(input && input->alive);
    input->alive = false;
    if (input->type == LV_INDEV_TYPE_KEYPAD) {
        assert(state.keyboard == input);
        state.keyboard = nullptr;
        state.events.emplace_back("keyboard-delete");
    } else {
        assert(state.input == input);
        state.input = nullptr;
        state.events.emplace_back("input-delete");
    }
    for (const auto &ui : state.uis)
        if (ui->alive && ui->input == input) ui->input = nullptr;
    notify_delete(input->hook);
}
void lv_indev_set_group(lv_indev_t *input, lv_group_t *group)
{
    assert(input && input->alive && group == state.group);
    input->group = group;
}
lv_indev_state_t lv_indev_get_state(const lv_indev_t *)
{
    return LV_INDEV_STATE_RELEASED;
}
uint32_t lv_indev_get_key(const lv_indev_t *)
{
    return 0;
}
void lv_indev_stop_processing(lv_indev_t *)
{
}
lv_group_t *lv_group_create()
{
    assert(!state.group);
    return new lv_group_t;
}
void lv_group_set_default(lv_group_t *group)
{
    state.group = group;
}
void lv_group_delete(lv_group_t *group)
{
    assert(group && group != state.group);
    delete group;
}
#if LV_LINUX_DRM_USE_EGL && defined(LILYGO_UI_HAS_DRM_HANDOFF)
lv_result_t lv_linux_drm_suspend(lv_display_t *display)
{
    assert(display->alive && !display->suspended && !state.timers);
    ++state.suspends;
    state.events.emplace_back("drm-suspend");
    if (state.fail_suspend) return LV_RESULT_INVALID;
    display->suspended = true;
    return LV_RESULT_OK;
}
lv_result_t lv_linux_drm_resume(lv_display_t *display)
{
    assert(display->alive && display->suspended && !state.timers);
    ++state.resumes;
    state.events.emplace_back("drm-resume");
    if (state.fail_resume) return LV_RESULT_INVALID;
    display->suspended = false;
    return LV_RESULT_OK;
}
#endif
lv_obj_t *lv_screen_active()
{
    assert(state.display && state.display->alive);
    return &state.screen;
}
void lv_obj_invalidate(lv_obj_t *object)
{
    assert(object == &state.screen);
}
void lv_refr_now(lv_display_t *display)
{
    assert(display && display->alive && !display->suspended);
    ++state.refreshes;
}
void lv_timer_enable(bool enable)
{
    state.timers = enable;
}

extern "C" launcher_ui_t *launcher_ui_create(launcher_registry_t *)
{
    assert(state.display && state.display->alive);
    if (state.fail_ui) return nullptr;
    state.uis.push_back(std::make_unique<launcher_ui_t>());
    auto *ui    = state.uis.back().get();
    ui->display = state.display;
    ui->input   = state.input;
    state.events.emplace_back("ui-create");
    return ui;
}
extern "C" void launcher_ui_destroy(launcher_ui_t *ui)
{
    if (!ui) return;
    assert(ui->alive && (!ui->input || ui->input->alive));
    ui->alive = false;
    state.events.emplace_back("ui-delete");
}
extern "C" void launcher_ui_rebind_input(launcher_ui_t *ui)
{
    assert(ui && ui->alive && ui->display == state.display);
    assert(!ui->input || ui->input->alive);
    assert(!state.input || state.input->display == ui->display);
    ui->input = state.input;
    state.keyboard_pressed = false;
    ++state.rebinds;
    state.events.emplace_back("ui-rebind");
}
extern "C" void launcher_ui_apply_orientation(launcher_ui_t *ui, launcher_orientation_t)
{
    assert(ui && ui->alive && ui->display->alive);
}
extern "C" bool launcher_ui_handle_keyboard_input(launcher_ui_t *, lv_indev_t *)
{
    return false;
}
namespace launcher::logging {
void info(const char *, ...) noexcept
{
}
void warn(const char *, ...) noexcept
{
}
void error(const char *, ...) noexcept
{
}
}  // namespace launcher::logging

int main()
{
    repeated_handoff();
    recreation_fallbacks();
    failed_recreation_cleanup();
    fbdev_and_external_deletion();
    keyboard_startup_and_manual_orientation();
    keyboard_hotplug_and_open_retry();
    keyboard_orientation_across_suspend();
    keyboard_reconnect_while_suspended();
    keyboard_power_loss_recovery();
    keyboard_recovery_retry_and_usb_ownership();
    std::cout << "DisplaySession ownership, handoff, recovery and keyboard orientation passed (EGL=" << LV_LINUX_DRM_USE_EGL
              << ", handoff=" << supports_handoff << ")\n";
}
