#include "first_boot_wizard.h"
#include "session.h"

#include <cm0/typography.h>
#include <lvgl.h>

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
#include <unistd.h>

using namespace launcher;
namespace {
void flush_display(lv_display_t *display, const lv_area_t *, uint8_t *) { lv_display_flush_ready(display); }
lv_obj_t *lookup(const char *name) { return lv_obj_find_by_name(lv_layer_top(), name); }
lv_obj_t *find(const char *name)
{
    auto *object = lookup(name);
    assert(object);
    return object;
}
void click(const char *name)
{
    auto *object = find(name);
    assert(!lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_state(object, LV_STATE_DISABLED));
    assert(lv_obj_send_event(object, LV_EVENT_CLICKED, nullptr) == LV_RESULT_OK);
}
bool ready(const char *name)
{
    auto *object = lookup(name);
    return object && !lv_obj_has_state(object, LV_STATE_DISABLED);
}
bool page(const char *title)
{
    auto *object = lookup("setup_title");
    return object && std::strcmp(lv_label_get_text(object), title) == 0;
}
void wait_until(FirstBootWizard &wizard, const std::function<bool()> &predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        wizard.tick();
        lv_tick_inc(1);
        lv_timer_handler();
        if (predicate()) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    } while (std::chrono::steady_clock::now() < deadline);
    if (auto *error = lookup("setup_error")) std::fprintf(stderr, "Setup error: %s\n", lv_label_get_text(error));
    if (auto *error = lookup("account_error")) std::fprintf(stderr, "Account error: %s\n", lv_label_get_text(error));
    assert(false && "Wizard did not reach the expected page");
}
struct Fixture {
    char directory[64] = "/tmp/lilygo-wizard-test-XXXXXX";
    std::string path;
    const std::string sentinel = "Host session state must remain unchanged.\n";
    Fixture()
    {
        assert(mkdtemp(directory));
        path = std::string(directory) + "/active-user";
        std::ofstream file(path);
        file << sentinel;
        assert(file.good());
    }
    ~Fixture()
    {
        std::ifstream file(path);
        const std::string contents{std::istreambuf_iterator<char>(file), {}};
        assert(contents == sentinel);
        assert(unlink(path.c_str()) == 0);
        assert(rmdir(directory) == 0); // No extra durable wizard/account files.
    }
};
void create_account(FirstBootWizard &wizard, const char *name)
{
    assert(page("Welcome"));
    click("setup_primary");
    wait_until(wizard, [] { return ready("account_submit"); });
    assert(!lookup("setup_modal"));
    assert(std::strcmp(lv_label_get_text(find("account_submit_label")), "Continue") == 0);
    assert(std::strstr(lv_label_get_text(find("account_subtitle")), "2 of 5"));
    assert(lv_obj_has_flag(find("account_mode_toggle"), LV_OBJ_FLAG_HIDDEN));
    lv_textarea_set_text(find("account_username"), name);
    lv_textarea_set_text(find("account_password"), "wizard-secret");
    lv_textarea_set_text(find("account_confirm_password"), "mismatch");
    click("account_submit");
    wizard.tick();
    assert(std::strstr(lv_label_get_text(find("account_error")), "do not match"));
    lv_textarea_set_text(find("account_password"), "wizard-secret");
    lv_textarea_set_text(find("account_confirm_password"), "wizard-secret");
    click("account_submit");
    assert(std::strcmp(lv_textarea_get_text(find("account_password")), "") == 0);
    wait_until(wizard, [] { return page("Connect to Wi-Fi") && ready("setup_primary"); });
    assert(!lookup("account_modal"));
}
void finish(FirstBootWizard &wizard, UserSession &session, const char *username)
{
    click("setup_primary");
    wait_until(wizard, [] { return page("You're ready") && ready("setup_primary"); });
    assert(wizard.active());
    click("setup_primary");
    wait_until(wizard, [&] { return !wizard.active(); });
    assert(!lookup("setup_modal") && !lookup("account_modal"));
    assert(session.username() == username);
    assert(session.setup_username().empty());
    std::string error;
    assert(session.reauthenticate("wizard-secret", error));
    assert(!session.reauthenticate("raspberry", error));
}
void test_disabled()
{
    Fixture fixture;
    UserSession session(fixture.path);
    std::string error;
    assert(session.initialize(error));
    FirstBootWizard wizard(session);
    assert(wizard.initialize(false, error));
    assert(!wizard.active());
    assert(!lookup("setup_modal") && !lookup("account_modal"));
    wizard.tick();
    assert(session.username().empty());
}
void test_skip_optional_steps()
{
    Fixture fixture;
    UserSession session(fixture.path);
    std::string error;
    assert(session.initialize(error));
    FirstBootWizard wizard(session);
    assert(wizard.initialize(true, error));
    wizard.set_hardware_keyboard_present(true);
    create_account(wizard, "offline-user");
    click("setup_secondary");
    wait_until(wizard, [] { return page("Keep up to date") && ready("setup_primary"); });
    click("setup_secondary");
    wait_until(wizard, [] { return page("You're ready") && ready("setup_primary"); });
    click("setup_primary");
    wait_until(wizard, [&] { return !wizard.active(); });
    assert(session.username() == "offline-user");
    assert(session.setup_username().empty());
    assert(!lookup("setup_modal") && !lookup("account_modal"));
}
void test_connect_and_install_with_back_and_retry()
{
    Fixture fixture;
    UserSession session(fixture.path);
    std::string error;
    assert(session.initialize(error));
    FirstBootWizard wizard(session);
    assert(wizard.initialize(true, error));
    create_account(wizard, "connected-user");
    assert(session.setup_account_saved());
    assert(session.setup_username() == "connected-user");
    click("setup_secondary");
    wait_until(wizard, [] { return page("Keep up to date") && ready("setup_primary"); });
    click("setup_back");
    wait_until(wizard, [] { return page("Connect to Wi-Fi") && ready("setup_primary"); });
    assert(!lookup("account_modal"));
    assert(session.username() == "connected-user");
    // A backend validation error leaves the saved account and network page intact.
    lv_dropdown_set_selected(find("setup_networks"), 1);
    lv_obj_send_event(find("setup_networks"), LV_EVENT_VALUE_CHANGED, nullptr);
    lv_textarea_set_text(find("setup_wifi_password"), "short");
    click("setup_primary");
    wait_until(wizard, [] {
        return ready("setup_primary") && std::strstr(lv_label_get_text(find("setup_error")), "WPA password");
    });
    assert(page("Connect to Wi-Fi"));
    assert(std::strcmp(lv_textarea_get_text(find("setup_wifi_password")), "") == 0);
    lv_textarea_set_text(find("setup_wifi_password"), "valid-wireless-password");
    click("setup_primary");
    wait_until(wizard, [] { return page("Keep up to date") && ready("setup_primary"); });
    click("setup_primary"); // Check.
    wait_until(wizard, [] {
        return ready("setup_primary") && std::strstr(lv_label_get_text(find("setup_status")), "3 updates available");
    });
    click("setup_primary"); // Install.
    wait_until(wizard, [] {
        return ready("setup_primary") && std::strstr(lv_label_get_text(find("setup_status")), "update complete");
    });
    finish(wizard, session, "connected-user");
}
}
int main()
{
    const uid_t uid = geteuid();
    const gid_t gid = getegid();
    lv_init();
    auto *display = lv_display_create(568, 1232);
    assert(display);
    static lv_color32_t buffer[568 * 40];
    lv_display_set_buffers(display, buffer, nullptr, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    test_disabled();
    test_skip_optional_steps();
    test_connect_and_install_with_back_and_retry();
    assert(geteuid() == uid && getegid() == gid);
    assert(lv_group_get_count() == 0);
    lilygo_ui_fonts_deinit();
    lv_deinit();
    return 0;
}
