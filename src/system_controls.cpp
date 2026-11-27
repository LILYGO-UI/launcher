#include "system_controls.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <utility>
#include <unistd.h>

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace launcher {
namespace {

lilygo::ui::HardwarePaths hardware_paths(std::string backlight_directory)
{
    lilygo::ui::HardwarePaths paths;
    paths.backlight_root = std::move(backlight_directory);
    return paths;
}

int percentage(int value, int maximum)
{
    if (maximum <= 0) return 0;
    const double fraction = static_cast<double>(value) / maximum;
    return static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 100.0));
}

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)

const char *systemctl_path()
{
    for (const char *path : {"/usr/bin/systemctl", "/bin/systemctl"})
        if (access(path, X_OK) == 0) return path;
    return nullptr;
}

const char *local_power_path(bool restart)
{
#if defined(LAUNCHER_SYSTEM_SESSION) && !LAUNCHER_SYSTEM_SESSION
    // Buildroot uses BusyBox init. Use its orderly commands without force flags.
    if (geteuid() != 0) return nullptr;
    for (const char *path : {restart ? "/sbin/reboot" : "/sbin/poweroff",
                             restart ? "/usr/sbin/reboot" : "/usr/sbin/poweroff"})
        if (access(path, X_OK) == 0) return path;
#else
    (void)restart;
#endif
    return nullptr;
}

#endif

} // namespace

SystemControls::SystemControls(bool simulated, std::string backlight_directory)
    : SystemControls(simulated, hardware_paths(std::move(backlight_directory)))
{
}

SystemControls::SystemControls(bool simulated, lilygo::ui::HardwarePaths paths)
    : hardware_(std::move(paths))
{
#if defined(LAUNCHER_SIMULATOR)
    (void)simulated;
    state_.simulated = true;
#else
    state_.simulated = simulated;
#endif
    if (state_.simulated) {
        state_.brightness_available = true;
        state_.brightness_percent = 65;
        state_.volume_available = true;
        state_.volume_percent = 45;
        state_.power_off_available = true;
        state_.reboot_available = true;
    }
    refresh();
}

const SystemControlsState &SystemControls::refresh()
{
    if (state_.simulated) return state_;
    refresh_brightness();
    refresh_volume();
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    const bool systemd = access("/run/systemd/system", F_OK) == 0;
    const char *systemctl = systemd ? systemctl_path() : nullptr;
    state_.power_off_available = systemd ? systemctl != nullptr : local_power_path(false) != nullptr;
    state_.reboot_available = systemd ? systemctl != nullptr : local_power_path(true) != nullptr;
#endif
    return state_;
}

void SystemControls::refresh_brightness()
{
    state_.brightness_available = false;
    state_.brightness_software = false;
    brightness_maximum_ = 0;
    lilygo::ui::BacklightState backlight;
    const auto result = hardware_.read_backlight(&backlight);
    if (result == lilygo::ui::HardwareError::ok) {
        brightness_maximum_ = backlight.maximum;
        state_.brightness_percent = percentage(backlight.brightness, backlight.maximum);
        state_.brightness_available = backlight.writable;
        return;
    }

    /* Some RM69A10 panel drivers do not register /sys/class/backlight.
     * Keep the control usable with the
     * launcher scene dimmer. Custom test directories still report hardware
     * as unavailable so failures remain observable in tests. */
    if (hardware_.paths().backlight_root == "/sys/class/backlight" &&
        (result == lilygo::ui::HardwareError::unsupported ||
         result == lilygo::ui::HardwareError::unavailable)) {
        state_.brightness_available = true;
        state_.brightness_software = true;
        state_.brightness_percent = state_.brightness_percent > 0
            ? std::clamp(state_.brightness_percent, 1, 100) : 100;
    }
}

void SystemControls::refresh_volume()
{
    lilygo::ui::VolumeState volume;
    state_.volume_available = hardware_.read_volume(&volume) == lilygo::ui::HardwareError::ok;
    if (state_.volume_available)
        state_.volume_percent = volume.muted ? 0 : volume.percent;
}

bool SystemControls::fail(const char *message)
{
    error_ = message;
    return false;
}

bool SystemControls::set_brightness(int percent)
{
    error_.clear();
    percent = std::clamp(percent, 1, 100);
    if (state_.simulated) {
        state_.brightness_percent = percent;
        return true;
    }
    refresh_brightness();
    if (!state_.brightness_available) return fail("Screen brightness is unavailable");
    if (state_.brightness_software) {
        state_.brightness_percent = percent;
        return true;
    }
    const int value = std::max(1, static_cast<int>(std::lround(
        static_cast<double>(brightness_maximum_) * percent / 100.0)));
    const auto result = hardware_.set_backlight(value);
    refresh_brightness();
    if (result != lilygo::ui::HardwareError::ok || !state_.brightness_available)
        return fail("Could not change screen brightness");
    return true;
}

bool SystemControls::set_volume(int percent)
{
    error_.clear();
    percent = std::clamp(percent, 0, 100);
    if (state_.simulated) {
        state_.volume_percent = percent;
        return true;
    }
    refresh_volume();
    if (!state_.volume_available) return fail("Audio volume is unavailable");
    const auto result = hardware_.set_volume(percent);
    refresh_volume();
    if (result != lilygo::ui::HardwareError::ok || !state_.volume_available)
        return fail("Could not change audio volume");
    return true;
}

bool SystemControls::power_off()
{
    return request_power(false);
}

bool SystemControls::reboot()
{
    return request_power(true);
}

bool SystemControls::request_power(bool restart)
{
    error_.clear();
    if (state_.simulated) return true;
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    const bool systemd = access("/run/systemd/system", F_OK) == 0;
    const char *command = systemd ? systemctl_path() : local_power_path(restart);
    if (!command || !(restart ? state_.reboot_available : state_.power_off_available))
        return fail(restart ? "Reboot is unavailable" : "Power off is unavailable");
    char *systemd_arguments[] = {const_cast<char *>(command), const_cast<char *>("--no-block"),
                                const_cast<char *>("--no-ask-password"),
                                const_cast<char *>(restart ? "reboot" : "poweroff"), nullptr};
    char *local_arguments[] = {const_cast<char *>(command), nullptr};
    char **arguments = systemd ? systemd_arguments : local_arguments;
    pid_t child = -1;
    if (posix_spawn(&child, command, nullptr, nullptr, arguments, environ) != 0)
        return fail(restart ? "Could not request reboot" : "Could not request power off");
    // Bound the request even if init or the system bus stops responding.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
            return fail(restart ? "The system could not reboot" : "The system could not power off");
        }
        if (result < 0 && errno != EINTR) return fail(restart ? "Could not confirm reboot" : "Could not confirm power off");
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(child, SIGKILL);
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    return fail(restart ? "The reboot request timed out" : "The power off request timed out");
#else
    return fail(restart ? "Reboot is unavailable" : "Power off is unavailable");
#endif
}

} // namespace launcher
