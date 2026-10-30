#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "../src/process.cpp"

namespace {

struct Fixture {
    std::string root;
    Fixture()
    {
        char directory[] = "/tmp/lilygo-process-locale-XXXXXX";
        assert(mkdtemp(directory));
        root = directory;
    }
    ~Fixture() { std::filesystem::remove_all(root); }
    void write(const std::string &path, const std::string &contents, mode_t mode = 0644)
    {
        const auto file = root + path;
        std::filesystem::create_directories(std::filesystem::path(file).parent_path());
        std::ofstream output(file);
        output << contents;
        assert(output.good());
        output.close();
        assert(chmod(file.c_str(), mode) == 0);
    }
};

void inherited_locale()
{
    assert(setenv("LANG", "en_GB.UTF-8", 1) == 0);
    assert(setenv("LANGUAGE", "en", 1) == 0);
    assert(setenv("LC_ALL", "en_GB.UTF-8", 1) == 0);
    assert(setenv("LC_TIME", "en_GB.UTF-8", 1) == 0);
}

bool equals(const char *name, const char *expected)
{
    const auto *value = getenv(name);
    return expected ? value && strcmp(value, expected) == 0 : !value;
}

void selected_language_replaces_inherited_overrides()
{
    Fixture fixture;
    inherited_locale();
    assert(apply_system_locale(fixture.root, geteuid()) == 0);
    assert(equals("LANG", "en_GB.UTF-8") && equals("LC_ALL", "en_GB.UTF-8"));
    fixture.write("/etc/locale.conf", "# selected by the first-boot wizard\nLANG=\"zh_CN.UTF-8\"\n");
    assert(apply_system_locale(fixture.root, geteuid()) == 0);
    assert(equals("LANG", "zh_CN.UTF-8"));
    assert(equals("LANGUAGE", nullptr) && equals("LC_ALL", nullptr) && equals("LC_TIME", nullptr));
    fixture.write("/etc/locale.conf", "LANG='de_DE.UTF-8' # chosen language\n LANGUAGE = de:en\nexport LC_TIME=\"en_GB.UTF-8\"\nLC_NUMERIC=\n");
    assert(apply_system_locale(fixture.root, geteuid()) == 0);
    assert(equals("LANG", "de_DE.UTF-8") && equals("LANGUAGE", "de:en"));
    assert(equals("LC_TIME", "en_GB.UTF-8") && equals("LC_NUMERIC", nullptr));
}

void modern_file_wins_and_legacy_symlink_is_supported()
{
    Fixture fixture;
    fixture.write("/etc/locale.conf", "LANG=zh_CN.UTF-8\n");
    fixture.write("/etc/default/locale", "LANG=fr_FR.UTF-8\n");
    assert(apply_system_locale(fixture.root, geteuid()) == 0);
    assert(equals("LANG", "zh_CN.UTF-8"));
    std::filesystem::remove(fixture.root + "/etc/locale.conf");
    assert(apply_system_locale(fixture.root, geteuid()) == 0);
    assert(equals("LANG", "fr_FR.UTF-8"));
    std::filesystem::create_symlink("default/locale", fixture.root + "/etc/locale.conf");
    assert(apply_system_locale(fixture.root, geteuid()) == 0);
    assert(equals("LANG", "fr_FR.UTF-8"));
    assert(chmod((fixture.root + "/etc/default/locale").c_str(), 0666) == 0);
    assert(apply_system_locale(fixture.root, geteuid()) == -1);
}

void locale_file_is_data_and_cannot_override_other_environment()
{
    Fixture fixture;
    assert(setenv("PATH", "/test/original", 1) == 0);
    assert(unsetenv("LD_PRELOAD") == 0);
    fixture.write("/etc/locale.conf", "PATH=/unwanted\nLD_PRELOAD=/unwanted.so\nLC_UNKNOWN=unwanted\nLANG=en_US.UTF-8\nLANG=ja_JP.UTF-8\n");
    assert(apply_system_locale(fixture.root, geteuid()) == 0);
    assert(equals("PATH", "/test/original") && equals("LD_PRELOAD", nullptr));
    assert(equals("LC_UNKNOWN", nullptr) && equals("LANG", "ja_JP.UTF-8"));
    for (const auto &contents : {
             std::string("LANG=\"$(touch /tmp/unwanted)\"\n"),
             std::string("LANG=`id`\n"), std::string("LANG=${VALUE}\n"),
             std::string("LANG=\"en_US.UTF-8\"; exec arbitrary\n"),
             std::string("LANG=\"unterminated\n"), std::string("LANG=en_US.UTF-8 trailing-data\n"),
             std::string("LANG=en_US.UTF-8\0injected", 25),
             std::string("LANG=") + std::string(257, 'x'), std::string(65537, '#')}) {
        inherited_locale();
        fixture.write("/etc/locale.conf", contents);
        assert(apply_system_locale(fixture.root, geteuid()) == -1);
        assert(equals("LANG", "en_GB.UTF-8") && equals("LC_ALL", "en_GB.UTF-8"));
    }
}

void unsafe_ownership_modes_and_file_types_are_rejected()
{
    Fixture fixture;
    inherited_locale();
    fixture.write("/etc/locale.conf", "LANG=zh_CN.UTF-8\n", 0664);
    assert(apply_system_locale(fixture.root, geteuid()) == -1);
    fixture.write("/etc/locale.conf", "LANG=zh_CN.UTF-8\n");
    assert(apply_system_locale(fixture.root, geteuid() + 1) == -1);
    assert(equals("LANG", "en_GB.UTF-8"));
    std::filesystem::remove(fixture.root + "/etc/locale.conf");
    assert(mkfifo((fixture.root + "/etc/locale.conf").c_str(), 0600) == 0);
    assert(apply_system_locale(fixture.root, geteuid()) == -1);
    std::filesystem::remove(fixture.root + "/etc/locale.conf");
    std::filesystem::create_directory(fixture.root + "/etc/locale.conf");
    assert(apply_system_locale(fixture.root, geteuid()) == -1);
    assert(equals("LANG", "en_GB.UTF-8"));
}

void simulator_child_preserves_host_environment(const char *executable)
{
    inherited_locale();
    setenv("LILYGO_LOCALE_TEST_CHILD", "1", 1);
    launcher_app_t app{};
    snprintf(app.executable_path, sizeof(app.executable_path), "%s", executable);
    snprintf(app.manifest.id, sizeof(app.manifest.id), "%s", "cc.lilygo.ui.ProcessLocaleTest");
    snprintf(app.manifest.name, sizeof(app.manifest.name), "%s", "Process Locale Test");
    launcher_process_options_t options{
        "sdl", "/dev/fb-test", "/dev/dri/card-test", 0, "/dev/input-test", "portrait", 640, 480, nullptr};
    volatile sig_atomic_t stop = 0;
    assert(launcher_process_run(&app, &options, &stop) == 23);
    assert(equals("LANG", "en_GB.UTF-8") && equals("LC_ALL", "en_GB.UTF-8"));
    unsetenv("LILYGO_LOCALE_TEST_CHILD");
}

} // namespace

int main(int, char **argv)
{
    if (getenv("LILYGO_LOCALE_TEST_CHILD"))
        return equals("LANG", "en_GB.UTF-8") && equals("LC_ALL", "en_GB.UTF-8") && equals("LANGUAGE", "en") ? 23 : 1;
    selected_language_replaces_inherited_overrides();
    modern_file_wins_and_legacy_symlink_is_supported();
    locale_file_is_data_and_cannot_override_other_environment();
    unsafe_ownership_modes_and_file_types_are_rejected();
    simulator_child_preserves_host_environment(argv[0]);
    return 0;
}
