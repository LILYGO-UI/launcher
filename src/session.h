#ifndef LILYGO_UI_LAUNCHER_SESSION_H
#define LILYGO_UI_LAUNCHER_SESSION_H

#include "user.h"

#include <memory>
#include <string>

namespace launcher {

// Owns the signed-in identity and, on devices, its authenticated process account.
// UserSession is deliberately single-threaded: callers serialize initialize,
// login, reauthenticate and logout (the UI uses one worker for these operations).
class UserSession {
public:
    // system_root redirects only home/configuration paths in isolated tests;
    // production always uses the empty default (it is not a helper chroot).
    explicit UserSession(std::string state_path = "/var/lib/lilygo-ui-launcher/active-user",
                         std::string system_root = "");
    ~UserSession();

    UserSession(const UserSession &) = delete;
    UserSession &operator=(const UserSession &) = delete;

    bool initialize(std::string &error);
    // Embedded images can delegate authentication to their invoking login or
    // init service. This mode keeps the current UID/GID and has no lock/logout.
    bool local_session() const;
    bool setup_available() const;
    bool setup_required() const;
    bool setup_account_saved() const;
    const std::string &setup_username() const;
    const std::string &last_user() const;
    // Display identity; empty until sign-in. Simulator identities are virtual.
    const std::string &username() const;
    bool login(const std::string &username, const std::string &password, bool create,
               std::string &error, bool finish_setup = true);
    // The full wizard delays upstream cleanup until regional/network/update steps finish.
    bool complete_setup(std::string &error);
    // Check the signed-in identity's password without changing its session.
    bool reauthenticate(const std::string &password, std::string &error);
    void logout();
    // Process identity for authenticated device sessions; null when credentials
    // must be inherited (local/simulator sessions), or after sign-out.
    const launcher_user_t *user() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace launcher

#endif
