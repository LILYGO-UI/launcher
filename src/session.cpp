#include "session.h"

#include "logging.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
#include "pam_compat.h"
#include <grp.h>
#endif

namespace launcher {
namespace {

[[maybe_unused]] constexpr const char *kDefaultShell = "/bin/bash";
constexpr const char *kNologin1 = "/sbin/nologin";
constexpr const char *kNologin2 = "/usr/sbin/nologin";
constexpr const char *kFalse1 = "/bin/false";
constexpr const char *kFalse2 = "/usr/bin/false";

#if (!defined(LAUNCHER_SYSTEM_SESSION) || !LAUNCHER_SYSTEM_SESSION) && defined(LAUNCHER_SIMULATOR) && LAUNCHER_SIMULATOR
constexpr const char *kSimulatorUsername = "pi";
constexpr const char *kSimulatorPassword = "raspberry";
#endif

void set_error(std::string &error, const char *message)
{
    error = message ? message : "user session failed";
}

bool valid_username(std::string_view username)
{
    // Linux account names are portable only in this conservative subset.  In
    // particular, reject "root" and names beginning with a digit.
    if (username.empty() || username.size() > 32 || username == "root" ||
        username.front() == '-' || (username.front() >= '0' && username.front() <= '9'))
        return false;
    for (const unsigned char ch : username) {
        if (!(ch == '_' || ch == '-' || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')))
            return false;
    }
    return true;
}

bool valid_password(std::string_view password)
{
    // PAM accepts arbitrary bytes, but an empty password is never useful for
    // the first-use screen.  Keep a reasonable bound before passing it to PAM.
    return !password.empty() && password.size() <= 4096 && password.find('\0') == std::string_view::npos &&
           password.find('\n') == std::string_view::npos && password.find('\r') == std::string_view::npos;
}

[[maybe_unused]] void clear_secret(std::string &secret)
{
    if (!secret.empty()) {
        volatile char *data = secret.data();
        std::fill(data, data + secret.size(), '\0');
    }
    secret.clear();
}

bool forbidden_shell(const char *shell)
{
    if (!shell || !shell[0]) return true;
    return std::strcmp(shell, kNologin1) == 0 || std::strcmp(shell, kNologin2) == 0 ||
           std::strcmp(shell, kFalse1) == 0 || std::strcmp(shell, kFalse2) == 0;
}

bool copy_field(char *destination, size_t capacity, const char *value)
{
    if (!destination || capacity == 0 || !value) return false;
    const size_t length = ::strnlen(value, capacity);
    if (length >= capacity) return false;
    std::memcpy(destination, value, length + 1);
    return true;
}

bool copy_field(char *destination, size_t capacity, const std::string &value)
{
    if (value.size() >= capacity) return false;
    std::memcpy(destination, value.c_str(), value.size() + 1);
    return true;
}

bool secure_state_parent(const std::string &path, std::string &parent)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        parent = slash == 0 ? "/" : ".";
    } else {
        parent = path.substr(0, slash);
    }
    struct stat st{};
    if (stat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    if (geteuid() == 0 && (st.st_uid != 0 || (st.st_mode & (S_IWGRP | S_IWOTH)))) return false;
#endif
    return true;
}

[[maybe_unused]] bool write_marker_atomic(const std::string &path, const std::string &username, std::string &error)
{
    std::string parent;
    if (!secure_state_parent(path, parent)) {
        set_error(error, "active-user directory is not secure");
        return false;
    }
    // O_EXCL prevents a second concurrent first-use flow from replacing the
    // account selected by the first one.
    std::string temporary = path + ".tmp." + std::to_string(static_cast<long long>(getpid()));
    int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_error(error, errno == EEXIST ? "active-user update already in progress" : "cannot create active-user state");
        return false;
    }
    bool ok = true;
    const char *data = username.c_str();
    size_t left = username.size();
    while (left > 0) {
        const ssize_t n = write(fd, data, left);
        if (n > 0) {
            data += n;
            left -= static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            ok = false;
            break;
        }
    }
    if (ok && (write(fd, "\n", 1) != 1 || fsync(fd) != 0)) ok = false;
    const int close_result = close(fd);
    if (close_result != 0) ok = false;
    /* link()+unlink() gives O_EXCL semantics for the final name: a second
     * first-use flow cannot replace the account selected by the first one. */
    if (ok && link(temporary.c_str(), path.c_str()) != 0) ok = false;
    unlink(temporary.c_str());
    if (!ok) {
        set_error(error, "cannot commit active-user state");
        return false;
    }
    const int parent_fd = open(parent.c_str(), O_RDONLY | O_CLOEXEC);
    if (parent_fd >= 0) {
        fsync(parent_fd);
        close(parent_fd);
    }
    return true;
}

[[maybe_unused]] bool read_marker(const std::string &path, std::string &username, std::string &error)
{
    struct stat state{};
    if (stat(path.c_str(), &state) != 0) {
        if (errno == ENOENT) return true; // first-use state
        set_error(error, "cannot inspect active-user state");
        return false;
    }
    if (!S_ISREG(state.st_mode) || (state.st_mode & (S_IWGRP | S_IWOTH)) || state.st_size > 64) {
        set_error(error, "active-user state has unsafe permissions");
        return false;
    }
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    if (geteuid() == 0 && state.st_uid != 0) {
        set_error(error, "active-user state is not owned by root");
        return false;
    }
#endif
    std::ifstream input(path);
    if (!input) {
        set_error(error, "cannot read active-user state");
        return false;
    }
    std::getline(input, username);
    if (!input && !input.eof()) {
        set_error(error, "cannot read active-user state");
        return false;
    }
    while (!username.empty() && (username.back() == '\r' || username.back() == '\n' || username.back() == ' ' || username.back() == '\t'))
        username.pop_back();
    if (!valid_username(username)) {
        set_error(error, "active-user state contains an invalid username");
        return false;
    }
    return true;
}

[[maybe_unused]] bool fill_user(const struct passwd *entry, launcher_user_t &output, std::string &error)
{
    if (!entry || entry->pw_uid == 0 || entry->pw_gid == 0 || forbidden_shell(entry->pw_shell)) {
        set_error(error, "account is not an interactive non-root user");
        return false;
    }
    if (!entry->pw_dir || entry->pw_dir[0] != '/' || std::strcmp(entry->pw_dir, "/") == 0) {
        set_error(error, "account has an unsafe home directory");
        return false;
    }
    struct stat home{};
    if (stat(entry->pw_dir, &home) != 0 || !S_ISDIR(home.st_mode) || home.st_uid != entry->pw_uid ||
        (home.st_mode & S_IWOTH)) {
        set_error(error, "account home directory is unavailable or unsafe");
        return false;
    }
    std::memset(&output, 0, sizeof(output));
    if (!copy_field(output.name, sizeof(output.name), entry->pw_name) ||
        !copy_field(output.home, sizeof(output.home), entry->pw_dir) ||
        !copy_field(output.shell, sizeof(output.shell), entry->pw_shell)) {
        set_error(error, "account information is too long");
        return false;
    }
    output.uid = entry->pw_uid;
    output.gid = entry->pw_gid;
    const std::string runtime = "/run/user/" + std::to_string(static_cast<unsigned long long>(entry->pw_uid));
    if (!copy_field(output.runtime_dir, sizeof(output.runtime_dir), runtime)) {
        set_error(error, "runtime directory path is too long");
        return false;
    }
    struct stat runtime_stat{};
    if (stat(runtime.c_str(), &runtime_stat) == 0) {
        if (!S_ISDIR(runtime_stat.st_mode) || runtime_stat.st_uid != entry->pw_uid || (runtime_stat.st_mode & S_IWOTH)) {
            set_error(error, "runtime directory is unsafe");
            return false;
        }
    } else if (errno != ENOENT) {
        set_error(error, "cannot inspect runtime directory");
        return false;
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    } else if (geteuid() == 0) {
        /* Minimal CM0 images may not run pam_systemd.  Give the selected
         * account a private runtime directory so XDG_RUNTIME_DIR remains
         * usable for every application it launches. */
        if (mkdir(runtime.c_str(), 0700) != 0 && errno != EEXIST) {
            set_error(error, "cannot create runtime directory");
            return false;
        }
        if (chown(runtime.c_str(), entry->pw_uid, entry->pw_gid) != 0 ||
            stat(runtime.c_str(), &runtime_stat) != 0 || !S_ISDIR(runtime_stat.st_mode) ||
            runtime_stat.st_uid != entry->pw_uid || (runtime_stat.st_mode & S_IWOTH)) {
            set_error(error, "runtime directory is unsafe");
            return false;
        }
#endif
    }
    return true;
}

#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
struct PamConversationData {
    const char *password = nullptr;
};

int pam_conversation(int messages, const struct pam_message **message, struct pam_response **response, void *data)
{
    if (messages <= 0 || messages > 32 || !message || !response || !data) return PAM_CONV_ERR;
    auto *conversation = static_cast<PamConversationData *>(data);
    auto *answers = static_cast<struct pam_response *>(calloc(static_cast<size_t>(messages), sizeof(struct pam_response)));
    if (!answers) return PAM_BUF_ERR;
    for (int i = 0; i < messages; ++i) {
        if (!message[i]) {
            for (int j = 0; j < i; ++j) free(answers[j].resp);
            free(answers);
            return PAM_CONV_ERR;
        }
        if (message[i]->msg_style == PAM_PROMPT_ECHO_OFF || message[i]->msg_style == PAM_PROMPT_ECHO_ON) {
            if (!conversation->password) {
                for (int j = 0; j < i; ++j) free(answers[j].resp);
                free(answers);
                return PAM_CONV_ERR;
            }
            const size_t length = std::strlen(conversation->password);
            answers[i].resp = static_cast<char *>(malloc(length + 1));
            if (!answers[i].resp) {
                for (int j = 0; j < i; ++j) free(answers[j].resp);
                free(answers);
                return PAM_BUF_ERR;
            }
            std::memcpy(answers[i].resp, conversation->password, length + 1);
        } else if (message[i]->msg_style == PAM_TEXT_INFO || message[i]->msg_style == PAM_ERROR_MSG) {
            answers[i].resp = nullptr;
        } else {
            for (int j = 0; j < i; ++j) free(answers[j].resp);
            free(answers);
            return PAM_CONV_ERR;
        }
    }
    *response = answers;
    return PAM_SUCCESS;
}

constexpr const char *kFirstBootHelper = "/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot";

std::string parent_path(const std::string &path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : slash == 0 ? "/" : path.substr(0, slash);
}

// Read privileged first-boot state and the small account mapping files without
// following a symlink or accepting a file writable by the selected account.
bool read_owned_file(const std::string &path, std::string &contents, struct stat &metadata,
                     bool &exists, std::string &error)
{
    contents.clear();
    exists = false;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return true;
        set_error(error, "cannot open first-boot account state safely");
        return false;
    }
    bool ok = fstat(fd, &metadata) == 0 && S_ISREG(metadata.st_mode) && metadata.st_uid == 0 &&
              !(metadata.st_mode & (S_IWGRP | S_IWOTH)) && metadata.st_size <= 1024 * 1024;
    char buffer[4096];
    while (ok) {
        const ssize_t length = read(fd, buffer, sizeof(buffer));
        if (length == 0) break;
        if (length < 0 && errno == EINTR) continue;
        if (length < 0 || contents.size() + static_cast<size_t>(length) > 1024 * 1024) {
            ok = false;
            break;
        }
        contents.append(buffer, static_cast<size_t>(length));
    }
    close(fd);
    if (!ok) {
        set_error(error, "first-boot account state has unsafe ownership, permissions, or contents");
        return false;
    }
    exists = true;
    return true;
}

bool read_pending(const std::string &path, uid_t &uid, std::string &original, bool &exists,
                  std::string &error)
{
    std::string contents;
    struct stat metadata{};
    if (!read_owned_file(path, contents, metadata, exists, error) || !exists) return !exists && error.empty();
    std::string parent;
    if (geteuid() != 0 || !secure_state_parent(path, parent)) {
        set_error(error, "first-boot account setup requires secure root-owned state");
        return false;
    }
    if (!contents.empty() && contents.back() == '\n') contents.pop_back();
    const auto separator = contents.find(':');
    if (separator == std::string::npos || separator == 0 || separator > 10 ||
        !valid_username(contents.substr(separator + 1))) {
        set_error(error, "invalid first-boot account state");
        return false;
    }
    unsigned long long parsed = 0;
    for (size_t i = 0; i < separator; ++i) {
        if (contents[i] < '0' || contents[i] > '9') {
            set_error(error, "invalid first-boot account UID");
            return false;
        }
        parsed = parsed * 10 + static_cast<unsigned>(contents[i] - '0');
    }
    if (parsed < 1000 || parsed == 65534 || parsed >= std::numeric_limits<uid_t>::max()) {
        set_error(error, "invalid first-boot account UID");
        return false;
    }
    uid = static_cast<uid_t>(parsed);
    original = contents.substr(separator + 1);
    return true;
}

bool write_owned_file(const std::string &path, const std::string &contents,
                      const struct stat &metadata, std::string &error)
{
    std::string parent;
    if (!secure_state_parent(path, parent)) {
        set_error(error, "account configuration directory is not secure");
        return false;
    }
    const std::string temporary = path + ".launcher-" + std::to_string(getpid());
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        set_error(error, "cannot prepare account configuration update");
        return false;
    }
    bool ok = fchown(fd, metadata.st_uid, metadata.st_gid) == 0 && fchmod(fd, metadata.st_mode & 0777) == 0;
    size_t offset = 0;
    while (ok && offset < contents.size()) {
        const ssize_t n = write(fd, contents.data() + offset, contents.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else offset += static_cast<size_t>(n);
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(temporary.c_str(), path.c_str()) != 0) ok = false;
    unlink(temporary.c_str());
    if (!ok) {
        set_error(error, "cannot update renamed account configuration");
        return false;
    }
    const int parent_fd = open(parent.c_str(), O_RDONLY | O_CLOEXEC);
    if (parent_fd >= 0) {
        fsync(parent_fd);
        close(parent_fd);
    }
    return true;
}

// usermod implementations differ in whether they rename subordinate-ID keys.
// Reconcile only exact account keys, and the image's explicit pi sudo rule;
// never replace substrings in unrelated users, comments, or policy text.
bool rename_account_mappings(const std::string &system_root, const std::string &original,
                             const std::string &current, const std::string &target, std::string &error,
                             bool apply)
{
    for (const char *suffix : {"/etc/subuid", "/etc/subgid", "/etc/sudoers.d/010_pi-nopasswd"}) {
        const std::string path = system_root + suffix;
        std::string contents;
        struct stat metadata{};
        bool exists = false;
        if (!read_owned_file(path, contents, metadata, exists, error)) return false;
        if (!exists) continue;
        const bool sudoers = std::strcmp(suffix, "/etc/sudoers.d/010_pi-nopasswd") == 0;
        std::string rewritten;
        size_t offset = 0;
        while (offset < contents.size()) {
            const auto end = contents.find('\n', offset);
            std::string line = contents.substr(offset, end == std::string::npos ? end : end - offset);
            const auto start = sudoers ? line.find_first_not_of(" \t") : 0;
            if (start != std::string::npos) {
                const auto separator = line.find_first_of(sudoers ? " \t" : ":", start);
                const auto key = line.substr(start, separator == std::string::npos ? separator : separator - start);
                if (separator != std::string::npos && (key == original || key == current))
                    line.replace(start, key.size(), target);
            }
            rewritten += line;
            if (end == std::string::npos) break;
            rewritten += '\n';
            offset = end + 1;
        }
        if (apply && rewritten != contents && !write_owned_file(path, rewritten, metadata, error)) return false;
    }
    return true;
}

bool run_program(const char *path, const std::vector<std::string> &arguments, const std::string *input, std::string &error)
{
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1);
    for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
    argv.push_back(nullptr);
    int pipe_fds[2] = {-1, -1};
    if (input && pipe2(pipe_fds, O_CLOEXEC) != 0) {
        set_error(error, "cannot prepare password input");
        return false;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        if (input) {
            close(pipe_fds[0]);
            close(pipe_fds[1]);
        }
        set_error(error, "cannot create account helper");
        return false;
    }
    if (pid == 0) {
        if (input) {
            dup2(pipe_fds[0], STDIN_FILENO);
            close(pipe_fds[0]);
            close(pipe_fds[1]);
        } else {
            int null_fd = open("/dev/null", O_RDONLY);
            if (null_fd >= 0) {
                dup2(null_fd, STDIN_FILENO);
                close(null_fd);
            }
        }
        execv(path, argv.data());
        _exit(127);
    }
    bool input_written = true;
    if (input) {
        close(pipe_fds[0]);
        struct sigaction ignore_pipe{};
        struct sigaction previous_pipe{};
        ignore_pipe.sa_handler = SIG_IGN;
        sigemptyset(&ignore_pipe.sa_mask);
        sigaction(SIGPIPE, &ignore_pipe, &previous_pipe);
        const char *data = input->data();
        size_t left = input->size();
        while (left) {
            const ssize_t n = write(pipe_fds[1], data, left);
            if (n > 0) {
                data += n;
                left -= static_cast<size_t>(n);
            } else if (n < 0 && errno == EINTR) {
                continue;
            } else {
                input_written = false;
                break;
            }
        }
        close(pipe_fds[1]);
        sigaction(SIGPIPE, &previous_pipe, nullptr);
    }
    int status = 0;
    pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (!input_written || waited != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        set_error(error, "account helper failed");
        return false;
    }
    return true;
}

