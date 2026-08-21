#include "logging.h"
#include "process.h"
#include "registry.h"
#include "ui.h"

#include <lvgl.h>

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

#if defined(LAUNCHER_SIMULATOR)
#include <SDL.h>
#endif

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t display_alive = 1;

static const char *orientation_name(launcher_orientation_t orientation) {
  return orientation == LAUNCHER_ORIENTATION_LANDSCAPE ? "landscape"
                                                       : "portrait";
}

static void apply_display_orientation(lv_display_t *display,
                                      launcher_orientation_t orientation,
                                      int native_width, int native_height) {
#if defined(LAUNCHER_SIMULATOR)
  const bool landscape = orientation == LAUNCHER_ORIENTATION_LANDSCAPE;
  lv_display_set_resolution(display, landscape ? native_height : native_width,
                            landscape ? native_width : native_height);
#else
  (void)native_width;
  (void)native_height;
  lv_display_set_rotation(display,
                          orientation == LAUNCHER_ORIENTATION_LANDSCAPE
                              ? LV_DISPLAY_ROTATION_90
                              : LV_DISPLAY_ROTATION_0);
#endif
}

#if defined(LAUNCHER_SIMULATOR)
static void fit_simulator_window(lv_display_t *display, int width, int height) {
  SDL_Window *window = lv_sdl_window_get_window(display);
  int display_index = window ? SDL_GetWindowDisplayIndex(window) : -1;
  SDL_Rect usable_bounds;
  if (display_index < 0 ||
      SDL_GetDisplayUsableBounds(display_index, &usable_bounds) != 0)
    return;

  const float usable_fraction = 0.90f;
  float horizontal_zoom = usable_bounds.w * usable_fraction / (float)width;
  float vertical_zoom = usable_bounds.h * usable_fraction / (float)height;
  float zoom =
      horizontal_zoom < vertical_zoom ? horizontal_zoom : vertical_zoom;
  if (zoom >= 1.0f)
    return;

  lv_sdl_window_set_zoom(display, zoom);
  lv_display_set_resolution(display, width, height);

  int window_width = (int)(width * zoom);
  int window_height = (int)(height * zoom);
  SDL_SetWindowPosition(
      window, usable_bounds.x + (usable_bounds.w - window_width) / 2,
      usable_bounds.y + (usable_bounds.h - window_height) / 2);
  launcher::logging::info("simulator scaled to %.0f%% to fit the display",
                          zoom * 100.0f);
}
#endif

static uint32_t tick_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint32_t)(now.tv_sec * 1000ULL +
                    (unsigned long long)now.tv_nsec / 1000000ULL);
}

static void lvgl_log(lv_log_level_t level, const char *message) {
  if (!message)
    return;
  size_t length = strlen(message);
  while (length > 0 &&
         (message[length - 1] == '\n' || message[length - 1] == '\r'))
    --length;
  if (level == LV_LOG_LEVEL_ERROR)
    launcher::logging::error("LVGL: %.*s", (int)length, message);
  else
    launcher::logging::warn("LVGL: %.*s", (int)length, message);
}

static void stop_handler(int signal_number) {
  (void)signal_number;
  stop_requested = 1;
}

static void display_deleted(lv_event_t *event) {
  (void)event;
  display_alive = 0;
}

class RegistryOwner {
public:
  RegistryOwner() { launcher_registry_init(&registry_); }

  ~RegistryOwner() { launcher_registry_destroy(&registry_); }

  launcher_registry_t *get() { return &registry_; }

