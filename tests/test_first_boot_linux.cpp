#include "session.h"
#include "setup_flow.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <string>
#include <unistd.h>

namespace {
constexpr const char *password = "fixture-first-boot-password";
constexpr const char *state = "/var/lib/lilygo-ui-launcher/active-user";

void require(bool value, const char *message, const std::string &error = {})
{
    if (value) return;
    std::fprintf(stderr, "FAIL: %s%s%s\n", message, error.empty() ? "" : ": ", error.c_str());
    std::exit(1);
}
}

// Real account helpers and PAM: only execute via test_first_boot_linux.sh in a
// disposable container. The public API is compiled from the production source.
int main(int argc, char **argv)
{
    require(geteuid() == 0 && access("/.dockerenv", F_OK) == 0 &&
                std::getenv("LILYGO_FIRST_BOOT_DISPOSABLE_TEST"), "disposable Docker fixture required");
    require(argc == 3, "usage: driver setup|restart|collision|cleanup_failure|disabled USER");
    const std::string mode = argv[1];
    const std::string username = argv[2];
    launcher::UserSession session(state);
    std::string error;
    require(session.initialize(error), "initialize", error);

    if (mode == "full_begin" || mode == "full_finish") {
        launcher::SetupFlow flow("/var/lib/lilygo-ui-launcher", false);
        require(flow.initialize(session.setup_required(), session.setup_account_saved(), error), "full setup progress", error);
        require(flow.active(), "full setup is active");
        if (mode == "full_begin") {
            require(flow.advance(launcher::SetupStage::Account, flow.region(), error), "save regional step", error);
            require(session.login(username, password, true, error, false), "save account without cleanup", error);
            require(session.setup_account_saved(), "account committed");
            require(access("/var/lib/lilygo-ui-launcher/first-boot-pending", F_OK) == 0, "retain upstream request");
            require(flow.advance(launcher::SetupStage::Network, flow.region(), error), "save network step", error);
        } else {
            require(flow.stage() == launcher::SetupStage::Network, "resume network after restart");
            require(!session.complete_setup(error), "cannot complete without authentication");
            require(!session.login(username, "wrong-password", false, error, false), "resume requires correct password");
            require(session.login(username, password, false, error, false), "authenticate resumed account", error);
            require(flow.advance(launcher::SetupStage::Update, flow.region(), error), "skip network", error);
            require(flow.advance(launcher::SetupStage::Complete, flow.region(), error), "skip updates", error);
            require(session.complete_setup(error), "complete upstream cleanup", error);
            require(flow.finish(error), "finish progress", error);
        }
        return 0;
    }

    if (mode == "disabled") {
        require(!session.setup_required(), "disabled image does not force setup");
        require(!session.setup_available(), "disabled image cannot create accounts");
        require(!session.login(username, password, true, error), "reject setup on disabled image");
        require(session.last_user() == "pi", "disabled image offers existing account");
        require(session.login("pi", "fixture-original-password", false, error), "disabled image permits existing login", error);
        require(session.user() && session.user()->uid == 1000, "disabled image retains UID");
        return 0;
    }
    if (mode == "setup" || mode == "collision" || mode == "cleanup_failure") {
        require(session.setup_required(), "piwiz image requires setup");
        require(session.setup_available(), "root can configure the image account");
        require(!session.setup_username().empty(), "source account is offered");
        require(!session.login(username, password, false, error), "cannot bypass setup with sign-in");
        if (mode == "collision") {
            require(!session.login(username, password, true, error), "reject occupied account name");
            const passwd *source = getpwnam("pi");
            require(source && source->pw_uid == 1000, "collision preserves original account");
            require(access(state, F_OK) != 0, "collision must not commit active state");
            return 0;
        }
        if (mode == "cleanup_failure") {
            require(!session.login(username, password, true, error), "failed cleanup keeps setup pending");
            require(access(state, F_OK) == 0, "cleanup failure retains committed account");
            require(access("/var/lib/lilygo-ui-launcher/first-boot-pending", F_OK) == 0,
                    "cleanup failure retains retry state");
            require(session.user() == nullptr, "cleanup failure closes authenticated session");
            return 0;
        }
        require(session.login(username, password, true, error), "complete setup", error);
    } else {
        require(mode == "restart", "known test mode");
        require(!session.setup_required(), "completed setup survives restart");
        require(!session.setup_available(), "completed setup cannot run twice");
        require(session.last_user() == username, "restart restores chosen account");
        require(!session.login(username, "fixture-original-password", false, error), "old password rejected");
        require(session.login(username, password, false, error), "restart authenticates new password", error);
    }
    require(session.user() && session.user()->uid == 1000, "preserve original UID 1000");
    require(session.username() == username, "select requested account");
    require(std::string(session.user()->home) == "/home/" + username, "select requested home");
    require(!session.reauthenticate("fixture-wrong-password", error), "PAM rejects wrong password");
    require(session.reauthenticate(password, error), "PAM accepts new password", error);
    session.logout();
    require(session.user() == nullptr, "logout closes identity");
    return 0;
}
