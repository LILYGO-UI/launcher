#ifndef LILYGO_UI_LAUNCHER_BLUETOOTH_ADVERTISER_H
#define LILYGO_UI_LAUNCHER_BLUETOOTH_ADVERTISER_H

#include <string>
#include <sys/types.h>

namespace launcher {

/* Owns the small BlueZ advertisement used by the browser device chooser.
 * This is deliberately separate from the WebRTC transport: the browser only
 * needs a short-lived local name advertisement before it opens the WSS session. */
class BluetoothAdvertiser {
public:
    BluetoothAdvertiser() = default;
    ~BluetoothAdvertiser();

    BluetoothAdvertiser(const BluetoothAdvertiser &) = delete;
    BluetoothAdvertiser &operator=(const BluetoothAdvertiser &) = delete;

    bool start(std::string &error);
    void stop();
    bool active() const { return active_; }

private:
    bool active_ = false;
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    pid_t process_id_ = -1;
    int input_fd_ = -1;
#endif
};

} // namespace launcher

#endif
