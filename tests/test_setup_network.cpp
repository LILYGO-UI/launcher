#include "setup.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <vector>

using namespace launcher;
namespace {
bool contains(const SetupCommand &command, const std::string &value) {
    return std::find(command.arguments.begin(), command.arguments.end(), value) != command.arguments.end();
}
void scan_handles_real_nmcli_escaping_and_authentication() {
    unsigned calls = 0;
    SetupCommandRunner runner = [&](const SetupCommand &command) {
        assert(contains(command, "LC_ALL=C") == false);
        assert(command.environment == std::vector<std::string>{"LC_ALL=C"});
        ++calls;
        if (calls == 1) return SetupCommandResult{0, "eth0:ethernet:connected\nwlan0:wifi:disconnected\n"};
        if (calls == 2) { assert(contains(command, "radio")); return SetupCommandResult{0, ""}; }
        assert(contains(command, "--rescan"));
        return SetupCommandResult{0, "*:Home\\:Net:71:WPA2\n:Home\\:Net:95:WPA2\n:Guest:50:--\n:Office:66:WPA2 802.1X\n:Old:80:WEP\n:New:92:WPA3\n:Enhanced:44:OWE\n:Slash\\\\AP:30:WPA2\n::2:--\n:Bad:xx:WPA2\n"};
    };
    const auto state = scan_setup_networks(runner, false);
    assert(calls == 3 && state.available && state.connected && state.interface == "wlan0");
    assert(state.networks.size() == 7);
    assert(state.networks[0].ssid == "Home:Net" && state.networks[0].connected && state.networks[0].signal == 95);
    assert(state.networks[1].security == "sae");
    assert(!state.networks[2].supported && state.networks[2].ssid == "Old");
    assert(!state.networks[3].supported && state.networks[3].ssid == "Office");
    assert(state.networks[4].security == "open");
    assert(state.networks[5].security == "owe");
    assert(state.networks.back().ssid == "Slash\\AP");
    const auto no_adapter = scan_setup_networks([](const SetupCommand &) { return SetupCommandResult{0,"eth0:ethernet:connected\n"}; }, false);
    assert(!no_adapter.available && no_adapter.connected);
    const auto unavailable = scan_setup_networks([](const SetupCommand &) { return SetupCommandResult{127,"missing"}; }, false);
    assert(!unavailable.available && !unavailable.message.empty());
}
void connection_keeps_secrets_out_of_argv_and_cleans_failed_profile() {
    SetupNetworkState state;
    state.available = true;
    state.interface = "wlan0";
    const std::string password = "private: $();\\ password";
    SetupWifiRequest request{"hidden: $net", password, "wpa-psk", true};
    std::vector<SetupCommand> calls;
    SetupCommandRunner runner = [&](const SetupCommand &command) {
        for (const auto &arg : command.arguments) assert(arg.find(password) == std::string::npos);
        calls.push_back(command);
        return SetupCommandResult{0, ""};
    };
    std::string error;
    assert(connect_setup_network(state, request, runner, error, false));
    assert(calls.size() == 3);
    assert(contains(calls[0], "802-11-wireless.hidden") && contains(calls[0], "yes"));
    assert(contains(calls[0], "802-11-wireless-security.psk-flags"));
    assert(contains(calls[1], "/dev/stdin"));
    assert(calls[1].input == "802-11-wireless-security.psk:" + password + "\n");
    assert(contains(calls[2], "connection.autoconnect"));
    const auto uuid_arg = std::find(calls[0].arguments.begin(), calls[0].arguments.end(), "connection.uuid");
    const auto uuid = *(uuid_arg + 1);
    assert(contains(calls[1], uuid) && contains(calls[2], uuid));
    calls.clear();
    runner = [&](const SetupCommand &command) {
        calls.push_back(command);
        return SetupCommandResult{calls.size() == 2 ? 4 : 0, password};
    };
    assert(!connect_setup_network(state, request, runner, error, false));
    assert(calls.size() == 3 && contains(calls[2], "delete"));
    assert(error.find(password) == std::string::npos);
    assert(contains(calls[1], calls[2].arguments.back()));
}
void validation_and_simulation_never_run_commands() {
    SetupNetworkState state;
    state.available = true;
    state.interface = "wlan0";
    SetupCommandRunner runner = [](const SetupCommand &) -> SetupCommandResult { assert(false); return {}; };
    std::string error;
    assert(!connect_setup_network(state, {"Office", "password", "unsupported", false}, runner, error, false));
    assert(!connect_setup_network(state, {"Home", "short", "wpa-psk", false}, runner, error, false));
    assert(!connect_setup_network(state, {"Home", "password\ninjected", "wpa-psk", false}, runner, error, false));
    assert(!connect_setup_network(state, {"Open", "password", "open", false}, runner, error, false));
    assert(!connect_setup_network(state, {std::string(33, 'a'), "", "open", false}, runner, error, false));
    assert(connect_setup_network(state, {"Open", "", "open", false}, runner, error, true));
    assert(connect_setup_network(state, {"Modern", "a", "sae", true}, runner, error, true));
    assert(!scan_setup_networks(runner, true).networks.empty());
}
} // namespace
int main() {
    scan_handles_real_nmcli_escaping_and_authentication();
    connection_keeps_secrets_out_of_argv_and_cleans_failed_profile();
    validation_and_simulation_never_run_commands();
}