bool account_helper(const std::vector<std::string> &arguments, std::string &error)
{
    if (access(arguments.front().c_str(), X_OK) != 0) {
        set_error(error, "required account helper is unavailable");
        return false;
    }
    return run_program(arguments.front().c_str(), arguments, nullptr, error);
}

bool set_account_password(const std::string &username, const std::string &password, std::string &error)
{
    std::string input = username + ":" + password + "\n";
    const bool result = run_program("/usr/sbin/chpasswd", {"/usr/sbin/chpasswd"}, &input, error);
    clear_secret(input);
    return result;
}

void remove_created_account(const std::string &username)
{
    std::string ignored;
    account_helper({"/usr/sbin/userdel", "--remove", username}, ignored);
}

bool configure_pending_account(uid_t uid, const std::string &original, const std::string &username,
                               const std::string &password, const std::string &system_root, std::string &error)
{
    const passwd *entry = getpwuid(uid);
    if (!entry || entry->pw_uid < 1000 || entry->pw_uid == 65534 || entry->pw_gid == 0 || !entry->pw_name ||
        !valid_username(entry->pw_name) || !entry->pw_dir) {
        set_error(error, "first-boot source account is unavailable or unsafe");
        return false;
    }
    const std::string current = entry->pw_name;
    std::string home = entry->pw_dir;
    const gid_t gid = entry->pw_gid;
    if (current != original && username != current) {
        error = "account was renamed; finish setup as " + current;
        return false;
    }
    const passwd *collision = getpwnam(username.c_str());
    if (collision && collision->pw_uid != uid) {
        set_error(error, "username belongs to another account");
        return false;
    }
    if (!getgrnam("sudo")) {
        set_error(error, "sudo group is unavailable; install or repair sudo before setup");
        return false;
    }
    const group *primary = getgrgid(gid);
    const std::string group_name = primary && primary->gr_name ? primary->gr_name : "";
    const bool rename_group = (group_name == original || group_name == current) && group_name != username;
    const group *target_group = getgrnam(username.c_str());
    if (rename_group && target_group && target_group->gr_gid != gid) {
        set_error(error, "username conflicts with an existing group");
        return false;
    }
    const std::string target_home = system_root + "/home/" + username;
    struct stat home_state{};
    // usermod may commit the passwd name/home before its directory move fails.
    // The image's old /home/<name> is a recovery source only when it is still a
    // real directory owned by this exact UID and the destination is absent.
    const std::string original_home = system_root + "/home/" + original;
    const bool missing_home = lstat(home.c_str(), &home_state) != 0 && errno == ENOENT;
    if (current != original && home == target_home && missing_home) {
        struct stat source_home{};
        if (lstat(original_home.c_str(), &source_home) == 0 && S_ISDIR(source_home.st_mode) &&
            source_home.st_uid == uid && !(source_home.st_mode & S_IWOTH)) {
            if (!account_helper({"/usr/sbin/usermod", "--home", original_home, current}, error)) return false;
            home = original_home;
        }
    }
    if (home.empty() || home.front() != '/' || home == "/" || lstat(home.c_str(), &home_state) != 0 ||
        !S_ISDIR(home_state.st_mode) || home_state.st_uid != uid || (home_state.st_mode & S_IWOTH)) {
        set_error(error, "first-boot source home is unavailable or unsafe");
        return false;
    }
    if (current != original && home == target_home) {
        struct stat leftover{};
        if (lstat(original_home.c_str(), &leftover) == 0) {
            set_error(error, "both account home directories exist; finish recovering the home move before retrying");
            return false;
        }
    }
    // Keeping the account name also keeps a custom home. After a partial
    // rename, original != username tells us to finish moving /home/original.
    const bool move_home = (current != username || username != original) && home != target_home;
    if (move_home) {
        struct stat target{};
        if (lstat(target_home.c_str(), &target) == 0 || errno != ENOENT) {
            set_error(error, "new home directory already exists or cannot be inspected");
            return false;
        }
    }
    if (!rename_account_mappings(system_root, original, current, username, error, false)) return false;
    if (current != username || move_home) {
        std::vector<std::string> arguments{"/usr/sbin/usermod"};
        if (current != username) arguments.insert(arguments.end(), {"--login", username});
        if (move_home) arguments.insert(arguments.end(), {"--home", target_home, "--move-home"});
        arguments.push_back(current);
        if (!account_helper(arguments, error)) return false;
    }
    if (rename_group && !account_helper({"/usr/sbin/groupmod", "--new-name", username, group_name}, error)) return false;
    if (!rename_account_mappings(system_root, original, current, username, error, true)) return false;
    // Append preserves all image-specific groups (gpio, i2c, spi, audio, ...).
    // pi-gen deliberately gives the pending account nologin when no password
    // was supplied. Enabling its login shell belongs to successful setup.
    if (!account_helper({"/usr/sbin/usermod", "--shell", kDefaultShell, "--append", "--groups", "sudo", username}, error))
        return false;
    return set_account_password(username, password, error);
}

