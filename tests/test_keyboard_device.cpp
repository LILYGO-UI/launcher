#include "keyboard_device.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
using ProbeCall = std::pair<std::string, unsigned int>;
std::vector<ProbeCall> calls;
int configuration = -1;

int read_configuration(const char *device, unsigned int address)
{
    calls.emplace_back(device, address);
    return configuration;
}

struct I2cDirectory {
    char directory[64] = "/tmp/lilygo-keyboard-test-XXXXXX";
    fs::path driver;

    I2cDirectory()
    {
        assert(mkdtemp(directory) != nullptr);
        calls.clear();
        configuration = -1;
        driver = fs::path(directory) / "drivers/tca8418_keypad";
        fs::create_directories(driver);
        clear_controls();
    }
    ~I2cDirectory() { fs::remove_all(directory); }

    void add(const char *client, const char *name)
    {
        const fs::path device = fs::path(directory) / client;
        fs::create_directories(device);
        std::ofstream output(device / "name");
        output << name << '\n';
        assert(output.good());
    }

    void bind(const char *client, fs::path target = {})
    {
        const fs::path client_path = fs::path(directory) / client;
        fs::create_directory_symlink(fs::relative(target.empty() ? driver : target, client_path),
                                     client_path / "driver");
    }

    void clear_controls() const
    {
        for (const char *command : {"bind", "unbind"}) {
            std::ofstream output(driver / command, std::ios::trunc);
            assert(output.good());
        }
    }

