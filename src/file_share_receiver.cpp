#include "file_share_receiver.h"
#include "file_share.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

#ifndef LAUNCHER_FILE_SHARE_AGENT
#define LAUNCHER_FILE_SHARE_AGENT "/usr/lib/lilygo-ui-launcher/file-share/lilygo-ui-file-share-agent"
#endif

namespace launcher::file_share {
namespace {

constexpr std::size_t kMaxFrameBytes = kMaxChunkBytes + 9;
constexpr std::size_t kMaxStatusBytes = 1024;

bool read_all(int fd, void *buffer, std::size_t size)
{
    auto *cursor = static_cast<unsigned char *>(buffer);
    while (size) {
        const ssize_t count = read(fd, cursor, size);
        if (count > 0) {
            cursor += count;
            size -= static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

bool write_all(int fd, const void *buffer, std::size_t size)
{
    const auto *cursor = static_cast<const unsigned char *>(buffer);
    while (size) {
#if defined(MSG_NOSIGNAL)
        const ssize_t count = send(fd, cursor, size, MSG_NOSIGNAL);
#else
        const ssize_t count = send(fd, cursor, size, 0);
#endif
        if (count > 0) {
            cursor += count;
            size -= static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

std::uint64_t read_u64(const unsigned char *data)
{
    std::uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index) value = (value << 8) | data[index];
    return value;
}

bool respond(int fd, bool ok, const std::string &message)
{
    const std::uint32_t size = static_cast<std::uint32_t>(message.size() + 2);
    const unsigned char header[] = {
        static_cast<unsigned char>(size >> 24), static_cast<unsigned char>(size >> 16),
        static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size), 'R',
        static_cast<unsigned char>(ok ? 1 : 0)};
    return write_all(fd, header, sizeof(header)) && write_all(fd, message.data(), message.size());
}

void close_child_descriptors(long descriptor_limit)
{
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 4U, ~0U, 0U) == 0) return;
#endif
    for (long fd = 4; fd < descriptor_limit; ++fd) close(static_cast<int>(fd));
}

} // namespace

Receiver::Receiver(std::string helper_path)
    : helper_path_(helper_path.empty() ? LAUNCHER_FILE_SHARE_AGENT : helper_path),
      explicit_helper_(!helper_path.empty())
{
}

Receiver::~Receiver()
{
    stop();
}

void Receiver::set_status(std::string message)
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    status_ = std::move(message);
}

std::string Receiver::status() const
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    return status_;
}

std::optional<PendingApproval> Receiver::pending_approval() const
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (std::chrono::steady_clock::now() >= approval_deadline_) return {};
    return pending_approval_;
}

bool Receiver::resolve_approval(std::uint64_t id, bool accepted)
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    if (!pending_approval_ || pending_approval_->id != id || stopping_ ||
        std::chrono::steady_clock::now() >= approval_deadline_) return false;
    resolved_approval_id_ = id;
    resolved_approval_ = accepted;
    pending_approval_.reset();
    return true;
}

bool Receiver::start(const launcher_user_t *user, std::string &error)
{
    if (active_) {
        error.clear();
        return true;
    }
    stop();
#if !defined(__linux__) || defined(LAUNCHER_SIMULATOR)
    if (!explicit_helper_) {
        error = "Bluetooth file sharing is available on the device.";
        set_status(error);
        return false;
    }
#endif
    if (!user) {
        error = "Sign in before starting file sharing.";
        set_status(error);
        return false;
    }
    std::string downloads;
    if (!resolve_download_directory(user, downloads, error)) {
        set_status(error);
        return false;
    }
    if (helper_path_.empty() || helper_path_.front() != '/' ||
        access(helper_path_.c_str(), X_OK) != 0) {
        error = "The file sharing transport is not installed.";
        set_status(error);
        return false;
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
        error = std::string("Cannot create the file sharing channel: ") + std::strerror(errno);
        set_status(error);
        return false;
    }
    for (int fd : sockets) {
        if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) {
            close(sockets[0]);
            close(sockets[1]);
            error = "Cannot protect the file sharing channel.";
            set_status(error);
            return false;
        }
#if defined(SO_NOSIGPIPE)
        int enabled = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
    }
    long descriptor_limit = sysconf(_SC_OPEN_MAX);
    if (descriptor_limit < 0) descriptor_limit = 65536;
    const pid_t parent_pid = getpid();
    const pid_t child = fork();
    if (child < 0) {
        close(sockets[0]);
        close(sockets[1]);
        error = std::string("Cannot start file sharing: ") + std::strerror(errno);
        set_status(error);
        return false;
    }
    if (child == 0) {
#if defined(__linux__)
        /* A launcher crash must not leave its Bluetooth service running. */
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent_pid) _exit(126);
#else
        (void)parent_pid;