bool create_account(const std::string &username, const std::string &password, std::string &error)
{
    if (geteuid() != 0) {
        set_error(error, "creating users requires root");
        return false;
    }
    // The first-use account owns this device and must be able to administer
    // it through the distribution's sudo policy. Fail before creating a user
    // if that policy's group is unavailable; do not silently create an owner
    // who cannot manage the device.
    if (!getgrnam("sudo")) {
        set_error(error, "sudo group is unavailable; install or repair sudo before setup");
        return false;
    }
    std::string supplementary_groups = "sudo";
    for (const char *group_name : {"video", "render", "input"}) {
        if (!getgrnam(group_name)) continue;
        supplementary_groups.push_back(',');
        supplementary_groups += group_name;
    }
    std::vector<std::string> add_args = {"/usr/sbin/useradd", "--create-home", "--shell", kDefaultShell,
                                         "--groups", supplementary_groups};
    add_args.push_back(username);
    if (access(add_args[0].c_str(), X_OK) != 0 || !run_program(add_args[0].c_str(), add_args, nullptr, error)) return false;
    std::string secret_input = username + ":" + password + "\n";
    const std::vector<std::string> pass_args = {"/usr/sbin/chpasswd"};
    if (access(pass_args[0].c_str(), X_OK) != 0 || !run_program(pass_args[0].c_str(), pass_args, &secret_input, error)) {
        clear_secret(secret_input);
        const std::string delete_path = "/usr/sbin/userdel";
        if (access(delete_path.c_str(), X_OK) == 0) {
            std::string ignored;
            run_program(delete_path.c_str(), {delete_path, "--remove", username}, nullptr, ignored);
        }
        return false;
    }
    clear_secret(secret_input);
    return true;
}
#endif

} // namespace

