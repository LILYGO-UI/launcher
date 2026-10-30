#ifndef LILYGO_UI_LAUNCHER_SETUP_H
#define LILYGO_UI_LAUNCHER_SETUP_H

#include <functional>
#include <string>
#include <vector>

namespace launcher {

#if defined(LAUNCHER_SIMULATOR) || !defined(__linux__)
inline constexpr bool kSetupSimulated = true;
#else
inline constexpr bool kSetupSimulated = false;
#endif

struct SetupCommand {
    std::vector<std::string> arguments;
    std::string input;
    std::vector<std::string> environment;
    unsigned timeout_seconds = 60;
};
struct SetupCommandResult {
    int exit_code = -1;
    std::string output;
    bool timed_out = false;
};
using SetupCommandRunner = std::function<SetupCommandResult(const SetupCommand &)>;
SetupCommandResult run_setup_command(const SetupCommand &command);
void clear_setup_secret(std::string &secret);

struct SetupChoice {
    std::string value;
    std::string label;
    std::string country;
};
struct SetupRegion {
    std::string country = "GB";
    std::string locale = "en_GB.UTF-8";
    std::string timezone = "Europe/London";
    std::string keyboard = "gb";
};
struct SetupCatalog {
    std::vector<SetupChoice> countries;
    std::vector<SetupChoice> locales;
    std::vector<SetupChoice> timezones;
    std::vector<SetupChoice> keyboards;
    SetupRegion current;
};
SetupCatalog load_setup_catalog(const std::string &root = "", bool simulated = kSetupSimulated);
bool apply_setup_region(const SetupRegion &region, const SetupCatalog &catalog,
                        const SetupCommandRunner &runner, std::string &error,
                        const std::string &root = "", bool simulated = kSetupSimulated);

struct SetupWifiNetwork {
    std::string ssid;
    std::string security;
    int signal = 0;
    bool connected = false;
    bool supported = true;
};
struct SetupNetworkState {
    bool available = false;
    bool connected = false;
    std::string interface;
    std::string message;
    std::vector<SetupWifiNetwork> networks;
};
struct SetupWifiRequest {
    std::string ssid;
    std::string password;
    std::string security;
    bool hidden = false;
};
SetupNetworkState scan_setup_networks(const SetupCommandRunner &runner,
                                     bool simulated = kSetupSimulated);
bool connect_setup_network(const SetupNetworkState &state, const SetupWifiRequest &request,
                           const SetupCommandRunner &runner, std::string &error,
                           bool simulated = kSetupSimulated);

enum class SetupUpdateStatus { Idle, Checking, Available, Running, Succeeded, Failed };
struct SetupUpdateState {
    SetupUpdateStatus status = SetupUpdateStatus::Idle;
    std::string message;
    bool reboot_required = false;
};
// Check refreshes package indexes; install runs as an independent systemd job.
bool start_setup_update(bool install, const SetupCommandRunner &runner, std::string &error,
                        bool simulated = kSetupSimulated);
SetupUpdateState read_setup_update(const SetupCommandRunner &runner,
                                  bool simulated = kSetupSimulated);

} // namespace launcher
#endif
