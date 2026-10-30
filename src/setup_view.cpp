#include "setup_view.h"
#include "launcher_theme.h"

#include <cm0/typography.h>
#include <lvgl.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace launcher {
namespace {
bool group_exists(lv_group_t *group)
{
    for (uint32_t i = 0; group && i < lv_group_get_count(); ++i)
        if (lv_group_by_index(i) == group) return true;
    return false;
}
void erase_secret(std::string &secret)
{
    if (!secret.empty()) {
        volatile char *p = &secret[0];
        for (size_t i = 0; i < secret.size(); ++i) p[i] = 0;
    }
    secret.clear();
}
std::string one_line(std::string text)
{
    for (char &c : text) if (c == '\n' || c == '\r') c = ' ';
    return text;
}
}

struct SetupView::Impl {
    struct Binding { lv_indev_t *input; lv_group_t *group; };
    SetupCatalog catalog;
    SetupRegion region;
    SetupNetworkState network;
    SetupUpdateState update;
    SetupPage page = SetupPage::Region;
    SetupViewRequest pending_request;
    bool pending = false;
    bool busy = false;
    bool restore_update_focus = false;
    bool hardware_keyboard = false;
    bool software_keyboard = false;
    lv_obj_t *modal = nullptr;
    lv_obj_t *card = nullptr;
    lv_obj_t *body = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *error = nullptr;
    lv_obj_t *primary = nullptr;
    lv_obj_t *primary_label = nullptr;
    lv_obj_t *secondary = nullptr;
    lv_obj_t *secondary_label = nullptr;
    lv_obj_t *keyboard = nullptr;
    lv_obj_t *country = nullptr;
    lv_obj_t *locale = nullptr;
    lv_obj_t *timezone = nullptr;
    lv_obj_t *keymap = nullptr;
    lv_obj_t *networks = nullptr;
    lv_obj_t *ssid = nullptr;
    lv_obj_t *password = nullptr;
    lv_obj_t *hidden = nullptr;
    lv_obj_t *security = nullptr;
    lv_obj_t *scan = nullptr;
    lv_obj_t *progress = nullptr;
    lv_group_t *group = nullptr;
    lv_group_t *previous_group = nullptr;
    lv_obj_t *previous_focus = nullptr;
    const lv_font_t *font14 = nullptr;
    const lv_font_t *font22 = nullptr;
    const lv_font_t *font36 = nullptr;
    std::vector<Binding> bindings;
    std::vector<lv_obj_t *> controls;