struct UserSession::Impl {
    explicit Impl(std::string state, std::string root) : state_path(std::move(state)), system_root(std::move(root)) {}
    std::string state_path;
    std::string system_root;
    std::string marker_user;
    std::string selected_user;
    std::string setup_user;
    std::string original_user;
    uid_t setup_uid = 0;
    bool pending_setup = false;
    bool needs_setup = false;
    launcher_user_t account{};
    bool marker_exists = false;
    bool initialized = false;
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    pam_handle_t *pamh = nullptr;
#elif defined(LAUNCHER_SIMULATOR) && LAUNCHER_SIMULATOR
    std::string simulator_password;
#endif
};

UserSession::UserSession(std::string state_path, std::string system_root)
    : impl_(std::make_unique<Impl>(std::move(state_path), std::move(system_root))) {}

UserSession::~UserSession()
{
    logout();
#if (!defined(LAUNCHER_SYSTEM_SESSION) || !LAUNCHER_SYSTEM_SESSION) && defined(LAUNCHER_SIMULATOR) && LAUNCHER_SIMULATOR
    clear_secret(impl_->simulator_password);
#endif
}

bool UserSession::setup_available() const
{
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    return impl_ && impl_->needs_setup && geteuid() == 0 &&
           access(impl_->pending_setup ? "/usr/sbin/usermod" : "/usr/sbin/useradd", X_OK) == 0 &&
           access("/usr/sbin/chpasswd", X_OK) == 0;
#else
    return false;
#endif
}

