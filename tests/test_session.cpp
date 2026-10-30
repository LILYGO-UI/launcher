#include "session.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct TempState {
    char directory[64] = "/tmp/lilygo-session-test-XXXXXX";
    std::string path;

    TempState()
    {
        assert(mkdtemp(directory) != nullptr);
        path = std::string(directory) + "/active-user";
    }

    ~TempState()
    {
        unlink(path.c_str());
        rmdir(directory);
    }
};

void host_build_rejects_login_without_creating_accounts()
{
    TempState state;
    launcher::UserSession session(state.path);
    std::string error;
    assert(session.initialize(error));
    assert(session.last_user().empty());
    assert(session.user() == nullptr);
    assert(!session.setup_available());
    assert(!session.login("newuser", "not-a-real-password", true, error));
    assert(!error.empty());
    assert(access(state.path.c_str(), F_OK) != 0);
    session.logout();
    assert(session.user() == nullptr);
}

void validation_happens_before_backend()
{
    TempState state;
    launcher::UserSession session(state.path);
    std::string error;
    assert(session.initialize(error));
    assert(!session.login("../bad", "secret", false, error));
    assert(!session.login("valid_user", "", false, error));
    assert(!session.login("valid_user", std::string(4097, 'x'), false, error));
    assert(access(state.path.c_str(), F_OK) != 0);
}

void host_build_rejects_unlock_without_changing_state()
{
    TempState state;
    launcher::UserSession session(state.path);
    std::string error;
    assert(!session.reauthenticate("secret", error));
    assert(!error.empty());
    assert(session.initialize(error));
    for (const auto &password : {std::string(), std::string("secret"), std::string(4097, 'x'),
                                 std::string("embedded\0zero", 13), std::string("line\nbreak")}) {
        error.clear();
        assert(!session.reauthenticate(password, error));
        assert(!error.empty());
        assert(session.user() == nullptr);
        assert(session.last_user().empty());
        assert(access(state.path.c_str(), F_OK) != 0);
    }
}

void marker_is_loaded_and_malformed_marker_rejected()
{
    TempState state;
    {
        std::ofstream output(state.path);
        output << "existing_user\n";
    }
    chmod(state.path.c_str(), 0600);
    launcher::UserSession session(state.path);
    std::string error;
    assert(session.initialize(error));
    assert(session.last_user() == "existing_user");
    assert(!session.login("existing_user", "secret", true, error));

    std::ofstream malformed(state.path, std::ios::trunc);
    malformed << "root\n";
    malformed.close();
    launcher::UserSession broken(state.path);
    assert(!broken.initialize(error));
}

} // namespace

int main()
{
    host_build_rejects_login_without_creating_accounts();
    validation_happens_before_backend();
    host_build_rejects_unlock_without_changing_state();
    marker_is_loaded_and_malformed_marker_rejected();
    return 0;
}
