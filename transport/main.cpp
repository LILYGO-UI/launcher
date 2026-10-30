#include "bluez_agent.h"
#include "storage_ipc.h"

#include <charconv>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/socket.h>
#include <utility>

namespace {

struct Options {
    int ipc_fd = -1;
};

Options parse_options(int argc, char **argv) {
    Options result;
    bool have_descriptor = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const auto equals = argument.find('=');
        const auto name = argument.substr(0, equals);
        if (name != "--ipc-fd") {
            throw std::invalid_argument("Expected --ipc-fd");
        }
        std::string_view value;
        if (equals == std::string_view::npos) {
            if (++index >= argc) { throw std::invalid_argument("A command-line option is missing its value"); }
            value = argv[index];
        } else {
            value = argument.substr(equals + 1);
        }
        if (name == "--ipc-fd") {
            if (have_descriptor || value.empty()) { throw std::invalid_argument("Supply --ipc-fd exactly once"); }
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.ipc_fd);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result.ipc_fd < 0) {
                throw std::invalid_argument("The IPC descriptor must be a nonnegative integer");
            }
            have_descriptor = true;
        }
    }
    if (!have_descriptor) { throw std::invalid_argument("Supply --ipc-fd exactly once"); }
    const int flags = fcntl(result.ipc_fd, F_GETFD);
    int socket_type = 0;
    socklen_t socket_type_size = sizeof(socket_type);
    if (flags < 0 || getsockopt(result.ipc_fd, SOL_SOCKET, SO_TYPE, &socket_type, &socket_type_size) != 0
        || socket_type != SOCK_STREAM) {
        throw std::invalid_argument("The IPC descriptor must be an open stream socket");
    }
    if (fcntl(result.ipc_fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        throw std::runtime_error("Cannot protect the IPC descriptor from inheritance");
    }
    return result;
}

}

int main(int argc, char **argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::puts("Usage: lilygo-ui-file-share-agent --ipc-fd FD");
        return 0;
    }
    std::unique_ptr<launcher::transport::Storage> storage;
    try {
        auto options = parse_options(argc, argv);
        storage = std::make_unique<launcher::transport::Storage>(options.ipc_fd);
        launcher::transport::BluezAgent agent(*storage);
        agent.run();
        return 0;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "File sharing stopped: %s\n", error.what());
        if (storage != nullptr) {
            try {
                const std::string status = (std::string("File sharing unavailable: ") + error.what()).substr(0, 1024);
                storage->request('S', {status.begin(), status.end()});
            } catch (const std::exception &) {}
        }
        return 1;
    }
}
