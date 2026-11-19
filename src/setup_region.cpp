#include "setup.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace launcher {
namespace {

std::string path_at(const std::string &root, const std::string &path)
{
    return root.empty() ? path : root + path;
}

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

std::string read_text(const std::string &path)
{
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

// These files use shell-style assignments, but are data here: never source them.
std::string assignment(const std::string &path, const std::string &key)
{
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        const auto equals = line.find('=');
        if (equals == std::string::npos || trim(line.substr(0, equals)) != key) continue;
        auto value = trim(line.substr(equals + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'')) {
            const auto end = value.find(value.front(), 1);
            if (end == std::string::npos) return {};
            return value.substr(1, end - 1);
        }
        const auto comment = value.find_first_of(" \t#");
        return value.substr(0, comment);
    }
    return {};
}

bool identifier(const std::string &value, const std::string &extra)
{
    return !value.empty() && std::all_of(value.begin(), value.end(), [&](unsigned char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
               (ch >= '0' && ch <= '9') || extra.find(static_cast<char>(ch)) != std::string::npos;
    });
}

bool country_code(const std::string &value)
{
    return value.size() == 2 && value[0] >= 'A' && value[0] <= 'Z' &&
           value[1] >= 'A' && value[1] <= 'Z';
}

const SetupChoice *choice(const std::vector<SetupChoice> &choices, const std::string &value)
{
    const auto item = std::find_if(choices.begin(), choices.end(), [&](const SetupChoice &entry) {
        return entry.value == value;
    });
    return item == choices.end() ? nullptr : &*item;
}

void add_choice(std::vector<SetupChoice> &choices, std::string value, std::string label,
                std::string country = {})
{
    if (!choice(choices, value)) choices.push_back({std::move(value), std::move(label), std::move(country)});
}

void sort_choices(std::vector<SetupChoice> &choices)
{
    std::sort(choices.begin(), choices.end(), [](const SetupChoice &a, const SetupChoice &b) {
        return a.label < b.label;
    });
}

std::string locale_country(const std::string &locale)
{
    const auto separator = locale.find('_');
    if (separator == std::string::npos) return {};
    const auto country = locale.substr(separator + 1, 2);
    return country_code(country) ? country : std::string();
}

std::string normalized_locale(const std::string &locale)
{
    std::string result;
    for (const auto ch : locale) {
        if (ch != '-' && ch != '.') result += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return result;
}

std::string canonical_locale(const std::vector<SetupChoice> &locales, const std::string &locale)
{
    const auto normalized = normalized_locale(locale);
    for (const auto &entry : locales) {
        if (normalized_locale(entry.value) == normalized) return entry.value;
    }
    return {};
}

std::string read_locale(const std::string &root)
{
    auto locale = assignment(path_at(root, "/etc/locale.conf"), "LANG");
    if (locale.empty()) locale = assignment(path_at(root, "/etc/default/locale"), "LANG");
    return locale;
}

std::string read_timezone(const std::string &root)
{
    std::error_code error;
    const auto link = std::filesystem::read_symlink(path_at(root, "/etc/localtime"), error).string();
    const auto start = link.find("zoneinfo/");
    if (!error && start != std::string::npos) return link.substr(start + 9);
    return trim(read_text(path_at(root, "/etc/timezone")));
}

std::string read_country(const std::string &root)
{
    for (const auto *path : {"/boot/firmware/cmdline.txt", "/boot/cmdline.txt"}) {
        std::istringstream input(read_text(path_at(root, path)));
        std::string token;
        const std::string prefix = "cfg80211.ieee80211_regdom=";
        while (input >> token) {
            if (token.rfind(prefix, 0) == 0) return token.substr(prefix.size());
        }
    }
    return assignment(path_at(root, "/etc/default/crda"), "REGDOMAIN");
}

std::string read_keyboard(const std::string &root)
{
    auto layout = assignment(path_at(root, "/etc/default/keyboard"), "XKBLAYOUT");
    return layout.substr(0, layout.find(','));
}

std::string locale_label(const std::string &root, const std::string &locale)
{
    // glibc's source locale files supply language names without an extra runtime
    // JSON/XML dependency. Encoded Unicode definitions fall back to the locale ID.
    auto source = locale.substr(0, locale.find('.'));
    const auto modifier = locale.find('@');
    if (modifier != std::string::npos) source += locale.substr(modifier);
    std::ifstream input(path_at(root, "/usr/share/i18n/locales/" + source));
    std::string line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.rfind("language", 0) != 0 || line.size() <= 8 || !std::isspace(static_cast<unsigned char>(line[8]))) continue;
        const auto first = line.find('"');
        const auto last = line.find('"', first == std::string::npos ? first : first + 1);
        if (first == std::string::npos || last == std::string::npos) break;
        const auto language = line.substr(first + 1, last - first - 1);
        if (!language.empty() && language.find('<') == std::string::npos) return language + " (" + locale + ")";
    }
    return locale;
}

