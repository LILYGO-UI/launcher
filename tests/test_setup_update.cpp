#include "setup.h"

#include <cassert>
#include <string>

using namespace launcher;
int main() {
    std::string error;
    unsigned calls = 0;
    SetupCommandRunner queue = [&](const SetupCommand &command) {
        assert(command.arguments.size() == 2);
        assert(command.arguments[0] == "/usr/lib/lilygo-ui-launcher/lilygo-ui-launcher-first-boot-update");
        assert(command.arguments[1] == (++calls == 1 ? "check" : "install"));
        assert(command.input.empty());
        return SetupCommandResult{0, ""};
    };
    assert(start_setup_update(false, queue, error, false));
    assert(start_setup_update(true, queue, error, false));
    assert(calls == 2);
    const auto read = [](const std::string &output) {
        return read_setup_update([&](const SetupCommand &command) {
            assert(command.arguments.back() == "status");
            return SetupCommandResult{0, output};
        }, false);
    };
    assert(read("status=idle\n").status == SetupUpdateStatus::Idle);
    assert(read("status=queued\n").status == SetupUpdateStatus::Checking);
    assert(read("status=checking\n").status == SetupUpdateStatus::Checking);
    assert(read("status=available\npackages=24\n").message.find("24") != std::string::npos);
    assert(read("status=installing\npercent=47\n").message.find("47%") != std::string::npos);
    assert(read("status=downloading\npercent=10000000\n").message.find('%') == std::string::npos);
    const auto completed = read("status=succeeded\nreboot=1\n");
    assert(completed.status == SetupUpdateStatus::Succeeded && completed.reboot_required);
    assert(read("status=failed\nmessage=interrupted\n").message.find("interrupted") != std::string::npos);
    assert(read("malformed\n").status == SetupUpdateStatus::Running);
    SetupCommandRunner failed = [](const SetupCommand &) { return SetupCommandResult{-1,"",true}; };
    assert(read_setup_update(failed, false).status == SetupUpdateStatus::Running);
    assert(!start_setup_update(true, failed, error, false));
    SetupCommandRunner untouched = [](const SetupCommand &) -> SetupCommandResult { assert(false); return {}; };
    assert(start_setup_update(false, untouched, error, true));
    assert(read_setup_update(untouched, true).status == SetupUpdateStatus::Available);
    assert(start_setup_update(true, untouched, error, true));
    assert(read_setup_update(untouched, true).status == SetupUpdateStatus::Succeeded);
}
