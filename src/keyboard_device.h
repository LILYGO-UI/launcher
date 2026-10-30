#ifndef LAUNCHER_KEYBOARD_DEVICE_H
#define LAUNCHER_KEYBOARD_DEVICE_H

namespace launcher {

// Return physical TCA8418 presence, independently of driver recovery success.
// A declared I2C client and its input node can outlive a disconnected keyboard.
// If the responding chip lost its configuration or has no bound driver, ask
// the kernel to initialize that client again. Healthy or foreign bindings are
// left alone. The optional reader returns CFG (0..255), or a negative value
// when the chip does not respond, and allows host tests without I2C access.
bool update_tca8418_keyboard(const char *i2c_directory = "/sys/bus/i2c/devices",
                            const char *device_directory = "/dev",
                            const char *driver_directory = "/sys/bus/i2c/drivers/tca8418_keypad",
                            int (*read_config)(const char *, unsigned int) = nullptr);

} // namespace launcher

#endif
