#include "setup.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

struct Fixture {
    std::string root;
    Fixture()
    {
        char directory[] = "/tmp/lilygo-region-test-XXXXXX";
        assert(mkdtemp(directory));
        root = directory;
    }
    ~Fixture() { std::filesystem::remove_all(root); }
    void write(const std::string &path, const std::string &contents)
    {
        const auto file = root + path;
        std::filesystem::create_directories(std::filesystem::path(file).parent_path());
        std::ofstream stream(file);
        stream << contents;
        assert(stream.good());
    }
    void catalogs()
    {
        write("/usr/share/zoneinfo/iso3166.tab", "# Test countries\nGB\tUnited Kingdom\nCN\tChina\nUS\tUnited States\nNP\tNepal\ninvalid\tIgnore\n");
        write("/usr/share/i18n/SUPPORTED", "en_GB.UTF-8 UTF-8\nzh_CN.UTF-8 UTF-8\nne_NP UTF-8\nen_US.UTF-8 UTF-8\nen_US ISO-8859-1\n#ignore UTF-8\n../../bad UTF-8\nen_US.UTF-8 UTF-8\n");
        write("/usr/share/i18n/locales/en_GB", "LC_IDENTIFICATION\nlanguage \"English\"\nEND LC_IDENTIFICATION\n");
        write("/usr/share/i18n/locales/ne_NP", "language \"<U0928><U0947>\"\n");
        write("/usr/share/zoneinfo/zone.tab", "# Zone database\nGB\t+513030-0000731\tEurope/London\nCN\t+3114+12128\tAsia/Shanghai\nNP\t+2743+08519\tAsia/Kathmandu\nUS\t+404251-0740023\tAmerica/New_York\nGB\t+0\tMissing/Zone\nGB\t+0\t../../etc/passwd\n");
        write("/usr/share/zoneinfo/zone1970.tab", "GB,GG,IM,JE\t+513030-0000731\tEurope/London\n");
        for (const auto *zone : {"Europe/London", "Asia/Shanghai", "Asia/Kathmandu", "America/New_York", "Etc/UTC"})
            write(std::string("/usr/share/zoneinfo/") + zone, "TZif");
        write("/usr/share/X11/xkb/rules/base.lst", "! model\n pc105 Generic 105-key PC\n! layout\n gb English (UK)\n us English (US)\n cn Chinese\n np Nepali\n! variant\n dvorak gb: English (UK, Dvorak)\n! option\n caps:escape Escape\n");
    }
    void settings(const launcher::SetupRegion &region)
    {
        write("/boot/firmware/cmdline.txt", "console=tty1 root=/dev/mmcblk0p2 cfg80211.ieee80211_regdom=" + region.country + " quiet\n");
        write("/etc/locale.conf", "LANG=\"" + region.locale + "\"\n");
        write("/etc/timezone", region.timezone + "\n");
        write("/etc/default/keyboard", "XKBMODEL=\"pc105\"\nXKBLAYOUT=\"" + region.keyboard + "\"\nXKBVARIANT=\"\"\n");
    }
};

bool has(const std::vector<launcher::SetupChoice> &choices, const std::string &value)
{
    return std::any_of(choices.begin(), choices.end(), [&](const auto &entry) { return entry.value == value; });
}

