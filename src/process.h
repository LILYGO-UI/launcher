#ifndef LAUNCHER_PROCESS_H
#define LAUNCHER_PROCESS_H

#include "registry.h"

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
} launcher_process_options_t;

#ifdef __cplusplus
extern "C" {
#endif

int launcher_process_run(const launcher_app_t *app,
                         const launcher_process_options_t *options,
                         volatile sig_atomic_t *stop_requested);

#ifdef __cplusplus
}
#endif

#endif
