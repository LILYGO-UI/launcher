#ifndef LILYGO_UI_LAUNCHER_FILE_SHARE_H
#define LILYGO_UI_LAUNCHER_FILE_SHARE_H

#include "user.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace launcher::file_share {

inline constexpr std::uint64_t kMaxTransferBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxChunkBytes = 1024ULL * 1024ULL;

enum class ConflictPolicy {
    Fail,
    Replace,
    AutoRename,
};

/* Resolve and, when necessary, create the signed-in user's private download
 * directory.  The path is always <user.home>/Downloads; callers must not
 * substitute HOME inherited by the root launcher service. */
bool resolve_download_directory(const launcher_user_t *user, std::string &path, std::string &error);

/* File names received from a browser are deliberately limited to one path
 * component.  This keeps the transfer API independent of directory traversal
 * and makes the final rename safe even when a peer sends hostile input. */
bool validate_relative_filename(std::string_view filename, std::string &error);

class Transfer {
public:
    static std::unique_ptr<Transfer> begin(const launcher_user_t *user, std::string_view filename,
                                           std::optional<uint64_t> expected_size,
                                           std::string_view expected_sha256,
                                           ConflictPolicy conflict_policy, std::string &error);

    ~Transfer();
    Transfer(const Transfer &) = delete;
    Transfer &operator=(const Transfer &) = delete;

    bool write_chunk(const void *data, std::size_t size, std::string &error);
    /* Transport adapters may provide the DataChannel sequence number. Chunks
     * must arrive in order; the convenience overload below assigns the next
     * sequence number for adapters that already serialize delivery. */
    bool write_chunk(std::uint64_t sequence, const void *data, std::size_t size, std::string &error);
    bool finish(std::string &final_name, std::string &error);
    void cancel();

    uint64_t bytes_written() const { return bytes_written_; }
    uint64_t next_sequence() const { return next_sequence_; }
    bool completed() const { return completed_; }

private:
    Transfer(int directory_fd, std::string temporary_name, std::string requested_name,
             std::optional<uint64_t> expected_size, std::array<unsigned char, 32> expected_digest,
             ConflictPolicy conflict_policy);

    int directory_fd_ = -1;
    int file_fd_      = -1;
    std::string temporary_name_;
    std::string requested_name_;
    std::optional<uint64_t> expected_size_;
    std::array<unsigned char, 32> expected_digest_{};
    ConflictPolicy conflict_policy_ = ConflictPolicy::Fail;
    uint64_t bytes_written_          = 0;
    uint64_t next_sequence_          = 0;
    bool completed_                  = false;
    bool cancelled_                  = false;

    class Sha256;
    std::unique_ptr<Sha256> digest_;
};

} // namespace launcher::file_share

#endif