    explicit Impl(const SetupCatalog &choices) : catalog(choices), region(choices.current)
    {
        font14 = lilygo_ui_font_get(14);
        font22 = lilygo_ui_font_get(22);
        font36 = lilygo_ui_font_get(36);
        if (!font14 || !font22 || !font36) return;
        previous_group = lv_group_get_default();
        previous_focus = previous_group ? lv_group_get_focused(previous_group) : nullptr;
        group = lv_group_create();
        if (!group) return;
        modal = lv_obj_create(lv_layer_top());
        lv_obj_set_name_static(modal, "setup_modal");
        lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
        lv_obj_center(modal);
        lv_obj_add_flag(modal, LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(modal, lv_color_hex(theme::kPageBackground), 0);
        lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(modal, 0, 0);
        lv_obj_set_style_pad_all(modal, 0, 0);
        lv_obj_add_event_cb(modal, [](lv_event_t *event) {
            static_cast<Impl *>(lv_event_get_user_data(event))->layout();
        }, LV_EVENT_SIZE_CHANGED, this);
        lv_obj_add_event_cb(modal, dismiss_event, LV_EVENT_CLICKED, this);
        card = lv_obj_create(modal);
        lv_obj_set_name_static(card, "setup_card");
        lv_obj_add_flag(card, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_scroll_dir(card, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_bg_color(card, lv_color_hex(theme::kContainerBackground), 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 22, 0);
        lv_obj_set_style_shadow_width(card, 28, 0);
        lv_obj_set_style_shadow_opa(card, LV_OPA_20, 0);
        lv_obj_set_style_shadow_color(card, lv_color_hex(theme::kPanelShadow), 0);
        lv_obj_set_style_shadow_offset_y(card, 8, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_event_cb(card, dismiss_event, LV_EVENT_CLICKED, this);
        keyboard = lv_keyboard_create(modal);
        lv_obj_set_name_static(keyboard, "setup_keyboard");
        lv_keyboard_set_popovers(keyboard, false);
        lv_obj_set_style_text_font(keyboard, font22, 0);
        lv_obj_set_style_bg_color(keyboard, lv_color_hex(theme::kAccountKeyboard), 0);
        lv_obj_set_style_bg_opa(keyboard, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(keyboard, 0, 0);
        lv_obj_set_style_bg_color(keyboard, lv_color_hex(theme::kContainerBackground), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(keyboard, lv_color_hex(theme::kControlPressed), LV_PART_ITEMS | LV_STATE_PRESSED);
        lv_obj_set_style_text_color(keyboard, lv_color_hex(theme::kText), LV_PART_ITEMS);
        lv_obj_set_style_radius(keyboard, 8, LV_PART_ITEMS);
        lv_group_remove_obj(keyboard);
        lv_obj_add_event_cb(keyboard, dismiss_event, LV_EVENT_READY, this);
        lv_obj_add_event_cb(keyboard, dismiss_event, LV_EVENT_CANCEL, this);
        build();
        capture();
        lv_obj_move_foreground(modal);
    }

    ~Impl()
    {
        erase_secret(pending_request.wifi.password);
        if (modal) lv_obj_delete(modal);
        lv_group_t *previous = group_exists(previous_group) ? previous_group : nullptr;
        if (lv_group_get_default() == group) lv_group_set_default(previous);
        for (lv_indev_t *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
            if (lv_indev_get_group(input) != group) continue;
            lv_group_t *target = previous;
            for (const auto &binding : bindings) {
                if (binding.input != input) continue;
                target = group_exists(binding.group) ? binding.group : nullptr;
                break;
            }
            lv_indev_reset(input, nullptr);
            lv_indev_set_group(input, target);
        }
        if (previous_focus && lv_obj_is_valid(previous_focus) && lv_obj_get_group(previous_focus) == previous)
            lv_group_focus_obj(previous_focus);
        if (group_exists(group)) lv_group_delete(group);
    }

    void capture()
    {
        for (lv_indev_t *input = lv_indev_get_next(nullptr); input; input = lv_indev_get_next(input)) {
            auto type = lv_indev_get_type(input);
            if (type != LV_INDEV_TYPE_KEYPAD && type != LV_INDEV_TYPE_ENCODER) continue;
            auto *original = lv_indev_get_group(input);
            if (original == group) continue;
            auto it = std::find_if(bindings.begin(), bindings.end(), [input](const Binding &b) { return b.input == input; });
            if (it == bindings.end()) bindings.push_back({input, original});
            else it->group = original;
            lv_indev_reset(input, nullptr);
            lv_indev_set_group(input, group);
        }
    }

    lv_obj_t *label(lv_obj_t *parent, const char *name, const char *text, const lv_font_t *font, uint32_t color)
    {
        auto *object = lv_label_create(parent);
        if (name) lv_obj_set_name_static(object, name);
        lv_label_set_text(object, text);
        lv_obj_set_width(object, LV_PCT(100));
        lv_label_set_long_mode(object, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(object, font, 0);
        lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
        return object;
    }

    void style_control(lv_obj_t *object)
    {
        lv_obj_set_width(object, LV_PCT(100));
        lv_obj_set_height(object, 52);
        lv_obj_set_style_min_height(object, 52, 0);
        lv_obj_set_style_text_font(object, font22, 0);
        lv_obj_set_style_text_color(object, lv_color_hex(theme::kText), 0);
        lv_obj_set_style_bg_color(object, lv_color_hex(theme::kAccountFieldBackground), 0);
        lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(object, lv_color_hex(theme::kAccountFieldBorder), 0);
        lv_obj_set_style_border_width(object, 1, 0);
        lv_obj_set_style_border_width(object, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_border_color(object, lv_color_hex(theme::kAccountAccent), LV_STATE_FOCUSED);
        lv_obj_set_style_border_width(object, 2, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_border_color(object, lv_color_hex(theme::kAccountAccent), LV_STATE_FOCUS_KEY);
        lv_obj_set_style_outline_width(object, 0, LV_STATE_FOCUS_KEY);
        lv_obj_set_style_bg_color(object, lv_color_hex(theme::kControlPressed), LV_STATE_DISABLED);
        lv_obj_set_style_text_color(object, lv_color_hex(theme::kMutedText), LV_STATE_DISABLED);
        lv_obj_set_style_radius(object, 12, 0);
        lv_obj_set_style_pad_left(object, 14, 0);
        lv_obj_set_style_pad_right(object, 14, 0);
        lv_obj_set_style_pad_top(object, 11, 0);
        lv_obj_set_style_pad_bottom(object, 11, 0);
        lv_group_add_obj(group, object);
        controls.push_back(object);
    }

    lv_obj_t *dropdown(const char *name, const char *caption, const std::vector<SetupChoice> &choices,
                       const std::string &selected)
    {
        label(body, nullptr, caption, font14, theme::kMutedText);
        auto *object = lv_dropdown_create(body);
        lv_obj_set_name_static(object, name);
        style_control(object);
        std::string options;
        unsigned selection = 0;
        for (size_t i = 0; i < choices.size(); ++i) {
            if (i) options += '\n';
            options += one_line(choices[i].label.empty() ? choices[i].value : choices[i].label);
            if (choices[i].value == selected) selection = static_cast<unsigned>(i);
        }
        lv_dropdown_set_options(object, options.empty() ? "Unavailable" : options.c_str());
        lv_dropdown_set_selected(object, selection);
        auto *list = lv_dropdown_get_list(object);
        lv_obj_set_style_text_font(list, font22, 0);
        lv_obj_set_style_max_height(list, 320, 0);
        lv_obj_set_style_text_color(list, lv_color_hex(theme::kText), 0);
        lv_obj_set_style_bg_color(list, lv_color_hex(theme::kContainerBackground), 0);
        lv_obj_add_event_cb(object, [](lv_event_t *event) {
            auto *dropdown = lv_event_get_current_target_obj(event);
            // Catalog and SSID labels can be wider than a compact display.
            // Keep the popup inside the field bounds; its content can scroll.
            lv_obj_set_style_max_width(lv_dropdown_get_list(dropdown), lv_obj_get_width(dropdown), 0);
        }, LV_EVENT_READY, this);
        lv_obj_add_event_cb(object, [](lv_event_t *event) {
            auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
            self->software_keyboard = false;
            self->layout();
        }, LV_EVENT_CLICKED, this);
        return object;
    }

    lv_obj_t *field(const char *name, const char *placeholder, bool secret)
    {
        auto *object = lv_textarea_create(body);
        lv_obj_set_name_static(object, name);
        style_control(object);
        lv_textarea_set_one_line(object, true);
        lv_textarea_set_max_length(object, secret ? 64 : 32);
        lv_textarea_set_placeholder_text(object, placeholder);
        lv_textarea_set_password_mode(object, secret);
        lv_obj_set_style_text_color(object, lv_color_hex(theme::kAccountPlaceholder), LV_PART_TEXTAREA_PLACEHOLDER);
        for (auto event : {LV_EVENT_CLICKED, LV_EVENT_FOCUSED})
            lv_obj_add_event_cb(object, [](lv_event_t *event) {
                auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
                if (self->busy) return;
                auto *field = lv_event_get_current_target_obj(event);
                lv_keyboard_set_textarea(self->keyboard, field);
                self->software_keyboard = true;
                self->layout();
                lv_obj_scroll_to_view_recursive(field, LV_ANIM_OFF);
            }, event, this);
        return object;
    }

    lv_obj_t *button(const char *name, const char *text, bool accent, lv_event_cb_t callback, lv_obj_t **text_object = nullptr)
    {
        auto *object = lv_button_create(card);
        lv_obj_set_name_static(object, name);
        style_control(object);
        lv_obj_set_style_bg_color(object, lv_color_hex(accent ? theme::kAccountAccent : theme::kAccountFieldBackground), 0);
        lv_obj_set_style_bg_color(object, lv_color_hex(accent ? theme::kAccountAccentPressed : theme::kControlPressed), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(object, 0, 0);
        auto *text_label = label(object, nullptr, text, font22, accent ? theme::kIconForeground : theme::kAccountAccent);
        lv_obj_set_style_text_align(text_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(text_label);
        if (text_object) *text_object = text_label;
        lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, this);
        return object;
    }

    static void dismiss_event(lv_event_t *event)
    {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
        self->software_keyboard = false;
        self->layout();
    }

    static void primary_event(lv_event_t *event)
    {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
        self->submit_primary();
    }

    static void secondary_event(lv_event_t *event)
    {
        auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
        if (self->page == SetupPage::Network) self->emit(SetupAction::SkipWifi);
        if (self->page == SetupPage::Update) self->emit(SetupAction::SkipUpdates);
    }

    bool update_running() const
    {
        return page == SetupPage::Update &&
               (update.status == SetupUpdateStatus::Checking || update.status == SetupUpdateStatus::Running);
    }

    void emit(SetupAction action)
    {
        if (busy || pending || update_running()) return;
        pending_request = {};
        pending_request.action = action;
        pending = true;
        software_keyboard = false;
        if (password) lv_textarea_set_text(password, "");
        lv_label_set_text(error, "");
        layout();
    }

    static std::string selected(lv_obj_t *object, const std::vector<SetupChoice> &choices)
    {
        const auto i = lv_dropdown_get_selected(object);
        return i < choices.size() ? choices[i].value : std::string{};
    }

    void select(lv_obj_t *object, const std::vector<SetupChoice> &choices, const std::string &value)
    {
        if (!object) return;
        for (size_t i = 0; i < choices.size(); ++i)
            if (choices[i].value == value) { lv_dropdown_set_selected(object, static_cast<uint32_t>(i)); return; }
    }

    void submit_primary()
    {
        if (busy || pending || update_running()) return;
        switch (page) {
        case SetupPage::Region:
            if (catalog.countries.empty() || catalog.locales.empty() || catalog.timezones.empty() || catalog.keyboards.empty()) {
                lv_label_set_text(error, "Regional settings are unavailable. Install the system locale and keyboard data, then restart setup.");
                return;
            }
            region = {selected(country, catalog.countries), selected(locale, catalog.locales),
                      selected(timezone, catalog.timezones), selected(keymap, catalog.keyboards)};
            emit(SetupAction::ApplyRegion);
            pending_request.region = region;
            break;
        case SetupPage::Network: {
            SetupWifiRequest request;
            request.ssid = lv_textarea_get_text(ssid);
            request.password = lv_textarea_get_text(password);
            request.hidden = lv_obj_has_state(hidden, LV_STATE_CHECKED);
            const unsigned secure = lv_dropdown_get_selected(security);
            request.security = secure == 0 ? "open" : secure == 2 ? "sae" : secure == 3 ? "owe" : "wpa-psk";
            if (request.ssid.empty()) {
                lv_label_set_text(error, "Choose a network or enter its name.");
                erase_secret(request.password);
                return;
            }
            if (request.security != "open" && request.security != "owe" && request.password.empty()) {
                lv_label_set_text(error, "Enter the Wi-Fi password.");
                return;
            }
            emit(SetupAction::ConnectWifi);
            pending_request.wifi = std::move(request);
            break;
        }
        case SetupPage::Update:
            emit(update.status == SetupUpdateStatus::Available ? SetupAction::InstallUpdates :
                 update.status == SetupUpdateStatus::Succeeded ? SetupAction::SkipUpdates : SetupAction::CheckUpdates);
            break;
        case SetupPage::Complete: emit(SetupAction::Finish); break;
        }
    }

    void build()
    {
        if (!modal || !group) return;
        restore_update_focus = false;
        software_keyboard = false;
        lv_keyboard_set_textarea(keyboard, nullptr);
        lv_obj_clean(card);
        controls.clear();
        country = locale = timezone = keymap = networks = ssid = password = hidden = security = scan = nullptr;
        primary = primary_label = secondary = secondary_label = progress = nullptr;
        auto *brand = label(card, "setup_brand", "LILYGO UI", font14, theme::kAccountAccent);
        lv_obj_set_style_text_align(brand, LV_TEXT_ALIGN_CENTER, 0);
        const char *step = page == SetupPage::Region ? "1 of 5  /  Regional settings" :
                           page == SetupPage::Network ? "3 of 5  /  Wi-Fi" :
                           page == SetupPage::Update ? "4 of 5  /  Software updates" : "5 of 5  /  Ready";
        auto *step_label = label(card, "setup_step", step, font14, theme::kMutedText);
        lv_obj_set_style_text_align(step_label, LV_TEXT_ALIGN_CENTER, 0);
        const char *title = page == SetupPage::Region ? "Welcome" : page == SetupPage::Network ? "Connect to Wi-Fi" :
                            page == SetupPage::Update ? "Keep up to date" : "You're ready";
        auto *title_label = label(card, "setup_title", title, font36, theme::kContainerTitle);
        lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
        const char *description = page == SetupPage::Region ? "Choose your system preferences. Launcher text input uses a US keyboard layout." :
                                  page == SetupPage::Network ? "Connect for updates and online services, or continue offline." :
                                  page == SetupPage::Update ? "Check for the latest system and security updates." :
                                  "Your account and preferences are saved. Restart when convenient to apply all language and keyboard changes.";
        label(card, "setup_description", description, font14, theme::kMutedText);
        body = lv_obj_create(card);
        lv_obj_set_name_static(body, "setup_body");
        lv_obj_set_size(body, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(body, 0, 0);
        lv_obj_set_style_pad_all(body, 0, 0);
        lv_obj_set_style_pad_row(body, 8, 0);
        lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
        lv_obj_add_event_cb(body, dismiss_event, LV_EVENT_CLICKED, this);
        if (page == SetupPage::Region) {
            country = dropdown("setup_country", "Country", catalog.countries, region.country);
            locale = dropdown("setup_locale", "System language", catalog.locales, region.locale);
            timezone = dropdown("setup_timezone", "Time zone", catalog.timezones, region.timezone);
            keymap = dropdown("setup_keymap", "System keyboard layout", catalog.keyboards, region.keyboard);
        } else if (page == SetupPage::Network) {
            networks = dropdown("setup_networks", "Available networks", {{"manual", "Choose a network or enter its name", ""}}, "manual");
            lv_obj_add_event_cb(networks, [](lv_event_t *event) {
                auto *self = static_cast<Impl *>(lv_event_get_user_data(event));
                if (self->busy) return;
                const unsigned i = lv_dropdown_get_selected(self->networks);
                if (!i || i > self->network.networks.size()) return;
                const auto &network = self->network.networks[i - 1];
                lv_textarea_set_text(self->ssid, network.ssid.c_str());
                lv_textarea_set_text(self->password, "");
                lv_obj_remove_state(self->hidden, LV_STATE_CHECKED);
                const auto &security = network.security;
                lv_dropdown_set_selected(self->security, security == "open" || security.empty() || security == "--" ? 0 : security == "sae" || security == "WPA3" ? 2 : security == "owe" ? 3 : 1);
                lv_label_set_text(self->error, network.supported ? "" : "This network uses unsupported authentication. Choose another network or continue offline.");
                if (!network.supported) lv_textarea_set_text(self->ssid, "");
                self->refresh_security();
            }, LV_EVENT_VALUE_CHANGED, this);
            ssid = field("setup_ssid", "Network name (SSID)", false);
            hidden = lv_checkbox_create(body);
            lv_obj_set_name_static(hidden, "setup_hidden_network");
            lv_checkbox_set_text(hidden, "Hidden network");
            lv_obj_set_style_text_font(hidden, font14, 0);
            lv_obj_set_style_text_color(hidden, lv_color_hex(theme::kText), 0);
            lv_obj_set_style_pad_top(hidden, 8, 0);
            lv_obj_set_style_pad_bottom(hidden, 8, 0);
            lv_obj_set_width(hidden, LV_PCT(100));
            controls.push_back(hidden);
            lv_group_add_obj(group, hidden);
            security = dropdown("setup_wifi_security", "Security", {{"open", "Open (no password)", ""}, {"wpa-psk", "WPA / WPA2 Personal", ""}, {"sae", "WPA3 Personal", ""}, {"owe", "Enhanced Open (OWE)", ""}}, "wpa-psk");
            lv_obj_add_event_cb(security, [](lv_event_t *event) {
                static_cast<Impl *>(lv_event_get_user_data(event))->refresh_security();
            }, LV_EVENT_VALUE_CHANGED, this);
            password = field("setup_wifi_password", "Wi-Fi password", true);
        }
        status = label(card, "setup_status", "", font14, theme::kMutedText);
        error = label(card, "setup_error", "", font14, theme::kAccountError);
        if (page == SetupPage::Update) {
            progress = lv_spinner_create(card);
            lv_obj_set_name_static(progress, "setup_progress");
            lv_obj_set_size(progress, 36, 36);
            lv_spinner_set_anim_params(progress, 1000, 200);
        }
        const char *button_text = page == SetupPage::Region ? "Continue to account" : page == SetupPage::Network ? "Connect" :
                                  page == SetupPage::Update ? "Check for updates" : "Start using LILYGO";
        primary = button("setup_primary", button_text, true, primary_event, &primary_label);
        if (page == SetupPage::Network) {
            scan = button("setup_scan", "Scan again", false, [](lv_event_t *event) {
                static_cast<Impl *>(lv_event_get_user_data(event))->emit(SetupAction::ScanWifi);
            });
            secondary = button("setup_secondary", "Skip Wi-Fi", false, secondary_event, &secondary_label);
            refresh_network();
        } else if (page == SetupPage::Update) {
            secondary = button("setup_secondary", "Skip updates", false, secondary_event, &secondary_label);
            button("setup_back", "Back to Wi-Fi", false, [](lv_event_t *event) {
                static_cast<Impl *>(lv_event_get_user_data(event))->emit(SetupAction::Back);
            });
            refresh_update();
        }
        if (page == SetupPage::Complete && update.reboot_required)
            lv_label_set_text(status, "Installed updates require a restart.");
        apply_busy();
        lv_obj_scroll_to_y(card, 0, LV_ANIM_OFF);
        layout();
        auto *initial_focus = page == SetupPage::Region ? country : page == SetupPage::Network ? networks : primary;
        if (initial_focus && !lv_obj_has_flag(initial_focus, LV_OBJ_FLAG_HIDDEN) &&
            !lv_obj_has_state(initial_focus, LV_STATE_DISABLED)) lv_group_focus_obj(initial_focus);
    }

    void refresh_security()
    {
        if (!password || !security) return;
        const auto selection = lv_dropdown_get_selected(security);
        const bool open = selection == 0 || selection == 3;
        if (open) {
            lv_textarea_set_text(password, "");
            lv_obj_add_state(password, LV_STATE_DISABLED);
            if (lv_keyboard_get_textarea(keyboard) == password) software_keyboard = false;
        } else if (!busy) lv_obj_remove_state(password, LV_STATE_DISABLED);
        layout();
    }

    void refresh_network()
    {
        if (page != SetupPage::Network || !networks) return;
        std::string options = "Enter a network manually";
        for (const auto &entry : network.networks) {
            options += '\n' + one_line(entry.ssid) + "  (" + std::to_string(entry.signal) + "%)";
            if (entry.connected) options += " - connected";
            else if (!entry.supported) options += " - unsupported";
            else if (entry.security.empty() || entry.security == "open" || entry.security == "--") options += " - open";
        }
        lv_dropdown_set_options(networks, options.c_str());
        lv_dropdown_set_selected(networks, 0);
        lv_label_set_text(status, network.message.c_str());
        lv_label_set_text(secondary_label, network.connected ? "Continue" : "Skip Wi-Fi");
    }

    void refresh_update()
    {
        if (page == SetupPage::Complete && status) {
            lv_label_set_text(status, update.reboot_required ? "Installed updates require a restart." : "");
            return;
        }
        if (page != SetupPage::Update || !primary) return;
        const bool running = update_running();
        if (running) {
            lv_obj_add_flag(primary, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(progress, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(primary, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(progress, LV_OBJ_FLAG_HIDDEN);
        }
        const char *text = update.status == SetupUpdateStatus::Available ? "Install updates" :
                           update.status == SetupUpdateStatus::Succeeded ? "Continue" :
                           update.status == SetupUpdateStatus::Failed ? "Retry updates" : "Check for updates";
        lv_label_set_text(primary_label, text);
        lv_label_set_text(secondary_label, "Skip updates");
        if (update.status == SetupUpdateStatus::Succeeded) lv_obj_add_flag(secondary, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(secondary, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(status, update.message.c_str());
        apply_busy();
    }

    void apply_busy()
    {
        const bool disabled = busy || update_running();
        for (auto *control : controls) {
            if (disabled) lv_obj_add_state(control, LV_STATE_DISABLED);
            else lv_obj_remove_state(control, LV_STATE_DISABLED);
        }
        if (disabled) lv_obj_add_state(keyboard, LV_STATE_DISABLED);
        else lv_obj_remove_state(keyboard, LV_STATE_DISABLED);
        refresh_security();
        // Hiding the running update's primary button moves LVGL's focus to
        // Skip. Restore it once, only after the result and busy state permit
        // interaction, so Enter performs Install / Retry / Continue.
        if (restore_update_focus && !disabled && page == SetupPage::Update && primary &&
            !lv_obj_has_flag(primary, LV_OBJ_FLAG_HIDDEN)) {
            restore_update_focus = false;
            lv_group_focus_obj(primary);
        }
    }

    void layout()
    {
        if (!modal || !card || !keyboard) return;
        const int32_t width = lv_obj_get_width(modal);
        const int32_t height = lv_obj_get_height(modal);
        if (width < 1 || height < 1) return;
        const bool shown = page == SetupPage::Network && software_keyboard && !hardware_keyboard;
        const int32_t keyboard_height = shown ? std::max<int32_t>(72, height * 36 / 100) : 0;
        if (shown) {
            lv_obj_remove_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            if (lv_obj_get_group(keyboard) != group) lv_group_add_obj(group, keyboard);
        } else {
            lv_obj_add_flag(keyboard, LV_OBJ_FLAG_HIDDEN);
            if (lv_obj_get_group(keyboard) == group) lv_group_remove_obj(keyboard);
        }
        lv_obj_set_size(keyboard, LV_PCT(100), keyboard_height);
        lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_pad_all(keyboard, height < 400 ? 3 : 8, 0);
        lv_obj_set_style_pad_row(keyboard, height < 400 ? 2 : 6, 0);
        lv_obj_set_style_pad_column(keyboard, height < 400 ? 2 : 6, 0);
        const bool wide = width > height && width >= 600;
        lv_obj_set_style_pad_all(card, width < 400 ? 16 : 24, 0);
        lv_obj_set_style_pad_row(card, height < 400 ? 8 : 12, 0);
        const int32_t desired_height = page == SetupPage::Region ? 780 : page == SetupPage::Network ? 1000 :
                                       page == SetupPage::Update ? 520 : 430;
        const int32_t card_height = std::max<int32_t>(80, std::min<int32_t>(height - keyboard_height - 24, desired_height));
        lv_obj_set_size(card, std::min<int32_t>(width - 24, wide ? 600 : 500), card_height);
        lv_obj_align(card, LV_ALIGN_TOP_MID, 0, std::max<int32_t>(12, (height - keyboard_height - card_height) / 2));
    }
};

SetupView::SetupView(const SetupCatalog &catalog) : impl_(std::make_unique<Impl>(catalog)) {}
SetupView::~SetupView() = default;
bool SetupView::valid() const { return impl_ && impl_->modal && impl_->group; }
void SetupView::show(SetupPage page)
{
    if (!valid()) return;
    erase_secret(impl_->pending_request.wifi.password);
    impl_->pending = false;
    impl_->page = page;
    impl_->build();
}
void SetupView::set_region(const SetupRegion &region)
{
    impl_->region = region;
    if (impl_->page == SetupPage::Region) {
        impl_->select(impl_->country, impl_->catalog.countries, region.country);
        impl_->select(impl_->locale, impl_->catalog.locales, region.locale);
        impl_->select(impl_->timezone, impl_->catalog.timezones, region.timezone);
        impl_->select(impl_->keymap, impl_->catalog.keyboards, region.keyboard);
    }
}
void SetupView::set_network(const SetupNetworkState &state) { impl_->network = state; impl_->refresh_network(); }
void SetupView::set_update(const SetupUpdateState &state)
{
    const bool was_running = impl_->update_running();
    impl_->update = state;
    if (was_running && !impl_->update_running()) impl_->restore_update_focus = true;
    impl_->refresh_update();
}
void SetupView::set_busy(bool busy, const std::string &message)
{
    if (!valid()) return;
    impl_->busy = busy;
    impl_->apply_busy();
    if (!message.empty()) lv_label_set_text(impl_->status, message.c_str());
}
void SetupView::set_error(const std::string &message)
{
    if (valid()) lv_label_set_text(impl_->error, message.c_str());
}
void SetupView::set_hardware_keyboard_present(bool present)
{
    if (!valid()) return;
    impl_->hardware_keyboard = present;
    impl_->layout();
}
void SetupView::capture_input() { if (valid()) impl_->capture(); }
bool SetupView::take_request(SetupViewRequest &request)
{
    if (!impl_ || !impl_->pending) return false;
    erase_secret(request.wifi.password);
    request = impl_->pending_request;
    erase_secret(impl_->pending_request.wifi.password);
    impl_->pending_request = {};
    impl_->pending = false;
    return true;
}
} // namespace launcher