void loads_distribution_catalogs_and_preserves_settings()
{
    Fixture fixture;
    fixture.catalogs();
    const launcher::SetupRegion settings{"NP", "ne_NP", "Asia/Kathmandu", "np"};
    fixture.settings(settings);
    const auto catalog = launcher::load_setup_catalog(fixture.root, false);
    assert(catalog.countries.size() == 4);
    assert(catalog.locales.size() == 4);
    assert(catalog.timezones.size() == 5);
    assert(catalog.keyboards.size() == 4);
    assert(catalog.current.country == "NP");
    assert(catalog.current.locale == "ne_NP");
    assert(catalog.current.timezone == "Asia/Kathmandu");
    assert(catalog.current.keyboard == "np");
    assert(has(catalog.locales, "ne_NP")); // Not limited to built-in common languages.
    assert(!has(catalog.locales, "en_US"));
    assert(!has(catalog.timezones, "Missing/Zone"));
    assert(!has(catalog.keyboards, "dvorak"));
    assert(catalog.countries.front().label == "China");
    const auto english = std::find_if(catalog.locales.begin(), catalog.locales.end(), [](const auto &entry) { return entry.value == "en_GB.UTF-8"; });
    assert(english != catalog.locales.end() && english->label == "English (en_GB.UTF-8)");

    fixture.write("/etc/default/locale", "LANG=\"en_GB.UTF-8\"\n");
    fixture.write("/etc/locale.conf", "LANG=zh_CN.utf8\n");
    fixture.write("/boot/firmware/cmdline.txt", "console=tty1\n");
    fixture.write("/etc/default/keyboard", "XKBLAYOUT='cn,us'\n");
    // The active localtime link wins over the legacy timezone text file.
    fixture.write("/etc/timezone", "Europe/London\n");
    std::filesystem::create_symlink("/usr/share/zoneinfo/Asia/Shanghai", fixture.root + "/etc/localtime");
    const auto migrated = launcher::load_setup_catalog(fixture.root, false);
    assert(migrated.current.locale == "zh_CN.UTF-8"); // Debian 13 takes precedence.
    assert(migrated.current.country == "CN");
    assert(migrated.current.timezone == "Asia/Shanghai");
    assert(migrated.current.keyboard == "cn");

    std::filesystem::remove(fixture.root + "/etc/locale.conf");
    fixture.write("/etc/default/crda", "REGDOMAIN=US\n");
    std::filesystem::rename(fixture.root + "/usr/share/X11/xkb/rules/base.lst", fixture.root + "/usr/share/X11/xkb/rules/evdev.lst");
    const auto legacy = launcher::load_setup_catalog(fixture.root, false);
    assert(legacy.current.locale == "en_GB.UTF-8");
    assert(legacy.current.country == "US");
    assert(legacy.keyboards.size() == 4);

    fixture.write("/usr/share/zoneinfo/Etc/GMT+8", "TZif");
    fixture.write("/usr/share/zoneinfo/right/Europe/London", "TZif");
    fixture.write("/usr/share/zoneinfo/posix/Europe/London", "TZif");
    fixture.write("/usr/share/zoneinfo/not-a-zone", "plain data");
    std::filesystem::remove(fixture.root + "/etc/localtime");
    fixture.write("/etc/timezone", "Etc/GMT+8\n");
    const auto fixed_zone = launcher::load_setup_catalog(fixture.root, false);
    assert(fixed_zone.current.timezone == "Etc/GMT+8");
    assert(!has(fixed_zone.timezones, "right/Europe/London"));
    assert(!has(fixed_zone.timezones, "posix/Europe/London"));
    assert(!has(fixed_zone.timezones, "not-a-zone"));
}

void missing_device_catalogs_fail_and_simulation_never_runs_commands()
{
    Fixture fixture;
    const auto empty = launcher::load_setup_catalog(fixture.root, false);
    assert(empty.countries.empty() && empty.locales.empty() && empty.timezones.empty() && empty.keyboards.empty());
    std::string error;
    unsigned calls = 0;
    const auto runner = [&](const launcher::SetupCommand &) {
        ++calls;
        return launcher::SetupCommandResult{0, "", false};
    };
    assert(!launcher::apply_setup_region({}, empty, runner, error, fixture.root, false));
    assert(!error.empty() && calls == 0);

    const auto simulated = launcher::load_setup_catalog(fixture.root, true);
    assert(has(simulated.countries, "CN") && has(simulated.locales, "zh_CN.UTF-8"));
    assert(launcher::apply_setup_region(simulated.current, simulated, runner, error, fixture.root, true));
    assert(error.empty() && calls == 0);
    assert(!std::filesystem::exists(fixture.root + "/etc"));
}

