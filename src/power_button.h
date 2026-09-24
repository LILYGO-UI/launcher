#ifndef LAUNCHER_POWER_BUTTON_H
#define LAUNCHER_POWER_BUTTON_H

#include <stdint.h>

enum PowerButtonEvent : unsigned int {
    POWER_BUTTON_EVENT_NONE     = 0,
    POWER_BUTTON_EVENT_PRESSED  = 1U << 0,
    POWER_BUTTON_EVENT_RELEASED = 1U << 1,
};

class PowerButtonMonitor {
public:
    PowerButtonMonitor() = default;
    ~PowerButtonMonitor();

    PowerButtonMonitor(const PowerButtonMonitor &)            = delete;
    PowerButtonMonitor &operator=(const PowerButtonMonitor &) = delete;

    // Open the requested event node, or discover the gpio-keys node when path
    // is null. Discovery is based on the KEY_POWER capability so event numbers
    // and udev naming do not become part of the launcher interface.
    bool open(const char *path = nullptr);
    void close();

    // Drain queued input events and return a bitmask of debounced state
    // changes. GEK100 emits an 8 Hz pulse train while held, so a release is
    // inferred after the documented 300 ms gap between pulse sequences.
    unsigned int poll(uint32_t now_ms);

private:
    int fd_                 = -1;
    uint32_t last_press_ms_ = 0;
    bool key_active_        = false;
};

#endif
