#include "bluetooth_advertiser.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace launcher {
namespace {

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
constexpr char kBluetoothctl[] = "/usr/bin/bluetoothctl";
constexpr char kBtmgmt[] = "/usr/bin/btmgmt";
constexpr char kStartScript[] =
    "if command -v rfkill >/dev/null 2>&1; then rfkill unblock bluetooth >/dev/null 2>&1 || true; fi\n"
    "power on\n"
    "menu advertise\n"
    "back\n"
    "advertise on\n";
constexpr char kStopScript[] = "advertise off\nquit\n";
#endif

void set_error(std::string &error, const char *message)
{
    error = message ? message : "Bluetooth advertising failed";
}

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
bool write_all(int fd, const char *data, size_t size)
{
    while (size > 0) {
        const ssize_t written = write(fd, data, size);
        if (written > 0) {
            data += written;
            size -= static_cast<size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool run_btmgmt(const char *command, const char *value, std::string &error)
{
    if (access(kBtmgmt, X_OK) != 0) {
        set_error(error, "btmgmt is unavailable");
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        error = std::string("cannot start btmgmt: ") + std::strerror(errno);
        return false;
    }
    if (child == 0) {
        int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        execl(kBtmgmt, kBtmgmt, command, value, static_cast<char *>(nullptr));
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        set_error(error, "btmgmt could not change controller advertising");
        return false;
    }
    return true;
}
#endif

} // namespace

BluetoothAdvertiser::~BluetoothAdvertiser()
{
    stop();
}

bool BluetoothAdvertiser::start(std::string &error)
{
    if (active_) return true;
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    if (access(kBluetoothctl, X_OK) != 0) {
        set_error(error, "bluetoothctl is unavailable");
        return false;
    }
    /* This legacy helper is not part of the active file-share path. Keep its
     * fallback name in the same shape as the GDBus transport name. */
    if (!run_btmgmt("power", "on", error) || !run_btmgmt("name", "lilygo_0000", error))
        return false;

    int input[2];
    if (pipe(input) != 0) {
        error = std::string("cannot create bluetoothctl pipe: ") + std::strerror(errno);
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        const int saved = errno;
        close(input[0]);
        close(input[1]);
        error = std::string("cannot start bluetoothctl: ") + std::strerror(saved);
        return false;
    }
    if (child == 0) {
        dup2(input[0], STDIN_FILENO);
        int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null_fd >= 0) {
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        close(input[0]);
        close(input[1]);
        execl(kBluetoothctl, kBluetoothctl, static_cast<char *>(nullptr));
        _exit(127);
    }

    close(input[0]);
    struct sigaction ignore_pipe{}, previous_pipe{};
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    sigaction(SIGPIPE, &ignore_pipe, &previous_pipe);
    const bool write_ok = write_all(input[1], kStartScript, std::strlen(kStartScript));
    sigaction(SIGPIPE, &previous_pipe, nullptr);
    if (!write_ok) {
        close(input[1]);
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        set_error(error, "cannot configure bluetoothctl advertisement");
        return false;
    }
    /* Keep bluetoothctl alive so BlueZ keeps the advertisement object owned by
     * this launcher. It receives the stop script during destruction. */
    process_id_ = child;
    input_fd_ = input[1];
    if (!run_btmgmt("advertising", "on", error)) {
        stop();
        return false;
    }
    active_ = true;
    error.clear();
    return true;
#else
    set_error(error, "Bluetooth advertising is unavailable in this build");
    return false;
#endif
}

void BluetoothAdvertiser::stop()
{
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    if (process_id_ < 0) {
        active_ = false;
        return;
    }
    if (input_fd_ >= 0) {
        struct sigaction ignore_pipe{}, previous_pipe{};
        ignore_pipe.sa_handler = SIG_IGN;
        sigemptyset(&ignore_pipe.sa_mask);
        sigaction(SIGPIPE, &ignore_pipe, &previous_pipe);
        write_all(input_fd_, kStopScript, std::strlen(kStopScript));
        sigaction(SIGPIPE, &previous_pipe, nullptr);
        close(input_fd_);
        input_fd_ = -1;
    }
    int status = 0;
    while (waitpid(process_id_, &status, 0) < 0 && errno == EINTR) {}
    process_id_ = -1;
    std::string ignored;
    run_btmgmt("advertising", "off", ignored);
    active_ = false;
#else
    active_ = false;
#endif
}

} // namespace launcher
