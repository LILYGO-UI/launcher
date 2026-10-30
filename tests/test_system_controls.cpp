#include "system_controls.h"

#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

struct Backlights {
    char directory[64] = "/tmp/lilygo-controls-test-XXXXXX";

    Backlights() { assert(mkdtemp(directory)); }
    ~Backlights() { fs::remove_all(directory); }

    void write(const char *name, const char *file, const std::string &value) const
    {
        fs::create_directories(fs::path(directory) / name);
        std::ofstream output(fs::path(directory) / name / file);
        output << value << '\n';
        assert(output.good());
    }

    void add(const char *name, const std::string &maximum, const std::string &current) const
    {
        write(name, "max_brightness", maximum);
        write(name, "brightness", current);
    }

    long brightness(const char *name) const
    {
        std::ifstream input(fs::path(directory) / name / "brightness");
        long value = -1;
        input >> value;
        assert(input.good());
        return value;
    }
};

void simulator_never_changes_devices()
{
    Backlights backlights;
    backlights.add("panel", "255", "128");
    launcher::SystemControls controls(true, backlights.directory);
    assert(controls.state().simulated);
    assert(controls.state().brightness_available);
    assert(controls.state().volume_available);
    assert(controls.state().power_off_available);
    assert(controls.set_brightness(-100));
    assert(controls.state().brightness_percent == 1);
    assert(controls.set_volume(110));
    assert(controls.state().volume_percent == 100);
    assert(controls.set_volume(-1));
    assert(controls.state().volume_percent == 0);
    assert(controls.set_brightness(72));
    assert(controls.refresh().brightness_percent == 72);
    assert(backlights.brightness("panel") == 128);
    // This is the only power_off invocation in the tests; it is in-memory.
    assert(controls.power_off());
    assert(controls.error().empty());
}

#if !defined(LAUNCHER_SIMULATOR)

void brightness_tracks_device_and_stays_visible()
{
    Backlights backlights;
    // Invalid driver entries must not hide the usable display backlight.
    backlights.add("00-empty", "0", "0");
    backlights.add("01-overflow", "9223372036854775807", "0");
    backlights.add("02-malformed", "255garbage", "0");
    backlights.add("03-negative", "255", "-1");
    backlights.add("04-outside-range", "255", "256");
    backlights.add("panel", "255", "128");
    launcher::SystemControls controls(false, backlights.directory);
    assert(!controls.state().simulated);
    assert(controls.state().brightness_available);
    assert(controls.state().brightness_percent == 50);

    assert(controls.set_brightness(80));
    assert(backlights.brightness("panel") == 204);
    assert(controls.state().brightness_percent == 80);
    assert(controls.set_brightness(999));
    assert(backlights.brightness("panel") == 255);
    assert(controls.state().brightness_percent == 100);
    assert(controls.set_brightness(0));
    assert(backlights.brightness("panel") > 0);
    assert(controls.state().brightness_percent == 1);

    // External brightness changes are reflected instead of displaying a cache.
    backlights.write("panel", "brightness", "51");
    assert(controls.refresh().brightness_percent == 20);
    fs::remove_all(fs::path(backlights.directory) / "panel");
    assert(!controls.set_brightness(50));
    assert(!controls.state().brightness_available);
    assert(!controls.error().empty());

    backlights.add("panel", "10", "5");
    assert(controls.set_brightness(0));
    assert(backlights.brightness("panel") == 1);
    assert(controls.state().brightness_percent == 10);
    assert(controls.error().empty());
}

void unavailable_backlight_does_not_report_success()
{
    Backlights backlights;
    launcher::SystemControls controls(false, backlights.directory);
    assert(!controls.state().brightness_available);
    assert(!controls.set_brightness(40));
    assert(!controls.error().empty());
    fs::remove_all(backlights.directory);
    assert(!controls.refresh().brightness_available);
    assert(!controls.set_brightness(40));
}

#else

void simulator_build_cannot_enable_hardware()
{
    Backlights backlights;
    backlights.add("panel", "255", "128");
    launcher::SystemControls controls(false, backlights.directory);
    assert(controls.state().simulated);
    assert(controls.set_brightness(80));
    assert(backlights.brightness("panel") == 128);
    assert(controls.power_off());
}

#endif

} // namespace

int main()
{
    simulator_never_changes_devices();
#if !defined(LAUNCHER_SIMULATOR)
    brightness_tracks_device_and_stays_visible();
    unavailable_backlight_does_not_report_success();
#else
    simulator_build_cannot_enable_hardware();
#endif
    return 0;
}
