#include "display_session.h"
#include "file_share.h"
#include "file_share_receiver.h"
#include "logging.h"
#include "process.h"
#include "power_button.h"
#include "registry.h"
#include "session.h"
#include "system_controls.h"
#include "ui.h"
#include "account_view.h"
#include "first_boot_wizard.h"

#include <lvgl.h>
#include <cm0/typography.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <memory>
#include <optional>

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
#include <fcntl.h>
#include <linux/kd.h>
#include <sys/ioctl.h>
#endif

static volatile sig_atomic_t stop_requested;

static const char *orientation_name(launcher_orientation_t orientation)
{
    return orientation == LAUNCHER_ORIENTATION_LANDSCAPE ? "landscape" : "portrait";
}

static void poll_hardware_keyboard(void *data)
{
    // The launcher retains device privileges while its foreground app runs
    // as the signed-in user. Keep recovery active without touching paused UI.
    static_cast<launcher::DisplaySession *>(data)->poll_keyboard();
}

static uint32_t tick_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint32_t)(now.tv_sec * 1000ULL + (unsigned long long)now.tv_nsec / 1000000ULL);
}

static void lvgl_log(lv_log_level_t level, const char *message)
{
    if (!message) return;
    size_t length = strlen(message);
    while (length > 0 && (message[length - 1] == '\n' || message[length - 1] == '\r')) --length;
    if (level == LV_LOG_LEVEL_ERROR)
        launcher::logging::error("LVGL: %.*s", (int)length, message);
    else
        launcher::logging::warn("LVGL: %.*s", (int)length, message);
}

