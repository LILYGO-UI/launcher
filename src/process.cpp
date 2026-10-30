#include "process.h"
#include "logging.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern char **environ;

static void write_all(int fd, const char *data, size_t size)
{
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

static void report_exec_failure(const char *executable, size_t executable_length)
{
    static constexpr char prefix[] = "launcher: cannot execute ";
    static constexpr char suffix[] = "\n";
    write_all(STDERR_FILENO, prefix, sizeof(prefix) - 1);
    write_all(STDERR_FILENO, executable, executable_length);
    write_all(STDERR_FILENO, suffix, sizeof(suffix) - 1);
}

static void report_child_setup_failure(void)
{
    static constexpr char message[] = "launcher: cannot prepare application user environment\n";
    write_all(STDERR_FILENO, message, sizeof(message) - 1);
}

/* Keep account names and paths suitable for use as environment values. */
static int valid_string(const char *value, size_t capacity)
{
    if (!value) return 0;
    const size_t length = strnlen(value, capacity);
    if (length == 0 || length >= capacity) return 0;
    for (size_t index = 0; index < length; ++index) {
        const unsigned char character = (unsigned char)value[index];
        if (character < 0x20 || character == 0x7f) return 0;
    }
    return 1;
}

static int valid_name(const char *name)
{
    if (!valid_string(name, LAUNCHER_USER_NAME_MAX)) return 0;
    for (const unsigned char *cursor = (const unsigned char *)name; *cursor; ++cursor) {
        if (!(('a' <= *cursor && *cursor <= 'z') || ('A' <= *cursor && *cursor <= 'Z') ||
              ('0' <= *cursor && *cursor <= '9') || *cursor == '_' || *cursor == '-' || *cursor == '.'))
            return 0;
    }
    return 1;
}

static int valid_path(const char *path, size_t capacity)
{
    if (!valid_string(path, capacity) || path[0] != '/') return 0;

    /* Reject dot components so a value cannot escape a user-owned root when
     * it is later joined with an application-specific directory. */
    size_t component_start = 1;
    const size_t length    = strlen(path);
    for (size_t index = 1; index <= length; ++index) {
        if (path[index] != '/' && path[index] != '\0') continue;
        const size_t component_length = index - component_start;
        if ((component_length == 1 && path[component_start] == '.') ||
            (component_length == 2 && path[component_start] == '.' && path[component_start + 1] == '.'))
            return 0;
        component_start = index + 1;
    }
    return 1;
}

static int append_path(char *destination, size_t capacity, const char *base, const char *suffix)
{
    const size_t base_length   = strlen(base);
    const size_t suffix_length = strlen(suffix);
    if (base_length + suffix_length >= capacity) return -1;
    memcpy(destination, base, base_length);
    memcpy(destination + base_length, suffix, suffix_length + 1);
    return 0;
}

static void unset_environment_prefix(const char *prefix)
{
    const size_t prefix_length = strlen(prefix);
    std::vector<std::string> names;
    for (char **entry = environ; entry && *entry; ++entry) {
        const char *equals = strchr(*entry, '=');
        if (equals && (size_t)(equals - *entry) >= prefix_length &&
            strncmp(*entry, prefix, prefix_length) == 0) {
            const size_t name_length = (size_t)(equals - *entry);
            if (name_length < 128) names.emplace_back(*entry, name_length);
        }
    }
    for (const std::string &name : names) unsetenv(name.c_str());
}

static void sanitize_user_environment(void)
{
    /* Clear inherited loader settings before selecting any trusted runtime. */
    unset_environment_prefix("LD_");
    unset_environment_prefix("SUDO_");
    unset_environment_prefix("PYTHON");
    unset_environment_prefix("PERL");
    unset_environment_prefix("RUBY");
    unsetenv("BASH_ENV");
    unsetenv("ENV");
    unsetenv("GCONV_PATH");
    unsetenv("GCONV_DIR");
    unsetenv("SSH_AUTH_SOCK");
    unsetenv("SSH_AGENT_PID");
    unsetenv("LAUNCHER_APP_DIRS");
    unsetenv("TZ"); // Use the system timezone selected by first-boot setup.
    /* A root PATH can contain writable administrator-only directories. */
    setenv("PATH", "/usr/local/bin:/usr/bin:/bin", 1);
}

using LocaleVariables = std::vector<std::pair<std::string, std::string>>;

static std::string_view trim_locale_field(std::string_view value)
{
    const auto first = value.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r") - first + 1);
}

static bool locale_variable(std::string_view name)
{
    for (const auto *allowed : {"LANG", "LANGUAGE", "LC_ALL", "LC_CTYPE", "LC_NUMERIC", "LC_TIME",
                                "LC_COLLATE", "LC_MONETARY", "LC_MESSAGES", "LC_PAPER", "LC_NAME",
                                "LC_ADDRESS", "LC_TELEPHONE", "LC_MEASUREMENT", "LC_IDENTIFICATION"}) {
        if (name == allowed) return true;
    }
    return false;
}

static bool parse_locale_variables(const std::string &contents, LocaleVariables &variables)
{
    if (contents.find('\0') != std::string::npos) return false;
    size_t offset = 0;
    while (offset < contents.size()) {
        const auto newline = contents.find('\n', offset);
        auto line = trim_locale_field(std::string_view(contents).substr(offset, newline == std::string::npos ? newline : newline - offset));
        offset = newline == std::string::npos ? contents.size() : newline + 1;
        if (line.empty() || line.front() == '#') continue;
        if (line.substr(0, 7) == "export ") line = trim_locale_field(line.substr(7));
        const auto equals = line.find('=');
        if (equals == std::string_view::npos) continue;
        const auto name = trim_locale_field(line.substr(0, equals));
        if (!locale_variable(name)) continue;
        auto value = trim_locale_field(line.substr(equals + 1));
        if (!value.empty() && (value.front() == '\'' || value.front() == '"')) {
            const auto quote = value.find(value.front(), 1);
            if (quote == std::string_view::npos) return false;
            const auto remainder = trim_locale_field(value.substr(quote + 1));
            if (!remainder.empty() && remainder.front() != '#') return false;
            value = value.substr(1, quote - 1);
        } else {
            const auto space = value.find_first_of(" \t#");
            if (space != std::string_view::npos) {
                const auto remainder = trim_locale_field(value.substr(space));
                if (!remainder.empty() && remainder.front() != '#') return false;
                value = value.substr(0, space);
            }
        }
        if (value.size() > 256) return false;
        for (const unsigned char character : value) {
            if (!(('a' <= character && character <= 'z') || ('A' <= character && character <= 'Z') ||
                  ('0' <= character && character <= '9') || character == '_' || character == '-' ||
                  character == '.' || character == '@' || character == ':')) return false;
        }
        auto existing = variables.begin();
        while (existing != variables.end() && existing->first != name) ++existing;
        if (existing == variables.end()) variables.emplace_back(name, value);
        else existing->second = value;
    }
    return true;
}

// Fixture root/UID are private test seams; device callers always use the real
// root-owned system configuration. Opening then checking the descriptor permits
// Debian's legitimate compatibility symlink without trusting a writable target.
static int apply_system_locale(const std::string &root = "", uid_t trusted_owner = 0)
{
    int fd = open((root + "/etc/locale.conf").c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT)
        fd = open((root + "/etc/default/locale").c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat metadata{};
    bool valid = fstat(fd, &metadata) == 0 && S_ISREG(metadata.st_mode) && metadata.st_uid == trusted_owner &&
                 !(metadata.st_mode & (S_IWGRP | S_IWOTH)) && metadata.st_size <= 64 * 1024;
    std::string contents;
    char buffer[4096];
    while (valid) {
        const auto length = read(fd, buffer, sizeof(buffer));
        if (length == 0) break;
        if (length < 0 && errno == EINTR) continue;
        if (length < 0 || contents.size() + static_cast<size_t>(length) > 64 * 1024) {
            valid = false;
            break;
        }
        contents.append(buffer, static_cast<size_t>(length));
    }
    close(fd);
    LocaleVariables variables;
    if (!valid || !parse_locale_variables(contents, variables)) return -1;
    // The launcher was started before the wizard changed these defaults. Read
    // them for each device child, so applications need no service restart.
    unset_environment_prefix("LC_");
    if (unsetenv("LANG") != 0 || unsetenv("LANGUAGE") != 0) return -1;
    for (const auto &variable : variables) {
        if (!variable.second.empty() && setenv(variable.first.c_str(), variable.second.c_str(), 1) != 0) return -1;
    }
    return 0;
}

static int prepare_user_environment(const launcher_user_t *user)
{
    if (!user || user->uid == (uid_t)0 || user->gid == (gid_t)0 || !valid_name(user->name) ||
        !valid_path(user->home, LAUNCHER_USER_PATH_MAX) ||
        !valid_path(user->shell, LAUNCHER_USER_SHELL_MAX) ||
        !valid_path(user->runtime_dir, LAUNCHER_USER_PATH_MAX))
        return -1;

    char config_home[LAUNCHER_USER_PATH_MAX];
    char data_home[LAUNCHER_USER_PATH_MAX];
    char cache_home[LAUNCHER_USER_PATH_MAX];
    char state_home[LAUNCHER_USER_PATH_MAX];
    if (append_path(config_home, sizeof(config_home), user->home, "/.config") != 0 ||
        append_path(data_home, sizeof(data_home), user->home, "/.local/share") != 0 ||
        append_path(cache_home, sizeof(cache_home), user->home, "/.cache") != 0 ||
        append_path(state_home, sizeof(state_home), user->home, "/.local/state") != 0)
        return -1;

    const uid_t effective_uid = geteuid();
    const gid_t effective_gid = getegid();
    if (effective_uid != (uid_t)0 &&
        (user->uid != effective_uid || user->gid != effective_gid))
        return -1;

    /* The service selects this packaged GBM/EGL/GLES/Gallium runtime as a
     * pair. Clearing only LD_LIBRARY_PATH leaves the private GBM backend
     * pointing at a Gallium library the child cannot find. Recognize only
     * the exact installation paths, never arbitrary loader search paths. */
    static constexpr char mesa_library_path[] = "/usr/lib/lilygo-ui/mesa-vc4/lib";
    static constexpr char mesa_backend_path[] = "/usr/lib/lilygo-ui/mesa-vc4/lib/gbm";
    const char *library_path = getenv("LD_LIBRARY_PATH");
    const char *backend_path = getenv("GBM_BACKENDS_PATH");
    const bool private_mesa = library_path && backend_path &&
                              strcmp(library_path, mesa_library_path) == 0 &&
                              strcmp(backend_path, mesa_backend_path) == 0;

    sanitize_user_environment();
    if (apply_system_locale() != 0) return -1;
    if (private_mesa && setenv("LD_LIBRARY_PATH", mesa_library_path, 1) != 0) return -1;
    if (setenv("HOME", user->home, 1) != 0 || setenv("USER", user->name, 1) != 0 ||
        setenv("LOGNAME", user->name, 1) != 0 || setenv("SHELL", user->shell, 1) != 0 ||
        setenv("XDG_CONFIG_HOME", config_home, 1) != 0 ||
        setenv("XDG_DATA_HOME", data_home, 1) != 0 || setenv("XDG_CACHE_HOME", cache_home, 1) != 0 ||
        setenv("XDG_STATE_HOME", state_home, 1) != 0 ||
        setenv("XDG_RUNTIME_DIR", user->runtime_dir, 1) != 0)
        return -1;

    if (effective_uid == (uid_t)0) {
        /* initgroups must run before dropping gid; it needs root privileges. */
        if (initgroups(user->name, user->gid) != 0 || setgid(user->gid) != 0 || setuid(user->uid) != 0)
            return -1;
    }
    /* Keep the signed-in user's normal sudo/polkit behavior available.  The
     * launcher boundary is the authenticated UID/GID and sanitized
     * environment; an inherited no-new-privileges flag would also block
     * legitimate Settings operations that use the system's policy. */
    return 0;
}

static long long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000LL + now.tv_nsec / 1000000LL;
}

int launcher_process_run(const launcher_app_t *app, const launcher_process_options_t *options,
                         volatile sig_atomic_t *stop_requested)
{
    return launcher_process_run_with_poll(app, options, stop_requested, nullptr, nullptr);
}

int launcher_process_run_with_poll(const launcher_app_t *app, const launcher_process_options_t *options,
                                   volatile sig_atomic_t *stop_requested, void (*poll)(void *), void *poll_data)
{
    if (!app || !options || !options->display_backend || !options->fbdev_path || !options->drm_path ||
        !options->input_path || !options->orientation)
        return -1;

    char width[16];
    char height[16];
    char drm_connector[32];
    snprintf(width, sizeof(width), "%d", options->width);
    snprintf(height, sizeof(height), "%d", options->height);
    snprintf(drm_connector, sizeof(drm_connector), "%lld", (long long)options->drm_connector_id);
    size_t executable_length = strnlen(app->executable_path, LAUNCHER_PATH_MAX);

    pid_t pid = fork();
    if (pid < 0) {
        launcher::logging::error("cannot fork %s: %s", app->executable_path, strerror(errno));
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        if (options->user && prepare_user_environment(options->user) != 0) {
            report_child_setup_failure();
            _exit(126);
        }
        const auto argument     = [](const char *value) { return const_cast<char *>(value); };
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
    launcher::logging::info("started %s as process %ld", app->manifest.id, (long)pid);

    int status             = 0;
    int terminating        = 0;
    long long terminate_at = 0;
    long long next_poll_at = monotonic_ms() + 500;
    for (;;) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) break;
        if (result < 0 && errno != EINTR) {
            launcher::logging::error("waitpid failed for %s: %s", app->manifest.id, strerror(errno));
            return -1;
        }

        if (stop_requested && *stop_requested && !terminating) {
            kill(-pid, SIGTERM);
            terminating  = 1;
            terminate_at = monotonic_ms();
        } else if (terminating == 1 && monotonic_ms() - terminate_at >= 3000) {
            kill(-pid, SIGKILL);
            terminating = 2;
        }
        if (poll && monotonic_ms() >= next_poll_at) {
            poll(poll_data);
            next_poll_at = monotonic_ms() + 500;
        }
        usleep(10000);
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        launcher::logging::warn("%s terminated by signal %d", app->manifest.id, WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    return -1;
}
