#ifndef LAUNCHER_SYSTEM_CONTROLS_H
#define LAUNCHER_SYSTEM_CONTROLS_H

#include <cm0/hardware.h>

#include <string>

namespace launcher {

struct SystemControlsState {
    bool brightness_available = false;
    /* Some RM69A10 drivers have no kernel backlight node; in that case the
     * launcher dims its rendered scene instead. */
    bool brightness_software = false;
    int brightness_percent = 0;
    bool volume_available = false;
    int volume_percent = 0;
    bool power_off_available = false;
    bool reboot_available = false;
    bool simulated = false;
};

class SystemControls {
public:
    explicit SystemControls(
#if defined(LAUNCHER_SIMULATOR) || !defined(__linux__)
        bool simulated = true,
#else
        bool simulated = false,
#endif
        std::string backlight_directory = "/sys/class/backlight");

    // Explicit backend paths support product profiles and host device mocks.
    SystemControls(bool simulated, lilygo::ui::HardwarePaths paths);

    // Refresh when opening Quick controls and periodically while it is open.
    const SystemControlsState &refresh();
    const SystemControlsState &state() const { return state_; }

    // Brightness has a 1% floor so its lowest setting keeps the UI visible.
    // Successful writes update state with the actual hardware value.
    bool set_brightness(int percent);
    bool set_volume(int percent);

    // Call only after confirmation. Simulator builds handle both actions in memory.
    // On Linux, success means init accepted the request.
    bool power_off();
    bool reboot();
    const std::string &error() const { return error_; }

private:
    void refresh_brightness();
    void refresh_volume();
    bool request_power(bool restart);
    bool fail(const char *message);

    SystemControlsState state_;
    lilygo::ui::Hardware hardware_;
    int brightness_maximum_ = 0;
    std::string error_;
};

} // namespace launcher

#endif
