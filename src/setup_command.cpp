#include "setup.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace launcher {
void clear_setup_secret(std::string &secret)
{
    volatile char *data = secret.empty() ? nullptr : secret.data();
    for (size_t i = 0; i < secret.size(); ++i) data[i] = 0;
    secret.clear();
}

SetupCommandResult run_setup_command(const SetupCommand &command)
{
    SetupCommandResult result;
    if (command.arguments.empty() || command.arguments.front().empty() || command.arguments.front()[0] != '/') return result;
    std::vector<char *> arguments;
    for (const auto &value : command.arguments) {
        if (value.find('\0') != std::string::npos) return result;
        arguments.push_back(const_cast<char *>(value.c_str()));
    }
    arguments.push_back(nullptr);
    std::vector<std::string> environment;
    for (char **value = environ; value && *value; ++value) environment.emplace_back(*value);
    for (const auto &value : command.environment) {
        const size_t equals = value.find('=');
        if (equals == std::string::npos || equals == 0 || value.find('\0') != std::string::npos) return result;
        const std::string key = value.substr(0, equals + 1);
        environment.erase(std::remove_if(environment.begin(), environment.end(), [&](const auto &old) {
            return old.compare(0, key.size(), key) == 0;
        }), environment.end());
        environment.push_back(value);
    }
    std::vector<char *> env;
    for (auto &value : environment) env.push_back(value.data());
    env.push_back(nullptr);
    int input[2] = {-1, -1}, output[2] = {-1, -1};
    if (pipe(input) || pipe(output)) {
        for (int fd : {input[0], input[1], output[0], output[1]}) if (fd >= 0) close(fd);
        return result;
    }
    for (int fd : {input[0], input[1], output[0], output[1]}) fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO);
    for (int fd : {input[0], input[1], output[0], output[1]})
        if (fd > STDERR_FILENO) posix_spawn_file_actions_addclose(&actions, fd);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t pid = -1;
    const int spawned = posix_spawn(&pid, arguments.front(), &actions, &attributes, arguments.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(input[0]); close(output[1]);
    if (spawned) { close(input[1]); close(output[0]); return result; }
    fcntl(input[1], F_SETFL, fcntl(input[1], F_GETFL) | O_NONBLOCK);
    fcntl(output[0], F_SETFL, fcntl(output[0], F_GETFL) | O_NONBLOCK);
    // Block SIGPIPE only on this worker; a failed secret consumer must not
    // terminate the UI process or change another thread's signal disposition.
    sigset_t blocked, previous;
    sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    const auto started = std::chrono::steady_clock::now();
    size_t written = 0;
    bool child_done = false, output_done = false;
    int status = 0;
    while (!child_done || !output_done) {
        if (input[1] >= 0 && written == command.input.size()) { close(input[1]); input[1] = -1; }
        pollfd pipes[] = {{output[0], POLLIN, 0}, {input[1], POLLOUT, 0}};
        poll(pipes, 2, 20);
        if (input[1] >= 0 && pipes[1].revents) {
            const ssize_t n = write(input[1], command.input.data() + written, command.input.size() - written);
            if (n > 0) written += static_cast<size_t>(n);
            else if (n < 0 && errno != EINTR && errno != EAGAIN) { close(input[1]); input[1] = -1; }
        }
        char buffer[4096];
        // Bound each drain pass: a child producing continuous output must not
        // prevent us from checking its deadline or writing password input.
        size_t drained = 0;
        while (drained < 64 * 1024) {
            const ssize_t n = read(output[0], buffer, sizeof(buffer));
            if (n > 0) {
                drained += static_cast<size_t>(n);
                constexpr size_t limit = 1024 * 1024;
                const size_t count = std::min(static_cast<size_t>(n), limit - result.output.size());
                result.output.append(buffer, count);
            } else {
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) output_done = true;
                break;
            }
        }
        if (!child_done) {
            const pid_t finished = waitpid(pid, &status, WNOHANG);
            if (finished == pid) child_done = true;
            else if (finished < 0 && errno != EINTR) { child_done = true; status = -1; }
        }
        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed >= std::chrono::seconds(std::max(1U, command.timeout_seconds))) {
            result.timed_out = true;
            kill(-pid, SIGKILL);
            if (!child_done) while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
    }
    if (input[1] >= 0) close(input[1]);
    close(output[0]);
    // Consume only a newly pending SIGPIPE before restoring the worker mask.
    sigset_t pending;
    sigpending(&pending);
    if (!sigismember(&previous, SIGPIPE) && sigismember(&pending, SIGPIPE)) {
        int caught = 0; sigwait(&blocked, &caught);
    }
    pthread_sigmask(SIG_SETMASK, &previous, nullptr);
    if (!result.timed_out && status >= 0 && WIFEXITED(status) && written == command.input.size())
        result.exit_code = WEXITSTATUS(status);
    return result;
}
} // namespace launcher
