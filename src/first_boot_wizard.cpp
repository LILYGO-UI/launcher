#include "first_boot_wizard.h"
#include "account_view.h"
#include "session.h"
#include "setup_flow.h"
#include "setup_view.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <ctime>
#include <utility>

namespace launcher {
namespace {
using Clock = std::chrono::steady_clock;
enum class Operation { None, Region, Account, Scan, Connect, StartUpdate, PollUpdate, CompletionStatus, Finish };
struct Result {
    bool ok = true;
    std::string error;
    SetupRegion region;
    SetupNetworkState network;
    SetupUpdateState update;
};
bool updating(const SetupUpdateState &state)
{
    return state.status == SetupUpdateStatus::Checking || state.status == SetupUpdateStatus::Running;
}
}

struct FirstBootWizard::Impl {
    UserSession &session;
    SetupFlow flow;
    SetupCatalog catalog;
    SetupNetworkState network;
    SetupUpdateState update;
    std::unique_ptr<SetupView> view;
    launcher_account_view_t *account = nullptr;
    std::future<Result> worker;
    Operation operation = Operation::None;
    Clock::time_point next_poll{};
    bool keyboard_present = false;
    bool install_requested = false;

    explicit Impl(UserSession &value) : session(value) {}
    ~Impl()
    {
        if (worker.valid()) worker.wait();
        launcher_account_view_destroy(account);
    }
    void error(const std::string &message)
    {
        if (account) {
            launcher_account_view_set_setup_username(account, session.setup_username().c_str());
            launcher_account_view_set_busy(account, false);
            launcher_account_view_set_error(account, message.c_str());
        }
        if (view) { view->set_busy(false); view->set_error(message); }
    }
    template<class Function> void start(Operation kind, const char *message, Function function)
    {
        if (account) launcher_account_view_set_busy(account, true);
        if (view) view->set_busy(true, message);
        operation = kind;
        worker = std::async(std::launch::async, std::move(function));
    }
    bool advance(SetupStage stage, const SetupRegion &region)
    {
        std::string message;
        if (flow.advance(stage, region, message)) return true;
        error(message);
        return false;
    }
    bool show_account(bool resume, std::string &message)
    {
        view.reset();
        launcher_account_view_destroy(account);
        account = resume
            ? launcher_account_view_create(false, session.last_user().c_str(), false)
            : launcher_account_view_create_setup(session.setup_username().c_str());
        if (!account) { message = "Cannot create the setup account page"; return false; }
        if (!resume) launcher_account_view_set_setup_continuation(account, true);
        launcher_account_view_set_hardware_keyboard_present(account, keyboard_present);
        return true;
    }
    void scan()
    {
        start(Operation::Scan, "Looking for Wi-Fi networks…", [] {
            Result result; result.network = scan_setup_networks(run_setup_command); return result;
        });
    }
    void poll_update(bool completion = false)
    {
        start(completion ? Operation::CompletionStatus : Operation::PollUpdate, "Reading update status…", [] {
            Result result; result.update = read_setup_update(run_setup_command); return result;
        });
    }
    bool show_stage(std::string &message)
    {
        if (flow.stage() == SetupStage::Account) return show_account(false, message);
        launcher_account_view_destroy(account); account = nullptr;
        if (!view) view = std::make_unique<SetupView>(catalog);
        if (!view->valid()) { message = "Cannot create the first-boot setup page"; return false; }
        view->set_hardware_keyboard_present(keyboard_present);
        view->set_region(flow.region());
        switch (flow.stage()) {
            case SetupStage::Region: view->show(SetupPage::Region); break;
            case SetupStage::Network:
                view->show(SetupPage::Network); scan(); break;
            case SetupStage::Update:
                view->show(SetupPage::Update); poll_update(); break;
            case SetupStage::Complete:
                view->show(SetupPage::Complete); poll_update(true); break;
            case SetupStage::Account: break;
        }
        return true;
    }
    void show_stage()
    {
        std::string message;
        if (!show_stage(message)) error(message);
    }
    void finish_result(Result result, Operation completed)
    {
        if (!result.ok) { error(result.error); return; }
        if (view) view->set_busy(false);
        switch (completed) {
            case Operation::Region:
                if (!kSetupSimulated) { unsetenv("TZ"); tzset(); }
                if (advance(SetupStage::Account, result.region)) show_stage();
                break;
            case Operation::Account:
                if (flow.stage() <= SetupStage::Account && !advance(SetupStage::Network, flow.region())) return;
                show_stage();
                break;
            case Operation::Scan:
                network = std::move(result.network); view->set_network(network); break;
            case Operation::Connect:
                if (advance(SetupStage::Update, flow.region())) show_stage();
                break;
            case Operation::StartUpdate:
            case Operation::PollUpdate:
                update = std::move(result.update);
                view->set_update(update);
                view->set_busy(updating(update), update.message);
                next_poll = Clock::now() + std::chrono::seconds(1);
                break;
            case Operation::CompletionStatus:
                update = std::move(result.update);
                view->set_update(update);
                break;
            case Operation::Finish: {
                std::string message;
                if (!flow.finish(message)) { error(message); return; }
                view.reset();
                break;
            }
            case Operation::None: break;
        }
    }
    void tick()
    {
        if (!flow.active()) return;
        if (worker.valid()) {
            if (worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            const Operation completed = operation;
            operation = Operation::None;
            try { finish_result(worker.get(), completed); }
            catch (...) { error("Setup operation failed. Please retry."); }
            return;
        }
        if (account) {
            char name[LAUNCHER_USER_NAME_MAX]{}; char password[128]{}; bool create = false;
            if (!launcher_account_view_take_request(account, name, sizeof(name), password, sizeof(password), &create)) return;
            // If only the durable stage write failed, the PAM session remains
            // valid. Retry that write without opening a second PAM session.
            if (!session.username().empty()) {
                volatile char *p = password; for (size_t i = 0; i < sizeof(password); ++i) p[i] = 0;
                finish_result(Result{}, Operation::Account); return;
            }
            if (session.setup_account_saved()) create = false;
            std::string secret(password);
            volatile char *p = password; for (size_t i = 0; i < sizeof(password); ++i) p[i] = 0;
            start(Operation::Account, "Saving account…", [this, username = std::string(name), secret = std::move(secret), create]() mutable {
                Result result; result.ok = session.login(username, secret, create, result.error, false);
                clear_setup_secret(secret); return result;
            });
            return;
        }
        if (!view) return;
        if (flow.stage() == SetupStage::Update && updating(update) && Clock::now() >= next_poll) {
            poll_update(); return;
        }
        SetupViewRequest request;
        if (!view->take_request(request)) return;
        switch (request.action) {
            case SetupAction::ApplyRegion:
                start(Operation::Region, "Applying regional settings…", [this, region = request.region] {
                    Result result; result.region = region;
                    result.ok = apply_setup_region(region, catalog, run_setup_command, result.error); return result;
                }); break;
            case SetupAction::ScanWifi: scan(); break;
            case SetupAction::ConnectWifi:
                start(Operation::Connect, "Connecting to Wi-Fi…", [this, wifi = std::move(request.wifi)]() mutable {
                    Result result; result.ok = connect_setup_network(network, wifi, run_setup_command, result.error);
                    clear_setup_secret(wifi.password); return result;
                }); break;
            case SetupAction::SkipWifi:
                if (advance(SetupStage::Update, flow.region())) show_stage();
                break;
            case SetupAction::CheckUpdates:
            case SetupAction::InstallUpdates: {
                const bool install = request.action == SetupAction::InstallUpdates ||
                    (update.status == SetupUpdateStatus::Failed && install_requested);
                install_requested = install;
                start(Operation::StartUpdate, "Starting the update service…", [install] {
                    Result result; result.ok = start_setup_update(install, run_setup_command, result.error);
                    // Even if start failed, inspect the service before unlocking
                    // navigation: another update may already be running.
                    result.update = read_setup_update(run_setup_command);
                    if (updating(result.update)) result.ok = true;
                    return result;
                }); break;
            }
            case SetupAction::SkipUpdates:
                if (!updating(update) && advance(SetupStage::Complete, flow.region())) show_stage();
                break;
            case SetupAction::Back:
                if (flow.stage() == SetupStage::Update && !updating(update) && advance(SetupStage::Network, flow.region())) show_stage();
                break;
            case SetupAction::Finish:
                start(Operation::Finish, "Finishing setup…", [this] {
                    Result result; result.ok = session.complete_setup(result.error); return result;
                }); break;
        }
        clear_setup_secret(request.wifi.password);
    }
};

FirstBootWizard::FirstBootWizard(UserSession &session) : impl_(std::make_unique<Impl>(session)) {}
FirstBootWizard::~FirstBootWizard() = default;
bool FirstBootWizard::initialize(bool demo, std::string &error)
{
    if (demo && !kSetupSimulated) { error = "Setup preview is available only in the simulator"; return false; }
    if (!impl_->flow.initialize(impl_->session.setup_required() || demo, impl_->session.setup_account_saved(), error)) return false;
    if (!impl_->flow.active()) return true;
    impl_->catalog = load_setup_catalog();
    if (impl_->flow.stage() == SetupStage::Region &&
        !impl_->flow.advance(SetupStage::Region, impl_->catalog.current, error)) return false;
    // Saved progress never authenticates an account. Resume only after PAM
    // verifies the committed account's password in this new launcher process.
    if (impl_->session.setup_account_saved()) return impl_->show_account(true, error);
    return impl_->show_stage(error);
}
bool FirstBootWizard::active() const { return impl_->flow.active(); }
void FirstBootWizard::tick() { impl_->tick(); }
void FirstBootWizard::set_hardware_keyboard_present(bool present)
{
    impl_->keyboard_present = present;
    if (impl_->account) {
        launcher_account_view_set_hardware_keyboard_present(impl_->account, present);
        launcher_account_view_capture_input(impl_->account);
    }
    if (impl_->view) { impl_->view->set_hardware_keyboard_present(present); impl_->view->capture_input(); }
}
}