void invalid_choices_are_rejected_before_mutation()
{
    Fixture fixture;
    fixture.catalogs();
    const auto catalog = launcher::load_setup_catalog(fixture.root, false);
    unsigned calls = 0;
    const auto runner = [&](const launcher::SetupCommand &) {
        ++calls;
        return launcher::SetupCommandResult{0, "", false};
    };
    std::string error;
    for (int field = 0; field < 4; ++field) {
        for (const auto *invalid : {"", "$(touch /tmp/unwanted)", "../../etc/passwd", "UNKNOWN", "gb\n--help"}) {
            auto region = catalog.current;
            auto *value = field == 0 ? &region.country : field == 1 ? &region.locale : field == 2 ? &region.timezone : &region.keyboard;
            *value = invalid;
            assert(!launcher::apply_setup_region(region, catalog, runner, error, fixture.root, false));
            assert(!error.empty());
        }
    }
    assert(calls == 0);
}

void helpers_apply_all_settings_and_clear_old_keyboard_variants()
{
    Fixture fixture;
    fixture.catalogs();
    fixture.write("/usr/bin/raspi-config", "Pi capability fixture\n");
    fixture.settings({});
    fixture.write("/etc/default/keyboard", "XKBLAYOUT=\"gb\"\nXKBVARIANT=\"dvorak\"\n");
    const auto catalog = launcher::load_setup_catalog(fixture.root, false);
    const launcher::SetupRegion requested{"CN", "zh_CN.UTF-8", "Asia/Shanghai", "cn"};
    std::vector<std::vector<std::string>> calls;
    const auto runner = [&](const launcher::SetupCommand &command) {
        calls.push_back(command.arguments);
        assert(command.input.empty());
        assert(command.timeout_seconds >= 300);
        assert(std::find(command.environment.begin(), command.environment.end(), "DEBIAN_FRONTEND=noninteractive") != command.environment.end());
        if (command.arguments[0] == "/usr/bin/sed") {
            assert(command.arguments == std::vector<std::string>({"/usr/bin/sed", "-i", "s/^XKBVARIANT=.*/XKBVARIANT=\"\"/", "/etc/default/keyboard"}));
            fixture.write("/etc/default/keyboard", "XKBLAYOUT=\"gb\"\nXKBVARIANT=\"\"\n");
        } else if (command.arguments[0] == "/usr/sbin/update-locale") {
            assert(command.arguments == std::vector<std::string>({"/usr/sbin/update-locale", "--reset", "LANG=zh_CN.UTF-8"}));
            fixture.write("/etc/locale.conf", "LANG=zh_CN.UTF-8\n");
        } else {
            assert(command.arguments.size() == 4 && command.arguments[0] == "/usr/bin/raspi-config" && command.arguments[1] == "nonint");
            const auto &operation = command.arguments[2];
            const auto &value = command.arguments[3];
            if (operation == "do_wifi_country") fixture.write("/boot/firmware/cmdline.txt", "cfg80211.ieee80211_regdom=" + value + "\n");
            else if (operation == "do_change_locale") fixture.write("/etc/locale.conf", "LANG=" + value + "\n");
            else if (operation == "do_change_timezone") fixture.write("/etc/timezone", value + "\n");
            else if (operation == "do_configure_keyboard") fixture.write("/etc/default/keyboard", "XKBLAYOUT=\"" + value + "\"\nXKBVARIANT=\"\"\n");
            else assert(false);
        }
        return launcher::SetupCommandResult{0, "", false};
    };
    std::string error;
    assert(launcher::apply_setup_region(requested, catalog, runner, error, fixture.root, false));
    assert(error.empty() && calls.size() == 6);
    const auto saved = launcher::load_setup_catalog(fixture.root, false);
    assert(saved.current.country == requested.country && saved.current.locale == requested.locale);
    assert(saved.current.timezone == requested.timezone && saved.current.keyboard == requested.keyboard);
}

