#include "file_share_receiver.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <limits.h>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

struct Fixture {
    char path[PATH_MAX] = "/tmp/lilygo-receiver-XXXXXX";
    launcher_user_t user{};
    int private_fd = -1;

    Fixture()
    {
        assert(mkdtemp(path));
        char resolved[PATH_MAX];
        assert(realpath(path, resolved));
        snprintf(path, sizeof(path), "%s", resolved);
        user.uid = getuid();
        user.gid = getgid();
        if (user.uid == 0) {
            const passwd *account = getpwnam("nobody");
            assert(account && account->pw_uid != 0 && account->pw_gid != 0);
            user.uid = account->pw_uid;
            user.gid = account->pw_gid;
            assert(chown(path, user.uid, user.gid) == 0);
        }
        snprintf(user.home, sizeof(user.home), "%s", path);
        snprintf(user.name, sizeof(user.name), "receiver-test");
        const int input = open("/dev/null", O_RDONLY);
        assert(input >= 0);
        private_fd = fcntl(input, F_DUPFD, 100);
        close(input);
        assert(private_fd >= 100);
        assert(setenv("LILYGO_TEST_PRIVATE_FD", std::to_string(private_fd).c_str(), 1) == 0);
    }

    ~Fixture()
    {
        close(private_fd);
        const std::string command = "rm -rf -- '" + std::string(path) + "'";
        assert(system(command.c_str()) == 0);
    }

    std::string downloads() const { return std::string(path) + "/Downloads"; }
};

