#include "setup.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <unistd.h>

using namespace launcher;
int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--fixture") {
        const std::string operation = argv[2];
        if (operation == "echo") {
            std::cout << argv[3] << '\n';
            std::cerr << "stderr\n";
        } else if (operation == "stdin") {
            char bytes[4096];
            ssize_t count;
            while ((count = read(STDIN_FILENO, bytes, sizeof(bytes))) > 0) {
                size_t offset = 0;
                while (offset < static_cast<size_t>(count)) {
                    ssize_t n = write(STDOUT_FILENO, bytes + offset, static_cast<size_t>(count) - offset);
                    if (n <= 0) return 2;
                    offset += static_cast<size_t>(n);
                }
            }
        } else if (operation == "environment") {
            std::cout << getenv("LILYGO_TEST_SETUP_VALUE");
        } else if (operation == "flood") {
            std::string data(4096, 'x');
            for (;;) if (write(STDOUT_FILENO, data.data(), data.size()) < 0) return 1;
        } else if (operation == "early-exit") {
            close(STDIN_FILENO);
            return 3;
        } else if (operation == "sleep") {
            sleep(30);
        } else return 2;
        return 0;
    }
    char *resolved = realpath(argv[0], nullptr);
    assert(resolved);
    std::string self(resolved);
    free(resolved);
    SetupCommand command;
    command.arguments = {self, "--fixture", "echo", "$(touch /tmp/never-created); `id` & <> |"};
    auto result = run_setup_command(command);
    assert(result.exit_code == 0 && !result.timed_out);
    assert(result.output.find(command.arguments.back()) != std::string::npos);
    assert(result.output.find("stderr") != std::string::npos);
    command.arguments = {self, "--fixture", "stdin"};
    command.input = std::string(200000, 'p') + "secret\nwith punctuation$();";
    result = run_setup_command(command);
    assert(result.exit_code == 0 && result.output == command.input);
    clear_setup_secret(command.input);
    assert(command.input.empty());
    setenv("LILYGO_TEST_SETUP_VALUE", "before", 1);
    command.arguments = {self, "--fixture", "environment"};
    command.environment = {"LILYGO_TEST_SETUP_VALUE=after with spaces"};
    result = run_setup_command(command);
    assert(result.exit_code == 0 && result.output == "after with spaces");
    assert(std::string(getenv("LILYGO_TEST_SETUP_VALUE")) == "before");
    command.environment = {"INVALID"};
    assert(run_setup_command(command).exit_code == -1);
    command.environment.clear();
    command.arguments = {"relative-command"};
    assert(run_setup_command(command).exit_code == -1);
    command.arguments = {self, std::string("bad\0argument", 12)};
    assert(run_setup_command(command).exit_code == -1);
    command.arguments = {"/this/path/does/not/exist"};
    assert(run_setup_command(command).exit_code == -1);
    command.arguments = {self, "--fixture", "early-exit"};
    command.input = std::string(1024 * 1024, 'x');
    result = run_setup_command(command);
    assert(result.exit_code != 0 && !result.timed_out);
    command.input.clear();
    for (const char *operation : {"sleep", "flood"}) {
        command.arguments = {self, "--fixture", operation};
        command.timeout_seconds = 1;
        const auto before = std::chrono::steady_clock::now();
        result = run_setup_command(command);
        const auto elapsed = std::chrono::steady_clock::now() - before;
        assert(result.timed_out && result.exit_code == -1);
        assert(elapsed < std::chrono::seconds(5));
        assert(result.output.size() <= 1024 * 1024);
    }
}
