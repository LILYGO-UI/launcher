#ifndef LAUNCHER_SYSTEM_CONTROLS_H
#define LAUNCHER_SYSTEM_CONTROLS_H

#include <string>
#include <vector>

namespace launcher {

struct SystemControlsState {
    bool brightness_available = false;
    /* The CM0 RM69A10 panel has no kernel backlight node; in that case the
     * launcher dims its rendered scene instead. */
    bool brightness_software = false;
    int brightness_percent = 0;
    bool volume_available = false;
    int volume_percent = 0;
    bool power_off_available = false;
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

    // Refresh when opening Quick controls and periodically while it is open.
    const SystemControlsState &refresh();
    const SystemControlsState &state() const { return state_; }

    // Brightness has a 1% floor so its lowest setting keeps the UI visible.
    // Successful writes update state with the actual hardware value.
    bool set_brightness(int percent);
    bool set_volume(int percent);

    // Call only after confirmation. Simulator builds never shut down the host.
    // On Linux, success means systemd accepted an orderly power-off request.
    bool power_off();
    const std::string &error() const { return error_; }

private:
    void refresh_brightness();
    void refresh_volume();
    bool fail(const char *message);

    SystemControlsState state_;
    std::string backlight_directory_;
    std::string brightness_path_;
    long brightness_maximum_ = 0;
    std::string volume_card_;
    std::vector<std::string> volume_elements_;
    std::string error_;
};

} // namespace launcher

#endif
