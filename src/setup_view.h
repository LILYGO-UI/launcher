#ifndef LILYGO_UI_LAUNCHER_SETUP_VIEW_H
#define LILYGO_UI_LAUNCHER_SETUP_VIEW_H

#include "setup.h"
#include <memory>

namespace launcher {
enum class SetupPage { Region, Network, Update, Complete };
enum class SetupAction { ApplyRegion, Back, ScanWifi, ConnectWifi, SkipWifi,
                         CheckUpdates, InstallUpdates, SkipUpdates, Finish };
struct SetupViewRequest {
    SetupAction action = SetupAction::ApplyRegion;
    SetupRegion region;
    SetupWifiRequest wifi;
};

class SetupView {
public:
    explicit SetupView(const SetupCatalog &catalog);
    ~SetupView();
    SetupView(const SetupView &) = delete;
    SetupView &operator=(const SetupView &) = delete;
    bool valid() const;
    void show(SetupPage page);
    void set_region(const SetupRegion &region);
    void set_network(const SetupNetworkState &state);
    void set_update(const SetupUpdateState &state);
    void set_busy(bool busy, const std::string &message = {});
    void set_error(const std::string &message);
    void set_hardware_keyboard_present(bool present);
    void capture_input();
    bool take_request(SetupViewRequest &request);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace launcher
#endif
