#include "setup_flow.h"

#include <cerrno>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace launcher {
namespace {
bool safe_directory(const std::string &path)
{
    struct stat state{};
    return lstat(path.c_str(), &state) == 0 && S_ISDIR(state.st_mode) &&
           state.st_uid == geteuid() && !(state.st_mode & (S_IWGRP | S_IWOTH));
}
bool safe_value(const std::string &value)
{
    return !value.empty() && value.size() < 128 && value.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_./@+-") == std::string::npos &&
        value.find("..") == std::string::npos;
}
bool valid_region(const SetupRegion &r)
{
    return safe_value(r.country) && safe_value(r.locale) && safe_value(r.timezone) && safe_value(r.keyboard);
}
bool fail(std::string &error, const char *message) { error = message; return false; }
bool sync_directory(const std::string &directory)
{
    int fd = open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}
}
SetupFlow::SetupFlow(std::string directory, bool simulated)
    : directory_(std::move(directory)), simulated_(simulated) {}

bool SetupFlow::initialize(bool required, bool account_saved, std::string &error)
{
    error.clear();
    active_ = required;
    stage_ = required && account_saved ? SetupStage::Network : SetupStage::Region;
    region_ = SetupRegion{};
    if (simulated_) return true;
    struct stat directory_state{};
    if (lstat(directory_.c_str(), &directory_state) != 0 && errno == ENOENT && !required) return true;
    if (!safe_directory(directory_)) return fail(error, "Setup state directory is unavailable or unsafe");
    const std::string path = directory_ + "/first-boot-progress";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        struct stat metadata{};
        if (fstat(fd, &metadata) || !S_ISREG(metadata.st_mode) || metadata.st_uid != geteuid() ||
            (metadata.st_mode & 07777) != 0600 || metadata.st_size > 1024) {
            close(fd); return fail(error, "Setup progress has unsafe permissions or contents");
        }
        char buffer[1025]{};
        size_t count = 0;
        bool readable = true;
        while (count < sizeof(buffer)) {
            const ssize_t amount = read(fd, buffer + count, sizeof(buffer) - count);
            if (amount < 0 && errno == EINTR) continue;
            if (amount < 0) { readable = false; break; }
            if (amount == 0) break;
            count += static_cast<size_t>(amount);
        }
        close(fd);
        if (!readable || count > 1024) return fail(error, "Cannot read setup progress");
        std::istringstream input(std::string(buffer, count));
        int version = 0, stage = -1;
        std::string extra;
        if (!(input >> version >> stage >> region_.country >> region_.locale >> region_.timezone >> region_.keyboard) ||
            (input >> extra) || version != 1 || stage < 0 || stage > 4 || !valid_region(region_))
            return fail(error, "Setup progress is invalid");
        active_ = true;
        stage_ = static_cast<SetupStage>(stage);
        if (stage_ == SetupStage::Account && account_saved) stage_ = SetupStage::Network;
        if (stage_ > SetupStage::Account && !account_saved)
            return fail(error, "Setup account is missing; repair account state before continuing");
    } else if (errno != ENOENT) return fail(error, "Cannot read setup progress safely");
    if (active_) return save(error);
    return true;
}
bool SetupFlow::save(std::string &error)
{
    if (simulated_) return true;
    if (!safe_directory(directory_)) return fail(error, "Setup state directory is unsafe");
    std::string temporary = directory_ + "/.setup-progress-XXXXXX";
    int fd = mkstemp(temporary.data());
    if (fd < 0) return fail(error, "Cannot save setup progress");
    const std::string contents = "1\n" + std::to_string(static_cast<int>(stage_)) + "\n" + region_.country + "\n" +
        region_.locale + "\n" + region_.timezone + "\n" + region_.keyboard + "\n";
    size_t written = 0;
    bool ok = fchmod(fd, 0600) == 0;
    while (ok && written < contents.size()) {
        ssize_t count = write(fd, contents.data() + written, contents.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) ok = false;
        else written += static_cast<size_t>(count);
    }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd)) ok = false;
    if (ok) ok = rename(temporary.c_str(), (directory_ + "/first-boot-progress").c_str()) == 0;
    unlink(temporary.c_str());
    if (ok) ok = sync_directory(directory_);
    return ok || fail(error, "Cannot commit setup progress");
}
bool SetupFlow::advance(SetupStage stage, const SetupRegion &region, std::string &error)
{
    error.clear();
    const int requested = static_cast<int>(stage);
    const int current = static_cast<int>(stage_);
    if (!active_ || !valid_region(region) || requested < 0 || requested > 4 ||
        (requested != current && requested != current + 1 &&
         !(stage_ == SetupStage::Update && stage == SetupStage::Network)))
        return fail(error, "Invalid setup transition");
    const auto old_stage = stage_; const auto old_region = region_;
    stage_ = stage; region_ = region;
    if (save(error)) return true;
    stage_ = old_stage; region_ = old_region;
    return false;
}
bool SetupFlow::finish(std::string &error)
{
    error.clear();
    if (!active_ || stage_ != SetupStage::Complete) return fail(error, "Complete every setup step first");
    if (!simulated_) {
        if (!safe_directory(directory_)) return fail(error, "Setup state directory is unsafe");
        if (unlink((directory_ + "/first-boot-progress").c_str()) && errno != ENOENT)
            return fail(error, "Cannot finish setup");
        if (!sync_directory(directory_)) return fail(error, "Cannot commit setup completion");
    }
    active_ = false;
    return true;
}
}
