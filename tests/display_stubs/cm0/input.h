#ifndef LAUNCHER_TEST_CM0_INPUT_H
#define LAUNCHER_TEST_CM0_INPUT_H

#include <stdbool.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_indev_t *cm0_input_create_pointer(const char *path);
lv_indev_t *cm0_input_create_keyboard(bool *present);
bool cm0_input_keyboard_present(void);

#ifdef __cplusplus
}
#endif

#endif
