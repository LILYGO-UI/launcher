#include "session.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct TempState {
    char directory[64] = "/tmp/lilygo-simulator-session-XXXXXX";
    std::string path;

    TempState()
    {
        assert(mkdtemp(directory));
        path = std::string(directory) + "/active-user";
    }

    ~TempState()
    {
        unlink(path.c_str());
        rmdir(directory);
    }
};

void fixed_credentials_and_session_lifecycle()
{
    TempState state;
    launcher::UserSession session(state.path);
    std::string error;
    const uid_t host_uid = geteuid();
    const gid_t host_gid = getegid();

    assert(!session.login("pi", "raspberry", false, error));
    assert(!error.empty());
    assert(!session.reauthenticate("raspberry", error));
    assert(!error.empty());
    assert(session.initialize(error));
    assert(session.last_user() == "pi");
    assert(session.username().empty());
    assert(session.user() == nullptr);
    assert(!session.setup_available());

    assert(!session.reauthenticate("raspberry", error));
    assert(!error.empty());
    assert(!session.login("other", "raspberry", false, error));
    assert(!error.empty());
    assert(!session.login("pi", "wrong-password", false, error));
    assert(!error.empty());
    assert(!session.login("pi", "", false, error));
    assert(!error.empty());
    assert(!session.login("pi", "raspberry", true, error));
    assert(!error.empty());
    assert(session.username().empty());
    assert(session.user() == nullptr);

    assert(session.login("pi", "raspberry", false, error));
    assert(error.empty());
    assert(session.username() == "pi");
    assert(session.user() == nullptr);
    assert(geteuid() == host_uid && getegid() == host_gid);

    assert(!session.reauthenticate("wrong-password", error));
    assert(!error.empty());
    assert(session.username() == "pi");
    assert(session.last_user() == "pi");
    assert(session.user() == nullptr);
    assert(session.reauthenticate("raspberry", error));
    assert(error.empty());
    assert(session.username() == "pi");

    session.logout();
    assert(session.username().empty());
    assert(session.last_user() == "pi");
    assert(session.user() == nullptr);
    assert(!session.reauthenticate("raspberry", error));
    assert(!error.empty());

    assert(session.login("pi", "raspberry", false, error));
    assert(error.empty());
    assert(session.username() == "pi");
    assert(session.user() == nullptr);
    session.logout();
    assert(session.username().empty());
    assert(geteuid() == host_uid && getegid() == host_gid);
    assert(access(state.path.c_str(), F_OK) != 0);
}

void device_state_is_ignored_and_preserved()
{
    TempState state;
    const std::string malformed_marker = "root\ninvalid second line\n";
    {
        std::ofstream output(state.path);
        output << malformed_marker;
        assert(output.good());
    }
    assert(chmod(state.path.c_str(), 0600) == 0);
    {
        launcher::UserSession session(state.path);
        std::string error;
        assert(session.initialize(error));
        assert(session.last_user() == "pi");
        assert(!session.setup_available());
        assert(session.login("pi", "raspberry", false, error));
        assert(session.username() == "pi");
        assert(session.user() == nullptr);
        session.logout();
    }
    std::ifstream input(state.path);
    assert(input.good());
    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    assert(contents == malformed_marker);
}

void default_state_path_needs_no_system_account_access()
{
    launcher::UserSession session;
    std::string error;
    assert(session.initialize(error));
    assert(session.last_user() == "pi");
    assert(!session.setup_available());
    assert(session.login("pi", "raspberry", false, error));
    assert(error.empty());
    assert(session.username() == "pi");
    assert(session.user() == nullptr);
}

void full_wizard_demo_has_an_ephemeral_authenticated_account()
{
    TempState state;
    const std::string original_marker = "leave this host file untouched\n";
    {
        std::ofstream output(state.path);
        output << original_marker;
    }
    const uid_t host_uid = geteuid();
    const gid_t host_gid = getegid();
    std::string error;
    {
        launcher::UserSession session(state.path);
        assert(!session.complete_setup(error));
        assert(session.initialize(error));
        assert(!session.setup_account_saved());
        assert(!session.complete_setup(error));
        assert(session.login("wing", "demo-password", true, error, false));
        assert(session.setup_account_saved() && !session.setup_required());
        assert(session.username() == "wing" && session.last_user() == "wing");
        assert(session.user() == nullptr);
        assert(!session.reauthenticate("raspberry", error));
        assert(session.reauthenticate("demo-password", error));
        assert(!session.login("other", "replacement", true, error, false));
        session.logout();
        assert(!session.complete_setup(error));
        assert(!session.login("pi", "raspberry", false, error, false));
        assert(!session.login("wing", "incorrect", false, error, false));
        assert(session.login("wing", "demo-password", false, error, false));
        assert(session.complete_setup(error));
        assert(error.empty() && session.setup_username().empty());
        session.logout();
        assert(session.login("wing", "demo-password", false, error));
        assert(geteuid() == host_uid && getegid() == host_gid);
    }
    // A new simulator process starts with its usual fixed demo credentials.
    launcher::UserSession restarted(state.path);
    assert(restarted.initialize(error));
    assert(!restarted.setup_account_saved());
    assert(!restarted.login("wing", "demo-password", false, error));
    assert(restarted.login("pi", "raspberry", false, error));
    std::ifstream input(state.path);
    const std::string contents{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    assert(contents == original_marker);
}

} // namespace

int main()
{
    fixed_credentials_and_session_lifecycle();
    device_state_is_ignored_and_preserved();
    default_state_path_needs_no_system_account_access();
    full_wizard_demo_has_an_ephemeral_authenticated_account();
    return 0;
}
