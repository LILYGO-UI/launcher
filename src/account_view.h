#ifndef LAUNCHER_ACCOUNT_VIEW_H
#define LAUNCHER_ACCOUNT_VIEW_H

#include <lvgl.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct launcher_account_view launcher_account_view_t;

launcher_account_view_t *launcher_account_view_create(bool setup_available, const char *last_user,
                                                      bool start_setup);
/* Show mandatory account setup, optionally retaining the image's username.
 * The wizard submits setup requests and cannot be switched to sign-in mode. */
launcher_account_view_t *launcher_account_view_create_setup(const char *username);
/* Use Continue when account creation is one step in the full setup wizard. */
void launcher_account_view_set_setup_continuation(launcher_account_view_t *view, bool continuation);
/* Require the current user's password without permitting account changes. */
launcher_account_view_t *launcher_account_view_create_locked(const char *username);
/* Redirect newly connected keyboard/encoder inputs before processing LVGL input. */
void launcher_account_view_capture_input(launcher_account_view_t *view);
/* Suppress the on-screen keyboard while the detachable TCA8418 keypad is present. */
void launcher_account_view_set_hardware_keyboard_present(launcher_account_view_t *view, bool present);
void launcher_account_view_destroy(launcher_account_view_t *view);
/* Consume a submitted login/setup request. Password buffers are cleared after a successful consume. */
bool launcher_account_view_take_request(launcher_account_view_t *view, char *username, size_t username_capacity,
                                        char *password, size_t password_capacity, bool *create);
void launcher_account_view_set_busy(launcher_account_view_t *view, bool busy);
void launcher_account_view_set_error(launcher_account_view_t *view, const char *message);
/* Keep retries on the same account after an interrupted rename. */
void launcher_account_view_set_setup_username(launcher_account_view_t *view, const char *username);
void launcher_account_view_clear_passwords(launcher_account_view_t *view);

#ifdef __cplusplus
}
#endif

#endif