void generic_debian_uses_distribution_helpers_without_pi_boot_files()
{
    Fixture fixture;
    fixture.catalogs();
    fixture.write("/etc/default/crda", "REGDOMAIN=GB\n");
    fixture.write("/etc/default/keyboard", "XKBLAYOUT=gb\nXKBVARIANT=dvorak\n");
    const auto catalog = launcher::load_setup_catalog(fixture.root, false);
    const launcher::SetupRegion requested{"CN", "zh_CN.UTF-8", "Asia/Shanghai", "cn"};
    std::vector<std::string> operations;
    const auto runner = [&](const launcher::SetupCommand &command) {
        assert(command.arguments.size() == 3);
        assert(command.arguments[0] == "/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-setup-region");
        const auto &operation = command.arguments[1];
        const auto &value = command.arguments[2];
        operations.push_back(operation);
        if (operation == "country") fixture.write("/etc/default/crda", "REGDOMAIN=" + value + "\n");
        else if (operation == "locale") fixture.write("/etc/locale.conf", "LANG=" + value + "\n");
        else if (operation == "timezone") fixture.write("/etc/timezone", value + "\n");
        else if (operation == "keyboard") fixture.write("/etc/default/keyboard", "XKBLAYOUT=" + value + "\nXKBVARIANT=\"\"\n");
        else assert(false);
        return launcher::SetupCommandResult{0, "", false};
    };
    std::string error;
    assert(launcher::apply_setup_region(requested, catalog, runner, error, fixture.root, false));
    assert(operations == std::vector<std::string>({"country", "locale", "timezone", "keyboard"}));
    assert(!std::filesystem::exists(fixture.root + "/boot"));
    const auto saved = launcher::load_setup_catalog(fixture.root, false);
    assert(saved.current.country == requested.country && saved.current.locale == requested.locale);
    assert(saved.current.timezone == requested.timezone && saved.current.keyboard == requested.keyboard);
}

void failed_or_unpersisted_operations_stop_and_can_retry()
{
    Fixture fixture;
    fixture.catalogs();
    fixture.settings({});
    const auto catalog = launcher::load_setup_catalog(fixture.root, false);
    const launcher::SetupRegion requested{"CN", "zh_CN.UTF-8", "Asia/Shanghai", "cn"};
    std::string error;
    unsigned calls = 0;
    auto runner = [&](const launcher::SetupCommand &) {
        ++calls;
        return launcher::SetupCommandResult{1, "failed", false};
    };
    assert(!launcher::apply_setup_region(requested, catalog, runner, error, fixture.root, false));
    assert(calls == 1 && error.find("country") != std::string::npos);
    calls = 0;
    const auto dishonest = [&](const launcher::SetupCommand &) {
        ++calls;
        return launcher::SetupCommandResult{0, "", false};
    };
    assert(!launcher::apply_setup_region(requested, catalog, dishonest, error, fixture.root, false));
    assert(calls == 1 && error.find("not saved") != std::string::npos);
    calls = 0;
    const auto timeout = [&](const launcher::SetupCommand &) {
        ++calls;
        return launcher::SetupCommandResult{0, "", true};
    };
    assert(!launcher::apply_setup_region(requested, catalog, timeout, error, fixture.root, false));
    assert(calls == 1 && error.find("timed out") != std::string::npos);
    assert(launcher::apply_setup_region(catalog.current, catalog, dishonest, error, fixture.root, false));
    assert(error.empty()); // A failed attempt never creates a sticky completion state.
}

} // namespace

int main()
{
    loads_distribution_catalogs_and_preserves_settings();
    missing_device_catalogs_fail_and_simulation_never_runs_commands();
    invalid_choices_are_rejected_before_mutation();
    helpers_apply_all_settings_and_clear_old_keyboard_variants();
    generic_debian_uses_distribution_helpers_without_pi_boot_files();
    failed_or_unpersisted_operations_stop_and_can_retry();
    return 0;
}
