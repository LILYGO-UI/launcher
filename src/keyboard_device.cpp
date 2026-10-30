#include "keyboard_device.h"
#include "logging.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#if defined(__linux__)
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#endif

namespace launcher {
namespace {

bool i2c_client(const char *name, unsigned int &bus, unsigned int &address)
{
    if (*name < '0' || *name > '9') return false;
    bus = 0;
    while (*name >= '0' && *name <= '9') {
        const unsigned int digit = static_cast<unsigned int>(*name++ - '0');
        if (bus > (INT_MAX - digit) / 10U) return false;
        bus = bus * 10U + digit;
    }
    if (*name != '-' || std::strlen(name + 1) != 4) return false;
    ++name;
    address = 0;
    for (; *name; ++name) {
        const unsigned char character = static_cast<unsigned char>(*name);
        if (!std::isxdigit(character)) return false;
        address = address * 16U + (character <= '9' ? character - '0' : std::tolower(character) - 'a' + 10);
    }
    return address >= 0x03 && address <= 0x77;
}

bool tca8418_name(std::string name)
{
    for (char &character : name)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

    // The in-kernel driver uses the I2C client's name, "tca8418".
    // Also accept descriptive names such as "TCA8418 Keypad", but require
    // token boundaries so similarly named devices do not match.
    constexpr size_t token_size = sizeof("tca8418") - 1;
    for (size_t position = name.find("tca8418"); position != std::string::npos;
         position = name.find("tca8418", position + token_size)) {
        const size_t end = position + token_size;
        if ((position == 0 || !std::isalnum(static_cast<unsigned char>(name[position - 1]))) &&
            (end == name.size() || !std::isalnum(static_cast<unsigned char>(name[end]))))
            return true;
    }
    return false;
}

int read_configuration(const char *device, unsigned int address)
{
#if defined(__linux__)
    const int fd = open(device, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;

    // Read only CFG. Reading KEY_EVENT_A would consume queued key presses.
    // One combined transfer holds the adapter lock across both messages, so
    // the kernel driver's own register accesses cannot change our pointer.
    unsigned char reg = 0x01;
    unsigned char configuration = 0;
    i2c_msg messages[2]{};
    messages[0].addr = static_cast<__u16>(address);
    messages[0].len = 1;
    messages[0].buf = &reg;
    messages[1].addr = static_cast<__u16>(address);
    messages[1].flags = I2C_M_RD;
    messages[1].len = 1;
    messages[1].buf = &configuration;
    i2c_rdwr_ioctl_data request{messages, 2};
    int result;
    do {
        result = ioctl(fd, I2C_RDWR, &request);
    } while (result < 0 && errno == EINTR);
    close(fd);
    return result == 2 ? configuration : -1;
#else
    (void)device;
    (void)address;
    return -1;
#endif
}

// Return 0 for no binding, 1 for this driver, and -1 for foreign or unreadable
// bindings. Resolve relative sysfs symlinks before comparing exact drivers.
int driver_binding(const std::string &client_path, const char *driver_directory)
{
    const std::string binding = client_path + "/driver";
    struct stat status{};
    if (lstat(binding.c_str(), &status) < 0) {
        if (errno == ENOENT) return 0;
        logging::warn("cannot inspect TCA8418 driver binding '%s': %s", binding.c_str(), std::strerror(errno));
        return -1;
    }
    if (!S_ISLNK(status.st_mode)) return -1;

    char actual[PATH_MAX], expected[PATH_MAX];
    // A sysfs driver link can disappear between lstat() and realpath() while
    // the kernel is unbinding or removing the device.  Treat that as a
    // transient/unusable binding and let the next poll retry discovery.  In
    // particular, do not turn the normal hotplug race into a warning every
    // 500 ms.
    if (!realpath(binding.c_str(), actual)) {
        if (errno == ENOENT) return -1;
        logging::warn("cannot resolve TCA8418 driver binding '%s': %s", binding.c_str(), std::strerror(errno));
        return -1;
    }
    if (!realpath(driver_directory, expected)) {
        if (errno == ENOENT) return -1;
        logging::warn("cannot resolve TCA8418 driver binding '%s': %s", binding.c_str(), std::strerror(errno));
        return -1;
    }
    return std::strcmp(actual, expected) == 0 ? 1 : -1;
}

bool write_driver_control(const char *driver_directory, const char *command, const char *client)
{
    const std::string control = std::string(driver_directory) + "/" + command;
    // Do not create a missing control file. sysfs accepts one complete client
    // identifier per write; a short write is a failed command, not a stream.
    const int fd = open(control.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        logging::warn("cannot %s TCA8418 keyboard %s: %s", command, client, std::strerror(errno));
        return false;
    }
    const size_t length = std::strlen(client);
    ssize_t written;
    do {
        written = write(fd, client, length);
    } while (written < 0 && errno == EINTR);
    int error = written < 0 ? errno : (static_cast<size_t>(written) == length ? 0 : EIO);
    if (close(fd) < 0 && error == 0) error = errno;
    if (error) {
        logging::warn("cannot %s TCA8418 keyboard %s: %s", command, client, std::strerror(error));
        return false;
    }
    return true;
}

void recover_keyboard(const std::string &client_path, const char *client, const char *driver_directory,
                      int configuration)
{
    const int binding = driver_binding(client_path, driver_directory);
    if (binding < 0) return;
    // The kernel enables INT_CFG, OVR_FLOW_IEN and KE_IEN during probe.
    // A power-cycled keyboard resets CFG to zero without removing its I2C
    // client or evdev node. Do not disturb an already initialized device.
    constexpr int required_configuration = 0x19;
    if (binding == 1 && (configuration & required_configuration) == required_configuration) return;
    if (binding == 1 && !write_driver_control(driver_directory, "unbind", client)) return;
    if (!write_driver_control(driver_directory, "bind", client)) return;
    logging::info("TCA8418 keyboard %s initialized after connection or configuration reset", client);
}

} // namespace

bool update_tca8418_keyboard(const char *i2c_directory, const char *device_directory,
                            const char *driver_directory, int (*read_config)(const char *, unsigned int))
{
    if (!i2c_directory || !i2c_directory[0] || !device_directory || !device_directory[0] ||
        !driver_directory || !driver_directory[0]) return false;
    DIR *directory = opendir(i2c_directory);
    if (!directory) return false;
    if (!read_config) read_config = read_configuration;

    bool present = false;
    while (dirent *entry = readdir(directory)) {
        unsigned int bus = 0, address = 0;
        if (!i2c_client(entry->d_name, bus, address)) continue;
        const std::string client_path = std::string(i2c_directory) + "/" + entry->d_name;
        std::ifstream input(client_path + "/name");
        std::string name;
        if (!std::getline(input, name) || !tca8418_name(name)) continue;
        const std::string device = std::string(device_directory) + "/i2c-" + std::to_string(bus);
        const int configuration = read_config(device.c_str(), address);
        if (configuration < 0) continue;
        present = true;
        recover_keyboard(client_path, entry->d_name, driver_directory, configuration);
    }
    closedir(directory);
    return present;
}

} // namespace launcher
