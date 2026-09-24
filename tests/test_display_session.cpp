// Compile the production controller against resource-checking LVGL/UI doubles.
// No real display, input device, SDK library or application process is used.
#include "display_session.h"
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
    lv_display_t *display;
    DeleteHook hook;
};
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
    lv_obj_t screen;
    bool timers          = true;
    bool fail_create     = false;
    bool fail_set_file   = false;
    bool fail_ui         = false;
    bool fail_input      = false;
    bool fail_suspend    = false;
    bool fail_resume     = false;
    bool external_delete = false;
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
    state = State{};
    unsetenv("LILYGO_UI_DRM_RECREATE");
}

launcher_process_options_t options(const char *backend = "drm")
{
    return {backend, "/dev/fb0", "/dev/dri/card0", 44, "/dev/input/test", "portrait", 568, 1232};
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
void lv_indev_add_event_cb(lv_indev_t *input, lv_event_cb_t cb, int code, void *data)
{
    assert(input->alive && code == LV_EVENT_DELETE);
    input->hook = {cb, data};
}
void lv_indev_delete(lv_indev_t *input)
{
    assert(input && input->alive);
    input->alive = false;
    state.input  = nullptr;
    state.events.emplace_back("input-delete");
    for (const auto &ui : state.uis)
        if (ui->alive && ui->input == input) ui->input = nullptr;
    notify_delete(input->hook);
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
    assert(ui && ui->alive && ui->display == state.display && !ui->input);
    assert(!state.input || state.input->display == ui->display);
    ui->input = state.input;
    ++state.rebinds;
    state.events.emplace_back("ui-rebind");
}
extern "C" void launcher_ui_apply_orientation(launcher_ui_t *ui, launcher_orientation_t)
{
    assert(ui && ui->alive && ui->display->alive);
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
    std::cout << "DisplaySession ownership, handoff and recovery passed (EGL=" << LV_LINUX_DRM_USE_EGL
              << ", handoff=" << supports_handoff << ")\n";
}
