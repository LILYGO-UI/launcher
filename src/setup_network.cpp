#include "setup.h"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <random>
#include <sstream>

namespace launcher {
namespace {
SetupCommand nmcli(std::vector<std::string> arguments, unsigned timeout = 30) {
    SetupCommand command;
    command.arguments = {"/usr/bin/nmcli", "--wait", std::to_string(timeout)};
    command.arguments.insert(command.arguments.end(), arguments.begin(), arguments.end());
    command.environment = {"LC_ALL=C"};
    command.timeout_seconds = timeout + 5;
    return command;
}

std::vector<std::string> split_nmcli(const std::string &line) {
    std::vector<std::string> fields(1);
    bool escaped = false;
    for (char c : line) {
        if (escaped) { fields.back().push_back(c); escaped = false; }
        else if (c == '\\') escaped = true;
        else if (c == ':') fields.emplace_back();
        else fields.back().push_back(c);
    }
    if (escaped) fields.back().push_back('\\');
    return fields;
}

bool clean_text(const std::string &text, size_t maximum) {
    return !text.empty() && text.size() <= maximum &&
        std::none_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; });
}

std::string security_type(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
    if (value.empty() || value == "--") return "open";
    if (value.find("802.1X") != std::string::npos || value.find("EAP") != std::string::npos ||
        value.find("WEP") != std::string::npos) return "unsupported";
    if (value.find("WPA2") != std::string::npos || value.find("WPA1") != std::string::npos) return "wpa-psk";
    if (value.find("WPA3") != std::string::npos || value.find("SAE") != std::string::npos) return "sae";
    if (value.find("OWE") != std::string::npos) return "owe";
    return "unsupported";
}

std::string new_uuid() {
    std::random_device source;
    unsigned char bytes[16];
    for (auto &byte : bytes) byte = static_cast<unsigned char>(source());
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (unsigned i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) stream << '-';
        stream << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return stream.str();
}
} // namespace

SetupNetworkState scan_setup_networks(const SetupCommandRunner &runner, bool simulated) {
    SetupNetworkState state;
    if (simulated) {
        state.available = true;
        state.interface = "wlan0";
        state.message = "Simulator networks (no host networking changes)";
        state.networks = {{"LILYGO Wi-Fi", "wpa-psk", 88, false, true},
                          {"Guest", "open", 65, false, true},
                          {"Office enterprise", "unsupported", 55, false, false}};
        return state;
    }
    auto result = runner(nmcli({"--terse", "--escape", "yes", "--fields", "DEVICE,TYPE,STATE", "device", "status"}));
    if (result.exit_code != 0) {
        state.message = "NetworkManager is unavailable. Connect Ethernet or skip Wi-Fi.";
        return state;
    }
    std::istringstream devices(result.output);
    std::string line;
    while (std::getline(devices, line)) {
        const auto fields = split_nmcli(line);
        if (fields.size() != 3) continue;
        if (fields[2] == "connected" || fields[2].rfind("connected (", 0) == 0) state.connected = true;
        if (fields[1] == "wifi" && state.interface.empty() && clean_text(fields[0], 15)) state.interface = fields[0];
    }
    if (state.interface.empty()) {
        state.message = state.connected ? "Network connected. No Wi-Fi adapter detected." : "No Wi-Fi adapter detected. Connect Ethernet or skip Wi-Fi.";
        return state;
    }
    state.available = true;
    result = runner(nmcli({"radio", "wifi", "on"}, 10));
    if (result.exit_code != 0) {
        state.message = "Wi-Fi is blocked or unavailable. Unblock the adapter and scan again.";
        return state;
    }
    result = runner(nmcli({"--terse", "--escape", "yes", "--fields", "IN-USE,SSID,SIGNAL,SECURITY",
                           "device", "wifi", "list", "--rescan", "yes", "ifname", state.interface}));
    if (result.exit_code != 0) {
        state.message = "Could not scan Wi-Fi. Check the adapter and country setting, then retry.";
        return state;
    }
    std::istringstream networks(result.output);
    while (std::getline(networks, line)) {
        const auto fields = split_nmcli(line);
        if (fields.size() != 4 || !clean_text(fields[1], 32)) continue;
        SetupWifiNetwork network;
        network.ssid = fields[1];
        network.security = security_type(fields[3]);
        network.supported = network.security != "unsupported";
        network.connected = fields[0] == "*";
        if (network.connected) state.connected = true;
        if (fields[2].empty() || fields[2].size() > 3 ||
            !std::all_of(fields[2].begin(), fields[2].end(), [](unsigned char c) { return std::isdigit(c); })) continue;
        network.signal = std::min(100, std::stoi(fields[2]));
        auto duplicate = std::find_if(state.networks.begin(), state.networks.end(), [&](const SetupWifiNetwork &known) {
            return known.ssid == network.ssid && known.security == network.security;
        });
        if (duplicate == state.networks.end()) state.networks.push_back(network);
        else {
            duplicate->signal = std::max(duplicate->signal, network.signal);
            duplicate->connected = duplicate->connected || network.connected;
        }
    }
    std::stable_sort(state.networks.begin(), state.networks.end(), [](const auto &a, const auto &b) {
        if (a.connected != b.connected) return a.connected;
        return a.signal > b.signal;
    });
    state.message = state.connected ? "Network connected. You can continue or choose another network." :
        (state.networks.empty() ? "No networks found. Scan again or enter a hidden network." : "Select a network or enter a hidden network.");
    return state;
}