  launcher_registry_t *operator->() { return &registry_; }

private:
  launcher_registry_t registry_{};
};

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
class ConsoleGraphicsMode {
public:
  explicit ConsoleGraphicsMode(bool enable) {
    if (!enable)
      return;

    fd_ = open("/dev/tty1", O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (fd_ < 0) {
      launcher::logging::warn("cannot open /dev/tty1: %s", strerror(errno));
      return;
    }

    int mode = KD_TEXT;
    if (ioctl(fd_, KDGETMODE, &mode) < 0) {
      launcher::logging::warn("cannot read /dev/tty1 display mode: %s",
                              strerror(errno));
      return;
    }
    if (mode != KD_GRAPHICS && ioctl(fd_, KDSETMODE, KD_GRAPHICS) < 0) {
      launcher::logging::warn("cannot set /dev/tty1 to graphics mode: %s",
                              strerror(errno));
      return;
    }

    active_ = true;
    launcher::logging::info("console /dev/tty1 set to graphics mode");
  }

  ~ConsoleGraphicsMode() {
    if (active_ && ioctl(fd_, KDSETMODE, KD_TEXT) < 0)
      launcher::logging::warn("cannot restore /dev/tty1 text mode: %s",
                              strerror(errno));
    if (fd_ >= 0)
      close(fd_);
  }

  ConsoleGraphicsMode(const ConsoleGraphicsMode &) = delete;
  ConsoleGraphicsMode &operator=(const ConsoleGraphicsMode &) = delete;

private:
  int fd_ = -1;
  bool active_ = false;
};

class DrmKeepalive {
public:
  DrmKeepalive() = default;

  ~DrmKeepalive() {
    if (fd_ >= 0)
      close(fd_);
  }

  void acquire(const char *path) {
    fd_ = open(path, O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
      launcher::logging::warn("cannot keep DRM device '%s' open: %s", path,
                              strerror(errno));
  }

  DrmKeepalive(const DrmKeepalive &) = delete;
  DrmKeepalive &operator=(const DrmKeepalive &) = delete;

private:
  int fd_ = -1;
};
#endif

class LvglRuntime {
public:
  LvglRuntime() {
    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_log_register_print_cb(lvgl_log);
  }

  ~LvglRuntime() {
    lv_log_register_print_cb(nullptr);
    lv_deinit();
  }
};

class DisplayOwner {
public:
  ~DisplayOwner() {
    clear();
#if defined(LAUNCHER_SIMULATOR)
    if (initialized_)
      lv_sdl_quit();
#endif
  }

  void clear() {
    if (display_ && display_alive) {
      lv_display_delete(display_);
    }
    display_ = nullptr;
  }

  void reset(lv_display_t *display) {
    display_ = display;
#if defined(LAUNCHER_SIMULATOR)
    if (display)
      initialized_ = true;
#endif
  }