static void stop_handler(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

class RegistryOwner {
public:
    RegistryOwner()
    {
        launcher_registry_init(&registry_);
    }

    ~RegistryOwner()
    {
        launcher_registry_destroy(&registry_);
    }

    launcher_registry_t *get()
    {
        return &registry_;
    }

    launcher_registry_t *operator->()
    {
        return &registry_;
    }

private:
    launcher_registry_t registry_{};
};

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
class ConsoleGraphicsMode {
public:
    explicit ConsoleGraphicsMode(bool enable)
    {
        if (!enable) return;

        fd_ = open("/dev/tty1", O_RDWR | O_CLOEXEC | O_NOCTTY);
        if (fd_ < 0) {
            launcher::logging::warn("cannot open /dev/tty1: %s", strerror(errno));
            return;
        }

        int mode = KD_TEXT;
        if (ioctl(fd_, KDGETMODE, &mode) < 0) {
            launcher::logging::warn("cannot read /dev/tty1 display mode: %s", strerror(errno));
            return;
        }
        if (mode != KD_GRAPHICS && ioctl(fd_, KDSETMODE, KD_GRAPHICS) < 0) {
            launcher::logging::warn("cannot set /dev/tty1 to graphics mode: %s", strerror(errno));
            return;
        }

        active_ = true;
        launcher::logging::info("console /dev/tty1 set to graphics mode");
    }

    ~ConsoleGraphicsMode()
    {
        if (active_ && ioctl(fd_, KDSETMODE, KD_TEXT) < 0)
            launcher::logging::warn("cannot restore /dev/tty1 text mode: %s", strerror(errno));
        if (fd_ >= 0) close(fd_);
    }

    ConsoleGraphicsMode(const ConsoleGraphicsMode &)            = delete;
    ConsoleGraphicsMode &operator=(const ConsoleGraphicsMode &) = delete;

private:
    int fd_      = -1;
    bool active_ = false;
};

class DrmKeepalive {
public:
    DrmKeepalive() = default;

    ~DrmKeepalive()
    {
        if (fd_ >= 0) close(fd_);
    }

    void acquire(const char *path)
    {
        // Keep the DRM device open across the full-recreation fallback, too.
        // O_CLOEXEC prevents the child application from inheriting this handle.
        fd_ = open(path, O_RDWR | O_CLOEXEC);
        if (fd_ < 0) launcher::logging::warn("cannot keep DRM device '%s' open: %s", path, strerror(errno));
    }

    DrmKeepalive(const DrmKeepalive &)            = delete;
    DrmKeepalive &operator=(const DrmKeepalive &) = delete;

private:
    int fd_ = -1;
};
#endif

class LvglRuntime {
public:
    LvglRuntime()
    {
        lv_init();
        lv_tick_set_cb(tick_ms);
        lv_log_register_print_cb(lvgl_log);
    }

    ~LvglRuntime()
    {
        lv_log_register_print_cb(nullptr);
        lilygo_ui_fonts_deinit();
        lv_deinit();
    }
};

static void scan_path_list(launcher_registry_t *registry, const char *paths)
{
    if (!paths || !paths[0]) return;
    std::unique_ptr<char, decltype(&free)> copy{strdup(paths), free};
    if (!copy) return;
    char *save = NULL;
    for (char *path = strtok_r(copy.get(), ":", &save); path; path = strtok_r(NULL, ":", &save))
        if (path[0]) launcher_registry_scan(registry, path);
}

static void discover_apps(launcher_registry_t *registry, const char *extra_directory,
                          const launcher_user_t *user = nullptr)
{
    scan_path_list(registry, getenv("LAUNCHER_APP_DIRS"));
    if (extra_directory) launcher_registry_scan(registry, extra_directory);
    const char *xdg  = user ? nullptr : getenv("XDG_DATA_HOME");
    const char *home = user ? user->home : getenv("HOME");
    char user_path[LAUNCHER_PATH_MAX];
    if (xdg && xdg[0]) {
        if (strlen(xdg) + sizeof("/launcher/apps") - 1 < sizeof(user_path)) {
            snprintf(user_path, sizeof(user_path), "%s/launcher/apps", xdg);
            launcher_registry_scan(registry, user_path);
        }
    } else if (home && home[0]) {
        if (strlen(home) + sizeof("/.local/share/launcher/apps") - 1 < sizeof(user_path)) {
            snprintf(user_path, sizeof(user_path), "%s/.local/share/launcher/apps", home);
            launcher_registry_scan(registry, user_path);
        }
    }
    launcher_registry_scan(registry, "/usr/local/share/launcher/apps");
    launcher_registry_scan(registry, "/usr/share/launcher/apps");
    launcher_registry_sort(registry);
}

static bool prepare_file_share_directory(const launcher::UserSession &session, std::string &path, std::string &error)
{
    path.clear();
    const launcher_user_t *user = session.user();
    launcher_user_t simulator_user{};
    if (!user) {
        /* Host sessions intentionally do not change process credentials. Use
         * the simulator's current HOME while keeping the same path and
         * ownership checks as a device session. */
        const char *home = getenv("HOME");
        if (!home || home[0] != '/') {
            error = "HOME is unavailable for the simulator file-share directory";
            return false;
        }
        simulator_user.uid = getuid();
        simulator_user.gid = getgid();
        const char *name = getenv("USER");
        snprintf(simulator_user.name, sizeof(simulator_user.name), "%s", name && name[0] ? name : "simulator");
        snprintf(simulator_user.home, sizeof(simulator_user.home), "%s", home);
        snprintf(simulator_user.shell, sizeof(simulator_user.shell), "/bin/sh");
        snprintf(simulator_user.runtime_dir, sizeof(simulator_user.runtime_dir), "/tmp");
        user = &simulator_user;
    }
    return launcher::file_share::resolve_download_directory(user, path, error);
}

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--display sdl|drm|fbdev] [--width PX] [--height PX]\n"
            "          [--drm PATH] [--drm-connector ID] [--fbdev PATH]\n"
            "          [--input PATH] [--app-dir PATH]\n"
            "          [--power-input PATH]\n"
            "          [--orientation portrait|landscape]\n",
            program);
#if defined(LAUNCHER_SIMULATOR)
    fprintf(stderr, "          [--setup-demo] (in-memory first-boot preview)\n");
#endif
}

