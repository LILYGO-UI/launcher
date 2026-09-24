#include "display_session.h"
#include "logging.h"
#include "process.h"
#include "power_button.h"
#include "registry.h"
#include "ui.h"

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

static void discover_apps(launcher_registry_t *registry, const char *extra_directory)
{
    scan_path_list(registry, getenv("LAUNCHER_APP_DIRS"));
    if (extra_directory) launcher_registry_scan(registry, extra_directory);
    const char *xdg  = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    char user_path[LAUNCHER_PATH_MAX];
    if (xdg && xdg[0]) {
        snprintf(user_path, sizeof(user_path), "%s/launcher/apps", xdg);
        launcher_registry_scan(registry, user_path);
    } else if (home && home[0]) {
        snprintf(user_path, sizeof(user_path), "%s/.local/share/launcher/apps", home);
        launcher_registry_scan(registry, user_path);
    }
    launcher_registry_scan(registry, "/usr/local/share/launcher/apps");
    launcher_registry_scan(registry, "/usr/share/launcher/apps");
    launcher_registry_sort(registry);
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
        width,   height};
    launcher::DisplaySession display{process_options, registry.get()};
    if (!display.initialize(current_orientation)) return 1;
    PowerButtonMonitor power_button;
    power_button.open(power_input_path);
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    if (strcmp(backend, "drm") == 0) drm_keepalive.acquire(drm_path);
#endif
    process_options.orientation = orientation_name(display.orientation());
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    display.refresh_ready();
    uint32_t next_app_scan = tick_ms() + 2000U;
    while (!stop_requested && display.alive()) {
        uint32_t delay = lv_timer_handler();
        if (!display.alive()) break;
        const unsigned int power_events = power_button.poll(tick_ms());
        if (power_events & POWER_BUTTON_EVENT_PRESSED) {
            launcher_ui_show_shutdown(display.ui());
            display.refresh();
        }
        if (power_events & POWER_BUTTON_EVENT_RELEASED) {
            launcher_ui_hide_shutdown(display.ui());
            display.refresh();
        }
        launcher_orientation_t requested_orientation;
        if (launcher_ui_take_orientation_request(display.ui(), &requested_orientation)) {
            current_orientation         = requested_orientation;
            process_options.orientation = orientation_name(current_orientation);
            display.set_orientation(current_orientation);
            launcher::logging::info("display orientation changed to %s", process_options.orientation);
        }
        const launcher_app_t *requested_app = launcher_ui_take_launch_request(display.ui());
        if (requested_app) {
            const uint32_t handoff_started = tick_ms();
            process_options.orientation    = orientation_name(display.orientation());
            power_button.close();
            display.suspend();
            launcher::logging::info("switch prepared app_id=%s release_ms=%u retained=%d", requested_app->manifest.id,
                                    tick_ms() - handoff_started, display.retained());
            int result                     = launcher_process_run(requested_app, &process_options, &stop_requested);
            const uint32_t restore_started = tick_ms();
            if (result == 0)
                launcher::logging::info("%s exited with code 0", requested_app->manifest.id);
            else
                launcher::logging::warn("%s exited with code %d", requested_app->manifest.id, result);
            if (!display.resume()) return 1;
            power_button.open(power_input_path);
            launcher::logging::info("switch restored app_id=%s restore_ms=%u retained=%d", requested_app->manifest.id,
                                    tick_ms() - restore_started, display.retained());
            next_app_scan = tick_ms() + 2000U;
            if (stop_requested) break;
        }
        uint32_t now = tick_ms();
        if (launcher_ui_is_home(display.ui()) && static_cast<int32_t>(now - next_app_scan) >= 0) {
            RegistryOwner refreshed_registry;
            discover_apps(refreshed_registry.get(), app_directory);
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

    return 0;
}
