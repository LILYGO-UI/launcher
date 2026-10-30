#include "setup_flow.h"

#include <cassert>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace launcher;
namespace {
struct Fixture {
    char path[64] = "/tmp/launcher-setup-flow-XXXXXX";
    std::string progress;
    Fixture() { assert(mkdtemp(path)); progress = std::string(path) + "/first-boot-progress"; }
    ~Fixture() { unlink(progress.c_str()); rmdir(path); }
    void write(const std::string &value, mode_t mode = 0600) {
        unlink(progress.c_str());
        std::ofstream file(progress); file << value; file.close();
        assert(chmod(progress.c_str(), mode) == 0);
    }
};
void advances_and_resumes_without_recreating_saved_account() {
    Fixture fixture;
    std::string error;
    SetupFlow first(fixture.path, false);
    assert(first.initialize(true, false, error));
    assert(first.active() && first.stage() == SetupStage::Region);
    SetupRegion region{"CN", "zh_CN.UTF-8", "Asia/Shanghai", "us"};
    assert(!first.advance(SetupStage::Complete, region, error));
    assert(!first.advance(static_cast<SetupStage>(999), region, error));
    assert(!first.finish(error));
    assert(first.advance(SetupStage::Account, region, error));
    struct stat state{};
    assert(stat(fixture.progress.c_str(), &state) == 0 && (state.st_mode & 07777) == 0600);
    SetupFlow account_saved(fixture.path, false);
    assert(account_saved.initialize(true, true, error));
    assert(account_saved.stage() == SetupStage::Network && account_saved.region().timezone == "Asia/Shanghai");
    assert(account_saved.advance(SetupStage::Update, region, error));
    SetupFlow restarted(fixture.path, false);
    assert(restarted.initialize(true, true, error) && restarted.stage() == SetupStage::Update);
    assert(restarted.advance(SetupStage::Network, region, error));
    assert(restarted.advance(SetupStage::Update, region, error));
    assert(restarted.advance(SetupStage::Complete, region, error));
    // Cleanup may have removed piwiz state before a crash. The progress file
    // still owns the final page until the user commits completion.
    SetupFlow cleaned(fixture.path, false);
    assert(cleaned.initialize(false, true, error));
    assert(cleaned.active() && cleaned.stage() == SetupStage::Complete);
    assert(cleaned.finish(error));
    assert(!cleaned.active() && access(fixture.progress.c_str(), F_OK) != 0);
    SetupFlow finished(fixture.path, false);
    assert(finished.initialize(false, true, error) && !finished.active());
}
void missing_state_and_saved_account_fallback() {
    Fixture fixture;
    std::string error;
    SetupFlow normal(std::string(fixture.path) + "/missing", false);
    assert(normal.initialize(false, false, error) && !normal.active());
    assert(!normal.initialize(true, false, error));
    SetupFlow saved(fixture.path, false);
    assert(saved.initialize(true, true, error));
    assert(saved.stage() == SetupStage::Network);
    SetupFlow missing_account(fixture.path, false);
    assert(!missing_account.initialize(true, false, error));
}
void rejects_tampered_or_invalid_progress() {
    Fixture fixture;
    std::string error;
    const std::string valid = "1\n3\nGB\nen_GB.UTF-8\nEurope/London\ngb\n";
    SetupFlow flow(fixture.path, false);
    fixture.write(valid, 0644);
    assert(!flow.initialize(true, true, error));
    fixture.write(valid);
    assert(!flow.initialize(true, false, error));
    fixture.write("2\n3\nGB\nen_GB.UTF-8\nEurope/London\ngb\n");
    assert(!flow.initialize(true, true, error));
    fixture.write("1\n3\nGB\nen_GB.UTF-8\n../../etc/shadow\ngb\n");
    assert(!flow.initialize(true, true, error));
    fixture.write(valid + "unexpected\n");
    assert(!flow.initialize(true, true, error));
    fixture.write(std::string(2048, 'x'));
    assert(!flow.initialize(true, true, error));
    unlink(fixture.progress.c_str());
    assert(symlink("/etc/passwd", fixture.progress.c_str()) == 0);
    assert(!flow.initialize(true, true, error));
    unlink(fixture.progress.c_str());
    assert(chmod(fixture.path, 0777) == 0);
    assert(!flow.initialize(true, false, error));
    assert(chmod(fixture.path, 0700) == 0);
    assert(flow.initialize(true, false, error));
    SetupRegion invalid;
    invalid.locale = "en_GB.UTF-8\ncommand";
    assert(!flow.advance(SetupStage::Account, invalid, error));
    assert(flow.stage() == SetupStage::Region);
}
}
int main() {
    advances_and_resumes_without_recreating_saved_account();
    missing_state_and_saved_account_fallback();
    rejects_tampered_or_invalid_progress();
}