#endif
        if (sockets[1] != 3 && dup2(sockets[1], 3) < 0) _exit(126);
        if (fcntl(3, F_SETFD, 0) != 0) _exit(126);
        close_child_descriptors(descriptor_limit);
        /* The transport needs neither the launcher's controlling terminal nor
         * its device descriptors. stdout/stderr remain in the journal. */
        const int input = open("/dev/null", O_RDONLY);
        if (input >= 0 && input != STDIN_FILENO) {
            dup2(input, STDIN_FILENO);
            close(input);
        }
        execl(helper_path_.c_str(), helper_path_.c_str(), "--ipc-fd", "3", static_cast<char *>(nullptr));
        _exit(127);
    }
    close(sockets[1]);
    socket_fd_ = sockets[0];
    child_ = child;
    stopping_ = false;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        pending_approval_.reset();
        resolved_approval_id_ = 0;
        resolved_approval_ = false;
        status_ = "Starting Bluetooth sharing...";
    }
    active_ = true;
    try {
        worker_ = std::thread(&Receiver::run, this, *user);
    } catch (...) {
        stop();
        error = "Cannot start the file sharing worker.";
        set_status(error);
        return false;
    }
    error.clear();
    return true;
}

void Receiver::reap_child()
{
    if (child_ <= 0) return;
    int result = 0;
    for (unsigned attempt = 0; attempt < 30; ++attempt) {
        const pid_t reaped = waitpid(child_, &result, WNOHANG);
        if (reaped == child_ || (reaped < 0 && errno == ECHILD)) {
            child_ = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(child_, SIGKILL);
    while (waitpid(child_, &result, 0) < 0 && errno == EINTR) {}
    child_ = -1;
}

void Receiver::stop()
{
    stopping_ = true;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        pending_approval_.reset();
        resolved_approval_ = false;
        resolved_approval_id_ = 0;
    }
    if (socket_fd_ >= 0) shutdown(socket_fd_, SHUT_RDWR);
    if (child_ > 0) kill(child_, SIGTERM);
    if (worker_.joinable()) worker_.join();
    if (socket_fd_ >= 0) close(socket_fd_);
    socket_fd_ = -1;
    reap_child();
    active_ = false;
}

void Receiver::run(launcher_user_t user)
{
    struct ApprovedFile {
        std::uint64_t transport_id;
        std::uint64_t ui_id;
        std::chrono::steady_clock::time_point deadline;
        std::string filename;
        std::uint64_t size;
    };
    std::optional<ApprovedFile> approved;
    std::optional<ApprovedFile> requested;
    std::unique_ptr<Transfer> transfer;
    std::string filename;
    std::uint64_t expected_size = 0;
    while (!stopping_) {
        unsigned char header[4];
        if (!read_all(socket_fd_, header, sizeof(header))) break;
        const std::uint32_t size = (static_cast<std::uint32_t>(header[0]) << 24) |
                                   (static_cast<std::uint32_t>(header[1]) << 16) |
                                   (static_cast<std::uint32_t>(header[2]) << 8) | header[3];
        if (size == 0 || size > kMaxFrameBytes) {
            set_status("The file sharing transport sent an invalid frame.");
            respond(socket_fd_, false, "Invalid IPC frame length.");
            break;
        }
        std::vector<unsigned char> frame(size);
        if (!read_all(socket_fd_, frame.data(), frame.size())) break;
        if (stopping_) break;
        const unsigned char type = frame.front();
        const unsigned char *body = frame.data() + 1;
        const std::size_t body_size = frame.size() - 1;
        std::string error, reply;
        bool ok = false;
        switch (type) {
        case 'P': {
            approved.reset();
            requested.reset();
            std::lock_guard<std::mutex> lock(status_mutex_);
            pending_approval_.reset();
            resolved_approval_id_ = 0;
            resolved_approval_ = false;
            if (transfer) {
                error = "A file transfer is already active.";
                break;
            }
            if (body_size < 17 || body_size > 16 + 255) {
                error = "Invalid file confirmation request.";
                break;
            }
            const std::uint64_t transport_id = read_u64(body);
            const std::uint64_t size = read_u64(body + 8);
            const std::string name(reinterpret_cast<const char *>(body + 16), body_size - 16);
            if (transport_id == 0 || size > kMaxTransferBytes || name.empty() ||
                name.find('\0') != std::string::npos || !validate_relative_filename(name, error)) {
                if (error.empty()) error = "Invalid file confirmation request.";
                break;
            }
            if (next_approval_id_ == 0) {
                error = "The file confirmation sequence is exhausted.";
                break;
            }
            const std::uint64_t id = next_approval_id_++;
            approval_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(45);
            requested = ApprovedFile{transport_id, id, approval_deadline_, name, size};
            pending_approval_ = PendingApproval{id, name, size};
            status_ = "Confirm receiving " + name + " (" + std::to_string(size) + " bytes).";
            reply = "pending";
            ok = true;
            break;
        }
        case 'Q': {
            std::lock_guard<std::mutex> lock(status_mutex_);
            if (body_size != 8 || !requested || read_u64(body) != requested->transport_id) {
                error = "No matching file confirmation is pending.";
                break;
            }
            if (std::chrono::steady_clock::now() >= requested->deadline) {
                error = "approval-timeout: Confirmation timed out.";
                status_ = "File receiving confirmation timed out.";
            } else if (resolved_approval_id_ != requested->ui_id) {
                reply = "pending";
                ok = true;
                break;
            } else if (resolved_approval_) {
                approved = requested;
                reply = "approved";
                ok = true;
                status_ = "Approved " + requested->filename + "; preparing to receive...";
            } else {
                error = "user-rejected: File receiving was not approved.";
                status_ = "File receiving was not approved.";
            }
            requested.reset();
            pending_approval_.reset();
            resolved_approval_id_ = 0;
            resolved_approval_ = false;
            break;
        }
        case 'B': {
            const auto approval = std::move(approved);
            approved.reset();
            if (transfer) {
                error = "A file transfer is already active.";
            } else if (body_size < 81 || body_size > 80 + 255 || !approval ||
                       std::chrono::steady_clock::now() >= approval->deadline ||
                       read_u64(body) != approval->transport_id ||
                       read_u64(body + 8) != approval->size ||
                       std::string(reinterpret_cast<const char *>(body + 80), body_size - 80) != approval->filename) {
                error = "The file transfer was not approved.";
            } else {
                expected_size = read_u64(body + 8);
                filename.assign(reinterpret_cast<const char *>(body + 80), body_size - 80);
                transfer = Transfer::begin(&user, filename, expected_size,
                                           std::string_view(reinterpret_cast<const char *>(body + 16), 64),
                                           ConflictPolicy::AutoRename, error);
                ok = static_cast<bool>(transfer);
                if (ok) set_status("Receiving " + filename + "...");
            }
            break;
        }
        case 'D':
            if (!transfer) {
                error = "No file transfer is active.";
            } else if (body_size < 8) {
                error = "Invalid transfer chunk.";
            } else {
                ok = transfer->write_chunk(read_u64(body), body + 8, body_size - 8, error);
                if (ok) set_status("Receiving " + filename + "\n" +
                                   std::to_string(transfer->bytes_written()) + " / " +
                                   std::to_string(expected_size) + " bytes");
                else transfer.reset();
            }
            break;
        case 'F':
            if (body_size != 0) {
                error = "Invalid finish request.";
            } else if (!transfer) {
                error = "No file transfer is active.";
            } else {
                ok = transfer->finish(reply, error);
                transfer.reset();
                if (ok) set_status("Saved " + reply + " to Downloads.");
            }
            break;
        case 'C': {
            const bool was_pending = requested.has_value() || approved.has_value();
            approved.reset();
            requested.reset();
            {
                std::lock_guard<std::mutex> lock(status_mutex_);
                pending_approval_.reset();
                resolved_approval_id_ = 0;
                resolved_approval_ = false;
            }
            if (body_size != 0) {
                error = "Invalid cancel request.";
            } else {
                if (transfer || was_pending) {
                    transfer.reset();
                    set_status("Transfer cancelled. Ready for another file.");
                }
                ok = true;
            }
            break;
        }
        case 'S':
            if (body_size > kMaxStatusBytes ||
                std::memchr(body, 0, body_size) != nullptr) {
                error = "Invalid sharing status.";
            } else {
                set_status(std::string(reinterpret_cast<const char *>(body), body_size));
                ok = true;
            }
            break;
        default:
            error = "Unknown file sharing request.";
            break;
        }
        if (!ok && type != 'Q') set_status("Transfer failed: " + error);
        if (!respond(socket_fd_, ok, ok ? reply : error)) break;
    }
    if (transfer) {
        transfer.reset();
        if (!stopping_) set_status("The connection closed. The incomplete file was removed.\n"
                                   "Close and reopen file sharing to reconnect.");
    } else if (!stopping_) {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (status_ == "Starting Bluetooth sharing...")
            status_ = "File sharing could not start. Check the device's Bluetooth service.";
        else
            status_ += "\nClose and reopen file sharing to reconnect.";
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        pending_approval_.reset();
        resolved_approval_id_ = 0;
        resolved_approval_ = false;
    }
    shutdown(socket_fd_, SHUT_RDWR);
    active_ = false;
}

} // namespace launcher::file_share
