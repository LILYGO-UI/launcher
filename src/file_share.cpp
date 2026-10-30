#include "file_share.h"

#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <memory>
#include <sstream>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

namespace launcher::file_share {
namespace {

constexpr std::size_t kMaxFilenameBytes = 255;
constexpr std::size_t kSha256HexBytes   = 64;

void set_error(std::string &error, const char *message)
{
    error = message ? message : "file transfer failed";
}

void set_errno_error(std::string &error, const char *operation)
{
    error = operation ? operation : "file transfer failed";
    error += ": ";
    error += std::strerror(errno);
}

bool valid_absolute_path(const char *path)
{
    if (!path || path[0] != '/' || path[1] == '\0') return false;
    const std::size_t length = strnlen(path, LAUNCHER_USER_PATH_MAX);
    if (length == 0 || length >= LAUNCHER_USER_PATH_MAX) return false;
    for (std::size_t index = 0; index < length; ++index)
        if (static_cast<unsigned char>(path[index]) < 0x20U || static_cast<unsigned char>(path[index]) == 0x7fU)
            return false;
    std::size_t component_start = 1;
    for (std::size_t index = 1; index <= length; ++index) {
        if (path[index] != '/' && path[index] != '\0') continue;
        const std::size_t component_length = index - component_start;
        if ((component_length == 1 && path[component_start] == '.') ||
            (component_length == 2 && path[component_start] == '.' && path[component_start + 1] == '.'))
            return false;
        component_start = index + 1;
    }
    return true;
}

int open_directory_path(const char *path)
{
    if (!valid_absolute_path(path)) {
        errno = EINVAL;
        return -1;
    }
    int current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current < 0) return -1;

    const std::size_t length = strlen(path);
    std::size_t component_start = 1;
    for (std::size_t index = 1; index <= length; ++index) {
        if (path[index] != '/' && path[index] != '\0') continue;
        const std::size_t component_length = index - component_start;
        if (component_length == 0) {
            component_start = index + 1;
            continue;
        }
        char component[NAME_MAX + 1];
        if (component_length > NAME_MAX) {
            close(current);
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(component, path + component_start, component_length);
        component[component_length] = '\0';
        int next = openat(current, component, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) {
            int saved = errno;
            close(current);
            errno = saved;
            return -1;
        }
        close(current);
        current          = next;
        component_start  = index + 1;
    }
    return current;
}

bool safe_directory_fd(int fd, uid_t owner)
{
    struct stat metadata{};
    return fstat(fd, &metadata) == 0 && S_ISDIR(metadata.st_mode) && metadata.st_uid == owner &&
           !(metadata.st_mode & (S_IWGRP | S_IWOTH));
}

bool private_directory_fd(int fd, uid_t owner)
{
    struct stat metadata{};
    return fstat(fd, &metadata) == 0 && S_ISDIR(metadata.st_mode) && metadata.st_uid == owner &&
           (metadata.st_mode & 0777) == 0700;
}

bool parse_sha256(std::string_view text, std::array<unsigned char, 32> &digest)
{
    if (text.size() != kSha256HexBytes) return false;
    auto nibble = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (std::size_t index = 0; index < digest.size(); ++index) {
        const int high = nibble(static_cast<unsigned char>(text[index * 2]));
        const int low  = nibble(static_cast<unsigned char>(text[index * 2 + 1]));
        if (high < 0 || low < 0) return false;
        digest[index] = static_cast<unsigned char>((high << 4) | low);
    }
    return true;
}

void truncate_utf8(std::string &text, std::size_t bytes)
{
    if (text.size() <= bytes) return;
    /* Browser names are UTF-8. Never leave a partial character at the end of
     * an automatically shortened name. */
    while (bytes > 0 && (static_cast<unsigned char>(text[bytes]) & 0xc0U) == 0x80U) --bytes;
    text.resize(bytes);
}

std::string auto_name(std::string_view requested, unsigned index)
{
    std::string stem(requested);
    std::string extension;
    const std::size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        extension = stem.substr(dot);
        stem.resize(dot);
    }
    const std::string suffix = " (" + std::to_string(index) + ")";
    /* A valid name can contain a 253-byte extension. Treat that exceptional
     * case as a single stem instead of underflowing the remaining budget. */
    if (extension.size() + suffix.size() >= kMaxFilenameBytes) {
        stem.assign(requested);
        extension.clear();
    }
    const std::size_t max_stem = kMaxFilenameBytes - suffix.size() - extension.size();
    truncate_utf8(stem, max_stem);
    if (stem.empty()) {
        stem.assign(requested);
        extension.clear();
        truncate_utf8(stem, kMaxFilenameBytes - suffix.size());
    }
    return stem + suffix + extension;
}

enum class CommitResult {
    Success,
    Exists,
    Error,
};

CommitResult commit_without_replace(int directory_fd, const std::string &temporary,
                                    const std::string &target, std::string &error)
{
#if defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, directory_fd, temporary.c_str(), directory_fd, target.c_str(), 1U) == 0)
        return CommitResult::Success;
    if (errno != EEXIST && errno != ENOSYS && errno != EINVAL) {
        set_errno_error(error, "cannot atomically install downloaded file");
        return CommitResult::Error;
    }
    if (errno == EEXIST) return CommitResult::Exists;
#endif
    if (linkat(directory_fd, temporary.c_str(), directory_fd, target.c_str(), 0) == 0) {
        if (unlinkat(directory_fd, temporary.c_str(), 0) != 0) {
            set_errno_error(error, "cannot remove transfer temporary file");
            return CommitResult::Error;
        }
        return CommitResult::Success;
    }
    if (errno == EEXIST) return CommitResult::Exists;
    set_errno_error(error, "cannot atomically install downloaded file");
    return CommitResult::Error;
}

std::atomic<unsigned long long> temporary_sequence{0};

int create_temporary(int directory_fd, std::string &name, std::string &error)
{
    const unsigned long long sequence = ++temporary_sequence;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        name = ".lilygo-transfer-" + std::to_string(static_cast<unsigned long long>(getpid())) + "-" +
               std::to_string(sequence) + "-" + std::to_string(attempt) + ".tmp";
        int fd = openat(directory_fd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0) return fd;
        if (errno != EEXIST) {
            set_errno_error(error, "cannot create transfer temporary file");
            return -1;
        }
    }
    set_error(error, "cannot allocate a unique transfer temporary file");
    return -1;
}

} // namespace

