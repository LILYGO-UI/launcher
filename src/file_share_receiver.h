#ifndef LILYGO_UI_LAUNCHER_FILE_SHARE_RECEIVER_H
#define LILYGO_UI_LAUNCHER_FILE_SHARE_RECEIVER_H

#include "user.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <cstdint>
#include <string>
#include <thread>
#include <sys/types.h>

namespace launcher::file_share {

struct PendingApproval {
    std::uint64_t id = 0;
    std::string filename;
    std::uint64_t size = 0;
};

/* The UI thread owns this controller. The transport child handles Bluetooth
 * and WebRTC; one worker serializes its requests against the authenticated
 * user's Transfer storage. No LVGL objects cross this boundary. */
class Receiver {
public:
    /* An explicit helper path also enables the transport in a host simulator.
     * Production always uses the installed, administrator-owned helper. */
    explicit Receiver(std::string helper_path = {});
    ~Receiver();
    Receiver(const Receiver &) = delete;
    Receiver &operator=(const Receiver &) = delete;

    bool start(const launcher_user_t *user, std::string &error);
    void stop();
    bool active() const { return active_.load(); }
    std::string status() const;
    std::optional<PendingApproval> pending_approval() const;
    bool resolve_approval(std::uint64_t id, bool accepted);

private:
    void run(launcher_user_t user);
    void set_status(std::string message);
    void reap_child();

    std::string helper_path_;
    bool explicit_helper_ = false;
    int socket_fd_ = -1;
    pid_t child_ = -1;
    std::thread worker_;
    std::atomic<bool> active_{false};
    std::atomic<bool> stopping_{false};
    mutable std::mutex status_mutex_;
    std::string status_ = "Open file sharing to connect from your browser.";
    std::optional<PendingApproval> pending_approval_;
    std::uint64_t next_approval_id_ = 1;
    std::uint64_t resolved_approval_id_ = 0;
    bool resolved_approval_ = false;
    std::chrono::steady_clock::time_point approval_deadline_{};
};

} // namespace launcher::file_share

#endif
