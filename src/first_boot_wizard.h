#ifndef LILYGO_UI_LAUNCHER_FIRST_BOOT_WIZARD_H
#define LILYGO_UI_LAUNCHER_FIRST_BOOT_WIZARD_H
#include <memory>
#include <string>
namespace launcher {
class UserSession;
// Owns the modal setup flow. All LVGL work stays on the UI thread; privileged
// operations run serially on a worker and never block touch/input processing.
class FirstBootWizard {
public:
    explicit FirstBootWizard(UserSession &session);
    ~FirstBootWizard();
    bool initialize(bool demo, std::string &error);
    bool active() const;
    void tick();
    void set_hardware_keyboard_present(bool present);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
#endif
