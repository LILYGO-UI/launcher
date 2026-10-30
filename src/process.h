#ifndef LAUNCHER_PROCESS_H
#define LAUNCHER_PROCESS_H

#include "registry.h"
#include "user.h"

#include <signal.h>
#include <stdint.h>

typedef struct launcher_process_options {
    const char *display_backend;
    const char *fbdev_path;
    const char *drm_path;
    int64_t drm_connector_id;
    const char *input_path;
    const char *orientation;
    int width;
    int height;
    /* NULL retains the launcher's existing identity and environment. */
    const launcher_user_t *user;
} launcher_process_options_t;

#ifdef __cplusplus
extern "C" {
#endif

int launcher_process_run(const launcher_app_t *app, const launcher_process_options_t *options,
                         volatile sig_atomic_t *stop_requested);

/* Run optional maintenance in the parent every 500 ms while the application
 * is alive. The callback runs on the waiting thread and must return promptly;
 * it must not access display/input objects suspended for the application. */
int launcher_process_run_with_poll(const launcher_app_t *app, const launcher_process_options_t *options,
                                   volatile sig_atomic_t *stop_requested, void (*poll)(void *), void *poll_data);

#ifdef __cplusplus
}
#endif

#endif
