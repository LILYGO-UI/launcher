#include "process.h"
#include "logging.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void write_all(int fd, const char *data, size_t size) {
  while (size > 0) {
    ssize_t written = write(fd, data, size);
    if (written > 0) {
      data += written;
      size -= (size_t)written;
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
}

static void report_exec_failure(const char *executable,
                                size_t executable_length) {
  static constexpr char prefix[] = "launcher: cannot execute ";
  static constexpr char suffix[] = "\n";
  write_all(STDERR_FILENO, prefix, sizeof(prefix) - 1);
  write_all(STDERR_FILENO, executable, executable_length);
  write_all(STDERR_FILENO, suffix, sizeof(suffix) - 1);
}

static long long monotonic_ms(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

int launcher_process_run(const launcher_app_t *app,
                         const launcher_process_options_t *options,
                         volatile sig_atomic_t *stop_requested) {
  if (!app || !options || !options->display_backend || !options->fbdev_path ||
      !options->drm_path || !options->input_path || !options->orientation)
    return -1;

  char width[16];
  char height[16];
  char drm_connector[32];
  snprintf(width, sizeof(width), "%d", options->width);
  snprintf(height, sizeof(height), "%d", options->height);
  snprintf(drm_connector, sizeof(drm_connector), "%lld",
           (long long)options->drm_connector_id);
  size_t executable_length = strnlen(app->executable_path, LAUNCHER_PATH_MAX);

  pid_t pid = fork();
  if (pid < 0) {
    launcher::logging::error("cannot fork %s: %s", app->executable_path,
                             strerror(errno));
    return -1;
  }
  if (pid == 0) {
    setpgid(0, 0);
    const auto argument = [](const char *value) {
      return const_cast<char *>(value);
    };
    char *const arguments[] = {
        argument(app->executable_path),
        argument("--app-id"),
        argument(app->manifest.id),
        argument("--app-name"),
        argument(app->manifest.name),
        argument("--display"),
        argument(options->display_backend),
        argument("--width"),
        width,
        argument("--height"),
        height,
        argument("--fbdev"),
        argument(options->fbdev_path),
        argument("--drm"),
        argument(options->drm_path),
        argument("--drm-connector"),
        drm_connector,
        argument("--input"),
        argument(options->input_path),
        argument("--orientation"),
        argument(options->orientation),
        nullptr,
    };
    execv(app->executable_path, arguments);
    report_exec_failure(app->executable_path, executable_length);
    _exit(127);
  }

  setpgid(pid, pid);
  launcher::logging::info("started %s as process %ld", app->manifest.id,
                          (long)pid);

  int status = 0;
  int terminating = 0;
  long long terminate_at = 0;
  for (;;) {
    pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid)
      break;
    if (result < 0 && errno != EINTR) {
      launcher::logging::error("waitpid failed for %s: %s", app->manifest.id,
                               strerror(errno));
      return -1;
    }

    if (stop_requested && *stop_requested && !terminating) {
      kill(-pid, SIGTERM);
      terminating = 1;
      terminate_at = monotonic_ms();
    } else if (terminating == 1 && monotonic_ms() - terminate_at >= 3000) {
      kill(-pid, SIGKILL);
      terminating = 2;
    }
    usleep(10000);
  }

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) {
    launcher::logging::warn("%s terminated by signal %d", app->manifest.id,
                            WTERMSIG(status));
    return 128 + WTERMSIG(status);
  }
  return -1;
}
