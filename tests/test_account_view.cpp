#include "account_view.h"

#include <cm0/typography.h>

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

struct keyboard_sample {
    uint32_t key = 0;
    lv_indev_state_t state = LV_INDEV_STATE_RELEASED;
};

void flush_display(lv_display_t *display, const lv_area_t *, uint8_t *)
{
    lv_display_flush_ready(display);
}

void read_keyboard(lv_indev_t *input, lv_indev_data_t *data)
{
    auto *sample = static_cast<keyboard_sample *>(lv_indev_get_user_data(input));
    data->key = sample->key;
    data->state = sample->state;
}

lv_indev_t *make_input(lv_indev_type_t type, lv_group_t *group, keyboard_sample *sample)
{
    lv_indev_t *input = lv_indev_create();
    assert(input);
    lv_indev_set_type(input, type);
    if (type == LV_INDEV_TYPE_KEYPAD || type == LV_INDEV_TYPE_ENCODER) lv_indev_set_group(input, group);
    lv_indev_set_user_data(input, sample);
    lv_indev_set_read_cb(input, read_keyboard);
    lv_timer_pause(lv_indev_get_read_timer(input));
    return input;
}

void press_key(lv_indev_t *input, uint32_t key)
{
    auto *sample = static_cast<keyboard_sample *>(lv_indev_get_user_data(input));
    sample->key = key;
    sample->state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(input);
    sample->state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(input);
}

lv_obj_t *find(const char *name)
{
    lv_obj_t *object = lv_obj_find_by_name(lv_layer_top(), name);
    assert(object);
    return object;
}

void assert_text_is_vertically_centered(lv_obj_t *field)
{
    lv_area_t field_area;
    lv_area_t text_area;
    lv_obj_get_coords(field, &field_area);
    lv_obj_get_coords(lv_textarea_get_label(field), &text_area);
    const int32_t field_center = field_area.y1 + field_area.y2;
    const int32_t text_center = text_area.y1 + text_area.y2;
    assert(std::abs(field_center - text_center) <= 2);
}

void assert_keyboard_focus_matches_touch(lv_obj_t *field)
{
    lv_obj_add_state(field, LV_STATE_FOCUS_KEY);
    assert(lv_obj_get_style_border_width(field, LV_PART_MAIN) == 2);
    assert(lv_obj_get_style_outline_width(field, LV_PART_MAIN) == 0);
    lv_obj_remove_state(field, LV_STATE_FOCUS_KEY);
}

void click(lv_obj_t *object)
{
    assert(lv_obj_send_event(object, LV_EVENT_CLICKED, nullptr) == LV_RESULT_OK);
}

void count_background_key(lv_event_t *event)
{
    auto *count = static_cast<unsigned *>(lv_event_get_user_data(event));
    ++*count;
}

void assert_no_request(launcher_account_view_t *view)
{
    char username[64]{};
    char password[128]{};
    bool create = true;
    assert(!launcher_account_view_take_request(view, username, sizeof(username), password, sizeof(password), &create));
}