int main(int argc, char **argv)
{
    launcher::logging::Runtime logging{"launcher"};
#if defined(LAUNCHER_SIMULATOR)
    const char *backend = "sdl";
#else
    const char *backend = "drm";
#endif
    const char *fbdev_path                     = "/dev/fb0";
    const char *drm_path                       = "/dev/dri/card0";
    int64_t drm_connector_id                   = -1;
    const char *input_path                     = "/dev/input/by-path/platform-3f205000.i2c-event";
    const char *power_input_path               = nullptr;
    const char *app_directory                  = NULL;
    launcher_orientation_t current_orientation = LAUNCHER_ORIENTATION_PORTRAIT;
    bool setup_demo = false;
    int width = LAUNCHER_DEFAULT_WIDTH, height = LAUNCHER_DEFAULT_HEIGHT;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--display") == 0 && i + 1 < argc)
            backend = argv[++i];
        else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc)
            width = atoi(argv[++i]);
        else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc)
            height = atoi(argv[++i]);
        else if (strcmp(argv[i], "--fbdev") == 0 && i + 1 < argc)
            fbdev_path = argv[++i];
        else if (strcmp(argv[i], "--drm") == 0 && i + 1 < argc)
            drm_path = argv[++i];
        else if (strcmp(argv[i], "--drm-connector") == 0 && i + 1 < argc)
            drm_connector_id = strtoll(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
            input_path = argv[++i];
        else if (strcmp(argv[i], "--power-input") == 0 && i + 1 < argc)
            power_input_path = argv[++i];
        else if (strcmp(argv[i], "--app-dir") == 0 && i + 1 < argc)
            app_directory = argv[++i];
#if defined(LAUNCHER_SIMULATOR)
        else if (strcmp(argv[i], "--setup-demo") == 0)
            setup_demo = true;
#endif
        else if (strcmp(argv[i], "--orientation") == 0 && i + 1 < argc) {
            const char *value = argv[++i];
            if (strcmp(value, "portrait") == 0)
                current_orientation = LAUNCHER_ORIENTATION_PORTRAIT;
            else if (strcmp(value, "landscape") == 0)
                current_orientation = LAUNCHER_ORIENTATION_LANDSCAPE;
            else {
                usage(argv[0]);
                return 2;
            }
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (width < 320 || height < 240) {
        launcher::logging::error("display size must be at least 320x240");
        return 2;
    }
    if (width >= height) {
        launcher::logging::error(
            "--width/--height must define a portrait base canvas (WIDTH < HEIGHT); "
            "use --orientation landscape for a horizontal display");
        return 2;
    }

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    ConsoleGraphicsMode console_graphics(strcmp(backend, "drm") == 0);
#endif
    RegistryOwner registry;
    discover_apps(registry.get(), app_directory);
    launcher::logging::info("discovered %zu application(s)", registry->count);

    LvglRuntime lvgl;
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    DrmKeepalive drm_keepalive;
#endif
    launcher_process_options_t process_options{
        backend, fbdev_path, drm_path, drm_connector_id, input_path, orientation_name(current_orientation),
        width,   height, nullptr};
    launcher::DisplaySession display{process_options, registry.get()};
    if (!display.initialize(current_orientation)) return 1;
    launcher::file_share::Receiver file_share_receiver;
    launcher::SystemControls system_controls;
    const auto sync_system_controls = [&]() {
        const auto &state = system_controls.state();
        launcher_ui_set_system_controls(display.ui(), state.brightness_available, state.brightness_percent,
                                        state.volume_available, state.volume_percent, state.power_off_available);
        launcher_ui_set_software_brightness(display.ui(), state.brightness_software, state.brightness_percent);
    };
    sync_system_controls();

    launcher::UserSession user_session;
    std::string session_error;
    if (!user_session.initialize(session_error)) {
        launcher::logging::error("cannot initialize user session: %s", session_error.c_str());
        return 1;
    }
    bool authenticated = false;
    bool screen_locked = false;
    launcher::FirstBootWizard setup_wizard(user_session);
    if (!setup_wizard.initialize(setup_demo, session_error)) {
        launcher::logging::error("cannot initialize first-boot setup: %s", session_error.c_str());
        return 1;
    }
    setup_wizard.set_hardware_keyboard_present(display.tca8418_present());
    launcher_account_view_t *account_view = setup_wizard.active() ? nullptr
        : launcher_account_view_create(user_session.setup_available(), user_session.last_user().c_str(), false);
    if (!setup_wizard.active() && !account_view) {
        launcher::logging::error("cannot create account screen");
        return 1;
    }
    /* Keep the home controls inert while the account modal owns focus. This
     * also makes the simulator a faithful preview of the device login flow. */
    launcher_ui_set_input_enabled(display.ui(), false);
    if (account_view) launcher_account_view_set_hardware_keyboard_present(account_view, display.tca8418_present());

    PowerButtonMonitor power_button;
    power_button.open(power_input_path);
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    if (strcmp(backend, "drm") == 0) drm_keepalive.acquire(drm_path);
#endif
    process_options.orientation = orientation_name(display.orientation());
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    display.refresh_ready();
    char account_name[LAUNCHER_USER_NAME_MAX]{};
    char account_password[128]{};
    uint32_t next_app_scan      = tick_ms() + 2000U;
    uint32_t next_keyboard_scan = tick_ms() + 500U;
    uint32_t next_controls_scan = tick_ms() + 2000U;
    uint32_t next_level_write = 0;
    bool file_share_open = false;
    std::optional<std::uint64_t> displayed_file_share_approval;
    std::string file_share_status;
    const auto enter_home = [&]() {
        authenticated = true;
        process_options.user = user_session.user();
        launcher_ui_set_session_user(display.ui(), user_session.username().c_str());
        std::string file_share_path;
        std::string file_share_error;
        if (!prepare_file_share_directory(user_session, file_share_path, file_share_error)) {
            launcher::logging::warn("file sharing is unavailable: %s", file_share_error.c_str());
            launcher_ui_set_file_share_directory(display.ui(), nullptr);
        } else {
            launcher_ui_set_file_share_directory(display.ui(), file_share_path.c_str());
        }
        launcher_ui_set_file_share_status(display.ui(), "Open file sharing to connect from your browser.");
        launcher_account_view_destroy(account_view);
        account_view = nullptr;
        launcher_ui_set_input_enabled(display.ui(), true);
        if (!screen_locked) {
            launcher_ui_prepare_registry_refresh(display.ui());
            launcher_registry_destroy(registry.get());
            launcher_registry_init(registry.get());
            discover_apps(registry.get(), app_directory, user_session.user());
            launcher_ui_refresh_home(display.ui());
        }
        screen_locked = false;
        display.refresh();
    };
    while (!stop_requested && display.alive()) {
        uint32_t delay = lv_timer_handler();
        if (!display.alive()) break;
        if (setup_wizard.active()) {
            setup_wizard.tick();
            if (!setup_wizard.active()) enter_home();
        }
        const uint32_t controls_now = tick_ms();
        bool levels_changed = false;
        if (authenticated && static_cast<int32_t>(controls_now - next_level_write) >= 0) {
            int percent = 0;
            if (launcher_ui_take_brightness_request(display.ui(), &percent)) {
                if (!system_controls.set_brightness(percent))
                    launcher::logging::warn("brightness adjustment failed: %s", system_controls.error().c_str());
                levels_changed = true;
            }
            if (launcher_ui_take_volume_request(display.ui(), &percent)) {
                if (!system_controls.set_volume(percent))
                    launcher::logging::warn("volume adjustment failed: %s", system_controls.error().c_str());
                levels_changed = true;
            }
            next_level_write = controls_now + 80U;
        }
        if (static_cast<int32_t>(controls_now - next_controls_scan) >= 0) {
            system_controls.refresh();
            next_controls_scan = controls_now + 2000U;
            levels_changed = true;
        }
        if (levels_changed) sync_system_controls();
        if (authenticated && launcher_ui_take_power_request(display.ui())) {
            display.refresh();
            const bool success = system_controls.power_off();
            if (!success) launcher::logging::warn("power off failed: %s", system_controls.error().c_str());
            launcher_ui_set_power_result(display.ui(), success, system_controls.state().simulated);
        }
        const unsigned int power_events = power_button.poll(tick_ms());
        if (power_events & POWER_BUTTON_EVENT_PRESSED) {
            launcher_ui_show_shutdown(display.ui());
            display.refresh();
        }
        if (power_events & POWER_BUTTON_EVENT_RELEASED) {
            launcher_ui_hide_shutdown(display.ui());
            display.refresh();
        }
        if (authenticated && launcher_ui_take_lock_request(display.ui())) {
            file_share_receiver.stop();
            displayed_file_share_approval.reset();
            file_share_open = false;
            launcher_ui_set_input_enabled(display.ui(), false);
            authenticated = false;
            screen_locked = true;
            process_options.user = nullptr;
            account_view = launcher_account_view_create_locked(user_session.username().c_str());
            if (!account_view) {
                launcher::logging::error("cannot create lock screen");
                return 1;
            }
            launcher_account_view_set_hardware_keyboard_present(account_view, display.tca8418_present());
            display.refresh();
        } else if (authenticated && launcher_ui_take_logout_request(display.ui())) {
            file_share_receiver.stop();
            displayed_file_share_approval.reset();
            file_share_open = false;
            launcher_ui_set_input_enabled(display.ui(), false);
            authenticated = false;
            process_options.user = nullptr;
            user_session.logout();
            launcher_ui_set_session_user(display.ui(), nullptr);
            account_view = launcher_account_view_create(false, user_session.last_user().c_str(), false);
            if (!account_view) {
                launcher::logging::error("cannot recreate account screen after logout");
                return 1;
            }
            launcher_account_view_set_hardware_keyboard_present(account_view, display.tca8418_present());
            display.refresh();
        }
        const bool sharing_requested = authenticated && launcher_ui_file_share_open(display.ui());
        if (sharing_requested && !file_share_open) {
            std::string sharing_error;
            if (!file_share_receiver.start(user_session.user(), sharing_error))
                launcher::logging::warn("file sharing is unavailable: %s", sharing_error.c_str());
            file_share_status.clear();
        } else if (!sharing_requested && file_share_open) {
            file_share_receiver.stop();
            displayed_file_share_approval.reset();
            launcher_ui_set_file_share_approval(display.ui(), nullptr, 0, false);
        }
        file_share_open = sharing_requested;
        if (sharing_requested) {
            const auto pending_approval = file_share_receiver.pending_approval();
            const std::string status = file_share_receiver.status();
            if (!pending_approval && (status != file_share_status || displayed_file_share_approval)) {
                launcher_ui_set_file_share_status(display.ui(), status.c_str());
                file_share_status = status;
            }
            if (pending_approval && displayed_file_share_approval != pending_approval->id) {
                displayed_file_share_approval = pending_approval->id;
                launcher_ui_set_file_share_approval(display.ui(), pending_approval->filename.c_str(),
                                                    pending_approval->size, true);
            } else if (!pending_approval && displayed_file_share_approval) {
                displayed_file_share_approval.reset();
                launcher_ui_set_file_share_approval(display.ui(), nullptr, 0, false);
            }
            bool approved = false;
            if (displayed_file_share_approval &&
                launcher_ui_take_file_share_decision(display.ui(), &approved)) {
                file_share_receiver.resolve_approval(*displayed_file_share_approval, approved);
                displayed_file_share_approval.reset();
                launcher_ui_set_file_share_approval(display.ui(), nullptr, 0, false);
            }
            /* Reap a failed/disconnected helper even if the user leaves the
             * panel open. Keep its final status; reopening starts a new session. */
            if (!file_share_receiver.active()) file_share_receiver.stop();
        }
        if (account_view) {
            bool create_account = false;
            if (launcher_account_view_take_request(account_view, account_name, sizeof(account_name), account_password,
                                                   sizeof(account_password), &create_account)) {
                launcher_account_view_set_busy(account_view, true);
                display.refresh();
                session_error.clear();
                const bool logged_in = screen_locked
                    ? user_session.reauthenticate(account_password, session_error)
                    : user_session.login(account_name, account_password, create_account, session_error);
                memset(account_password, 0, sizeof(account_password));
                if (!logged_in) {
                    launcher_account_view_set_setup_username(account_view, user_session.setup_username().c_str());
                    launcher_account_view_set_error(account_view, session_error.c_str());
                    launcher_account_view_set_busy(account_view, false);
                } else {
                    enter_home();
                }
            }
        }
        launcher_orientation_t requested_orientation;
        // Save the user's choice before polling: a simultaneous keyboard
        // connection must not consume or override a Quick controls request.
        const bool orientation_requested =
            launcher_ui_take_orientation_request(display.ui(), &requested_orientation);
        const uint32_t input_now = tick_ms();
        if (orientation_requested || (int32_t)(input_now - next_keyboard_scan) >= 0) {
            display.poll_keyboard();
            if (setup_wizard.active()) setup_wizard.set_hardware_keyboard_present(display.tca8418_present());
            if (account_view) {
                launcher_account_view_set_hardware_keyboard_present(account_view, display.tca8418_present());
                launcher_account_view_capture_input(account_view);
            }
            next_keyboard_scan = input_now + 500U;
        }
        if (orientation_requested) {
            current_orientation         = requested_orientation;
            process_options.orientation = orientation_name(current_orientation);
            display.set_orientation(current_orientation);
            launcher::logging::info("display orientation changed to %s", process_options.orientation);
        }
        const launcher_app_t *requested_app =
            authenticated ? launcher_ui_take_launch_request(display.ui()) : nullptr;
        if (requested_app) {
            file_share_receiver.stop();
            file_share_open = false;
            const uint32_t handoff_started = tick_ms();
            process_options.orientation    = orientation_name(display.orientation());
            power_button.close();
            display.suspend();
            launcher::logging::info("switch prepared app_id=%s release_ms=%u retained=%d", requested_app->manifest.id,
                                    tick_ms() - handoff_started, display.retained());
            int result = launcher_process_run_with_poll(requested_app, &process_options, &stop_requested,
                                                       poll_hardware_keyboard, &display);
            const uint32_t restore_started = tick_ms();
            if (result == 0)
                launcher::logging::info("%s exited with code 0", requested_app->manifest.id);
            else
                launcher::logging::warn("%s exited with code %d", requested_app->manifest.id, result);
            if (!display.resume()) return 1;
            system_controls.refresh();
            sync_system_controls();
            launcher_ui_set_session_user(display.ui(), user_session.username().c_str());
            power_button.open(power_input_path);
            launcher::logging::info("switch restored app_id=%s restore_ms=%u retained=%d", requested_app->manifest.id,
                                    tick_ms() - restore_started, display.retained());
            next_app_scan = tick_ms() + 2000U;
            if (stop_requested) break;
        }
        uint32_t now = tick_ms();
        if (authenticated && launcher_ui_is_home(display.ui()) && static_cast<int32_t>(now - next_app_scan) >= 0) {
            RegistryOwner refreshed_registry;
            discover_apps(refreshed_registry.get(), app_directory, user_session.user());
            if (!launcher_registry_equal(registry.get(), refreshed_registry.get())) {
                launcher_ui_prepare_registry_refresh(display.ui());
                *registry.get() = *refreshed_registry.get();
                launcher::logging::info("application list changed (%zu installed)", registry->count);
                launcher_ui_refresh_home(display.ui());
            }
            next_app_scan = now + 2000U;
        }
        if (delay < 1) delay = 1;
        if (delay > 20) delay = 20;
        usleep(delay * 1000U);
    }

    file_share_receiver.stop();
    launcher_account_view_destroy(account_view);
    // The setup worker is joined before UserSession's destructor closes PAM.

    return 0;
}
