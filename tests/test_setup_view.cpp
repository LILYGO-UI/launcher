#include "setup_view.h"

#include <cm0/typography.h>
#include <lvgl.h>

#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <vector>
#include <cstring>
#include <string>

using namespace launcher;
namespace {
std::vector<lv_color32_t> framebuffer;
void flush_display(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    if (!framebuffer.empty()) {
        const int width = area->x2 - area->x1 + 1;
        const int display_width = lv_display_get_horizontal_resolution(display);
        const auto *source = reinterpret_cast<const lv_color32_t *>(pixels);
        for (int y = area->y1; y <= area->y2; ++y)
            std::memcpy(framebuffer.data() + static_cast<size_t>(y) * display_width + area->x1,
                        source + static_cast<size_t>(y - area->y1) * width, width * sizeof(*source));
    }
    lv_display_flush_ready(display);
}
lv_obj_t *find(const char *name)
{
    auto *object = lv_obj_find_by_name(lv_layer_top(), name);
    assert(object);
    return object;
}
void click(const char *name) { assert(lv_obj_send_event(find(name), LV_EVENT_CLICKED, nullptr) == LV_RESULT_OK); }
void select(const char *name, unsigned index)
{
    auto *object = find(name);
    lv_dropdown_set_selected(object, index);
    assert(lv_obj_send_event(object, LV_EVENT_VALUE_CHANGED, nullptr) == LV_RESULT_OK);
}
SetupCatalog catalog()
{
    SetupCatalog choices;
    choices.countries = {{"GB", "United Kingdom", ""}, {"CN", "China", ""}};
    choices.locales = {{"en_GB.UTF-8", "English (United Kingdom)", "GB"}, {"zh_CN.UTF-8", "Chinese (Simplified)", "CN"}};
    choices.timezones = {{"Europe/London", "London", "GB"}, {"Asia/Shanghai", "Shanghai", "CN"}};
    choices.keyboards = {{"gb", "English (UK)", ""}, {"us", "English (US)", ""}};
    return choices;
}
void no_request(SetupView &view) { SetupViewRequest request; assert(!view.take_request(request)); }
SetupViewRequest take(SetupView &view, SetupAction action)
{
    SetupViewRequest request;
    assert(view.take_request(request));
    assert(request.action == action);
    no_request(view);
    return request;
}
void test_region()
{
    SetupView view(catalog());
    assert(view.valid());
    assert(lv_obj_has_flag(find("setup_keyboard"), LV_OBJ_FLAG_HIDDEN));
    assert(std::strstr(lv_label_get_text(find("setup_step")), "1 of 5"));
    select("setup_country", 1);
    select("setup_locale", 1);
    select("setup_timezone", 1);
    select("setup_keymap", 1);
    view.set_busy(true, "Applying regional settings...");
    click("setup_primary");
    no_request(view);
    assert(lv_obj_has_state(find("setup_country"), LV_STATE_DISABLED));
    view.set_busy(false);
    click("setup_primary");
    select("setup_country", 0);
    click("setup_primary"); // Cannot overwrite a pending submission.
    auto request = take(view, SetupAction::ApplyRegion);
    assert(request.region.country == "CN");
    assert(request.region.locale == "zh_CN.UTF-8");
    assert(request.region.timezone == "Asia/Shanghai");
    assert(request.region.keyboard == "us");
    view.set_region(request.region);
    assert(lv_dropdown_get_selected(find("setup_country")) == 1);
    view.set_error("Could not apply regional settings");
    assert(std::strstr(lv_label_get_text(find("setup_error")), "Could not"));
    view.show(SetupPage::Region);
    assert(lv_dropdown_get_selected(find("setup_locale")) == 1);
    assert(lv_dropdown_get_selected(find("setup_timezone")) == 1);
}
void test_unavailable_catalog()
{
    SetupView view({});
    click("setup_primary");
    no_request(view);
    assert(std::strstr(lv_label_get_text(find("setup_error")), "unavailable"));
}
void test_network()
{
    SetupView view(catalog());
    view.show(SetupPage::Network);
    assert(std::strstr(lv_label_get_text(find("setup_step")), "3 of 5"));
    auto *password = find("setup_wifi_password");
    assert(lv_textarea_get_password_mode(password));
    click("setup_primary");
    no_request(view);
    assert(std::strstr(lv_label_get_text(find("setup_error")), "network"));
    SetupNetworkState state;
    state.available = true;
    state.interface = "wlan0";
    state.message = "Choose a network.";
    state.networks = {{"Home", "wpa-psk", 90, false, true}, {"Guest", "open", 70, false, true},
                      {"Office", "unsupported", 80, false, false}, {"WPA3", "sae", 60, false, true}, {"Enhanced Open", "owe", 50, false, true}};
    view.set_network(state);
    select("setup_networks", 1);
    assert(std::strcmp(lv_textarea_get_text(find("setup_ssid")), "Home") == 0);
    click("setup_primary");
    no_request(view);
    assert(std::strstr(lv_label_get_text(find("setup_error")), "password"));
    lv_textarea_set_text(password, "wireless-password");
    click("setup_primary");
    assert(std::strcmp(lv_textarea_get_text(password), "") == 0);
    lv_textarea_set_text(password, "replacement-secret");
    click("setup_primary");
    auto request = take(view, SetupAction::ConnectWifi);
    assert(request.wifi.ssid == "Home" && request.wifi.password == "wireless-password");
    assert(request.wifi.security == "wpa-psk" && !request.wifi.hidden);
    select("setup_networks", 2);
    assert(lv_obj_has_state(password, LV_STATE_DISABLED));
    assert(std::strcmp(lv_textarea_get_text(password), "") == 0);
    click("setup_primary");
    request = take(view, SetupAction::ConnectWifi);
    assert(request.wifi.ssid == "Guest" && request.wifi.security == "open" && request.wifi.password.empty());
    select("setup_networks", 3);
    assert(std::strstr(lv_label_get_text(find("setup_error")), "unsupported"));
    assert(std::strcmp(lv_textarea_get_text(find("setup_ssid")), "") == 0);
    click("setup_primary");
    no_request(view);
    select("setup_networks", 4);
    assert(lv_dropdown_get_selected(find("setup_wifi_security")) == 2);
    lv_textarea_set_text(find("setup_ssid"), "hidden-wpa3");
    lv_obj_add_state(find("setup_hidden_network"), LV_STATE_CHECKED);
    lv_textarea_set_text(password, "a hidden password");
    click("setup_primary");
    request = take(view, SetupAction::ConnectWifi);
    assert(request.wifi.hidden && request.wifi.security == "sae");
    assert(request.wifi.ssid == "hidden-wpa3");
    select("setup_networks", 5);
    assert(lv_dropdown_get_selected(find("setup_wifi_security")) == 3);
    assert(lv_obj_has_state(password, LV_STATE_DISABLED));
    click("setup_primary");
    request = take(view, SetupAction::ConnectWifi);
    assert(request.wifi.security == "owe" && request.wifi.password.empty());
    select("setup_wifi_security", 1);
    lv_textarea_set_text(password, "discard me");
    view.set_busy(true);
    click("setup_scan");
    click("setup_secondary");
    no_request(view);
    view.set_busy(false);
    click("setup_scan");
    take(view, SetupAction::ScanWifi);
    assert(std::strcmp(lv_textarea_get_text(password), "") == 0);
    click("setup_secondary");
    take(view, SetupAction::SkipWifi);
    // Optional steps never offer a way back to mutate the already-saved account.
    assert(!lv_obj_find_by_name(lv_layer_top(), "setup_back"));
}
void test_updates()
{
    SetupView view(catalog());
    view.show(SetupPage::Update);
    click("setup_back");
    take(view, SetupAction::Back);
    click("setup_primary");
    take(view, SetupAction::CheckUpdates);
    view.set_update({SetupUpdateStatus::Checking, "Checking package indexes...", false});
    view.set_busy(true);
    assert(!lv_obj_has_flag(find("setup_progress"), LV_OBJ_FLAG_HIDDEN));
    click("setup_primary");
    click("setup_secondary");
    click("setup_back");
    no_request(view);
    view.set_update({SetupUpdateStatus::Available, "Updates are available", false});
    view.set_busy(false);
    click("setup_primary");
    take(view, SetupAction::InstallUpdates);
    view.set_update({SetupUpdateStatus::Running, "Installing updates...", false});
    view.set_busy(true);
    click("setup_secondary");
    no_request(view);
    view.set_update({SetupUpdateStatus::Failed, "Download failed. Check your connection.", false});
    view.set_busy(false);
    click("setup_primary");
    take(view, SetupAction::CheckUpdates);
    view.set_update({SetupUpdateStatus::Succeeded, "Updates installed", true});
    assert(lv_obj_has_flag(find("setup_progress"), LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(find("setup_secondary"), LV_OBJ_FLAG_HIDDEN));
    click("setup_primary");
    take(view, SetupAction::SkipUpdates);
    view.show(SetupPage::Complete);
    assert(std::strstr(lv_label_get_text(find("setup_description")), "Restart"));
    assert(std::strstr(lv_label_get_text(find("setup_status")), "restart"));
    click("setup_primary");
    take(view, SetupAction::Finish);
}
struct KeyboardSample { uint32_t key = 0; lv_indev_state_t state = LV_INDEV_STATE_RELEASED; };
void read_keyboard(lv_indev_t *input, lv_indev_data_t *data)
{
    auto *sample = static_cast<KeyboardSample *>(lv_indev_get_user_data(input));
    data->state = sample ? sample->state : LV_INDEV_STATE_RELEASED;
    data->key = sample ? sample->key : 0;
}
void press_key(lv_indev_t *input, KeyboardSample &sample, uint32_t key)
{
    sample.key = key;
    sample.state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    sample.state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
}
lv_indev_t *make_keyboard(lv_group_t *group, KeyboardSample *sample = nullptr)
{
    auto *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_group(input, group);
    lv_indev_set_user_data(input, sample);
    lv_indev_set_read_cb(input, read_keyboard);
    lv_timer_pause(lv_indev_get_read_timer(input));
    return input;
}
void test_update_keyboard_focus()
{
    KeyboardSample sample;
    auto *input = make_keyboard(nullptr, &sample);
    {
        SetupView view(catalog());
        view.show(SetupPage::Update);
        auto *group = lv_indev_get_group(input);
        assert(lv_group_get_focused(group) == find("setup_primary"));
        press_key(input, sample, LV_KEY_ENTER);
        take(view, SetupAction::CheckUpdates);
        struct Transition {
            SetupUpdateStatus running;
            SetupUpdateStatus finished;
            SetupAction action;
        };
        const Transition transitions[] = {
            {SetupUpdateStatus::Checking, SetupUpdateStatus::Available, SetupAction::InstallUpdates},
            {SetupUpdateStatus::Checking, SetupUpdateStatus::Failed, SetupAction::CheckUpdates},
            {SetupUpdateStatus::Running, SetupUpdateStatus::Failed, SetupAction::CheckUpdates},
            {SetupUpdateStatus::Running, SetupUpdateStatus::Succeeded, SetupAction::SkipUpdates},
        };
        for (const auto &transition : transitions) {
            for (bool clear_busy_first : {false, true}) {
                view.set_busy(true);
                view.set_busy(false); // The start worker has returned its status.
                view.set_update({transition.running, "Working", false});
                view.set_busy(true);
                assert(lv_obj_has_flag(find("setup_primary"), LV_OBJ_FLAG_HIDDEN));
                press_key(input, sample, LV_KEY_ENTER);
                no_request(view);
                if (clear_busy_first) view.set_busy(false);
                view.set_update({transition.finished, "Finished", false});
                if (!clear_busy_first) {
                    assert(lv_obj_has_state(find("setup_primary"), LV_STATE_DISABLED));
                    press_key(input, sample, LV_KEY_ENTER);
                    no_request(view);
                    view.set_busy(false);
                }
                assert(!lv_obj_has_state(find("setup_primary"), LV_STATE_DISABLED));
                assert(lv_group_get_focused(group) == find("setup_primary"));
                press_key(input, sample, LV_KEY_ENTER);
                take(view, transition.action);

                // Periodic status messages must preserve a deliberate keyboard
                // selection after the one-time completion focus restoration.
                auto *other = find(transition.finished == SetupUpdateStatus::Succeeded ?
                                   "setup_back" : "setup_secondary");
                lv_group_focus_obj(other);
                view.set_update({transition.finished, "Refreshed status", false});
                view.set_busy(false);
                assert(lv_group_get_focused(group) == other);
                lv_group_focus_obj(find("setup_primary"));
            }
        }
        // The status itself blocks navigation, including during the controller's
        // set_busy(false), set_update(...), set_busy(...) polling sequence.
        view.set_update({SetupUpdateStatus::Checking, "Checking", false});
        for (const char *name : {"setup_primary", "setup_secondary", "setup_back"}) {
            assert(lv_obj_has_state(find(name), LV_STATE_DISABLED));
            click(name);
        }
        view.set_busy(false);
        press_key(input, sample, LV_KEY_ENTER);
        no_request(view);
        view.set_update({SetupUpdateStatus::Available, "Available", false});
        assert(lv_group_get_focused(group) == find("setup_primary"));
        press_key(input, sample, LV_KEY_ENTER);
        take(view, SetupAction::InstallUpdates);
    }
    lv_indev_delete(input);
}
void test_capture_and_resize(lv_display_t *display)
{
    auto *background = lv_group_create();
    lv_group_set_default(background);
    auto *background_button = lv_button_create(lv_screen_active());
    lv_group_focus_obj(background_button);
    KeyboardSample sample;
    auto *input = make_keyboard(background, &sample);
    lv_indev_t *hotplug = nullptr;
    {
        SetupView view(catalog());
        assert(lv_group_get_default() == background);
        assert(lv_indev_get_group(input) != background);
        auto *private_group = lv_indev_get_group(input);
        assert(lv_obj_get_group(find("setup_country")) == private_group);
        hotplug = make_keyboard(nullptr);
        view.capture_input();
        assert(lv_indev_get_group(hotplug) == private_group);
        view.show(SetupPage::Network);
        lv_group_focus_obj(find("setup_wifi_password"));
        press_key(input, sample, 'x');
        assert(std::strcmp(lv_textarea_get_text(find("setup_wifi_password")), "x") == 0);
        press_key(input, sample, LV_KEY_ESC);
        assert(lv_obj_is_valid(find("setup_modal")));
        assert(!lv_obj_has_flag(find("setup_modal"), LV_OBJ_FLAG_HIDDEN));
        no_request(view);
        const int32_t sizes[][2] = {{568, 1232}, {1232, 568}, {320, 240}};
        for (const auto &size : sizes) {
            lv_display_set_resolution(display, size[0], size[1]);
            for (auto page : {SetupPage::Region, SetupPage::Network, SetupPage::Update, SetupPage::Complete}) {
                view.show(page);
                if (page == SetupPage::Network) {
                    click("setup_wifi_password");
                    assert(!lv_obj_has_flag(find("setup_keyboard"), LV_OBJ_FLAG_HIDDEN));
                    assert(lv_keyboard_get_textarea(find("setup_keyboard")) == find("setup_wifi_password"));
                }
                lv_obj_update_layout(lv_layer_top());
                auto *card = find("setup_card");
                lv_area_t area;
                lv_obj_get_coords(card, &area);
                assert(area.x1 >= 0 && area.x2 < size[0]);
                assert(area.y1 >= 0 && area.y2 < size[1]);
                if (page == SetupPage::Network) {
                    lv_area_t field_area;
                    lv_obj_get_coords(find("setup_wifi_password"), &field_area);
                    assert(field_area.y1 >= area.y1 && field_area.y2 <= area.y2);
                }
                if (page == SetupPage::Region) {
                    auto *choice = find("setup_locale");
                    lv_obj_scroll_to_view_recursive(choice, LV_ANIM_OFF);
                    lv_obj_update_layout(lv_layer_top());
                    lv_dropdown_open(choice);
                    lv_obj_update_layout(lv_layer_top());
                    lv_area_t popup;
                    lv_obj_get_coords(lv_dropdown_get_list(choice), &popup);
                    assert(popup.x1 >= 0 && popup.x2 < size[0]);
                    assert(popup.y1 >= 0 && popup.y2 < size[1]);
                    lv_dropdown_close(choice);
                }
                auto *primary = find("setup_primary");
                assert(lv_obj_get_height(primary) >= 52);
                lv_obj_scroll_to_view(primary, LV_ANIM_OFF);
                lv_obj_update_layout(lv_layer_top());
                lv_area_t button_area;
                lv_obj_get_coords(primary, &button_area);
                assert(button_area.x1 >= area.x1 && button_area.x2 <= area.x2);
                assert(button_area.y1 >= area.y1 && button_area.y2 <= area.y2);
                if (page == SetupPage::Network) {
                    lv_area_t keyboard_area;
                    lv_obj_get_coords(find("setup_keyboard"), &keyboard_area);
                    assert(area.y2 < keyboard_area.y1);
                    view.set_hardware_keyboard_present(true);
                    assert(lv_obj_has_flag(find("setup_keyboard"), LV_OBJ_FLAG_HIDDEN));
                    assert(lv_obj_get_group(find("setup_keyboard")) == nullptr);
                    view.set_hardware_keyboard_present(false);
                    assert(!lv_obj_has_flag(find("setup_keyboard"), LV_OBJ_FLAG_HIDDEN));
                    click("setup_card");
                    assert(lv_obj_has_flag(find("setup_keyboard"), LV_OBJ_FLAG_HIDDEN));
                }
            }
        }
    }
    assert(lv_indev_get_group(input) == background);
    assert(lv_indev_get_group(hotplug) == nullptr);
    assert(lv_group_get_default() == background);
    assert(lv_group_get_focused(background) == background_button);
    lv_indev_delete(input);
    lv_indev_delete(hotplug);
    lv_obj_delete(background_button);
    lv_group_set_default(nullptr);
    lv_group_delete(background);
    lv_display_set_resolution(display, 568, 1232);
}
void render_pages(lv_display_t *display, const char *directory)
{
    const int sizes[][2] = {{568, 1232}, {1232, 568}, {320, 240}};
    const char *page_names[] = {"region", "wifi", "updates", "complete"};
    for (const auto &size : sizes) {
        lv_display_set_resolution(display, size[0], size[1]);
        framebuffer.assign(static_cast<size_t>(size[0]) * size[1], {});
        SetupView view(catalog());
        for (unsigned i = 0; i < 4; ++i) {
            view.show(static_cast<SetupPage>(i));
            if (i == 1) view.set_network({true, false, "wlan0", "Choose a network, or enter a hidden network name.",
                                         {{"LILYGO Studio", "wpa-psk", 92, false, true}, {"Guest", "open", 68, false, true}}});
            if (i == 2) view.set_update({SetupUpdateStatus::Available, "Updates are available. Keep your device connected to power while installing.", false});
            lv_obj_update_layout(lv_layer_top());
            lv_obj_invalidate(lv_screen_active());
            lv_obj_invalidate(lv_layer_top());
            lv_tick_inc(20);
            lv_refr_now(display);
            std::string path = std::string(directory) + "/" + page_names[i] + "-" + std::to_string(size[0]) + "x" + std::to_string(size[1]) + ".ppm";
            FILE *file = std::fopen(path.c_str(), "wb");
            assert(file);
            std::fprintf(file, "P6\n%d %d\n255\n", size[0], size[1]);
            for (const auto &pixel : framebuffer) {
                const unsigned char rgb[] = {pixel.red, pixel.green, pixel.blue};
                assert(std::fwrite(rgb, 1, sizeof(rgb), file) == sizeof(rgb));
            }
            std::fclose(file);
        }
    }
    framebuffer.clear();
}
void test_deleted_background()
{
    auto *background = lv_group_create();
    lv_group_set_default(background);
    auto *button = lv_button_create(lv_screen_active());
    auto *input = make_keyboard(background);
    {
        SetupView view(catalog());
        lv_obj_delete(button);
        lv_group_delete(background);
    }
    assert(lv_indev_get_group(input) == nullptr);
    assert(lv_group_get_default() == nullptr);
    lv_indev_delete(input);
}
}

int main(int argc, char **argv)
{
    lv_init();
    auto *display = lv_display_create(568, 1232);
    assert(display);
    static lv_color32_t buffer[1232 * 40];
    lv_display_set_buffers(display, buffer, nullptr, sizeof(buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    test_region();
    test_unavailable_catalog();
    test_network();
    test_updates();
    test_update_keyboard_focus();
    test_capture_and_resize(display);
    test_deleted_background();
    if (argc == 2) render_pages(display, argv[1]);
    assert(lv_group_get_count() == 0);
    lilygo_ui_fonts_deinit();
    lv_deinit();
    return 0;
}