    std::string control(const char *command) const
    {
        std::ifstream input(driver / command);
        assert(input.good());
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    void assert_untouched() const
    {
        assert(control("bind").empty());
        assert(control("unbind").empty());
    }

    bool update() const
    {
        return launcher::update_tca8418_keyboard(directory, "/test/dev", driver.c_str(), read_configuration);
    }
};

void absent_or_unrelated_devices()
{
    I2cDirectory i2c;
    configuration = 0;
    assert(!i2c.update());
    assert(!launcher::update_tca8418_keyboard(nullptr, "/test/dev", i2c.driver.c_str(), read_configuration));
    assert(!launcher::update_tca8418_keyboard("", "/test/dev", i2c.driver.c_str(), read_configuration));
    assert(!launcher::update_tca8418_keyboard(i2c.directory, nullptr, i2c.driver.c_str(), read_configuration));
    assert(!launcher::update_tca8418_keyboard(i2c.directory, "", i2c.driver.c_str(), read_configuration));
    assert(!launcher::update_tca8418_keyboard(i2c.directory, "/test/dev", nullptr, read_configuration));
    assert(!launcher::update_tca8418_keyboard(i2c.directory, "/test/dev", "", read_configuration));
    assert(!launcher::update_tca8418_keyboard((fs::path(i2c.directory) / "missing").c_str(), "/test/dev",
                                             i2c.driver.c_str(), read_configuration));

    i2c.add("1-0034", "USB Keyboard");
    i2c.add("2-0034", "xtca8418");
    i2c.add("3-0034", "tca84180");
    for (const char *client : {"event4", "i2c-4", "4-34", "4-00034", "4-0034-extra", "-1-0034",
                               "4x-0034", "4-00xz", "4-0080", "4-0000", "4-0078",
                               "999999999999999999999-0034", "4"})
        i2c.add(client, "tca8418");
    fs::create_directories(fs::path(i2c.directory) / "5-0034");
    fs::create_symlink("missing-device", fs::path(i2c.directory) / "6-0034");
    assert(!i2c.update());
    assert(calls.empty());
    i2c.assert_untouched();
}

void recognized_names_and_physical_connection()
{
    I2cDirectory i2c;
    for (const char *name : {"tca8418", "TCA8418", "  TCA8418 Keypad  ", "tca8418_keypad"}) {
        i2c.add("10-0034", name);
        i2c.bind("10-0034");
        // The declared I2C client remains present even when its chip is unplugged.
        configuration = -1;
        assert(!i2c.update());
        configuration = 0x19;
        assert(i2c.update());
        configuration = -1;
        assert(!i2c.update());
        configuration = 0x19;
        assert(i2c.update());
        assert(calls.back() == ProbeCall("/test/dev/i2c-10", 0x34));
        fs::remove_all(fs::path(i2c.directory) / "10-0034");
        assert(!i2c.update());
        i2c.assert_untouched();
    }
    assert(calls.size() == 16);
}

void sysfs_symlink_is_followed_without_driver_binding()
{
    I2cDirectory i2c;
    i2c.add("client", "tca8418");
    fs::create_directory_symlink("client", fs::path(i2c.directory) / "2-0034");
    configuration = 0;
    assert(i2c.update());
    assert(calls.back() == ProbeCall("/test/dev/i2c-2", 0x34));
    assert(i2c.control("bind") == "2-0034");
    assert(i2c.control("unbind").empty());
    // Model the driver's successful probe and subsequent healthy polls.
    i2c.bind("client");
    configuration = 0x19;
    i2c.clear_controls();
    assert(i2c.update());
    i2c.assert_untouched();
    fs::remove(fs::path(i2c.directory) / "2-0034");
    assert(!i2c.update());
}

void reset_bound_keyboard_is_reinitialized()
{
    I2cDirectory i2c;
    i2c.add("1-0034", "tca8418");
    i2c.bind("1-0034");
    for (const int missing_configuration : {0, 0x18, 0x11, 0x09}) {
        configuration = missing_configuration;
        i2c.clear_controls();
        assert(i2c.update());
        assert(i2c.control("unbind") == "1-0034");
        assert(i2c.control("bind") == "1-0034");
        // Recovery does not become a recurring rebind once configured.
        configuration = 0x19;
        i2c.clear_controls();
        assert(i2c.update());
        i2c.assert_untouched();
    }
    // Additional configuration bits are not evidence of a power reset.
    configuration = 0x99;
    assert(i2c.update());
    i2c.assert_untouched();
    configuration = -1;
    assert(!i2c.update());
    i2c.assert_untouched();
}

void failed_recovery_is_retried_without_losing_physical_presence()
{
    I2cDirectory i2c;
    i2c.add("1-0034", "tca8418");
    i2c.bind("1-0034");
    configuration = 0;

    // Never bind after a failed unbind, and never create a missing control.
    fs::remove(i2c.driver / "unbind");
    assert(i2c.update());
    assert(!fs::exists(i2c.driver / "unbind"));
    assert(i2c.control("bind").empty());
    i2c.clear_controls();
    assert(i2c.update());
    assert(i2c.control("unbind") == "1-0034");
    assert(i2c.control("bind") == "1-0034");

    // Model unbind succeeding while bind fails, followed by another poll.
    i2c.clear_controls();
    fs::remove(i2c.driver / "bind");
    assert(i2c.update());
    assert(i2c.control("unbind") == "1-0034");
    assert(!fs::exists(i2c.driver / "bind"));
    fs::remove(fs::path(i2c.directory) / "1-0034/driver");
    i2c.clear_controls();
    // A probe may configure the chip before failing to register evdev. No
    // binding is sufficient reason to retry even when CFG is already normal.
    configuration = 0x19;
    assert(i2c.update());
    assert(i2c.control("unbind").empty());
    assert(i2c.control("bind") == "1-0034");
}

void foreign_or_unreadable_bindings_are_left_alone()
{
    I2cDirectory i2c;
    i2c.add("1-0034", "tca8418");
    const fs::path foreign = fs::path(i2c.directory) / "other/tca8418_keypad";
    fs::create_directories(foreign);
    i2c.bind("1-0034", foreign);
    configuration = 0;
    assert(i2c.update());
    i2c.assert_untouched();

    const fs::path binding = fs::path(i2c.directory) / "1-0034/driver";
    fs::remove(binding);
    fs::create_directory_symlink("missing-driver", binding);
    assert(i2c.update());
    i2c.assert_untouched();
    fs::remove(binding);
    std::ofstream(binding) << "not a driver symlink";
    assert(i2c.update());
    i2c.assert_untouched();
}

void all_declared_keyboards_are_checked()
{
    I2cDirectory i2c;
    i2c.add("1-0034", "tca8418");
    i2c.add("2-0034", "tca8418");
    i2c.bind("1-0034");
    i2c.bind("2-0034");
    configuration = 0x19;
    assert(i2c.update());
    assert(calls.size() == 2);
    i2c.assert_untouched();
}

void failed_physical_probe_does_not_trust_sysfs()
{
    I2cDirectory i2c;
    i2c.add("1-0034", "tca8418");
    assert(!launcher::update_tca8418_keyboard(i2c.directory, i2c.directory, i2c.driver.c_str()));
    // A normal file must not be mistaken for an I2C adapter either.
    std::ofstream(fs::path(i2c.directory) / "i2c-1") << "not an adapter";
    assert(!launcher::update_tca8418_keyboard(i2c.directory, i2c.directory, i2c.driver.c_str()));
    i2c.assert_untouched();
}

} // namespace

int main()
{
    absent_or_unrelated_devices();
    recognized_names_and_physical_connection();
    sysfs_symlink_is_followed_without_driver_binding();
    reset_bound_keyboard_is_reinitialized();
    failed_recovery_is_retried_without_losing_physical_presence();
    foreign_or_unreadable_bindings_are_left_alone();
    all_declared_keyboards_are_checked();
    failed_physical_probe_does_not_trust_sysfs();
    return 0;
}
