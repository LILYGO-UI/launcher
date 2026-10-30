#include "setup.h"

#include <sstream>
#include <unordered_map>

namespace launcher {
namespace {
constexpr const char *kHelper = "/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot-update";
SetupUpdateState simulated_state;
SetupCommand update_command(const char *operation) {
    SetupCommand command;
    command.arguments = {kHelper, operation};
    command.environment = {"LC_ALL=C"};
    command.timeout_seconds = 15;
    return command;
}
bool numeric(const std::string &value) {
    return !value.empty() && value.size() < 9 && value.find_first_not_of("0123456789") == std::string::npos;
}
} // namespace

bool start_setup_update(bool install, const SetupCommandRunner &runner, std::string &error, bool simulated) {
    error.clear();
    if (simulated) {
        simulated_state.status = install ? SetupUpdateStatus::Succeeded : SetupUpdateStatus::Available;
        simulated_state.message = install ? "Simulator update complete (host unchanged)." : "Simulator: 3 updates available.";
        return true;
    }
    const auto result = runner(update_command(install ? "install" : "check"));
    if (result.exit_code == 0) return true;
    error = "Could not start the update service. Retry after any running system update finishes.";
    return false;
}

SetupUpdateState read_setup_update(const SetupCommandRunner &runner, bool simulated) {
    if (simulated) return simulated_state;
    SetupUpdateState state;
    const auto result = runner(update_command("status"));
    if (result.exit_code != 0 || result.timed_out) {
        // A transport failure is not evidence that apt stopped. Keep navigation
        // locked until a subsequent status read confirms a terminal job state.
        state.status = SetupUpdateStatus::Running;
        state.message = "Update status unavailable. Reconnecting to the update service…";
        return state;
    }
    std::unordered_map<std::string, std::string> fields;
    std::istringstream stream(result.output);
    std::string line;
    while (std::getline(stream, line)) {
        const auto separator = line.find('=');
        if (separator != std::string::npos) fields[line.substr(0, separator)] = line.substr(separator + 1);
    }
    const auto &status = fields["status"];
    state.reboot_required = fields["reboot"] == "1";
    if (status == "idle") {
        state.status = SetupUpdateStatus::Idle;
        state.message = "Check for the latest operating system updates, or skip for now.";
    } else if (status == "queued" || status == "checking") {
        state.status = SetupUpdateStatus::Checking;
        state.message = "Checking for updates. Keep the device powered on.";
    } else if (status == "available") {
        state.status = SetupUpdateStatus::Available;
        state.message = numeric(fields["packages"]) ? fields["packages"] + " package updates available." : "Updates checked. Ready to install.";
    } else if (status == "downloading" || status == "installing") {
        state.status = SetupUpdateStatus::Running;
        state.message = status == "downloading" ? "Downloading updates" : "Installing updates";
        if (numeric(fields["percent"]) && std::stoi(fields["percent"]) <= 100) state.message += " — " + fields["percent"] + "%";
        state.message += ". Keep the device powered on.";
    } else if (status == "succeeded") {
        state.status = SetupUpdateStatus::Succeeded;
        state.message = state.reboot_required ? "Updates installed. Restart the device after setup." : "Updates installed successfully.";
    } else if (status == "failed") {
        state.status = SetupUpdateStatus::Failed;
        state.message = fields["message"] == "interrupted" ? "The previous update was interrupted. Retry to finish installing updates." :
            "Update failed. Check the network and retry, or skip for now.";
    } else {
        state.status = SetupUpdateStatus::Running;
        state.message = "Waiting for a valid update status…";
    }
    return state;
}
} // namespace launcher
