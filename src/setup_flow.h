#ifndef LILYGO_UI_LAUNCHER_SETUP_FLOW_H
#define LILYGO_UI_LAUNCHER_SETUP_FLOW_H
#include "setup.h"

namespace launcher {
enum class SetupStage { Region, Account, Network, Update, Complete };
class SetupFlow {
public:
    explicit SetupFlow(std::string directory = "/var/lib/lilygo-ui-launcher",
                       bool simulated = kSetupSimulated);
    bool initialize(bool required, bool account_saved, std::string &error);
    bool active() const { return active_; }
    SetupStage stage() const { return stage_; }
    const SetupRegion &region() const { return region_; }
    bool advance(SetupStage stage, const SetupRegion &region, std::string &error);
    bool finish(std::string &error);
private:
    bool save(std::string &error);
    std::string directory_;
    bool simulated_;
    bool active_ = false;
    SetupStage stage_ = SetupStage::Region;
    SetupRegion region_;
};
}
#endif