  lv_display_t *get() const { return display_; }

private:
  lv_display_t *display_ = nullptr;
#if defined(LAUNCHER_SIMULATOR)
  bool initialized_ = false;
#endif
};

struct UiDeleter {
  void operator()(launcher_ui_t *ui) const { launcher_ui_destroy(ui); }
};

using UiOwner = std::unique_ptr<launcher_ui_t, UiDeleter>;

static void scan_path_list(launcher_registry_t *registry, const char *paths) {
  if (!paths || !paths[0])
    return;
  std::unique_ptr<char, decltype(&free)> copy{strdup(paths), free};
  if (!copy)
    return;
  char *save = NULL;
  for (char *path = strtok_r(copy.get(), ":", &save); path;
       path = strtok_r(NULL, ":", &save))
    if (path[0])
      launcher_registry_scan(registry, path);
}

static void discover_apps(launcher_registry_t *registry,
                          const char *extra_directory) {
  scan_path_list(registry, getenv("LAUNCHER_APP_DIRS"));
  if (extra_directory)
    launcher_registry_scan(registry, extra_directory);
  const char *xdg = getenv("XDG_DATA_HOME");
  const char *home = getenv("HOME");
  char user_path[LAUNCHER_PATH_MAX];
  if (xdg && xdg[0]) {
    snprintf(user_path, sizeof(user_path), "%s/launcher/apps", xdg);
    launcher_registry_scan(registry, user_path);
  } else if (home && home[0]) {
    snprintf(user_path, sizeof(user_path), "%s/.local/share/launcher/apps",
             home);
    launcher_registry_scan(registry, user_path);
  }
  launcher_registry_scan(registry, "/usr/local/share/launcher/apps");
  launcher_registry_scan(registry, "/usr/share/launcher/apps");
  launcher_registry_sort(registry);
}

static void usage(const char *program) {
  fprintf(stderr,
          "Usage: %s [--display sdl|drm|fbdev] [--width PX] [--height PX]\n"
          "          [--drm PATH] [--drm-connector ID] [--fbdev PATH]\n"
          "          [--input PATH] [--app-dir PATH]\n"
          "          [--orientation portrait|landscape]\n",
          program);
}

int main(int argc, char **argv) {
  launcher::logging::Runtime logging{"launcher"};
#if defined(LAUNCHER_SIMULATOR)
  const char *backend = "sdl";
#else
  const char *backend = "drm";
#endif
  const char *fbdev_path = "/dev/fb0";
  const char *drm_path = "/dev/dri/card0";
  int64_t drm_connector_id = -1;
  const char *input_path = "/dev/input/by-path/platform-3f205000.i2c-event";
  const char *app_directory = NULL;
  launcher_orientation_t current_orientation =
      LAUNCHER_ORIENTATION_PORTRAIT;
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
    }
    else {
      usage(argv[0]);
      return 2;
    }
  }
  if (width < 320 || height < 240) {
    launcher::logging::error("display size must be at least 320x240");
    return 2;
  }
#if defined(LAUNCHER_SIMULATOR)
  (void)fbdev_path;
  (void)drm_path;
  (void)drm_connector_id;
  (void)input_path;
#endif

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
  DisplayOwner display;
  lv_display_t *created_display = nullptr;
  if (strcmp(backend, "sdl") == 0) {
#if defined(LAUNCHER_SIMULATOR)
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    created_display = lv_sdl_window_create(width, height);
    if (created_display) {
      lv_sdl_window_set_title(created_display, "CM0 Launcher");
      lv_sdl_window_set_resizeable(created_display, true);
      fit_simulator_window(created_display, width, height);
      lv_sdl_mouse_create();
    }
#else
    launcher::logging::error("SDL backend is not included in this build");
#endif
  } else if (strcmp(backend, "fbdev") == 0) {
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    created_display = lv_linux_fbdev_create();
    if (created_display &&
        lv_linux_fbdev_set_file(created_display, fbdev_path) != LV_RESULT_OK) {
      launcher::logging::error("cannot initialize framebuffer '%s'",
                               fbdev_path);
      lv_display_delete(created_display);
      created_display = nullptr;
    }
#else
    launcher::logging::error("fbdev backend is only included in device builds");
#endif
  } else if (strcmp(backend, "drm") == 0) {
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    created_display = lv_linux_drm_create();
    if (created_display &&
        lv_linux_drm_set_file(created_display, drm_path, drm_connector_id) !=
            LV_RESULT_OK) {
      launcher::logging::error(
          "cannot initialize DRM device '%s' connector %lld", drm_path,
          (long long)drm_connector_id);
      lv_display_delete(created_display);
      created_display = nullptr;
    }
#else
    launcher::logging::error("DRM backend is only included in device builds");
#endif
  } else {
    launcher::logging::error("unknown display backend '%s'", backend);
  }
  if (!created_display)
    return 1;
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
  if (strcmp(backend, "drm") == 0)
    drm_keepalive.acquire(drm_path);
#endif
  apply_display_orientation(created_display, current_orientation, width,
                            height);
  launcher::logging::info(
      "initialized %s display at %dx%d", backend,
      (int)lv_display_get_horizontal_resolution(created_display),
      (int)lv_display_get_vertical_resolution(created_display));
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
  lv_indev_t *input_device = lv_evdev_create(LV_INDEV_TYPE_POINTER, input_path);
  if (!input_device)
    launcher::logging::warn("cannot initialize input device '%s'", input_path);
#endif
  display.reset(created_display);
  lv_display_add_event_cb(display.get(), display_deleted, LV_EVENT_DELETE,
                          NULL);