void test_locked_form(launcher_account_view_t *view, lv_indev_t *input, lv_group_t *private_group,
                      unsigned *background_keys)
{
    lv_obj_t *modal = find("account_modal");
    lv_obj_t *username = find("account_username");
    lv_obj_t *password = find("account_password");
    lv_obj_t *confirm = find("account_confirm_password");
    lv_obj_t *fields = find("account_fields");
    lv_obj_t *mode = find("account_mode_toggle");
    lv_obj_t *submit = find("account_submit");
    lv_obj_t *keyboard = find("account_keyboard");
    assert(std::strcmp(lv_label_get_text(find("account_title")), "Screen locked") == 0);
    assert(std::strcmp(lv_label_get_text(find("account_submit_label")), "Unlock") == 0);
    assert(std::strcmp(lv_textarea_get_text(username), "alice") == 0);
    assert(lv_obj_has_state(username, LV_STATE_DISABLED));
    assert(!lv_obj_has_flag(username, LV_OBJ_FLAG_CLICKABLE));
    assert(lv_obj_get_group(username) == nullptr);
    assert(lv_obj_has_flag(mode, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(confirm, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(fields, LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    assert(lv_keyboard_get_textarea(keyboard) == password);
    assert(lv_textarea_get_password_mode(password));
    assert(lv_obj_get_group(keyboard) == private_group);
    assert(lv_obj_get_group(submit) == private_group);
    assert(lv_group_get_focused(private_group) == password);
    lv_obj_update_layout(lv_layer_top());
    assert_text_is_vertically_centered(username);
    assert_text_is_vertically_centered(password);
    assert_keyboard_focus_matches_touch(password);

    /* A connected TCA8418 uses the physical keypad and frees the full
     * display for the account form. Disconnecting it restores the software
     * keyboard and its input-group membership. */
    launcher_account_view_set_hardware_keyboard_present(view, true);
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_group(keyboard) == nullptr);
    launcher_account_view_set_hardware_keyboard_present(view, false);
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_group(keyboard) == private_group);
    assert(lv_keyboard_get_textarea(keyboard) == password);

    /* Tapping outside an input dismisses the software keyboard. Tapping the
     * active field brings it back without changing the keyboard target. */
    click(modal);
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_group(keyboard) == nullptr);
    click(password);
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_group(keyboard) == private_group);
    assert(lv_keyboard_get_textarea(keyboard) == password);
    click(fields);
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_group(keyboard) == nullptr);
    click(password);
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_group(keyboard) == private_group);

    /* Even a synthetic click on the hidden control cannot enable account creation. */
    click(mode);
    assert(lv_obj_has_flag(confirm, LV_OBJ_FLAG_HIDDEN));
    assert(std::strcmp(lv_label_get_text(find("account_title")), "Screen locked") == 0);
    click(username);
    assert(lv_keyboard_get_textarea(keyboard) == password);

    press_key(input, 'x');
    assert(std::strcmp(lv_textarea_get_text(password), "x") == 0);
    press_key(input, LV_KEY_ESC);
    assert(lv_obj_is_valid(modal));
    assert(!lv_obj_has_flag(modal, LV_OBJ_FLAG_HIDDEN));
    assert(*background_keys == 0);
    assert_no_request(view);

    launcher_account_view_clear_passwords(view);
    click(submit);
    assert_no_request(view);
    assert(std::strcmp(lv_label_get_text(find("account_error")), "Enter a password") == 0);

    /* Submission must use the authenticated identity, even if widget text changes. */
    lv_textarea_set_text(username, "mallory");
    lv_textarea_set_text(password, "unlock-secret");
    lv_textarea_set_text(confirm, "unused-secret");
    launcher_account_view_set_busy(view, true);
    click(submit);
    assert_no_request(view);
    launcher_account_view_set_busy(view, false);
    click(submit);
    assert(std::strcmp(lv_textarea_get_text(password), "") == 0);
    assert(std::strcmp(lv_textarea_get_text(confirm), "") == 0);
    assert(std::strcmp(lv_label_get_text(find("account_error")), "") == 0);

    char requested_username[64]{};
    char requested_password[128]{};
    bool create = true;
    assert(!launcher_account_view_take_request(view, requested_username, sizeof(requested_username),
                                               requested_password, 2, &create));
    /* Repeated clicks cannot replace a request awaiting authentication. */
    lv_textarea_set_text(password, "replacement-secret");
    click(submit);
    assert(launcher_account_view_take_request(view, requested_username, sizeof(requested_username),
                                              requested_password, sizeof(requested_password), &create));
    assert(std::strcmp(requested_username, "alice") == 0);
    assert(std::strcmp(requested_password, "unlock-secret") == 0);
    assert(!create);
    assert_no_request(view);

    launcher_account_view_set_error(view, "Authentication failed");
    launcher_account_view_clear_passwords(view);
    assert(std::strcmp(lv_textarea_get_text(password), "") == 0);
    assert(std::strcmp(lv_textarea_get_text(confirm), "") == 0);
    assert(std::strcmp(lv_label_get_text(find("account_error")), "Authentication failed") == 0);
}

