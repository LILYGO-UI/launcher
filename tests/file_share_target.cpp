/* Opt-in board integration harness. This is never registered with CTest and
 * never installed in the production package. */
#include "file_share_receiver.h"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <string>
#include <thread>

namespace {
volatile sig_atomic_t stopped = 0;
void stop_handler(int) { stopped = 1; }
}

int main(int argc, char **argv)
{
    const char *username = nullptr;
    std::string agent;
    long duration = 300;
    for (int index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--user") == 0 && index + 1 < argc) username = argv[++index];
        else if (strcmp(argv[index], "--agent") == 0 && index + 1 < argc) agent = argv[++index];
        else if (strcmp(argv[index], "--duration") == 0 && index + 1 < argc) {
            char *end = nullptr;
            duration = strtol(argv[++index], &end, 10);
            if (!end || *end || duration <= 0 || duration > 86400) {
                fprintf(stderr, "--duration must be between 1 and 86400 seconds\n");
                return 2;
            }
        } else {
            fprintf(stderr, "Usage: %s --user USER [--agent /absolute/lilygo-ui-file-share-agent] [--duration SECONDS]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!username) {
        fprintf(stderr, "--user is required\n");
        return 2;
    }
    const passwd *account = getpwnam(username);
    if (!account || !account->pw_dir || !account->pw_shell ||
        strlen(account->pw_dir) >= LAUNCHER_USER_PATH_MAX ||
        strlen(account->pw_name) >= LAUNCHER_USER_NAME_MAX ||
        strlen(account->pw_shell) >= LAUNCHER_USER_SHELL_MAX) {
        fprintf(stderr, "The selected account is unavailable or invalid\n");
        return 2;
    }
    launcher_user_t user{};
    user.uid = account->pw_uid;
    user.gid = account->pw_gid;
    snprintf(user.name, sizeof(user.name), "%s", account->pw_name);
    snprintf(user.home, sizeof(user.home), "%s", account->pw_dir);
    snprintf(user.shell, sizeof(user.shell), "%s", account->pw_shell);
    snprintf(user.runtime_dir, sizeof(user.runtime_dir), "/run/user/%lu", static_cast<unsigned long>(user.uid));
    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    /* Diagnostic stdout may be an SSH pipe whose reader has disconnected.
     * Keep that output failure from terminating the integration run. Receiver
     * socket writes already suppress SIGPIPE per send/socket. */
    signal(SIGPIPE, SIG_IGN);
    launcher::file_share::Receiver receiver(agent);
    std::string error;
    if (!receiver.start(&user, error)) {
        fprintf(stderr, "File sharing failed: %s\n", error.c_str());
        return 1;
    }
    std::string previous;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration);
    while (!stopped && std::chrono::steady_clock::now() < deadline) {
        const std::string status = receiver.status();
        if (status != previous) {
            printf("%s\n", status.c_str());
            fflush(stdout);
            previous = status;
        }
        if (!receiver.active()) return 1;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    receiver.stop();
    return 0;
}