  UiOwner ui{launcher_ui_create(registry.get())};
  if (!ui) {
    launcher::logging::error("cannot allocate UI");
    return 1;
  }
  signal(SIGINT, stop_handler);
  signal(SIGTERM, stop_handler);
  launcher_process_options_t process_options{
      backend,    fbdev_path, drm_path, drm_connector_id,
      input_path, orientation_name(current_orientation), width, height};
  uint32_t next_app_scan = tick_ms() + 2000U;
  while (!stop_requested && display_alive) {
    uint32_t delay = lv_timer_handler();
    launcher_orientation_t requested_orientation;
    if (launcher_ui_take_orientation_request(ui.get(),
                                             &requested_orientation)) {
      current_orientation = requested_orientation;
      process_options.orientation = orientation_name(current_orientation);
      apply_display_orientation(display.get(), current_orientation, width,
                                height);
      launcher_ui_apply_orientation(ui.get(), current_orientation);
      lv_obj_invalidate(lv_screen_active());
      lv_refr_now(display.get());
      launcher::logging::info("display orientation changed to %s",
                              process_options.orientation);
    }
    const launcher_app_t *requested_app =
        launcher_ui_take_launch_request(ui.get());
    if (requested_app) {
      lv_refr_now(display.get());
      lv_timer_enable(false);
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
      const bool release_drm = strcmp(backend, "drm") == 0;
      const bool release_display = release_drm;
      if (release_display) {
        if (input_device) {
          lv_indev_delete(input_device);
          input_device = nullptr;
        }
        ui.reset();
        display.clear();
      }
#endif
#if defined(LAUNCHER_SIMULATOR)
      SDL_Window *launcher_window = lv_sdl_window_get_window(display.get());
      if (launcher_window)
        SDL_HideWindow(launcher_window);
#endif
      int result = launcher_process_run(requested_app, &process_options,
                                        &stop_requested);
      if (result == 0)
        launcher::logging::info("%s exited with code 0",
                                requested_app->manifest.id);
      else
        launcher::logging::warn("%s exited with code %d",
                                requested_app->manifest.id, result);
#if defined(LAUNCHER_SIMULATOR)
      if (launcher_window) {
        SDL_ShowWindow(launcher_window);
        SDL_RaiseWindow(launcher_window);
      }
#elif defined(__linux__)
      if (release_display) {
        if (release_drm) {
          created_display = lv_linux_drm_create();
          if (created_display &&
              lv_linux_drm_set_file(created_display, drm_path,
                                    drm_connector_id) != LV_RESULT_OK) {
            lv_display_delete(created_display);
            created_display = nullptr;
          }
        }

        if (!created_display) {
          launcher::logging::error("cannot restore %s display", backend);
          return 1;
        }

        display_alive = 1;
        display.reset(created_display);
        apply_display_orientation(display.get(), current_orientation, width,
                                  height);
        lv_display_add_event_cb(display.get(), display_deleted, LV_EVENT_DELETE,
                                NULL);
        if (release_drm) {
          input_device = lv_evdev_create(LV_INDEV_TYPE_POINTER, input_path);
          if (!input_device)
            launcher::logging::warn("cannot initialize input device '%s'",
                                    input_path);
        }
        ui.reset(launcher_ui_create(registry.get()));
        if (!ui) {
          launcher::logging::error("cannot rebuild UI after application exit");
          return 1;
        }
        // Prime one DRM scanout buffer here; the common refresh below fills
        // the second buffer before status-bar updates begin using partial redraws.
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(display.get());
      }
#endif
      lv_timer_enable(true);
      lv_obj_invalidate(lv_screen_active());
      lv_refr_now(display.get());
      next_app_scan = tick_ms() + 2000U;
      if (stop_requested)
        break;
    }
    uint32_t now = tick_ms();
    if (launcher_ui_is_home(ui.get()) &&
        static_cast<int32_t>(now - next_app_scan) >= 0) {
      size_t previous_count = registry->count;
      size_t removed = launcher_registry_prune_missing(registry.get());
      discover_apps(registry.get(), app_directory);
      if (removed > 0 || registry->count != previous_count) {
        launcher::logging::info("application list changed (%zu installed)",
                                registry->count);
        launcher_ui_refresh_home(ui.get());
      }
      next_app_scan = now + 2000U;
    }
    if (delay < 1)
      delay = 1;
    if (delay > 20)
      delay = 20;
    usleep(delay * 1000U);
  }

  return 0;
}