bool UserSession::setup_required() const { return impl_ && impl_->needs_setup; }
bool UserSession::setup_account_saved() const { return impl_ && impl_->marker_exists; }
const std::string &UserSession::setup_username() const { return impl_->setup_user; }

bool UserSession::initialize(std::string &error)
{
    error.clear();
    impl_->marker_user.clear();
    impl_->marker_exists = false;
    impl_->initialized = false;
    impl_->needs_setup = false;
    impl_->pending_setup = false;
    impl_->setup_user.clear();
    impl_->original_user.clear();
#if (!defined(LAUNCHER_SYSTEM_SESSION) || !LAUNCHER_SYSTEM_SESSION) && defined(LAUNCHER_SIMULATOR) && LAUNCHER_SIMULATOR
    // The simulator identity belongs only to this process. Never read or write
    // device account state or look up a matching host operating-system account.
    impl_->marker_user = kSimulatorUsername;
    clear_secret(impl_->simulator_password);
#else
    if (!read_marker(impl_->state_path, impl_->marker_user, error)) return false;
    impl_->marker_exists = !impl_->marker_user.empty();
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    const std::string pending = parent_path(impl_->state_path) + "/first-boot-pending";
    if (!read_pending(pending, impl_->setup_uid, impl_->original_user, impl_->pending_setup, error)) return false;
    if (impl_->pending_setup) {
        const passwd *source = getpwuid(impl_->setup_uid);
        if (!source || !source->pw_name || !valid_username(source->pw_name) || source->pw_gid == 0) {
            set_error(error, "first-boot source account is unavailable or unsafe");
            return false;
        }
        impl_->setup_user = source->pw_name;
        if (impl_->marker_exists && impl_->marker_user != impl_->setup_user) {
            set_error(error, "completed account does not match first-boot state");
            return false;
        }
        impl_->needs_setup = true;
    } else if (!impl_->marker_exists) {
        const passwd *configured = getpwuid(1000);
        if (configured && configured->pw_name && valid_username(configured->pw_name) &&
            configured->pw_gid != 0 && !forbidden_shell(configured->pw_shell)) {
            // No pending rename means the image already supplied its account.
            // Prefill it without granting setup authority or persisting a login.
            impl_->marker_user = configured->pw_name;
        } else {
            impl_->needs_setup = true;
        }
    }
#endif
#endif
    impl_->initialized = true;
    return true;
}

