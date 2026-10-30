#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <filesystem>
#include <grp.h>
#include <pwd.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

// Every account and helper is simulated within a temporary root. No host
// account command, PAM transaction, or privileged configuration file is used.
namespace {
std::string test_home;
uid_t test_euid = 1000;
bool account_helpers_enabled = false;
int helper_accesses = 0;
std::vector<std::string> available_groups;
std::string failed_helper;
bool unsafe_home = false;
bool hidden_account = false;
bool collision_group = false;
bool fail_home_move_after_commit = false;
bool wrong_original_home_owner = false;

struct Account { std::string name; std::string home; std::string group; std::string groups; std::string shell = "/bin/sh"; };

std::string read_text(const std::string &path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_text(const std::string &path, const std::string &value, mode_t mode = 0600)
{
    std::ofstream out(path);
    out << value;
    out.close();
    assert(out && chmod(path.c_str(), mode) == 0);
}

Account load_account()
{
    Account result;
    std::ifstream input(test_home + "/account");
    std::getline(input, result.name);
    std::getline(input, result.home);
    std::getline(input, result.group);
    std::getline(input, result.groups);
    if (!std::getline(input, result.shell)) result.shell = "/bin/sh";
    return result;
}

void save_account(const Account &account)
{
    write_text(test_home + "/account", account.name + "\n" + account.home + "\n" + account.group + "\n" + account.groups + "\n" + account.shell + "\n");
}

passwd *test_getpwuid(uid_t uid)
{
    if (uid != 1000 || hidden_account) return nullptr;
    static Account current;
    current = load_account();
    if (current.name.empty()) return nullptr;
    static passwd account{};
    account.pw_name = current.name.data();
    account.pw_uid = 1000;
    account.pw_gid = 1000;
    account.pw_dir = current.home.data();
    account.pw_shell = current.shell.data();
    return &account;
}

passwd *test_getpwnam(const char *name)
{
    if (std::strcmp(name, "taken") == 0) {
        static passwd collision{};
        collision.pw_uid = 1001;
        return &collision;
    }
    passwd *account = test_getpwuid(1000);
    return account && std::strcmp(account->pw_name, name) == 0 ? account : nullptr;
}

group *test_getgrgid(gid_t gid)
{
    if (gid != 1000) return nullptr;
    static std::string name;
    name = load_account().group;
    static group result{};
    result.gr_name = name.data();
    result.gr_gid = 1000;
    return name.empty() ? nullptr : &result;
}

group *test_getgrnam(const char *name)
{
    if (load_account().group == name) return test_getgrgid(1000);
    if ((collision_group && std::strcmp(name, "wing") == 0) ||
        std::find(available_groups.begin(), available_groups.end(), name) != available_groups.end()) {
        static group result{};
        result.gr_gid = 2000;
        return &result;
    }
    return nullptr;
}

bool helper_path(const char *path)
{
    return std::strncmp(path, "/usr/sbin/", 10) == 0 ||
           std::strcmp(path, "/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot") == 0;
}

int test_access(const char *path, int mode)
{
    if (!helper_path(path)) return access(path, mode);
    assert(account_helpers_enabled && "unexpected host helper access");
    ++helper_accesses;
    assert(mode == X_OK);
    return 0;
}

pid_t test_fork()
{
    assert(account_helpers_enabled && "unexpected host account operation");
    return fork();
}

int test_execv(const char *path, char *const arguments[])
{
    assert(account_helpers_enabled && helper_path(path));
    std::vector<std::string> args;
    std::ofstream output(test_home + "/helper-calls", std::ios::app);
    assert(output);
    for (size_t i = 0; arguments[i]; ++i) {
        args.emplace_back(arguments[i]);
        output << arguments[i] << '\n';
    }
    output.close();
    if (failed_helper == path) _exit(1);
    auto account = load_account();
    if (args[0] == "/usr/sbin/useradd") {
        account = {args.back(), test_home + "/home/" + args.back(), args.back(), args[5]};
        assert(mkdir(account.home.c_str(), 0700) == 0 || errno == EEXIST);
        save_account(account);
    } else if (args[0] == "/usr/sbin/chpasswd") {
        std::string input;
        char buffer[128];
        ssize_t length;
        while ((length = read(STDIN_FILENO, buffer, sizeof(buffer))) > 0)
            input.append(buffer, static_cast<size_t>(length));
        assert(length == 0 && input == account.name + ":correct-password\n");
        std::ofstream accepted(test_home + "/helper-calls", std::ios::app);
        accepted << "password accepted\n";
        accepted.close();
    } else if (args[0] == "/usr/sbin/usermod") {
        assert(args.back() == account.name);
        for (size_t i = 1; i + 1 < args.size(); ++i) {
            if (args[i] == "--login") account.name = args[++i];
            else if (args[i] == "--shell") account.shell = args[++i];
            else if (args[i] == "--home") {
                const auto home = args[++i];
                if (std::find(args.begin(), args.end(), "--move-home") != args.end()) {
                    if (fail_home_move_after_commit) {
                        account.home = home;
                        save_account(account);
                        _exit(1);
                    }
                    assert(rename(account.home.c_str(), home.c_str()) == 0);
                }
                account.home = home;
            } else if (args[i] == "--groups") account.groups += "," + args[++i];
        }
        save_account(account);
    } else if (args[0] == "/usr/sbin/groupmod") {
        assert(args[1] == "--new-name" && args[3] == account.group);
        account.group = args[2];
        save_account(account);
    } else if (args[0] == "/usr/sbin/userdel") {
        assert(args.back() == account.name);
        assert(rmdir(account.home.c_str()) == 0);
        assert(unlink((test_home + "/account").c_str()) == 0);
    } else {
        assert(args.size() == 2 && args[1] == "complete");
        assert(read_text(test_home + "/active-user") == account.name + "\n");
        assert(unlink((test_home + "/first-boot-pending").c_str()) == 0);
    }
    _exit(0);
}

int test_stat(const char *path, struct stat *status)
{
    if (std::strcmp(path, "/run/user/1000") == 0) {
        std::memset(status, 0, sizeof(*status));
        status->st_mode = S_IFDIR | 0700;
        status->st_uid = 1000;
        return 0;
    }
    const int result = stat(path, status);
    if (result == 0) {
        status->st_uid = load_account().home == path ? 1000 : 0;
        if (unsafe_home && load_account().home == path) status->st_mode |= S_IWOTH;
    }
    return result;
}

int test_lstat(const char *path, struct stat *status)
{
    const int result = lstat(path, status);
    if (result == 0) {
        status->st_uid = std::string(path).find(test_home + "/home/") == 0 ? 1000 : 0;
        if (wrong_original_home_owner && test_home + "/home/pi" == path) status->st_uid = 1001;
    }
    return result;
}

int test_fstat(int fd, struct stat *status)
{
    const int result = fstat(fd, status);
    if (result == 0) status->st_uid = 0;
    return result;
}

int test_fchown(int, uid_t uid, gid_t) { assert(uid == 0); return 0; }

int test_pipe2(int *fds, int flags)
{
    assert(account_helpers_enabled);
    assert(flags == O_CLOEXEC);
    if (pipe(fds) != 0) return -1;
    assert(fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0);
    assert(fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0);
    return 0;
}
} // namespace

#define LAUNCHER_SYSTEM_SESSION 1
#define getpwnam(name) test_getpwnam(name)
#define getpwuid(uid) test_getpwuid(uid)
#define getgrnam(name) test_getgrnam(name)
#define getgrgid(gid) test_getgrgid(gid)
#define geteuid() test_euid
#define access(path, mode) test_access(path, mode)
#define fork() test_fork()
#define execv(path, arguments) test_execv(path, arguments)
#define stat(path, status) test_stat(path, status)
#define lstat(path, status) test_lstat(path, status)
#define fstat(fd, status) test_fstat(fd, status)
#define fchown(fd, uid, gid) test_fchown(fd, uid, gid)
#define pipe2(fds, flags) test_pipe2(fds, flags)
#include "../src/session.cpp"
#undef pipe2
#undef fchown
#undef fstat
#undef lstat
#undef stat
#undef execv
#undef fork
#undef access
#undef geteuid
#undef getgrgid
#undef getgrnam
#undef getpwuid
#undef getpwnam

struct pam_handle {
    pam_conv conversation;
};

namespace {
enum class Failure { None, Start, StartAllocated, Tty, Authenticate, Account };
Failure failure = Failure::None;
pam_handle_t *active_session = nullptr;
int starts = 0;
int ends = 0;
int opens = 0;
int closes = 0;
int credentials = 0;
int account_checks = 0;
std::vector<std::string> authenticated_users;

void test_account_creation()
{
    account_helpers_enabled = true;
    test_euid = 0;
    const std::string helper_log = test_home + "/helper-calls";
    for (const auto &groups : {std::vector<std::string>{"sudo", "video", "render", "input"},
                              std::vector<std::string>{"sudo"},
                              std::vector<std::string>{"sudo", "input"}}) {
        available_groups = groups;
        helper_accesses = 0;
        std::string error;
        assert(launcher::create_account("session_test", "correct-password", error));
        assert(error.empty() && helper_accesses == 2);
        std::string expected_groups;
        for (const auto &group_name : groups) {
            if (!expected_groups.empty()) expected_groups += ',';
            expected_groups += group_name;
        }
        std::ifstream input(helper_log);
        const std::string calls((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        assert(calls == "/usr/sbin/useradd\n--create-home\n--shell\n/bin/bash\n--groups\n" +
                        expected_groups + "\nsession_test\n/usr/sbin/chpasswd\npassword accepted\n");
        assert(unlink(helper_log.c_str()) == 0);
    }

    // A broken image must fail before useradd can leave an unprivileged first
    // account behind. Hardware groups cannot substitute for the sudo group.
    available_groups = {"video", "render", "input"};
    helper_accesses = 0;
    std::string error;
    assert(!launcher::create_account("session_test", "correct-password", error));
    assert(error.find("sudo") != std::string::npos && helper_accesses == 0);
    assert(access(helper_log.c_str(), F_OK) == -1 && errno == ENOENT);

    available_groups = {"sudo", "video", "render", "input"};
    test_euid = 1000;
    error.clear();
    assert(!launcher::create_account("session_test", "correct-password", error));
    assert(error.find("root") != std::string::npos && helper_accesses == 0);
    assert(access(helper_log.c_str(), F_OK) == -1 && errno == ENOENT);
    account_helpers_enabled = false;
}
} // namespace

extern "C" int pam_start(const char *service, const char *username, const pam_conv *conversation, pam_handle_t **out)
{
    assert(std::strcmp(service, "lilygo-ui-launcher") == 0);
    assert(load_account().name == username);
    authenticated_users.emplace_back(username);
    ++starts;
    *out = failure == Failure::Start ? nullptr : new pam_handle{*conversation};
    return failure == Failure::Start || failure == Failure::StartAllocated ? PAM_BUF_ERR : PAM_SUCCESS;
}

extern "C" int pam_end(pam_handle_t *handle, int)
{
    assert(handle && handle != active_session);
    ++ends;
    delete handle;
    return PAM_SUCCESS;
}

extern "C" int pam_set_item(pam_handle_t *, int item, const void *value)
{
    assert(item == PAM_TTY && std::strcmp(static_cast<const char *>(value), "tty1") == 0);
    return failure == Failure::Tty ? PAM_BUF_ERR : PAM_SUCCESS;
}

extern "C" int pam_authenticate(pam_handle_t *handle, int)
{
    const pam_message prompt{PAM_PROMPT_ECHO_OFF, "Password:"};
    const pam_message *messages[] = {&prompt};
    pam_response *answers = nullptr;
    assert(handle->conversation.conv(1, messages, &answers, handle->conversation.appdata_ptr) == PAM_SUCCESS);
    assert(answers && answers[0].resp);
    const bool accepted = std::strcmp(answers[0].resp, "correct-password") == 0;
    std::free(answers[0].resp);
    std::free(answers);
    return accepted && failure != Failure::Authenticate ? PAM_SUCCESS : PAM_CONV_ERR;
}

extern "C" int pam_acct_mgmt(pam_handle_t *, int)
{
    ++account_checks;
    return failure == Failure::Account ? PAM_CONV_ERR : PAM_SUCCESS;
}

extern "C" int pam_setcred(pam_handle_t *, int)
{
    ++credentials;
    return PAM_SUCCESS;
}

extern "C" int pam_open_session(pam_handle_t *handle, int)
{
    assert(!active_session);
    active_session = handle;
    ++opens;
    return PAM_SUCCESS;
}

extern "C" int pam_close_session(pam_handle_t *handle, int)
{
    assert(active_session == handle);
    active_session = nullptr;
    ++closes;
    return PAM_SUCCESS;
}

namespace {
void reset_fixture(const std::string &name = "pi", bool pending = true)
{
    assert(!active_session);
    for (const char *directory : {"/home", "/etc"}) {
        std::filesystem::remove_all(test_home + directory);
    }
    for (const char *file : {"/account", "/active-user", "/first-boot-pending", "/helper-calls"})
        std::filesystem::remove_all(test_home + file);
    std::filesystem::create_directories(test_home + "/home");
    std::filesystem::create_directories(test_home + "/etc/sudoers.d");
    if (!name.empty()) {
        const auto home = test_home + "/home/" + name;
        assert(mkdir(home.c_str(), 0700) == 0);
        save_account({name, home, name, "audio,spi,video"});
    }
    if (pending) write_text(test_home + "/first-boot-pending", "1000:" + name + "\n");
    available_groups = {"sudo", "video", "render", "input"};
    test_euid = 0;
    account_helpers_enabled = true;
    failed_helper.clear();
    unsafe_home = false;
    hidden_account = false;
    collision_group = false;
    fail_home_move_after_commit = false;
    wrong_original_home_owner = false;
    failure = Failure::None;
}

void test_configured_image_login()
{
    reset_fixture("pi", false);
    std::string error;
    launcher::UserSession session(test_home + "/active-user", test_home);
    assert(session.initialize(error));
    assert(session.last_user() == "pi" && !session.setup_required() && !session.setup_available());
    assert(!session.login("newname", "correct-password", true, error));
    assert(session.login("pi", "correct-password", false, error));
    assert(session.user()->uid == 1000);
    assert(read_text(test_home + "/active-user") == "pi\n");
    assert(read_text(test_home + "/helper-calls").empty());
}

void test_pending_keep_name_and_rename()
{
    for (const std::string target : {"pi", "wing"}) {
        reset_fixture();
        auto original = load_account();
        original.shell = "/usr/sbin/nologin";
        save_account(original);
        write_text(test_home + "/home/pi/keep-me", "preserved data");
        write_text(test_home + "/etc/subuid", "pi:100000:65536\npilot:200000:65536\n", 0640);
        write_text(test_home + "/etc/subgid", "pi:100000:65536\n", 0644);
        write_text(test_home + "/etc/sudoers.d/010_pi-nopasswd", "# pi policy\npi ALL=(ALL) NOPASSWD: ALL\n", 0440);
        std::string error;
        {
            launcher::UserSession session(test_home + "/active-user", test_home);
            assert(session.initialize(error) && session.setup_required() && session.setup_available());
            assert(session.setup_username() == "pi");
            assert(!session.login("pi", "correct-password", false, error));
            assert(session.login(target, "correct-password", true, error));
            assert(error.empty() && !session.setup_required() && !session.setup_available());
            assert(session.user()->uid == 1000 && session.user()->gid == 1000);
            assert(session.username() == target && session.last_user() == target);
            assert(read_text(test_home + "/active-user") == target + "\n");
            assert(access((test_home + "/first-boot-pending").c_str(), F_OK) != 0);
            const auto account = load_account();
            assert(account.name == target && account.group == target && account.shell == "/bin/bash");
            assert(account.groups == "audio,spi,video,sudo");
            assert(read_text(account.home + "/keep-me") == "preserved data");
            const auto calls = read_text(test_home + "/helper-calls");
            assert(calls.find("/usr/sbin/useradd") == std::string::npos);
            assert(calls.find("/usr/sbin/userdel") == std::string::npos);
            assert(calls.find("correct-password") == std::string::npos);
            if (target == "pi") assert(calls.find("--login") == std::string::npos);
        }
        assert(read_text(test_home + "/etc/subuid") == target + ":100000:65536\npilot:200000:65536\n");
        assert(read_text(test_home + "/etc/subgid") == target + ":100000:65536\n");
        assert(read_text(test_home + "/etc/sudoers.d/010_pi-nopasswd") == "# pi policy\n" + target + " ALL=(ALL) NOPASSWD: ALL\n");
        struct stat mode{};
        assert(stat((test_home + "/etc/subuid").c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0640);
        assert(stat((test_home + "/etc/sudoers.d/010_pi-nopasswd").c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0440);
        const auto calls = read_text(test_home + "/helper-calls");
        launcher::UserSession restarted(test_home + "/active-user", test_home);
        assert(restarted.initialize(error) && !restarted.setup_required());
        assert(restarted.last_user() == target);
        assert(restarted.login(target, "correct-password", false, error));
        assert(read_text(test_home + "/helper-calls") == calls);
    }
}

void test_pending_collision_preflight()
{
    for (int collision = 0; collision < 4; ++collision) {
        reset_fixture();
        if (collision == 1) assert(mkdir((test_home + "/home/wing").c_str(), 0700) == 0);
        if (collision == 2) collision_group = true;
        if (collision == 3) write_text(test_home + "/etc/subuid", "pi:100000:65536\n", 0666);
        launcher::UserSession session(test_home + "/active-user", test_home);
        std::string error;
        assert(session.initialize(error));
        assert(!session.login(collision == 0 ? "taken" : "wing", "correct-password", true, error));
        assert(!error.empty() && load_account().name == "pi");
        assert(read_text(test_home + "/helper-calls").empty());
        assert(access((test_home + "/active-user").c_str(), F_OK) != 0);
    }
}

void test_pending_retries()
{
    // A failure after the login/home rename must resolve the same UID on the
    // next process start. A second rename is blocked until setup is complete.
    {
        const std::string target = "wing";
        reset_fixture();
        write_text(test_home + "/home/pi/keep-me", "still here");
        write_text(test_home + "/etc/subuid", "pi:100000:65536\n");
        failed_helper = "/usr/sbin/groupmod";
        {
            launcher::UserSession session(test_home + "/active-user", test_home);
            std::string error;
            assert(session.initialize(error));
            assert(!session.login("wing", "correct-password", true, error));
            assert(session.setup_username() == "wing");
            assert(load_account().name == "wing" && load_account().group == "pi");
            assert(access((test_home + "/active-user").c_str(), F_OK) != 0);
        }
        failed_helper.clear();
        launcher::UserSession retry(test_home + "/active-user", test_home);
        std::string error;
        assert(retry.initialize(error) && retry.setup_username() == "wing");
        for (const std::string other : {"pi", "bird"}) {
            assert(!retry.login(other, "correct-password", true, error));
            assert(error.find("finish setup as wing") != std::string::npos);
            assert(load_account().name == "wing");
        }
        assert(retry.login(target, "correct-password", true, error));
        assert(load_account().name == target && load_account().group == target);
        assert(load_account().home == test_home + "/home/" + target);
        assert(read_text(load_account().home + "/keep-me") == "still here");
        assert(read_text(test_home + "/etc/subuid") == target + ":100000:65536\n");
        assert(read_text(test_home + "/helper-calls").find("userdel") == std::string::npos);
    }

    for (int point = 0; point < 3; ++point) {
        reset_fixture();
        std::string error;
        launcher::UserSession session(test_home + "/active-user", test_home);
        assert(session.initialize(error));
        if (point == 0) failed_helper = "/usr/sbin/chpasswd";
        if (point == 1) failure = Failure::Authenticate;
        if (point == 2) assert(mkdir((test_home + "/active-user").c_str(), 0700) == 0);
        assert(!session.login("wing", "correct-password", true, error));
        assert(load_account().name == "wing" && session.user() == nullptr);
        assert(read_text(test_home + "/helper-calls").find("userdel") == std::string::npos);
        failed_helper.clear();
        failure = Failure::None;
        if (point == 2) assert(rmdir((test_home + "/active-user").c_str()) == 0);
        assert(session.login("wing", "correct-password", true, error));
    }
}

void test_interrupted_home_move()
{
    reset_fixture();
    write_text(test_home + "/home/pi/keep-me", "survived interrupted move");
    fail_home_move_after_commit = true;
    {
        launcher::UserSession session(test_home + "/active-user", test_home);
        std::string error;
        assert(session.initialize(error));
        assert(!session.login("wing", "correct-password", true, error));
        assert(load_account().name == "wing" && load_account().home == test_home + "/home/wing");
        assert(access((test_home + "/home/wing").c_str(), F_OK) != 0);
        assert(read_text(test_home + "/home/pi/keep-me") == "survived interrupted move");
    }
    fail_home_move_after_commit = false;
    launcher::UserSession retry(test_home + "/active-user", test_home);
    std::string error;
    assert(retry.initialize(error) && retry.setup_username() == "wing");
    wrong_original_home_owner = true;
    const auto calls = read_text(test_home + "/helper-calls");
    assert(!retry.login("wing", "correct-password", true, error));
    assert(read_text(test_home + "/helper-calls") == calls);
    wrong_original_home_owner = false;
    assert(retry.login("wing", "correct-password", true, error));
    assert(read_text(test_home + "/home/wing/keep-me") == "survived interrupted move");
    assert(access((test_home + "/home/pi").c_str(), F_OK) != 0);
}

void test_cleanup_retry_after_commit()
{
    reset_fixture();
    failed_helper = "/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot";
    {
        launcher::UserSession session(test_home + "/active-user", test_home);
        std::string error;
        assert(session.initialize(error));
        assert(!session.login("wing", "correct-password", true, error));
        assert(error.find("account saved") != std::string::npos);
        assert(session.user() == nullptr && session.setup_required());
        assert(read_text(test_home + "/active-user") == "wing\n");
        assert(access((test_home + "/first-boot-pending").c_str(), F_OK) == 0);
    }
    failed_helper.clear();
    launcher::UserSession retry(test_home + "/active-user", test_home);
    std::string error;
    assert(retry.initialize(error) && retry.setup_username() == "wing");
    const auto calls = read_text(test_home + "/helper-calls");
    assert(!retry.login("pi", "correct-password", true, error));
    assert(!retry.login("wing", "wrong-password", true, error));
    assert(read_text(test_home + "/helper-calls") == calls);
    assert(retry.login("wing", "correct-password", true, error));
    assert(read_text(test_home + "/helper-calls") == calls +
           "/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot\ncomplete\n");
}

void test_full_wizard_defers_cleanup_and_resumes_authenticated_account()
{
    reset_fixture();
    std::string error;
    const std::string complete_call = "/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot\ncomplete\n";
    {
        launcher::UserSession session(test_home + "/active-user", test_home);
        assert(!session.complete_setup(error));
        assert(session.initialize(error));
        assert(!session.setup_account_saved());
        assert(!session.complete_setup(error));
        // Deferred cleanup does not let an unconfigured account bypass setup.
        assert(!session.login("pi", "correct-password", false, error, false));
        assert(session.login("wing", "correct-password", true, error, false));
        assert(error.empty() && session.setup_account_saved() && !session.setup_required());
        assert(session.username() == "wing" && session.user()->uid == 1000);
        assert(read_text(test_home + "/active-user") == "wing\n");
        assert(access((test_home + "/first-boot-pending").c_str(), F_OK) == 0);
        assert(read_text(test_home + "/helper-calls").find(complete_call) == std::string::npos);
        session.logout();
        assert(!session.complete_setup(error));
    }

    const auto account_calls = read_text(test_home + "/helper-calls");
    launcher::UserSession resumed(test_home + "/active-user", test_home);
    assert(resumed.initialize(error));
    assert(resumed.setup_required() && resumed.setup_account_saved());
    assert(resumed.setup_username() == "wing");
    assert(!resumed.complete_setup(error));
    assert(!resumed.login("wing", "correct-password", false, error));
    assert(!resumed.login("pi", "correct-password", false, error, false));
    assert(!resumed.login("wing", "wrong-password", false, error, false));
    assert(read_text(test_home + "/helper-calls") == account_calls);
    assert(resumed.login("wing", "correct-password", false, error, false));
    assert(!resumed.setup_required() && resumed.setup_account_saved());
    assert(resumed.reauthenticate("correct-password", error));
    assert(read_text(test_home + "/helper-calls") == account_calls);
    // Retrying Finish must retain the authenticated session and account choice.
    failed_helper = "/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot";
    assert(!resumed.complete_setup(error));
    assert(resumed.username() == "wing" && resumed.user()->uid == 1000);
    assert(access((test_home + "/first-boot-pending").c_str(), F_OK) == 0);
    failed_helper.clear();
    assert(resumed.complete_setup(error));
    assert(error.empty() && !resumed.setup_required() && resumed.setup_account_saved());
    assert(resumed.setup_username().empty());
    assert(access((test_home + "/first-boot-pending").c_str(), F_OK) != 0);
    assert(read_text(test_home + "/helper-calls") == account_calls + complete_call + complete_call);
    assert(resumed.complete_setup(error));
    assert(read_text(test_home + "/helper-calls") == account_calls + complete_call + complete_call);
}

void test_pending_state_validation()
{
    for (const std::string invalid : {"0:pi\n", "999:pi\n", "65534:pi\n", "-1:pi\n", "1000:root\n", "1000:pi\n1001:other\n", "4294967296:pi\n"}) {
        reset_fixture();
        write_text(test_home + "/first-boot-pending", invalid);
        launcher::UserSession session(test_home + "/active-user", test_home);
        std::string error;
        assert(!session.initialize(error) && !error.empty());
    }
    reset_fixture();
    assert(chmod((test_home + "/first-boot-pending").c_str(), 0666) == 0);
    launcher::UserSession writable(test_home + "/active-user", test_home);
    std::string error;
    assert(!writable.initialize(error));
    reset_fixture();
    assert(unlink((test_home + "/first-boot-pending").c_str()) == 0);
    write_text(test_home + "/untrusted", "1000:pi\n");
    assert(symlink((test_home + "/untrusted").c_str(), (test_home + "/first-boot-pending").c_str()) == 0);
    launcher::UserSession symlinked(test_home + "/active-user", test_home);
    assert(!symlinked.initialize(error));
    assert(unlink((test_home + "/untrusted").c_str()) == 0);
    reset_fixture();
    write_text(test_home + "/active-user", "someone_else\n");
    launcher::UserSession mismatch(test_home + "/active-user", test_home);
    assert(!mismatch.initialize(error));
}

void test_fresh_creation_rollback()
{
    for (bool bad_home : {false, true}) {
        reset_fixture("", false);
        launcher::UserSession session(test_home + "/active-user", test_home);
        std::string error;
        assert(session.initialize(error) && session.setup_required());
        if (bad_home) unsafe_home = true;
        else hidden_account = true;
        assert(!session.login("newuser", "correct-password", true, error));
        assert(load_account().name.empty());
        assert(read_text(test_home + "/helper-calls").find("/usr/sbin/userdel") != std::string::npos);
        assert(access((test_home + "/active-user").c_str(), F_OK) != 0);
        unsafe_home = false;
        hidden_account = false;
        assert(session.login("newuser", "correct-password", true, error));
        assert(session.user()->uid == 1000 && !session.setup_required());
    }
}
} // namespace

int main()
{
    char directory[] = "/tmp/lilygo-session-pam-XXXXXX";
    assert(mkdtemp(directory));
    test_home = directory;
    std::filesystem::create_directories(test_home + "/home");
    std::filesystem::create_directories(test_home + "/etc/sudoers.d");
    test_account_creation();
    const std::string marker = test_home + "/active-user";
    {
        std::ofstream output(marker);
        output << "session_test\n";
    }
    assert(chmod(marker.c_str(), 0600) == 0);
    {
        launcher::UserSession session(marker);
        std::string error;
        assert(!session.reauthenticate("correct-password", error));
        assert(session.initialize(error));
        assert(!session.reauthenticate("correct-password", error));
        assert(starts == 0);
        assert(session.login("session_test", "correct-password", false, error));
        assert(opens == 1 && closes == 0 && credentials == 1);
        const auto identity = *session.user();
        const auto original_session = active_session;

        for (const auto &password : {std::string(), std::string(4097, 'x'),
                                     std::string("embedded\0zero", 13), std::string("line\nbreak")}) {
            assert(!session.reauthenticate(password, error));
            assert(!error.empty() && starts == 1);
        }
        const int original_account_checks = account_checks;
        assert(!session.reauthenticate("wrong-password", error));
        assert(!error.empty() && account_checks == original_account_checks);

        for (const auto point : {Failure::Start, Failure::StartAllocated, Failure::Tty,
                                 Failure::Authenticate, Failure::Account, Failure::None, Failure::None}) {
            failure = point;
            const int previous_starts = starts;
            const int previous_ends = ends;
            const bool accepted = session.reauthenticate("correct-password", error);
            assert(accepted == (point == Failure::None));
            assert(error.empty() == accepted);
            assert(starts == previous_starts + 1);
            assert(ends == previous_ends + (point == Failure::Start ? 0 : 1));
            assert(active_session == original_session);
            assert(opens == 1 && closes == 0 && credentials == 1);
            assert(std::memcmp(session.user(), &identity, sizeof(identity)) == 0);
            assert(session.last_user() == "session_test");
        }

        session.logout();
        assert(!active_session && opens == 1 && closes == 1 && credentials == 2);
        assert(session.user() == nullptr);
        const int previous_starts = starts;
        assert(!session.reauthenticate("correct-password", error));
        assert(!error.empty() && starts == previous_starts);
        // The only transaction without pam_end was the failed allocation.
        assert(ends == starts - 1);
    }
    assert(unlink(marker.c_str()) == 0);
    test_configured_image_login();
    test_pending_keep_name_and_rename();
    test_pending_collision_preflight();
    test_pending_retries();
    test_interrupted_home_move();
    test_cleanup_retry_after_commit();
    test_full_wizard_defers_cleanup_and_resumes_authenticated_account();
    test_pending_state_validation();
    test_fresh_creation_rollback();
    std::filesystem::remove_all(test_home);
    return 0;
}
