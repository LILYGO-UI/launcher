#include "power_button.h"

#include "logging.h"

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr size_t kBitsPerByte = 8;

bool bit_is_set(const unsigned char *bits, size_t size, unsigned int bit)
{
    const size_t byte = bit / kBitsPerByte;
    return byte < size && (bits[byte] & (1U << (bit % kBitsPerByte))) != 0;
}

bool has_power_key(int fd)
{
    unsigned char key_bits[(KEY_MAX + kBitsPerByte) / kBitsPerByte]{};
    const int bytes = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);
    return bytes > 0 && bit_is_set(key_bits, static_cast<size_t>(bytes), KEY_POWER);
}

bool event_node(const char *name)
{
    if (!name || strncmp(name, "event", 5) != 0 || !name[5]) return false;
    for (const char *cursor = name + 5; *cursor; ++cursor)
        if (*cursor < '0' || *cursor > '9') return false;
    return true;
}

void add_directory_entries(const char *directory, std::vector<std::string> *paths)
{
    DIR *handle = opendir(directory);
    if (!handle) return;
    while (dirent *entry = readdir(handle)) {
        const bool by_path = strcmp(directory, "/dev/input/by-path") == 0;
        if ((!by_path && !event_node(entry->d_name)) || (by_path && !strstr(entry->d_name, "event"))) continue;
        paths->emplace_back(std::string(directory) + "/" + entry->d_name);
    }
    closedir(handle);
}

int open_power_device(const char *path)
{
    if (!path || !path[0]) return -1;
    const int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0 || !has_power_key(fd)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

#endif

PowerButtonMonitor::~PowerButtonMonitor()
{
    close();
}

bool PowerButtonMonitor::open(const char *path)
{
    close();

#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    if (path) {
        fd_ = open_power_device(path);
        if (fd_ < 0) launcher::logging::warn("cannot open power button event device '%s'", path);
    } else {
        std::vector<std::string> paths;
        add_directory_entries("/dev/input/by-path", &paths);
        add_directory_entries("/dev/input", &paths);
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        for (const std::string &candidate : paths) {
            fd_ = open_power_device(candidate.c_str());
            if (fd_ >= 0) {
                launcher::logging::info("power button event device: %s", candidate.c_str());
                break;
            }
        }
    }
#else
    (void)path;
#endif
    return fd_ >= 0;
}

void PowerButtonMonitor::close()
{
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    if (fd_ >= 0) ::close(fd_);
#endif
    fd_            = -1;
    last_press_ms_ = 0;
    key_active_    = false;
}

unsigned int PowerButtonMonitor::poll(uint32_t now_ms)
{
#if defined(__linux__) && !defined(LAUNCHER_SIMULATOR)
    constexpr uint32_t kReleaseGapMs = 300;
    if (fd_ < 0) return POWER_BUTTON_EVENT_NONE;
    unsigned int events = POWER_BUTTON_EVENT_NONE;
    for (;;) {
        input_event event{};
        const ssize_t bytes = read(fd_, &event, sizeof(event));
        if (bytes == static_cast<ssize_t>(sizeof(event))) {
            if (event.type == EV_KEY && event.code == KEY_POWER) {
                if (event.value != 0) {
                    // A delayed poll can find the first pulse of a new tap
                    // already queued after the previous sequence ended. End
                    // that sequence before starting the new one so the UI
                    // receives a release even when the main loop was busy.
                    if (key_active_ && static_cast<int32_t>(now_ms - last_press_ms_) >= (int32_t)kReleaseGapMs) {
                        key_active_ = false;
                        events |= POWER_BUTTON_EVENT_RELEASED;
                    }
                    const bool was_active = key_active_;
                    last_press_ms_        = now_ms;
                    key_active_           = true;
                    if (!was_active) events |= POWER_BUTTON_EVENT_PRESSED;
                }
            }
            continue;
        }
        if (bytes < 0 && errno == EINTR) continue;
        if (bytes < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (bytes == 0 || (bytes < 0 && (errno == ENODEV || errno == EIO))) close();
        break;
    }
    if (key_active_ && static_cast<int32_t>(now_ms - last_press_ms_) >= (int32_t)kReleaseGapMs) {
        key_active_ = false;
        events |= POWER_BUTTON_EVENT_RELEASED;
    }
    return events;
#else
    (void)now_ms;
    return POWER_BUTTON_EVENT_NONE;
#endif
}