void load_countries(SetupCatalog &catalog, const std::string &root)
{
    std::ifstream input(path_at(root, "/usr/share/zoneinfo/iso3166.tab"));
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string code, name;
        if (!(fields >> code) || !country_code(code)) continue;
        std::getline(fields, name);
        name = trim(name);
        if (!name.empty()) add_choice(catalog.countries, code, name, code);
    }
}

void load_locales(SetupCatalog &catalog, const std::string &root)
{
    std::ifstream input(path_at(root, "/usr/share/i18n/SUPPORTED"));
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string locale, encoding;
        if (!(fields >> locale >> encoding) || encoding != "UTF-8" || !identifier(locale, "_.@-")) continue;
        add_choice(catalog.locales, locale, locale_label(root, locale), locale_country(locale));
    }
}

void load_timezones(SetupCatalog &catalog, const std::string &root)
{
    const auto zone_root = std::filesystem::path(path_at(root, "/usr/share/zoneinfo"));
    for (const auto *file : {"/usr/share/zoneinfo/zone.tab", "/usr/share/zoneinfo/zone1970.tab"}) {
        std::ifstream input(path_at(root, file));
        std::string line;
        while (std::getline(input, line)) {
            std::istringstream fields(line);
            std::string countries, coordinates, zone;
            if (!(fields >> countries >> coordinates >> zone) || countries[0] == '#' ||
                !identifier(zone, "/_+-") || zone.find("..") != std::string::npos) continue;
            std::error_code error;
            if (!std::filesystem::is_regular_file(path_at(root, "/usr/share/zoneinfo/" + zone), error)) continue;
            add_choice(catalog.timezones, zone, zone, countries.substr(0, countries.find(',')));
        }
    }
    // zone.tab provides country associations. TZif files additionally include
    // fixed-offset zones and compatibility names already selected on an image.
    // Keep these valid current settings rather than silently replacing them.
    std::error_code error;
    std::filesystem::recursive_directory_iterator item(zone_root, std::filesystem::directory_options::skip_permission_denied, error), end;
    while (!error && item != end) {
        const auto relative = item->path().lexically_relative(zone_root).generic_string();
        if (relative == "posix" || relative == "right") {
            item.disable_recursion_pending();
        } else if (relative != "localtime" && relative != "posixrules" &&
                   identifier(relative, "/_+-") && item->is_regular_file(error)) {
            std::ifstream input(item->path(), std::ios::binary);
            char magic[4]{};
            input.read(magic, sizeof(magic));
            if (input.gcount() == sizeof(magic) && std::string(magic, sizeof(magic)) == "TZif")
                add_choice(catalog.timezones, relative, relative);
        }
        item.increment(error);
    }
}