class Transfer::Sha256 {
public:
    Sha256()
    {
        state_ = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
                  0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};
    }

    void update(const unsigned char *data, std::size_t length)
    {
        while (length > 0) {
            const std::size_t copy = std::min(length, block_.size() - block_length_);
            memcpy(block_.data() + block_length_, data, copy);
            block_length_ += copy;
            data += copy;
            length -= copy;
            bit_length_ += static_cast<uint64_t>(copy) * 8U;
            if (block_length_ == block_.size()) {
                transform(block_);
                block_length_ = 0;
            }
        }
    }

    std::array<unsigned char, 32> finish() const
    {
        Sha256 copy = *this;
        copy.block_[copy.block_length_++] = 0x80;
        if (copy.block_length_ > 56) {
            std::fill(copy.block_.begin() + static_cast<std::ptrdiff_t>(copy.block_length_), copy.block_.end(), 0);
            copy.transform(copy.block_);
            copy.block_length_ = 0;
        }
        std::fill(copy.block_.begin() + static_cast<std::ptrdiff_t>(copy.block_length_), copy.block_.begin() + 56, 0);
        const uint64_t bits = copy.bit_length_;
        for (unsigned index = 0; index < 8; ++index)
            copy.block_[56 + index] = static_cast<unsigned char>(bits >> (56 - 8 * index));
        copy.transform(copy.block_);

        std::array<unsigned char, 32> digest{};
        for (unsigned index = 0; index < 8; ++index) {
            digest[index * 4]     = static_cast<unsigned char>(copy.state_[index] >> 24);
            digest[index * 4 + 1] = static_cast<unsigned char>(copy.state_[index] >> 16);
            digest[index * 4 + 2] = static_cast<unsigned char>(copy.state_[index] >> 8);
            digest[index * 4 + 3] = static_cast<unsigned char>(copy.state_[index]);
        }
        return digest;
    }

