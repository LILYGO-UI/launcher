#include "process.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
  assert(argc == 2);
  launcher_app_t app = {0};
  assert(snprintf(app.executable_path, sizeof(app.executable_path), "%s",
                  argv[1]) < (int)sizeof(app.executable_path));
  snprintf(app.manifest.id, sizeof(app.manifest.id),
           "cc.lilygo.ui.ProcessTest");
  snprintf(app.manifest.name, sizeof(app.manifest.name), "Process Test");

  launcher_process_options_t options = {
      .display_backend = "sdl",
      .fbdev_path = "/dev/fb-test",
      .drm_path = "/dev/dri/card-test",
      .drm_connector_id = 42,
      .input_path = "/dev/input-test",
      .orientation = "landscape",
      .width = 568,
      .height = 1232,
  };
  volatile sig_atomic_t stop_requested = 0;
  assert(launcher_process_run(&app, &options, &stop_requested) == 23);

  snprintf(app.executable_path, sizeof(app.executable_path), "/missing/app");
  assert(launcher_process_run(&app, &options, &stop_requested) == 127);
  return 0;
}