const std::string &UserSession::last_user() const { return impl_->marker_user; }
const std::string &UserSession::username() const { return impl_->selected_user; }

bool UserSession::login(const std::string &username, const std::string &password, bool create,
                        std::string &error, bool finish_setup)
{
    error.clear();
    if (!impl_->initialized) {
        set_error(error, "user session is not initialized");
        return false;
    }
    if (!valid_username(username) || !valid_password(password)) {
        set_error(error, "invalid username or password");
        return false;
    }
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    if (create && !impl_->needs_setup) {
        set_error(error, "first-use account setup is not enabled");
        return false;
    }
    if ((impl_->needs_setup || impl_->pending_setup) && !create &&
        !(impl_->marker_exists && !finish_setup && username == impl_->marker_user)) {
        set_error(error, "first-use account setup is required");
        return false;
    }
    if (create && !setup_available()) {
        set_error(error, "account setup requires root and account helpers");
        return false;
    }
    if (!impl_->marker_exists && geteuid() != 0) {
        set_error(error, "first-use account selection requires root");
        return false;
    }
    bool created = false;
    if (create && impl_->pending_setup) {
        if (impl_->marker_exists) {
            // Account/PAM commit already succeeded, but cleanup failed. Verify
            // the committed password and retry cleanup without renaming again.
            if (username != impl_->marker_user) {
                set_error(error, "finish setup with the account already selected");
                return false;
            }
        } else if (!configure_pending_account(impl_->setup_uid, impl_->original_user, username,
                                              password, impl_->system_root, error)) {
            if (const passwd *current = getpwuid(impl_->setup_uid)) impl_->setup_user = current->pw_name;
            return false;
        }
        impl_->setup_user = username;
    } else if (create) {
        if (getpwnam(username.c_str())) {
            set_error(error, "account already exists");
            return false;
        }
        if (!create_account(username, password, error)) return false;
        created = true;
    }
    struct passwd *entry = getpwnam(username.c_str());
    if (!entry || (impl_->pending_setup && entry->pw_uid != impl_->setup_uid)) {
        set_error(error, "selected account cannot be inspected");
        if (created) remove_created_account(username);
        return false;
    }
    if (!fill_user(entry, impl_->account, error)) {
        if (created) remove_created_account(username);
        return false;
    }

    PamConversationData conversation{password.c_str()};
    pam_handle_t *pamh = nullptr;
    struct pam_conv conv{pam_conversation, &conversation};
    int result = pam_start("lilygo-ui-launcher", username.c_str(), &conv, &pamh);
    if (result == PAM_SUCCESS) result = pam_set_item(pamh, PAM_TTY, "tty1");
    if (result == PAM_SUCCESS) result = pam_authenticate(pamh, 0);
    if (result == PAM_SUCCESS) result = pam_acct_mgmt(pamh, 0);
    bool credentials_established = false;
    if (result == PAM_SUCCESS) {
        result = pam_setcred(pamh, PAM_ESTABLISH_CRED);
        credentials_established = result == PAM_SUCCESS;
    }
    if (result == PAM_SUCCESS) result = pam_open_session(pamh, 0);
    if (result != PAM_SUCCESS) {
        if (pamh) {
            if (credentials_established) pam_setcred(pamh, PAM_DELETE_CRED);
            pam_end(pamh, result);
        }
        if (created) remove_created_account(username);
        set_error(error, "authentication or account session failed");
        return false;
    }
    impl_->pamh = pamh;
    impl_->selected_user = username;
    if (!impl_->marker_exists && !write_marker_atomic(impl_->state_path, username, error)) {
        logout();
        if (created) remove_created_account(username);
        return false;
    }
    impl_->marker_user = username;
    impl_->marker_exists = true;
    if (finish_setup && !complete_setup(error)) {
        logout();
        set_error(error, "account saved; first-boot cleanup failed, retry with the same account and password");
        return false;
    }
    impl_->needs_setup = false;
    if (!impl_->pending_setup) impl_->setup_user.clear();
    return true;
#elif defined(LAUNCHER_SIMULATOR) && LAUNCHER_SIMULATOR
    if (create) {
        if (finish_setup || impl_->marker_exists) {
            set_error(error, "account creation is only available in the simulator first-boot demo");
            return false;
        }
        // Explicit full-wizard preview only. No passwd/PAM lookup, privilege
        // change or persistent device state is involved in this identity.
        clear_secret(impl_->simulator_password);
        impl_->simulator_password = password;
        impl_->marker_user = username;
        impl_->marker_exists = true;
        impl_->pending_setup = true;
        impl_->needs_setup = false;
        impl_->setup_user = username;
    } else {
        const std::string_view expected_user = impl_->marker_exists ? std::string_view(impl_->marker_user) : kSimulatorUsername;
        const std::string_view expected_password = impl_->marker_exists ? std::string_view(impl_->simulator_password) : kSimulatorPassword;
        if ((impl_->pending_setup && finish_setup) || username != expected_user || password != expected_password) {
            set_error(error, "invalid username or password");
            return false;
        }
    }
    impl_->selected_user = username;
    return true;
#else
    (void)create;
    (void)finish_setup;
    set_error(error, "user sessions are unavailable in this build");
    return false;
#endif
}