private:
    static uint32_t rotate_right(uint32_t value, unsigned bits)
    {
        return (value >> bits) | (value << (32U - bits));
    }

    void transform(const std::array<unsigned char, 64> &block)
    {
        static constexpr uint32_t constants[] = {
            0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
            0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
            0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
            0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
            0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
            0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
            0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
            0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
            0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
            0xc67178f2U};
        uint32_t words[64];
        for (unsigned index = 0; index < 16; ++index)
            words[index] = (static_cast<uint32_t>(block[index * 4]) << 24) |
                           (static_cast<uint32_t>(block[index * 4 + 1]) << 16) |
                           (static_cast<uint32_t>(block[index * 4 + 2]) << 8) |
                           static_cast<uint32_t>(block[index * 4 + 3]);
        for (unsigned index = 16; index < 64; ++index) {
            const uint32_t s0 = rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18) ^
                                 (words[index - 15] >> 3);
            const uint32_t s1 = rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19) ^
                                 (words[index - 2] >> 10);
            words[index] = words[index - 16] + s0 + words[index - 7] + s1;
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
        uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (unsigned index = 0; index < 64; ++index) {
            const uint32_t s1  = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const uint32_t ch  = (e & f) ^ ((~e) & g);
            const uint32_t t1  = h + s1 + ch + constants[index] + words[index];
            const uint32_t s0  = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2  = s0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        state_[0] += a;
        state_[1] += b;
        state_[2] += c;
        state_[3] += d;
        state_[4] += e;
        state_[5] += f;
        state_[6] += g;
        state_[7] += h;
    }

    std::array<uint32_t, 8> state_{};
    std::array<unsigned char, 64> block_{};
    std::size_t block_length_ = 0;
    uint64_t bit_length_      = 0;
};

bool validate_relative_filename(std::string_view filename, std::string &error)
{
    if (filename.empty() || filename.size() > kMaxFilenameBytes) {
        set_error(error, "file name must contain 1 to 255 bytes");
        return false;
    }
    if (filename.front() == '/' || filename.back() == '/' || filename.find('/') != std::string_view::npos ||
        filename == "." || filename == "..") {
        set_error(error, "file name must be one relative path component");
        return false;
    }
    for (const unsigned char character : filename) {
        if (character == 0 || character < 0x20U || character == 0x7fU || character == '\\') {
            set_error(error, "file name contains an unsafe character");
            return false;
        }
    }
    error.clear();
    return true;
}