void load_keyboards(SetupCatalog &catalog, const std::string &root)
{
    std::ifstream input(path_at(root, "/usr/share/X11/xkb/rules/base.lst"));
    if (!input) input.open(path_at(root, "/usr/share/X11/xkb/rules/evdev.lst"));
    std::string line;
    bool layouts = false;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty()) continue;
        if (line[0] == '!') {
            layouts = trim(line.substr(1)) == "layout";
            continue;
        }
        if (!layouts) continue;
        std::istringstream fields(line);
        std::string layout, name;
        fields >> layout;
        std::getline(fields, name);
        name = trim(name);
        if (identifier(layout, "_-") && !name.empty()) add_choice(catalog.keyboards, layout, name + " (" + layout + ")");
    }
}

void simulator_fallbacks(SetupCatalog &catalog)
{
    if (catalog.countries.empty()) catalog.countries = {
        {"CN", "China", "CN"}, {"FR", "France", "FR"}, {"DE", "Germany", "DE"},
        {"JP", "Japan", "JP"}, {"GB", "United Kingdom", "GB"}, {"US", "United States", "US"}};
    if (catalog.locales.empty()) catalog.locales = {
        {"zh_CN.UTF-8", "Chinese (zh_CN.UTF-8)", "CN"}, {"en_GB.UTF-8", "English (en_GB.UTF-8)", "GB"},
        {"en_US.UTF-8", "English (en_US.UTF-8)", "US"}, {"fr_FR.UTF-8", "French (fr_FR.UTF-8)", "FR"},
        {"de_DE.UTF-8", "German (de_DE.UTF-8)", "DE"}, {"ja_JP.UTF-8", "Japanese (ja_JP.UTF-8)", "JP"}};
    if (catalog.timezones.empty()) catalog.timezones = {
        {"Asia/Shanghai", "Asia/Shanghai", "CN"}, {"Asia/Tokyo", "Asia/Tokyo", "JP"},
        {"Europe/Berlin", "Europe/Berlin", "DE"}, {"Europe/London", "Europe/London", "GB"},
        {"Europe/Paris", "Europe/Paris", "FR"}, {"America/New_York", "America/New_York", "US"},
        {"Etc/UTC", "Etc/UTC", ""}};
    if (catalog.keyboards.empty()) catalog.keyboards = {
        {"us", "English (US)", "US"}, {"gb", "English (UK)", "GB"}, {"fr", "French", "FR"},
        {"de", "German", "DE"}, {"jp", "Japanese", "JP"}};
}

void select_current(std::string &selected, const std::string &current, const std::vector<SetupChoice> &choices)
{
    if (choice(choices, current)) selected = current;
    else if (!choice(choices, selected)) selected = choices.empty() ? std::string() : choices.front().value;
}

bool run_region_helper(const SetupCommandRunner &runner, const std::vector<std::string> &arguments,
                       const char *name, std::string &error)
{
    SetupCommand command;
    command.arguments = arguments;
    command.environment = {"LC_ALL=C", "LANG=C", "DEBIAN_FRONTEND=noninteractive"};
    command.timeout_seconds = 600; // Generating locales is slow on small boards.
    const auto result = runner(command);
    if (result.exit_code == 0 && !result.timed_out) return true;
    error = std::string("Could not apply ") + name + (result.timed_out ? ": operation timed out. Retry this step." : ". Check the system configuration and retry.");
    return false;
}

} // namespace

SetupCatalog load_setup_catalog(const std::string &root, bool simulated)
{
    SetupCatalog catalog;
    load_countries(catalog, root);
    load_locales(catalog, root);
    load_timezones(catalog, root);
    load_keyboards(catalog, root);
    if (simulated) simulator_fallbacks(catalog);
    sort_choices(catalog.countries);
    sort_choices(catalog.locales);
    sort_choices(catalog.timezones);
    sort_choices(catalog.keyboards);
    const auto locale = canonical_locale(catalog.locales, read_locale(root));
    auto country = read_country(root);
    if (!choice(catalog.countries, country)) country = locale_country(locale);
    select_current(catalog.current.country, country, catalog.countries);
    select_current(catalog.current.locale, locale, catalog.locales);
    select_current(catalog.current.timezone, read_timezone(root), catalog.timezones);
    select_current(catalog.current.keyboard, read_keyboard(root), catalog.keyboards);
    return catalog;
}

