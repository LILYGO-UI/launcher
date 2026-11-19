#include "session.h"

#include <cassert>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

int main()
{
    char directory[] = "/tmp/lilygo-local-session-XXXXXX";
    assert(mkdtemp(directory));
    const std::string marker = std::string(directory) + "/active-user";
    // Local mode must ignore distribution-specific first-boot state, including
    // malformed content left by a different image. It never changes accounts.
    const std::string original = "root\nnot a PAM account marker\n";
    { std::ofstream output(marker); output << original; assert(output.good()); }
    assert(chmod(marker.c_str(), 0600) == 0);

    const uid_t uid = getuid();
    const gid_t gid = getgid();
    const passwd *entry = getpwuid(uid);
    assert(entry && entry->pw_name);
    const std::string name = entry->pw_name;
    std::string error;
    launcher::UserSession session(marker);
    assert(session.local_session());
    assert(session.initialize(error));
    assert(error.empty());
    assert(session.username() == name && session.last_user() == name);
    assert(session.user() == nullptr); // Children inherit the caller's UID/GID.
    assert(!session.setup_required() && !session.setup_available() && !session.setup_account_saved());
    assert(!session.login("another_user", "secret", false, error));
    assert(!session.login("another_user", "secret", true, error));
    assert(!session.reauthenticate("secret", error));
    assert(!session.complete_setup(error));
    assert(session.username() == name && session.user() == nullptr);
    assert(getuid() == uid && geteuid() == uid && getgid() == gid && getegid() == gid);

    std::ifstream input(marker);
    const std::string after{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    assert(after == original);
    input.close();
    session.logout();
    assert(session.username().empty() && session.user() == nullptr);
    assert(session.initialize(error));
    assert(session.username() == name);
    assert(unlink(marker.c_str()) == 0);
    assert(rmdir(directory) == 0);

    launcher::UserSession default_path;
    assert(default_path.initialize(error));
    assert(default_path.username() == name && default_path.user() == nullptr);
}