void test_resize(lv_display_t *display, launcher_account_view_t *view)
{
    const int32_t sizes[][2] = {{568, 1232}, {1232, 568}, {320, 240}};
    lv_obj_t *modal = find("account_modal");
    lv_obj_t *card = find("account_card");
    lv_obj_t *keyboard = find("account_keyboard");
    lv_obj_t *password = find("account_password");
    lv_textarea_set_text(password, "in-progress");
    for (const auto &size : sizes) {
        lv_display_set_resolution(display, size[0], size[1]);
        lv_obj_update_layout(lv_layer_top());
        assert(lv_obj_get_width(modal) == size[0]);
        assert(lv_obj_get_height(modal) == size[1]);
        lv_area_t card_area;
        lv_area_t keyboard_area;
        lv_obj_get_coords(card, &card_area);
        lv_obj_get_coords(keyboard, &keyboard_area);
        assert(card_area.x1 >= 0 && card_area.x2 < size[0]);
        assert(card_area.y1 >= 0 && card_area.y2 < keyboard_area.y1);
        assert(keyboard_area.x1 == 0 && keyboard_area.x2 == size[0] - 1);
        assert(keyboard_area.y2 == size[1] - 1);
        assert(lv_keyboard_get_textarea(keyboard) == password);
        assert(std::strcmp(lv_textarea_get_text(password), "in-progress") == 0);
        if (size[0] > size[1]) {
            /* The landscape card scrolls when needed, but touch targets keep
             * their designed heights instead of flex-shrinking above the
             * keyboard. Fields stay stacked for readable labels. */
            lv_area_t username_area;
            lv_area_t password_area;
            lv_obj_get_coords(find("account_username"), &username_area);
            lv_obj_get_coords(password, &password_area);
            assert(username_area.y2 < password_area.y1);
            assert(lv_obj_get_height(find("account_username")) >= 52);
            assert(lv_obj_get_height(password) >= 52);
            assert(lv_obj_get_height(find("account_submit")) >= 52);
        }
    }

    /* In landscape with the TCA8418 attached, the form is centered in the
     * full canvas instead of stretching above an invisible keyboard. */
    launcher_account_view_set_hardware_keyboard_present(view, true);
    lv_display_set_resolution(display, 1232, 568);
    lv_obj_update_layout(lv_layer_top());
    lv_area_t centered_card;
    lv_obj_get_coords(card, &centered_card);
    assert(lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_group(keyboard) == nullptr);
    assert(centered_card.x1 >= 0 && centered_card.x2 < 1232);
    assert(centered_card.y1 > 12 && centered_card.y2 < 568);

    launcher_account_view_set_hardware_keyboard_present(view, false);
    lv_obj_update_layout(lv_layer_top());
    assert(!lv_obj_has_flag(keyboard, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_group(keyboard) != nullptr);
}

void test_input_capture_and_restore(lv_display_t *display)
{
    lv_group_t *original = lv_group_create();
    lv_group_t *other = lv_group_create();
    lv_group_set_default(original);
    lv_obj_t *background = lv_button_create(lv_screen_active());
    lv_group_focus_obj(background);
    unsigned background_keys = 0;
    lv_obj_add_event_cb(background, count_background_key, LV_EVENT_KEY, &background_keys);
    keyboard_sample keyboard_data;
    keyboard_sample encoder_data;
    keyboard_sample pointer_data;
    lv_indev_t *keyboard = make_input(LV_INDEV_TYPE_KEYPAD, original, &keyboard_data);
    lv_indev_t *encoder = make_input(LV_INDEV_TYPE_ENCODER, other, &encoder_data);
    lv_indev_t *pointer = make_input(LV_INDEV_TYPE_POINTER, nullptr, &pointer_data);
    const uint32_t original_group_count = lv_group_get_count();
    launcher_account_view_t *view = launcher_account_view_create_locked("alice");
    assert(view);
    lv_group_t *private_group = lv_indev_get_group(keyboard);
    assert(private_group && private_group != original && private_group != other);
    assert(lv_indev_get_group(encoder) == private_group);
    assert(lv_indev_get_group(pointer) == nullptr);
    assert(lv_group_get_default() == original);
    test_locked_form(view, keyboard, private_group, &background_keys);
    test_resize(display, view);

    /* Launcher rebuilds continue adding controls to their own default group. */
    lv_obj_t *rebuilt_background = lv_button_create(lv_screen_active());
    assert(lv_obj_get_group(rebuilt_background) == original);
    lv_group_focus_obj(rebuilt_background);
    lv_indev_set_group(keyboard, other);
    launcher_account_view_capture_input(view);
    assert(lv_indev_get_group(keyboard) == private_group);

    keyboard_sample hotplug_data;
    keyboard_sample ungrouped_data;
    lv_indev_t *hotplug = make_input(LV_INDEV_TYPE_KEYPAD, original, &hotplug_data);
    lv_indev_t *ungrouped = make_input(LV_INDEV_TYPE_ENCODER, nullptr, &ungrouped_data);
    launcher_account_view_capture_input(view);
    assert(lv_indev_get_group(hotplug) == private_group);
    assert(lv_indev_get_group(ungrouped) == private_group);
    launcher_account_view_capture_input(view);
    assert(lv_indev_get_group(keyboard) == private_group);

    /* Disconnecting a captured device must not leave a stale restoration target. */
    lv_indev_delete(encoder);
    launcher_account_view_destroy(view);
    assert(lv_obj_find_by_name(lv_layer_top(), "account_modal") == nullptr);
    assert(lv_group_get_count() == original_group_count);
    assert(lv_group_get_default() == original);
    assert(lv_indev_get_group(keyboard) == other);
    assert(lv_indev_get_group(hotplug) == original);
    assert(lv_indev_get_group(ungrouped) == nullptr);
    assert(lv_group_get_focused(original) == background);
    assert(background_keys == 0);
    lv_indev_delete(keyboard);
    lv_indev_delete(pointer);
    lv_indev_delete(hotplug);
    lv_indev_delete(ungrouped);
    lv_obj_clean(lv_screen_active());
    lv_group_set_default(nullptr);
    lv_group_delete(original);
    lv_group_delete(other);
}

void test_deleted_background_group()
{
    lv_group_t *background_group = lv_group_create();
    lv_group_set_default(background_group);
    lv_obj_t *background = lv_button_create(lv_screen_active());
    lv_group_focus_obj(background);
    keyboard_sample sample;
    lv_indev_t *input = make_input(LV_INDEV_TYPE_KEYPAD, background_group, &sample);
    launcher_account_view_t *view = launcher_account_view_create_locked("alice");
    assert(view);
    lv_obj_delete(background);
    lv_group_delete(background_group);
    launcher_account_view_destroy(view);
    assert(lv_indev_get_group(input) == nullptr);
    assert(lv_group_get_default() == nullptr);
    lv_indev_delete(input);
}

void test_create_landscape_layout(lv_display_t *display)
{
    lv_group_t *original = lv_group_create();
    lv_group_set_default(original);
    launcher_account_view_t *view = launcher_account_view_create(true, nullptr, true);
    assert(view);
    lv_display_set_resolution(display, 1232, 568);
    lv_obj_update_layout(lv_layer_top());

    lv_obj_t *card = find("account_card");
    lv_obj_t *keyboard = find("account_keyboard");
    lv_obj_t *username = find("account_username");
    lv_obj_t *password = find("account_password");
    lv_obj_t *confirm = find("account_confirm_password");
    lv_obj_t *submit = find("account_submit");
    lv_obj_t *mode = find("account_mode_toggle");
    lv_area_t card_area;
    lv_area_t keyboard_area;
    lv_area_t username_area;
    lv_area_t password_area;
    lv_area_t confirm_area;
    lv_area_t submit_area;
    lv_area_t mode_area;
    lv_obj_get_coords(card, &card_area);
    lv_obj_get_coords(keyboard, &keyboard_area);
    lv_obj_get_coords(username, &username_area);
    lv_obj_get_coords(password, &password_area);
    lv_obj_get_coords(confirm, &confirm_area);
    lv_obj_get_coords(submit, &submit_area);
    lv_obj_get_coords(mode, &mode_area);
    assert(lv_obj_get_width(username) > 0 && lv_obj_get_width(password) > 0);
    assert(lv_obj_get_width(confirm) > 0);
    assert_keyboard_focus_matches_touch(username);
    assert_keyboard_focus_matches_touch(password);
    assert_keyboard_focus_matches_touch(confirm);
    assert(username_area.y2 < password_area.y1);
    assert(password_area.y2 < confirm_area.y1);
    assert(card_area.y2 < keyboard_area.y1);

    /* The full create-account form is intentionally scrollable above the
     * landscape keyboard. Verify the lower controls can be brought into view
     * instead of requiring a compressed or horizontal field row. */
    lv_obj_scroll_to_view(submit, LV_ANIM_OFF);
    lv_obj_update_layout(lv_layer_top());
    lv_obj_get_coords(submit, &submit_area);
    assert(submit_area.y2 < keyboard_area.y1);
    lv_obj_scroll_to_view(mode, LV_ANIM_OFF);
    lv_obj_update_layout(lv_layer_top());
    lv_obj_get_coords(mode, &mode_area);
    assert(mode_area.y2 < keyboard_area.y1);

    launcher_account_view_destroy(view);
    assert(lv_group_get_default() == original);
    lv_group_delete(original);
}

void test_first_use_wizard()
{
    lv_group_t *original = lv_group_create();
    lv_group_set_default(original);
    launcher_account_view_t *view = launcher_account_view_create_setup("pi");
    assert(view);

    lv_obj_t *mode = find("account_mode_toggle");
    lv_obj_t *confirm = find("account_confirm_password");
    lv_obj_t *username = find("account_username");
    lv_obj_t *password = find("account_password");
    lv_obj_t *submit = find("account_submit");
    assert(std::strcmp(lv_label_get_text(find("account_title")), "First-time setup") == 0);
    assert(std::strcmp(lv_label_get_text(find("account_subtitle")),
                       "Choose your username and password to get started") == 0);
    assert(std::strcmp(lv_label_get_text(find("account_submit_label")), "Finish setup") == 0);
    assert(std::strcmp(lv_textarea_get_text(username), "pi") == 0);
    assert(lv_obj_has_flag(mode, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(confirm, LV_OBJ_FLAG_HIDDEN));

    launcher_account_view_set_busy(view, true);
    assert(lv_obj_has_state(submit, LV_STATE_DISABLED));
    assert(lv_obj_has_state(username, LV_STATE_DISABLED));
    assert(std::strcmp(lv_label_get_text(find("account_submit_label")), "Setting up...") == 0);
    launcher_account_view_set_busy(view, false);
    assert(!lv_obj_has_state(submit, LV_STATE_DISABLED));
    assert(!lv_obj_has_state(username, LV_STATE_DISABLED));
    assert(std::strcmp(lv_label_get_text(find("account_submit_label")), "Finish setup") == 0);

    /* The one-time wizard cannot be switched to an existing-account login. */
    click(mode);
    assert(!lv_obj_has_flag(confirm, LV_OBJ_FLAG_HIDDEN));
    assert(std::strcmp(lv_label_get_text(find("account_title")), "First-time setup") == 0);

    // Keeping the prefilled name is valid; changing it remains possible.
    lv_textarea_set_text(password, "secret-password");
    lv_textarea_set_text(confirm, "different-password");
    click(submit);
    assert_no_request(view);
    assert(std::strcmp(lv_label_get_text(find("account_error")), "Passwords do not match") == 0);
    assert(std::strcmp(lv_textarea_get_text(password), "") == 0);
    assert(std::strcmp(lv_textarea_get_text(confirm), "") == 0);

    lv_textarea_set_text(password, "secret-password");
    lv_textarea_set_text(confirm, "secret-password");
    click(submit);

    char requested_username[64]{};
    char requested_password[128]{};
    bool create = false;
    assert(launcher_account_view_take_request(view, requested_username, sizeof(requested_username),
                                              requested_password, sizeof(requested_password), &create));
    assert(std::strcmp(requested_username, "pi") == 0);
    assert(std::strcmp(requested_password, "secret-password") == 0);
    assert(create);
    launcher_account_view_set_error(view, "Setup failed; try again");
    lv_textarea_set_text(username, "wing");
    lv_textarea_set_text(password, "new-password");
    lv_textarea_set_text(confirm, "new-password");
    click(submit);
    assert(launcher_account_view_take_request(view, requested_username, sizeof(requested_username),
                                              requested_password, sizeof(requested_password), &create));
    assert(std::strcmp(requested_username, "wing") == 0 && create);
    assert(std::strcmp(requested_password, "new-password") == 0);
    lv_textarea_set_text(username, "another-name");
    launcher_account_view_set_setup_username(view, "wing");
    assert(std::strcmp(lv_textarea_get_text(username), "wing") == 0);
    launcher_account_view_destroy(view);
    assert(lv_group_get_default() == original);
    lv_group_delete(original);
}

void test_full_setup_continuation()
{
    auto *view = launcher_account_view_create_setup("pi");
    assert(view);
    launcher_account_view_set_setup_continuation(view, true);
    assert(std::strcmp(lv_label_get_text(find("account_submit_label")), "Continue") == 0);
    assert(std::strstr(lv_label_get_text(find("account_subtitle")), "2 of 5"));
    launcher_account_view_set_busy(view, true);
    assert(std::strcmp(lv_label_get_text(find("account_submit_label")), "Setting up...") == 0);
    launcher_account_view_set_busy(view, false);
    assert(std::strcmp(lv_label_get_text(find("account_submit_label")), "Continue") == 0);
    launcher_account_view_set_setup_continuation(view, false);
    assert(std::strcmp(lv_label_get_text(find("account_submit_label")), "Finish setup") == 0);
    launcher_account_view_destroy(view);
    view = launcher_account_view_create_locked("pi");
    assert(view);
    launcher_account_view_set_setup_continuation(view, true);
    assert(std::strcmp(lv_label_get_text(find("account_submit_label")), "Unlock") == 0);
    launcher_account_view_destroy(view);
}

} // namespace

int main()
{
    lv_init();
    lv_display_t *display = lv_display_create(568, 1232);
    assert(display);
    static lv_color32_t draw_buffer[1232 * 40];
    lv_display_set_buffers(display, draw_buffer, nullptr, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    assert(!launcher_account_view_create_locked(nullptr));
    assert(!launcher_account_view_create_locked(""));
    assert(!launcher_account_view_create_locked(std::string(64, 'a').c_str()));
    test_first_use_wizard();
    test_full_setup_continuation();
    test_create_landscape_layout(display);
    test_input_capture_and_restore(display);
    test_deleted_background_group();
    assert(lv_group_get_count() == 0);
    lilygo_ui_fonts_deinit();
    lv_deinit();
    return 0;
}