bool apply_setup_region(const SetupRegion &region, const SetupCatalog &catalog,
                        const SetupCommandRunner &runner, std::string &error,
                        const std::string &root, bool simulated)
{
    error.clear();
    if (catalog.countries.empty() || catalog.locales.empty() || catalog.timezones.empty() || catalog.keyboards.empty()) {
        error = "Regional settings are unavailable. Install locales, tzdata and xkb-data, then retry.";
        return false;
    }
    if (!country_code(region.country) || !choice(catalog.countries, region.country) ||
        !identifier(region.locale, "_.@-") || !choice(catalog.locales, region.locale) ||
        !identifier(region.timezone, "/_+-") || region.timezone.find("..") != std::string::npos || !choice(catalog.timezones, region.timezone) ||
        !identifier(region.keyboard, "_-") || !choice(catalog.keyboards, region.keyboard)) {
        error = "Choose a valid country, language, time zone and keyboard layout.";
        return false;
    }
    if (simulated) return true;
    if (!runner) {
        error = "System configuration is unavailable.";
        return false;
    }

    const struct Operation {
        const char *function;
        const char *debian_function;
        const char *name;
        const std::string *value;
        std::string (*read)(const std::string &);
    } operations[] = {
        {"do_wifi_country", "country", "country", &region.country, read_country},
        {"do_change_locale", "locale", "language", &region.locale, read_locale},
        {"do_change_timezone", "timezone", "time zone", &region.timezone, read_timezone},
        {"do_configure_keyboard", "keyboard", "keyboard layout", &region.keyboard, read_keyboard},
    };
    std::error_code filesystem_error;
    const bool raspberry_pi = std::filesystem::is_regular_file(path_at(root, "/usr/bin/raspi-config"), filesystem_error);
    for (const auto &operation : operations) {
        if (raspberry_pi && std::string(operation.function) == "do_configure_keyboard" &&
            !assignment(path_at(root, "/etc/default/keyboard"), "XKBVARIANT").empty()) {
            // The UI chooses base layouts. A variant belonging to the previous
            // layout must not survive and invalidate the new XKB keymap.
            if (!run_region_helper(runner, {"/usr/bin/sed", "-i", "s/^XKBVARIANT=.*/XKBVARIANT=\"\"/", "/etc/default/keyboard"},
                                   "keyboard variant", error)) return false;
        }
        const std::vector<std::string> arguments = raspberry_pi
            ? std::vector<std::string>{"/usr/bin/raspi-config", "nonint", operation.function, *operation.value}
            : std::vector<std::string>{"/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-setup-region", operation.debian_function, *operation.value};
        if (!run_region_helper(runner, arguments, operation.name, error)) return false;
        if (raspberry_pi && std::string(operation.function) == "do_change_locale") {
            // Existing LANGUAGE or LC_* values would override the selected
            // language. First-use setup establishes one consistent new default.
            if (!run_region_helper(runner, {"/usr/sbin/update-locale", "--reset", "LANG=" + region.locale},
                                   "language defaults", error)) return false;
        }
        if (operation.read(root) != *operation.value) {
            error = std::string("The ") + operation.name + " setting was not saved. Check the system configuration and retry.";
            return false;
        }
        if (std::string(operation.function) == "do_configure_keyboard" &&
            !assignment(path_at(root, "/etc/default/keyboard"), "XKBVARIANT").empty()) {
            error = "The keyboard variant was not reset. Check /etc/default/keyboard and retry.";
            return false;
        }
    }
    return true;
}

} // namespace launcher