std::string contents(const std::string &path)
{
    std::ifstream file(path, std::ios::binary);
    assert(file);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

void wait_done(launcher::file_share::Receiver &receiver)
{
    for (unsigned attempt = 0; attempt < 500 && receiver.active(); ++attempt) {
        if (const auto pending = receiver.pending_approval()) {
            assert(receiver.resolve_approval(pending->id, true));
            assert(!receiver.resolve_approval(pending->id, true));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(!receiver.active());
}

void assert_empty_directory(const std::string &path)
{
    DIR *directory = opendir(path.c_str());
    assert(directory);
    while (dirent *entry = readdir(directory))
        assert(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0);
    closedir(directory);
}

void run_scenario(const char *helper, const char *scenario)
{
    Fixture fixture;
    assert(setenv("LILYGO_TEST_SHARE_SCENARIO", scenario, 1) == 0);
    launcher::file_share::Receiver receiver(helper);
    std::string error;
    assert(receiver.start(&fixture.user, error));
    assert(error.empty());
    wait_done(receiver);
    const std::string status = receiver.status();
    receiver.stop();
    if (strcmp(scenario, "success") == 0) {
        assert(status.find("Saved empty.txt to Downloads.") == 0);
        assert(contents(fixture.downloads() + "/hello.txt") == "hello world");
        assert(contents(fixture.downloads() + "/hello (1).txt") == "hello world");
        assert(contents(fixture.downloads() + "/empty.txt").empty());
        struct stat metadata{};
        assert(stat((fixture.downloads() + "/hello.txt").c_str(), &metadata) == 0);
        assert(metadata.st_uid == fixture.user.uid && (metadata.st_mode & 0777) == 0600);
    } else {
        if (strcmp(scenario, "rejected") == 0)
            assert(status.find("Transfer failed: downloaded file SHA-256 does not match") == 0);
        if (strcmp(scenario, "status-close") == 0) assert(status.find("File sharing closed.") == 0);
        if (strcmp(scenario, "interrupted") == 0) assert(status.find("incomplete file was removed") != std::string::npos);
        if (strstr(scenario, "frame")) assert(status.find("invalid frame") != std::string::npos);
        assert_empty_directory(fixture.downloads());
    }
}

void test_stop(const char *helper)
{
    Fixture fixture;
    assert(setenv("LILYGO_TEST_SHARE_SCENARIO", "stop", 1) == 0);
    launcher::file_share::Receiver receiver(helper);
    std::string error;
    assert(receiver.start(&fixture.user, error));
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
        if (const auto pending = receiver.pending_approval())
            assert(receiver.resolve_approval(pending->id, true));
        if (receiver.status().find("Ready for stop test") != std::string::npos) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const std::string status = receiver.status();
    assert(status.find("Ready for stop test") != std::string::npos);
    const pid_t helper_pid = static_cast<pid_t>(std::strtol(status.substr(status.rfind(": ") + 2).c_str(), nullptr, 10));
    assert(helper_pid > 0);
    const auto started = std::chrono::steady_clock::now();
    receiver.stop();
    assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
    assert(!receiver.active());
    int child_status = 0;
    assert(waitpid(helper_pid, &child_status, WNOHANG) == -1 && errno == ECHILD);
    assert_empty_directory(fixture.downloads());
    receiver.stop();
}

void test_approval(const char *helper, bool accepted)
{
    Fixture fixture;
    assert(setenv("LILYGO_TEST_SHARE_SCENARIO", "approval", 1) == 0);
    if (accepted) assert(setenv("LILYGO_TEST_APPROVAL_ACCEPT", "1", 1) == 0);
    else unsetenv("LILYGO_TEST_APPROVAL_ACCEPT");
    launcher::file_share::Receiver receiver(helper);
    std::string error;
    assert(receiver.start(&fixture.user, error));
    std::optional<launcher::file_share::PendingApproval> pending;
    for (unsigned attempt = 0; attempt < 500 && !pending; ++attempt) {
        pending = receiver.pending_approval();
        if (!pending) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(pending && pending->filename == "approval.txt" && pending->size == 11);
    assert(!receiver.resolve_approval(pending->id + 1, true));
    assert(receiver.resolve_approval(pending->id, accepted));
    assert(!receiver.resolve_approval(pending->id, true));
    wait_done(receiver);
    receiver.stop();
    if (accepted) {
        assert(contents(fixture.downloads() + "/approval.txt") == "hello world");
    } else {
        assert_empty_directory(fixture.downloads());
        assert(receiver.status().find("not approved") != std::string::npos);
    }
}

void test_stop_pending_approval(const char *helper)
{
    Fixture fixture;
    assert(setenv("LILYGO_TEST_SHARE_SCENARIO", "approval-stop", 1) == 0);
    launcher::file_share::Receiver receiver(helper);
    std::string error;
    std::uint64_t previous_id = 0;
    for (unsigned round = 0; round < 2; ++round) {
        assert(receiver.start(&fixture.user, error));
        std::optional<launcher::file_share::PendingApproval> pending;
        for (unsigned attempt = 0; attempt < 500 && !pending; ++attempt) {
            pending = receiver.pending_approval();
            if (!pending) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(pending && pending->id > previous_id);
        if (previous_id) assert(!receiver.resolve_approval(previous_id, true));
        previous_id = pending->id;
        const auto started = std::chrono::steady_clock::now();
        receiver.stop();
        assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));
        assert(!receiver.pending_approval());
        assert(!receiver.resolve_approval(previous_id, true));
        assert_empty_directory(fixture.downloads());
    }
}

void test_approval_timeout(const char *helper)
{
    Fixture fixture;
    assert(setenv("LILYGO_TEST_SHARE_SCENARIO", "approval-timeout", 1) == 0);
    launcher::file_share::Receiver receiver(helper);
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    assert(receiver.start(&fixture.user, error));
    std::uint64_t id = 0;
    for (unsigned attempt = 0; attempt < 5000 && receiver.active(); ++attempt) {
        if (const auto pending = receiver.pending_approval()) id = pending->id;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    assert(!receiver.active() && id != 0);
    assert(std::chrono::steady_clock::now() - started >= std::chrono::seconds(44));
    assert(receiver.status().find("File receiving confirmation timed out.") == 0);
    assert(!receiver.pending_approval() && !receiver.resolve_approval(id, true));
    receiver.stop();
    assert_empty_directory(fixture.downloads());
}

} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2);
    for (const char *scenario : {"success", "rejected", "status-close", "interrupted", "zero-frame", "large-frame", "approval-binding"})
        run_scenario(argv[1], scenario);
    test_stop(argv[1]);
    test_approval(argv[1], true);
    test_approval(argv[1], false);
    test_stop_pending_approval(argv[1]);
    test_approval_timeout(argv[1]);
    Fixture fixture;
    launcher::file_share::Receiver unavailable("/nonexistent/lilygo-ui-file-share-agent");
    std::string error;
    assert(!unavailable.start(&fixture.user, error) && !error.empty());
    assert(!unavailable.start(nullptr, error) && !error.empty());
    return 0;
}