bool UserSession::complete_setup(std::string &error)
{
    error.clear();
    if (!impl_->initialized || impl_->selected_user.empty()) {
        set_error(error, "sign in before completing first-boot setup");
        return false;
    }
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    if (!impl_->pamh) {
        set_error(error, "an authenticated account session is required");
        return false;
    }
    if (impl_->pending_setup && !account_helper({kFirstBootHelper, "complete"}, error)) {
        set_error(error, "first-boot cleanup failed; retry finishing setup");
        return false;
    }
#elif !defined(LAUNCHER_SIMULATOR) || !LAUNCHER_SIMULATOR
    set_error(error, "user sessions are unavailable in this build");
    return false;
#endif
    impl_->pending_setup = false;
    impl_->needs_setup = false;
    impl_->setup_user.clear();
    return true;
}

bool UserSession::reauthenticate(const std::string &password, std::string &error)
{
    error.clear();
    if (!impl_->initialized) {
        set_error(error, "user session is not initialized");
        return false;
    }
    if (!valid_password(password)) {
        set_error(error, "invalid password");
        return false;
    }
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    if (!impl_->pamh || impl_->selected_user.empty()) {
        set_error(error, "no signed-in account to unlock");
        return false;
    }

    // A separate authentication transaction keeps a failed unlock from changing
    // the live session's PAM state or credentials. The account is always the one
    // selected by the successful login, never a username supplied by the screen.
    PamConversationData conversation{password.c_str()};
    pam_handle_t *pamh = nullptr;
    struct pam_conv conv{pam_conversation, &conversation};
    int result = pam_start("lilygo-ui-launcher", impl_->selected_user.c_str(), &conv, &pamh);
    if (result == PAM_SUCCESS) result = pam_set_item(pamh, PAM_TTY, "tty1");
    if (result == PAM_SUCCESS) result = pam_authenticate(pamh, 0);
    if (result == PAM_SUCCESS) result = pam_acct_mgmt(pamh, 0);
    if (pamh) pam_end(pamh, result);
    if (result != PAM_SUCCESS) {
        set_error(error, "authentication or account verification failed");
        return false;
    }
    return true;
#elif defined(LAUNCHER_SIMULATOR) && LAUNCHER_SIMULATOR
    if (impl_->selected_user.empty()) {
        set_error(error, "no signed-in account to unlock");
        return false;
    }
    const std::string_view expected_password = impl_->marker_exists ? std::string_view(impl_->simulator_password) : kSimulatorPassword;
    if (password != expected_password) {
        set_error(error, "invalid password");
        return false;
    }
    return true;
#else
    set_error(error, "user sessions are unavailable in this build");
    return false;
#endif
}

void UserSession::logout()
{
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    if (impl_ && impl_->pamh) {
        pam_close_session(impl_->pamh, 0);
        pam_setcred(impl_->pamh, PAM_DELETE_CRED);
        pam_end(impl_->pamh, PAM_SUCCESS);
        impl_->pamh = nullptr;
    }
#endif
    if (impl_) {
        std::memset(&impl_->account, 0, sizeof(impl_->account));
        impl_->selected_user.clear();
    }
}

const launcher_user_t *UserSession::user() const
{
#if defined(LAUNCHER_SYSTEM_SESSION) && LAUNCHER_SYSTEM_SESSION
    if (!impl_ || impl_->selected_user.empty()) return nullptr;
    return &impl_->account;
#else
    // A simulator sign-in must not change child-process credentials or their
    // inherited host HOME and XDG environment.
    return nullptr;
#endif
}

} // namespace launcher