bool connect_setup_network(const SetupNetworkState &state, const SetupWifiRequest &request,
                           const SetupCommandRunner &runner, std::string &error, bool simulated) {
    error.clear();
    if (!state.available || !clean_text(state.interface, 15)) {
        error = "No Wi-Fi adapter is available.";
        return false;
    }
    if (!clean_text(request.ssid, 32)) {
        error = "Enter a network name of 1 to 32 bytes without control characters.";
        return false;
    }
    const bool secret_required = request.security == "wpa-psk" || request.security == "sae";
    if (!secret_required && request.security != "open" && request.security != "owe") {
        error = "Enterprise and WEP networks are not supported by this wizard. Use a personal network or Ethernet.";
        return false;
    }
    if (secret_required) {
        const bool hex_psk = request.security == "wpa-psk" && request.password.size() == 64 &&
            std::all_of(request.password.begin(), request.password.end(), [](unsigned char c) { return std::isxdigit(c); });
        const size_t minimum = request.security == "sae" ? 1 : 8;
        if (!hex_psk && (!clean_text(request.password, 63) || request.password.size() < minimum)) {
            error = request.security == "sae" ? "Enter a WPA3 password of 1 to 63 bytes." :
                "Enter a WPA password of 8 to 63 bytes, or 64 hexadecimal digits.";
            return false;
        }
    } else if (!request.password.empty()) {
        error = "This network does not use a password. Clear the password field.";
        return false;
    }
    if (simulated) return true;

    const std::string uuid = new_uuid();
    auto command = nmcli({"connection", "add", "type", "wifi", "ifname", state.interface,
        "con-name", "LILYGO setup " + request.ssid, "connection.uuid", uuid,
        "connection.autoconnect", "no", "ssid", request.ssid,
        "802-11-wireless.hidden", request.hidden ? "yes" : "no"});
    if (request.security != "open") {
        command.arguments.insert(command.arguments.end(), {"802-11-wireless-security.key-mgmt", request.security});
        if (secret_required) command.arguments.insert(command.arguments.end(), {"802-11-wireless-security.psk-flags", "0"});
    }
    auto result = runner(command);
    if (result.exit_code != 0) {
        error = "Could not create the Wi-Fi connection. Check NetworkManager and retry.";
        return false;
    }
    command = nmcli({"connection", "up", "uuid", uuid, "ifname", state.interface}, 60);
    if (secret_required) {
        command.arguments.insert(command.arguments.end(), {"passwd-file", "/dev/stdin"});
        command.input = "802-11-wireless-security.psk:" + request.password + "\n";
    }
    result = runner(command);
    clear_setup_secret(command.input);
    // Do not return nmcli output: neither passwords nor potentially sensitive
    // helper diagnostics belong in UI messages or logs.
    clear_setup_secret(result.output);
    if (result.exit_code != 0) {
        runner(nmcli({"connection", "delete", "uuid", uuid}));
        error = "Wi-Fi connection failed. Check the password and signal, then retry.";
        return false;
    }
    result = runner(nmcli({"connection", "modify", "uuid", uuid, "connection.autoconnect", "yes"}));
    if (result.exit_code != 0) {
        error = "Wi-Fi connected, but automatic reconnect could not be saved. Please retry.";
        return false;
    }
    return true;
}
} // namespace launcher