bool resolve_download_directory(const launcher_user_t *user, std::string &path, std::string &error)
{
    path.clear();
    if (!user || user->uid == 0 || user->gid == 0 || !valid_absolute_path(user->home)) {
        set_error(error, "signed-in user has an unsafe home directory");
        return false;
    }
    if (geteuid() != 0 && geteuid() != user->uid) {
        set_error(error, "file transfer is not running as the signed-in user");
        return false;
    }

    const int home_fd = open_directory_path(user->home);
    if (home_fd < 0) {
        set_errno_error(error, "cannot open signed-in user home");
        return false;
    }
    if (!safe_directory_fd(home_fd, user->uid)) {
        close(home_fd);
        set_error(error, "signed-in user home is unavailable or unsafe");
        return false;
    }

    if (mkdirat(home_fd, "Downloads", 0700) != 0 && errno != EEXIST) {
        set_errno_error(error, "cannot create Downloads directory");
        close(home_fd);
        return false;
    }
    const int downloads_fd = openat(home_fd, "Downloads", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (downloads_fd < 0) {
        set_errno_error(error, "cannot open Downloads directory");
        close(home_fd);
        return false;
    }
    if (geteuid() == 0) {
        struct stat metadata{};
        if (fstat(downloads_fd, &metadata) != 0 ||
            ((metadata.st_uid != user->uid || metadata.st_gid != user->gid) &&
             fchown(downloads_fd, user->uid, user->gid) != 0)) {
            set_errno_error(error, "cannot assign Downloads directory to signed-in user");
            close(downloads_fd);
            close(home_fd);
            return false;
        }
    }
    struct stat metadata{};
    bool safe = fstat(downloads_fd, &metadata) == 0 && S_ISDIR(metadata.st_mode) && metadata.st_uid == user->uid;
    if (safe && (metadata.st_mode & 0777) != 0700) safe = fchmod(downloads_fd, 0700) == 0;
    safe = safe && private_directory_fd(downloads_fd, user->uid);
    close(downloads_fd);
    close(home_fd);
    if (!safe) {
        set_error(error, "Downloads directory is unavailable or unsafe");
        return false;
    }
    path = std::string(user->home) + "/Downloads";
    error.clear();
    return true;
}

Transfer::Transfer(int directory_fd, std::string temporary_name, std::string requested_name,
                   std::optional<uint64_t> expected_size, std::array<unsigned char, 32> expected_digest,
                   ConflictPolicy conflict_policy)
    : directory_fd_(directory_fd), temporary_name_(std::move(temporary_name)), requested_name_(std::move(requested_name)),
      expected_size_(expected_size), expected_digest_(expected_digest), conflict_policy_(conflict_policy),
      digest_(std::make_unique<Sha256>())
{
}

Transfer::~Transfer()
{
    cancel();
}

std::unique_ptr<Transfer> Transfer::begin(const launcher_user_t *user, std::string_view filename,
                                          std::optional<uint64_t> expected_size, std::string_view expected_sha256,
                                          ConflictPolicy conflict_policy, std::string &error)
{
    std::string directory_path;
    if (!resolve_download_directory(user, directory_path, error) || !validate_relative_filename(filename, error)) return nullptr;
    std::array<unsigned char, 32> expected_digest{};
    if (!parse_sha256(expected_sha256, expected_digest)) {
        set_error(error, "expected SHA-256 must contain 64 hexadecimal digits");
        return nullptr;
    }
    if (expected_size && *expected_size > kMaxTransferBytes) {
        set_error(error, "declared file is larger than the transfer limit");
        return nullptr;
    }
    int directory_fd = open_directory_path(directory_path.c_str());
    if (directory_fd < 0) {
        set_errno_error(error, "cannot open Downloads directory");
        return nullptr;
    }
    if (!private_directory_fd(directory_fd, user->uid)) {
        close(directory_fd);
        set_error(error, "Downloads directory is unavailable or unsafe");
        return nullptr;
    }
    std::string temporary_name;
    const int file_fd = create_temporary(directory_fd, temporary_name, error);
    if (file_fd < 0) {
        close(directory_fd);
        return nullptr;
    }
    if (geteuid() == 0 && (fchown(file_fd, user->uid, user->gid) != 0 || fchmod(file_fd, 0600) != 0)) {
        set_errno_error(error, "cannot set downloaded file ownership");
        close(file_fd);
        unlinkat(directory_fd, temporary_name.c_str(), 0);
        close(directory_fd);
        return nullptr;
    }
    auto transfer = std::unique_ptr<Transfer>(new Transfer(directory_fd, std::move(temporary_name),
                                                            std::string(filename), expected_size, expected_digest,
                                                            conflict_policy));
    transfer->file_fd_ = file_fd;
    error.clear();
    return transfer;
}

bool Transfer::write_chunk(const void *data, std::size_t size, std::string &error)
{
    return write_chunk(next_sequence_, data, size, error);
}

bool Transfer::write_chunk(std::uint64_t sequence, const void *data, std::size_t size, std::string &error)
{
    if (completed_ || cancelled_ || file_fd_ < 0) {
        set_error(error, "transfer is no longer writable");
        return false;
    }
    if (sequence != next_sequence_) {
        set_error(error, "transfer chunk sequence is out of order");
        cancel();
        return false;
    }
    if (size == 0) {
        ++next_sequence_;
        error.clear();
        return true;
    }
    if (!data) {
        set_error(error, "transfer chunk is null");
        cancel();
        return false;
    }
    if (size > kMaxChunkBytes) {
        set_error(error, "transfer chunk exceeds the 1 MiB limit");
        cancel();
        return false;
    }
    if (bytes_written_ > kMaxTransferBytes || size > kMaxTransferBytes - bytes_written_) {
        set_error(error, "received file exceeds the transfer limit");
        cancel();
        return false;
    }
    if (expected_size_ && (bytes_written_ > *expected_size_ || size > *expected_size_ - bytes_written_)) {
        set_error(error, "received more bytes than declared");
        cancel();
        return false;
    }
    const unsigned char *cursor = static_cast<const unsigned char *>(data);
    std::size_t left            = size;
    while (left > 0) {
        const ssize_t written = write(file_fd_, cursor, left);
        if (written > 0) {
            digest_->update(cursor, static_cast<std::size_t>(written));
            cursor += written;
            left -= static_cast<std::size_t>(written);
            bytes_written_ += static_cast<std::size_t>(written);
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            set_errno_error(error, "cannot write transfer chunk");
            cancel();
            return false;
        }
    }
    ++next_sequence_;
    error.clear();
    return true;
}

bool Transfer::finish(std::string &final_name, std::string &error)
{
    final_name.clear();
    if (completed_) {
        set_error(error, "transfer is already complete");
        return false;
    }
    if (cancelled_ || file_fd_ < 0) {
        set_error(error, "transfer was cancelled");
        return false;
    }
    if (expected_size_ && bytes_written_ != *expected_size_) {
        set_error(error, "received file size does not match the declared size");
        cancel();
        return false;
    }
    if (fsync(file_fd_) != 0) {
        set_errno_error(error, "cannot flush downloaded file");
        cancel();
        return false;
    }
    if (close(file_fd_) != 0) {
        file_fd_ = -1;
        set_errno_error(error, "cannot close downloaded file");
        cancel();
        return false;
    }
    file_fd_ = -1;
    if (digest_->finish() != expected_digest_) {
        set_error(error, "downloaded file SHA-256 does not match the declared digest");
        cancel();
        return false;
    }

    auto existing_type = [&](const std::string &name, bool &exists, bool &symlink) {
        struct stat metadata{};
        if (fstatat(directory_fd_, name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) == 0) {
            exists  = true;
            symlink = S_ISLNK(metadata.st_mode);
            return true;
        }
        if (errno == ENOENT) {
            exists  = false;
            symlink = false;
            return true;
        }
        set_errno_error(error, "cannot inspect destination file");
        return false;
    };

    std::string target = requested_name_;
    if (conflict_policy_ == ConflictPolicy::Replace) {
        bool exists = false, symlink = false;
        if (!existing_type(target, exists, symlink)) {
            cancel();
            return false;
        }
        if (symlink) {
            set_error(error, "destination is a symbolic link");
            cancel();
            return false;
        }
        if (renameat(directory_fd_, temporary_name_.c_str(), directory_fd_, target.c_str()) != 0) {
            set_errno_error(error, "cannot install downloaded file");
            cancel();
            return false;
        }
    } else {
        const unsigned max_attempts = conflict_policy_ == ConflictPolicy::AutoRename ? 10000U : 1U;
        bool committed = false;
        for (unsigned attempt = 0; attempt < max_attempts; ++attempt) {
            target = attempt == 0 ? requested_name_ : auto_name(requested_name_, attempt);
            const CommitResult result = commit_without_replace(directory_fd_, temporary_name_, target, error);
            if (result == CommitResult::Success) {
                committed = true;
                break;
            }
            if (result == CommitResult::Error || conflict_policy_ == ConflictPolicy::Fail) break;
        }
        if (!committed) {
            if (error.empty()) set_error(error, "destination file already exists");
            cancel();
            return false;
        }
    }
    if (fsync(directory_fd_) != 0) {
        set_errno_error(error, "cannot flush Downloads directory");
        close(directory_fd_);
        directory_fd_ = -1;
        completed_ = true;
        final_name.clear();
        return false;
    }
    temporary_name_.clear();
    close(directory_fd_);
    directory_fd_ = -1;
    completed_ = true;
    final_name = std::move(target);
    error.clear();
    return true;
}

void Transfer::cancel()
{
    if (completed_ || cancelled_) return;
    if (file_fd_ >= 0) {
        close(file_fd_);
        file_fd_ = -1;
    }
    if (directory_fd_ >= 0 && !temporary_name_.empty()) {
        unlinkat(directory_fd_, temporary_name_.c_str(), 0);
        temporary_name_.clear();
    }
    if (directory_fd_ >= 0) {
        close(directory_fd_);
        directory_fd_ = -1;
    }
    cancelled_ = true;
}

} // namespace launcher::file_share
