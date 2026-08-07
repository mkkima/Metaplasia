#include "metaplasia/host/engine_controller.hpp"

#include "metaplasia/base/utf.hpp"
#include "metaplasia/platform/process.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwchar>
#include <exception>
#include <sstream>
#include <utility>

namespace metaplasia::host {
namespace {

constexpr auto kMonitorInterval = std::chrono::milliseconds(200);
constexpr auto kRetryDelay = std::chrono::seconds(10);
constexpr auto kLoadedAgentRetryDelay = std::chrono::milliseconds(200);
constexpr auto kRepeatedErrorLogInterval = std::chrono::seconds(30);
constexpr auto kCompatibilityRefreshInterval = std::chrono::seconds(30);
constexpr auto kPendingCompatibilityRefreshInterval =
    std::chrono::milliseconds(200);
constexpr auto kCrashWindow = std::chrono::seconds(60);
constexpr std::size_t kCrashLimit = 3;
constexpr std::size_t kMaximumExplorerWindowProcesses = 64;

std::atomic<EngineController*> g_explorer_window_event_controller{nullptr};

struct ExplorerWindowProcessCollector final {
    std::array<std::uint32_t, kMaximumExplorerWindowProcesses> process_ids{};
    std::size_t count{0};
    bool overflow{false};
};

struct ExplorerWindowProcessSnapshot final {
    std::vector<std::uint32_t> process_ids;
    bool overflow{false};
};

BOOL CALLBACK CollectExplorerWindowProcess(
    const HWND window,
    const LPARAM raw_collector) noexcept {
    auto* collector = reinterpret_cast<ExplorerWindowProcessCollector*>(
        raw_collector);
    if (collector == nullptr) {
        return FALSE;
    }
    std::array<wchar_t, 64> class_name{};
    const int length = ::GetClassNameW(
        window,
        class_name.data(),
        static_cast<int>(class_name.size()));
    if (length <= 0 || static_cast<std::size_t>(length) >= class_name.size() ||
        (_wcsicmp(class_name.data(), L"CabinetWClass") != 0 &&
         _wcsicmp(class_name.data(), L"ExploreWClass") != 0)) {
        return TRUE;
    }
    DWORD process_id = 0;
    ::GetWindowThreadProcessId(window, &process_id);
    if (process_id == 0 ||
        std::find(
            collector->process_ids.begin(),
            collector->process_ids.begin() + collector->count,
            process_id) !=
            collector->process_ids.begin() + collector->count) {
        return TRUE;
    }
    if (collector->count >= collector->process_ids.size()) {
        collector->overflow = true;
        return FALSE;
    }
    collector->process_ids[collector->count++] = process_id;
    return TRUE;
}

ExplorerWindowProcessSnapshot ExplorerWindowProcessIds() {
    ExplorerWindowProcessCollector collector;
    static_cast<void>(::EnumWindows(
        &CollectExplorerWindowProcess,
        reinterpret_cast<LPARAM>(&collector)));
    ExplorerWindowProcessSnapshot snapshot;
    snapshot.process_ids.assign(
        collector.process_ids.begin(),
        collector.process_ids.begin() + collector.count);
    snapshot.overflow = collector.overflow;
    std::ranges::sort(snapshot.process_ids);
    return snapshot;
}

const platform::ProcessInfo* SelectProcess(
    const std::vector<platform::ProcessInfo>& processes) noexcept {
    if (processes.empty()) {
        return nullptr;
    }
    return &*std::min_element(
        processes.begin(),
        processes.end(),
        [](const auto& left, const auto& right) {
            return left.process_id < right.process_id;
        });
}

const platform::ProcessInfo* SelectExplorerShellProcess(
    const std::vector<platform::ProcessInfo>& processes) noexcept {
    const HWND taskbar = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar != nullptr) {
        DWORD taskbar_process_id = 0;
        ::GetWindowThreadProcessId(taskbar, &taskbar_process_id);
        const auto match = std::find_if(
            processes.begin(),
            processes.end(),
            [taskbar_process_id](const auto& process) {
                return process.process_id == taskbar_process_id;
            });
        if (match != processes.end()) {
            return &*match;
        }
    }
    return SelectProcess(processes);
}

std::string_view TargetName(const protocol::TargetId target) noexcept {
    switch (target) {
        case protocol::TargetId::taskbar:
            return "taskbar";
        case protocol::TargetId::file_explorer:
            return "file-explorer";
        case protocol::TargetId::start_menu:
            return "start-menu";
        default:
            return "unknown";
    }
}

std::string_view CustomizationTarget(
    const protocol::CustomizationId customization) noexcept {
    switch (customization) {
        case protocol::CustomizationId::taskbar_clock_prefix:
        case protocol::CustomizationId::taskbar_opacity_milli:
        case protocol::CustomizationId::taskbar_hide_notification_center:
        case protocol::CustomizationId::taskbar_hide_control_center:
        case protocol::CustomizationId::taskbar_hide_show_desktop:
        case protocol::CustomizationId::taskbar_background_color_enabled:
        case protocol::CustomizationId::taskbar_background_color:
        case protocol::CustomizationId::taskbar_capsule_enabled:
            return "taskbar";
        case protocol::CustomizationId::file_explorer_title_prefix:
        case protocol::CustomizationId::file_explorer_background_color_enabled:
        case protocol::CustomizationId::file_explorer_background_color:
        case protocol::CustomizationId::file_explorer_transition_animation:
        case protocol::CustomizationId::file_explorer_custom_scrollbar_enabled:
            return "file-explorer";
        case protocol::CustomizationId::start_menu_opacity_milli:
        case protocol::CustomizationId::start_menu_hide_recommended:
        case protocol::CustomizationId::start_menu_background_color_enabled:
        case protocol::CustomizationId::start_menu_background_color:
        case protocol::CustomizationId::start_menu_three_panel_layout_enabled:
        case protocol::CustomizationId::start_menu_hide_all_apps:
            return "start-menu";
        default:
            return "unknown";
    }
}

std::string_view ProcessTargetName(
    const protocol::AgentTarget target,
    const std::uint32_t features) noexcept {
    if (target == protocol::AgentTarget::start_menu) {
        return "start-menu";
    }
    constexpr std::uint32_t taskbar_features =
        protocol::agent_feature_taskbar_clock_prefix |
        protocol::agent_feature_taskbar_background_color |
        protocol::agent_feature_taskbar_capsule;
    constexpr std::uint32_t explorer_features =
        protocol::agent_feature_file_explorer_title_prefix |
        protocol::agent_feature_file_explorer_background_color |
        protocol::agent_feature_file_explorer_custom_scrollbar;
    const bool taskbar = (features & taskbar_features) != 0;
    const bool explorer = (features & explorer_features) != 0;
    if (taskbar && explorer) {
        return "taskbar+file-explorer";
    }
    if (taskbar) {
        return "taskbar";
    }
    return explorer ? "file-explorer" : "explorer-shell";
}

}  // namespace

EngineController::EngineController(
    std::filesystem::path agent_path,
    SettingsStore settings_store,
    HostSettings initial_settings,
    const std::uint32_t session_id,
    DiagnosticLog& diagnostic_log,
    const injector::AgentTrustPolicy agent_trust_policy)
    : agent_path_(std::move(agent_path)),
      settings_store_(std::move(settings_store)),
      session_id_(session_id),
      diagnostic_log_(diagnostic_log),
      settings_(initial_settings),
      injector_(agent_trust_policy) {}

EngineController::~EngineController() {
    Stop();
}

void EngineController::Start() {
    std::lock_guard lock(mutex_);
    if (monitor_thread_.joinable()) {
        return;
    }
    EngineController* expected = nullptr;
    if (!g_explorer_window_event_controller.compare_exchange_strong(
            expected, this, std::memory_order_acq_rel)) {
        throw std::logic_error(
            "Only one EngineController can monitor Explorer window events");
    }
    try {
        explorer_window_event_thread_ = std::jthread(
            [this](const std::stop_token token) {
                ExplorerWindowEventLoop(token);
            });
        monitor_thread_ = std::jthread(
            [this](const std::stop_token token) { MonitorLoop(token); });
        diagnostic_log_.Write(
            DiagnosticLevel::info,
            "engine",
            "monitor-started",
            "host",
            ::GetCurrentProcessId(),
            "Shell process monitoring started");
    } catch (...) {
        // Prevent a late WinEvent callback from waiting on mutex_ while this
        // thread owns it and joins the event pump during partial startup.
        g_explorer_window_event_controller.store(
            nullptr, std::memory_order_release);
        if (explorer_window_event_thread_.joinable()) {
            explorer_window_event_thread_.request_stop();
            const DWORD thread_id = explorer_window_event_thread_id_.load(
                std::memory_order_acquire);
            if (thread_id != 0) {
                static_cast<void>(::PostThreadMessageW(
                    thread_id, WM_QUIT, 0, 0));
            }
            explorer_window_event_thread_.join();
        }
        throw;
    }
}

void EngineController::Stop() noexcept {
    if (!monitor_thread_.joinable() &&
        !explorer_window_event_thread_.joinable()) {
        return;
    }
    diagnostic_log_.Write(
        DiagnosticLevel::info,
        "engine",
        "monitor-stopping",
        "host",
        ::GetCurrentProcessId(),
        "Shell process monitoring is stopping");
    if (explorer_window_event_thread_.joinable()) {
        explorer_window_event_thread_.request_stop();
        const DWORD thread_id = explorer_window_event_thread_id_.load(
            std::memory_order_acquire);
        if (thread_id != 0) {
            static_cast<void>(::PostThreadMessageW(
                thread_id, WM_QUIT, 0, 0));
        }
        explorer_window_event_thread_.join();
    }
    EngineController* expected = this;
    static_cast<void>(g_explorer_window_event_controller.compare_exchange_strong(
        expected, nullptr, std::memory_order_acq_rel));
    if (!monitor_thread_.joinable()) {
        return;
    }
    monitor_thread_.request_stop();
    {
        std::lock_guard lock(mutex_);
        wake_requested_ = true;
    }
    wake_condition_.notify_all();
    monitor_thread_.join();
    DeactivateLoadedAgents();
}

void EngineController::RequestWake() noexcept {
    {
        std::lock_guard lock(mutex_);
        wake_requested_ = true;
    }
    wake_condition_.notify_one();
}

void CALLBACK EngineController::ExplorerWindowEventCallback(
    HWINEVENTHOOK,
    const DWORD event,
    const HWND window,
    const LONG object_id,
    const LONG child_id,
    DWORD,
    DWORD) noexcept {
    if ((event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW) ||
        window == nullptr || object_id != OBJID_WINDOW ||
        child_id != CHILDID_SELF || ::GetAncestor(window, GA_ROOT) != window) {
        return;
    }
    std::array<wchar_t, 64> class_name{};
    const int length = ::GetClassNameW(
        window,
        class_name.data(),
        static_cast<int>(class_name.size()));
    if (length <= 0 || static_cast<std::size_t>(length) >= class_name.size() ||
        (_wcsicmp(class_name.data(), L"CabinetWClass") != 0 &&
         _wcsicmp(class_name.data(), L"ExploreWClass") != 0)) {
        return;
    }
    EngineController* const controller =
        g_explorer_window_event_controller.load(std::memory_order_acquire);
    if (controller != nullptr) {
        controller->RequestWake();
    }
}

void EngineController::ExplorerWindowEventLoop(
    const std::stop_token stop_token) noexcept {
    MSG message{};
    static_cast<void>(::PeekMessageW(
        &message, nullptr, WM_USER, WM_USER, PM_NOREMOVE));
    explorer_window_event_thread_id_.store(
        ::GetCurrentThreadId(), std::memory_order_release);

    const HWINEVENTHOOK hook = ::SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_SHOW,
        nullptr,
        &EngineController::ExplorerWindowEventCallback,
        0,
        0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (hook == nullptr) {
        ::OutputDebugStringW(
            L"[Metaplasia Host] Explorer window event hook unavailable; "
            L"using the reconciliation monitor fallback.\n");
    }

    while (!stop_token.stop_requested()) {
        const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            break;
        }
        ::TranslateMessage(&message);
        static_cast<void>(::DispatchMessageW(&message));
    }
    if (hook != nullptr) {
        static_cast<void>(::UnhookWinEvent(hook));
    }
    explorer_window_event_thread_id_.store(0, std::memory_order_release);
}

std::vector<protocol::TargetSnapshot> EngineController::Snapshot() const {
    std::lock_guard lock(mutex_);
    return {
        BuildSnapshot(protocol::TargetId::taskbar, explorer_),
        BuildFileExplorerSnapshot(),
        BuildSnapshot(protocol::TargetId::start_menu, start_menu_),
    };
}

protocol::CustomizationSettings EngineController::Settings() const {
    std::lock_guard lock(mutex_);
    return settings_.ToProtocol();
}

Result<protocol::XamlDiagnosticsResponse> EngineController::XamlDiagnostics(
    const protocol::TargetId target) const {
    if (target != protocol::TargetId::taskbar &&
        target != protocol::TargetId::start_menu) {
        return Status(
            ErrorCode::incompatible,
            "XAML diagnostics is not implemented for this target yet");
    }

    std::uint32_t process_id = 0;
    bool agent_loaded = false;
    protocol::AgentTarget agent_target = protocol::AgentTarget::start_menu;
    const char* process_name = "Start menu";
    {
        std::lock_guard lock(mutex_);
        if (target == protocol::TargetId::taskbar) {
            process_id = explorer_.process_id;
            agent_loaded = explorer_.agent_loaded;
            agent_target = protocol::AgentTarget::explorer_shell;
            process_name = "Explorer";
        } else {
            process_id = start_menu_.process_id;
            agent_loaded = start_menu_.agent_loaded;
        }
    }
    if (process_id == 0) {
        return Status(
            ErrorCode::not_found,
            std::string(process_name) + " process is not running");
    }
    if (!agent_loaded) {
        return Status(
            ErrorCode::not_found,
            std::string(process_name) +
                " agent is not loaded; diagnostics never injects it");
    }

    auto snapshot = injector_.QueryLoadedXamlDiagnostics(
        process_id,
        agent_path_,
        agent_target);
    if (!snapshot.ok()) {
        return snapshot.status();
    }

    protocol::XamlDiagnosticsResponse response;
    response.target = target;
    response.dropped_type_count = snapshot.value().dropped_type_count;
    response.dropped_element_count = snapshot.value().dropped_element_count;
    response.tracked_element_count = snapshot.value().tracked_element_count;
    response.types.reserve(snapshot.value().type_count);
    for (std::uint32_t index = 0;
         index < snapshot.value().type_count;
         ++index) {
        const auto& type = snapshot.value().types[index];
        const std::size_t length = wcsnlen_s(
            type.type_name,
            std::size(type.type_name));
        auto name = WideToUtf8(std::wstring_view(type.type_name, length));
        if (!name.ok()) {
            return name.status();
        }
        response.types.push_back(
            {std::move(name).value(), type.observation_count});
    }
    response.elements.reserve(snapshot.value().element_count);
    for (std::uint32_t index = 0;
         index < snapshot.value().element_count;
         ++index) {
        const auto& element = snapshot.value().elements[index];
        const std::size_t name_length = wcsnlen_s(
            element.name,
            std::size(element.name));
        auto name = WideToUtf8(
            std::wstring_view(element.name, name_length));
        if (!name.ok()) {
            return name.status();
        }
        response.elements.push_back(
            {element.handle,
             element.parent_handle,
             element.child_index,
             element.child_count,
             element.type_index,
             std::move(name).value()});
    }
    return response;
}

Result<void> EngineController::SetEnabled(
    const protocol::TargetId target,
    const bool enabled) {
    if (!protocol::IsValidTarget(target)) {
        return Status(ErrorCode::invalid_argument, "Unknown target");
    }

    {
        // Saving while holding the controller lock serializes user changes
        // with crash-loop fail-safe persistence.
        std::lock_guard lock(mutex_);
        HostSettings updated = settings_;
        if (!updated.SetEnabled(target, enabled)) {
            return Status(ErrorCode::invalid_argument, "Unknown target");
        }

        auto save = settings_store_.Save(updated);
        if (!save.ok()) {
            return save.status();
        }

        settings_ = updated;
        if (target == protocol::TargetId::start_menu) {
            ++start_menu_configuration_generation_;
        } else {
            ++explorer_configuration_generation_;
        }
        if (enabled) {
            if (target == protocol::TargetId::start_menu) {
                start_menu_crashes_.clear();
                start_menu_guard_detail_.clear();
            } else {
                explorer_crashes_.clear();
                explorer_guard_detail_.clear();
            }
        }
        wake_requested_ = true;
    }
    wake_condition_.notify_one();
    diagnostic_log_.Write(
        DiagnosticLevel::info,
        "engine",
        enabled ? "target-enabled" : "target-disabled",
        TargetName(target),
        0,
        enabled ? "Target customization was enabled"
                : "Target customization was disabled");
    return {};
}

Result<void> EngineController::SetCustomization(
    const protocol::SetCustomizationRequest& request) {
    {
        std::lock_guard lock(mutex_);
        HostSettings updated = settings_;
        auto changed = updated.SetCustomization(request);
        if (!changed.ok()) {
            return changed.status();
        }
        if (!changed.value()) {
            return {};
        }

        auto save = settings_store_.Save(updated);
        if (!save.ok()) {
            return save.status();
        }

        settings_ = std::move(updated);
        if (request.customization ==
                protocol::CustomizationId::start_menu_opacity_milli ||
            request.customization ==
                protocol::CustomizationId::start_menu_hide_recommended ||
            request.customization ==
                protocol::CustomizationId::start_menu_background_color_enabled ||
            request.customization ==
                protocol::CustomizationId::start_menu_background_color ||
            request.customization ==
                protocol::CustomizationId::start_menu_three_panel_layout_enabled ||
            request.customization ==
                protocol::CustomizationId::start_menu_hide_all_apps) {
            ++start_menu_configuration_generation_;
        } else {
            ++explorer_configuration_generation_;
        }
        wake_requested_ = true;
    }
    wake_condition_.notify_one();
    diagnostic_log_.Write(
        DiagnosticLevel::info,
        "engine",
        "customization-updated",
        CustomizationTarget(request.customization),
        0,
        "A customization setting was persisted; values are intentionally omitted from logs");
    return {};
}

void EngineController::DeactivateLoadedAgents() noexcept {
    struct LoadedAgent final {
        std::uint32_t process_id;
        protocol::AgentTarget target;
    };
    std::array<LoadedAgent, 256> agents{};
    std::size_t count = 0;
    {
        std::lock_guard lock(mutex_);
        if (explorer_.agent_loaded && explorer_.process_id != 0) {
            agents[count++] = {
                explorer_.process_id,
                protocol::AgentTarget::explorer_shell};
        }
        if (start_menu_.agent_loaded && start_menu_.process_id != 0) {
            agents[count++] = {
                start_menu_.process_id,
                protocol::AgentTarget::start_menu};
        }
        for (const auto& [process_id, state] : explorer_auxiliary_) {
            if (!state.agent_loaded || process_id == 0 ||
                count >= agents.size()) {
                continue;
            }
            agents[count++] = {
                process_id,
                protocol::AgentTarget::explorer_shell};
        }
    }

    for (std::size_t index = 0; index < count; ++index) {
        protocol::AgentConfiguration configuration;
        configuration.target = agents[index].target;
        configuration.feature_flags = protocol::agent_feature_none;
        bool deactivated = false;
        try {
            const auto result = injector_.ConfigureLoaded(
                agents[index].process_id,
                agent_path_,
                configuration,
                std::chrono::seconds(3));
            deactivated = result.ok();
        } catch (...) {
            deactivated = false;
        }
        if (!deactivated) {
            ::OutputDebugStringW(
                L"[Metaplasia Host] Unable to deactivate an agent during "
                L"shutdown; the shell process may need to be restarted.\n");
            diagnostic_log_.Write(
                DiagnosticLevel::warning,
                "engine",
                "agent-deactivation-failed",
                agents[index].target == protocol::AgentTarget::start_menu
                    ? "start-menu"
                    : "explorer-shell",
                agents[index].process_id,
                "Unable to deactivate the native agent during host shutdown");
        }
    }
}

void EngineController::MonitorLoop(const std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
        try {
            Reconcile();
        } catch (const std::exception& exception) {
            const std::string message =
                std::string("Monitor exception: ") + exception.what();
            diagnostic_log_.Write(
                DiagnosticLevel::error,
                "engine",
                "monitor-exception",
                "host",
                ::GetCurrentProcessId(),
                message);
            std::lock_guard lock(mutex_);
            explorer_.error = message;
            start_menu_.error = explorer_.error;
            for (auto& [process_id, state] : explorer_auxiliary_) {
                static_cast<void>(process_id);
                state.error = explorer_.error;
            }
        } catch (...) {
            diagnostic_log_.Write(
                DiagnosticLevel::error,
                "engine",
                "monitor-exception",
                "host",
                ::GetCurrentProcessId(),
                "Unknown monitor exception");
            std::lock_guard lock(mutex_);
            explorer_.error = "Unknown monitor exception";
            start_menu_.error = explorer_.error;
            for (auto& [process_id, state] : explorer_auxiliary_) {
                static_cast<void>(process_id);
                state.error = explorer_.error;
            }
        }

        std::unique_lock lock(mutex_);
        wake_condition_.wait_for(lock, kMonitorInterval, [this, &stop_token] {
            return wake_requested_ || stop_token.stop_requested();
        });
        wake_requested_ = false;
    }
}

void EngineController::Reconcile() {
    auto processes = platform::EnumerateProcesses();
    if (!processes.ok()) {
        std::lock_guard lock(mutex_);
        explorer_.error = processes.status().message();
        start_menu_.error = processes.status().message();
        for (auto& [process_id, state] : explorer_auxiliary_) {
            static_cast<void>(process_id);
            state.error = explorer_.error;
        }
        return;
    }

    std::vector<platform::ProcessInfo> explorer_processes;
    std::vector<platform::ProcessInfo> start_processes;
    for (const auto& process : processes.value()) {
        if (process.session_id != session_id_) {
            continue;
        }
        if (_wcsicmp(process.image_name.c_str(), L"explorer.exe") == 0) {
            explorer_processes.push_back(process);
        } else if (
            _wcsicmp(
                process.image_name.c_str(),
                L"StartMenuExperienceHost.exe") == 0) {
            start_processes.push_back(process);
        }
    }

    ReconcileExplorer(explorer_processes);
    ReconcileStartMenu(start_processes);
}

void EngineController::ReconcileExplorer(
    const std::vector<platform::ProcessInfo>& processes) {
    const auto* selected = SelectExplorerShellProcess(processes);
    HostSettings settings;
    std::uint64_t configuration_generation = 0;
    {
        std::lock_guard lock(mutex_);
        settings = settings_;
        configuration_generation = explorer_configuration_generation_;
    }
    auto clock_prefix = Utf8ToWide(settings.taskbar_clock_prefix);
    auto explorer_title_prefix =
        Utf8ToWide(settings.file_explorer_title_prefix);
    if (!clock_prefix.ok() || !explorer_title_prefix.ok()) {
        std::lock_guard lock(mutex_);
        explorer_.error = clock_prefix.ok()
            ? explorer_title_prefix.status().message()
            : clock_prefix.status().message();
        for (auto& [process_id, state] : explorer_auxiliary_) {
            static_cast<void>(process_id);
            state.error = explorer_.error;
        }
        return;
    }
    const std::uint32_t shell_process_id =
        selected != nullptr ? selected->process_id : 0;
    auto window_process_snapshot = ExplorerWindowProcessIds();
    auto window_process_ids =
        std::move(window_process_snapshot.process_ids);
    if (window_process_snapshot.overflow) {
        // Do not leave a partially covered title adapter active when the
        // bounded discovery snapshot cannot represent every owner process.
        window_process_ids.clear();
    }
    std::erase_if(
        window_process_ids,
        [&processes](const std::uint32_t process_id) {
            return !std::ranges::any_of(
                processes,
                [process_id](const platform::ProcessInfo& process) {
                    return process.process_id == process_id;
                });
        });

    {
        std::lock_guard lock(mutex_);
        file_explorer_process_ids_ = window_process_ids;
        file_explorer_discovery_error_ = window_process_snapshot.overflow
            ? "Too many Explorer window processes to configure safely"
            : "";
        const auto is_current_process = [&processes](
                                            const std::uint32_t process_id) {
            return std::ranges::any_of(
                processes,
                [process_id](const platform::ProcessInfo& process) {
                    return process.process_id == process_id;
                });
        };
        std::erase_if(
            explorer_auxiliary_,
            [shell_process_id, &is_current_process](const auto& entry) {
                return entry.first == shell_process_id ||
                       !is_current_process(entry.first);
            });
        std::erase_if(
            explorer_compatibility_,
            [&is_current_process](const auto& entry) {
                return !is_current_process(entry.first);
            });
    }

    std::uint32_t shell_features = protocol::agent_feature_none;
    CompatibilityState taskbar_compatibility;
    CompatibilityState shell_explorer_compatibility;
    if (settings.taskbar_enabled && shell_process_id != 0) {
        taskbar_compatibility = EvaluateCompatibility(
            compatibility::AdapterId::taskbar_clock,
            shell_process_id);
    }
    // Arm the Explorer adapter before the first CabinetWClass is shown. The
    // hooks are scoped to Explorer windows, so preloading them in every
    // explorer.exe process is inert until that process creates a folder
    // window, and it prevents the stock first frame from flashing.
    if (settings.file_explorer_enabled && shell_process_id != 0) {
        shell_explorer_compatibility = EvaluateCompatibility(
            compatibility::AdapterId::file_explorer_title,
            shell_process_id);
    }
    if (settings.taskbar_enabled && taskbar_compatibility.supported) {
        shell_features |= protocol::agent_feature_taskbar_clock_prefix;
        if (settings.taskbar_background_color_enabled) {
            shell_features |=
                protocol::agent_feature_taskbar_background_color;
        }
        if (settings.taskbar_capsule_enabled) {
            shell_features |= protocol::agent_feature_taskbar_capsule;
        }
    }
    if (settings.file_explorer_enabled &&
        shell_explorer_compatibility.supported) {
        shell_features |=
            protocol::agent_feature_file_explorer_title_prefix;
        if (settings.file_explorer_background_color_enabled) {
            shell_features |=
                protocol::agent_feature_file_explorer_background_color;
            if (settings.file_explorer_custom_scrollbar_enabled) {
                shell_features |=
                    protocol::agent_feature_file_explorer_custom_scrollbar;
            }
        }
    }
    ReconcileProcess(
        ProcessSlot::explorer_shell,
        shell_process_id,
        protocol::AgentTarget::explorer_shell,
        shell_features,
        configuration_generation,
        settings.taskbar_enabled ? clock_prefix.value().c_str() : L"",
        settings.file_explorer_enabled
            ? explorer_title_prefix.value().c_str()
            : L"",
        settings.taskbar_opacity_milli,
        settings.taskbar_hide_notification_center,
        settings.taskbar_hide_control_center,
        settings.taskbar_hide_show_desktop,
        settings.taskbar_capsule_enabled,
        settings.taskbar_background_color_enabled,
        settings.taskbar_background_color,
        settings.file_explorer_background_color_enabled,
        settings.file_explorer_background_color,
        settings.file_explorer_transition_animation,
        protocol::kDefaultStartMenuOpacityMilli,
        false,
        false,
        protocol::kDefaultShellBackgroundColor,
        false,
        false);

    for (const auto& process : processes) {
        if (process.process_id == shell_process_id) {
            continue;
        }
        CompatibilityState compatibility_state;
        if (settings.file_explorer_enabled) {
            compatibility_state = EvaluateCompatibility(
                compatibility::AdapterId::file_explorer_title,
                process.process_id);
        }
        std::uint32_t desired_features =
            settings.file_explorer_enabled && compatibility_state.supported
            ? protocol::agent_feature_file_explorer_title_prefix
            : protocol::agent_feature_none;
        if (desired_features != protocol::agent_feature_none &&
            settings.file_explorer_background_color_enabled) {
            desired_features |=
                protocol::agent_feature_file_explorer_background_color;
            if (settings.file_explorer_custom_scrollbar_enabled) {
                desired_features |=
                    protocol::agent_feature_file_explorer_custom_scrollbar;
            }
        }
        ReconcileProcess(
            ProcessSlot::explorer_auxiliary,
            process.process_id,
            protocol::AgentTarget::explorer_shell,
            desired_features,
            configuration_generation,
            L"",
            settings.file_explorer_enabled
                ? explorer_title_prefix.value().c_str()
                : L"",
            protocol::kDefaultTaskbarOpacityMilli,
            false,
            false,
            false,
            false,
            false,
            protocol::kDefaultShellBackgroundColor,
            settings.file_explorer_background_color_enabled,
            settings.file_explorer_background_color,
            settings.file_explorer_transition_animation,
            protocol::kDefaultStartMenuOpacityMilli,
            false,
            false,
            protocol::kDefaultShellBackgroundColor,
            false,
            false);
    }
}

void EngineController::ReconcileStartMenu(
    const std::vector<platform::ProcessInfo>& processes) {
    const auto* selected = SelectProcess(processes);
    HostSettings settings;
    std::uint64_t configuration_generation = 0;
    {
        std::lock_guard lock(mutex_);
        settings = settings_;
        configuration_generation = start_menu_configuration_generation_;
    }
    std::uint32_t desired_features = settings.start_menu_enabled
        ? protocol::agent_feature_start_menu_root_opacity
        : protocol::agent_feature_none;
    if (settings.start_menu_enabled &&
        settings.start_menu_background_color_enabled) {
        desired_features |= protocol::agent_feature_start_menu_background_color;
    }
    if (settings.start_menu_enabled &&
        settings.start_menu_three_panel_layout_enabled) {
        desired_features |=
            protocol::agent_feature_start_menu_three_panel_layout;
    }
    const std::uint32_t process_id =
        selected != nullptr ? selected->process_id : 0;
    CompatibilityState compatibility_state;
    if (settings.start_menu_enabled && process_id != 0) {
        compatibility_state = EvaluateCompatibility(
            compatibility::AdapterId::start_menu_xaml,
            process_id);
    }
    const std::uint32_t compatible_features =
        compatibility_state.supported
            ? desired_features
            : protocol::agent_feature_none;
    ReconcileProcess(
        ProcessSlot::start_menu,
        process_id,
        protocol::AgentTarget::start_menu,
        compatible_features,
        configuration_generation,
        L"",
        L"",
        protocol::kDefaultTaskbarOpacityMilli,
        false,
        false,
        false,
        false,
        false,
        protocol::kDefaultShellBackgroundColor,
        false,
        protocol::kDefaultShellBackgroundColor,
        protocol::kDefaultExplorerTransition,
        settings.start_menu_opacity_milli,
        settings.start_menu_hide_recommended,
        settings.start_menu_background_color_enabled,
        settings.start_menu_background_color,
        settings.start_menu_three_panel_layout_enabled,
        settings.start_menu_hide_all_apps);
}

EngineController::ProcessGroupState& EngineController::ProcessStateLocked(
    const ProcessSlot slot,
    const std::uint32_t process_id) {
    switch (slot) {
        case ProcessSlot::explorer_shell:
            return explorer_;
        case ProcessSlot::explorer_auxiliary: {
            auto [entry, inserted] = explorer_auxiliary_.try_emplace(process_id);
            if (inserted) {
                entry->second.process_id = process_id;
            }
            return entry->second;
        }
        case ProcessSlot::start_menu:
        default:
            return start_menu_;
    }
}

void EngineController::ReconcileProcess(
    const ProcessSlot slot,
    const std::uint32_t process_id,
    const protocol::AgentTarget target,
    const std::uint32_t desired_features,
    const std::uint64_t desired_generation,
    const wchar_t* clock_prefix,
    const wchar_t* explorer_title_prefix,
    const std::uint32_t taskbar_opacity_milli,
    const bool taskbar_hide_notification_center,
    const bool taskbar_hide_control_center,
    const bool taskbar_hide_show_desktop,
    const bool taskbar_capsule_enabled,
    const bool taskbar_background_color_enabled,
    const std::uint32_t taskbar_background_color,
    const bool file_explorer_background_color_enabled,
    const std::uint32_t file_explorer_background_color,
    const std::uint32_t file_explorer_transition_animation,
    const std::uint32_t start_menu_opacity_milli,
    const bool start_menu_hide_recommended,
    const bool start_menu_background_color_enabled,
    const std::uint32_t start_menu_background_color,
    const bool start_menu_three_panel_layout_enabled,
    const bool start_menu_hide_all_apps) {
    {
        std::lock_guard lock(mutex_);
        auto& state = ProcessStateLocked(slot, process_id);
        if (state.process_id != process_id) {
            if (state.process_id != 0 && state.agent_loaded &&
                state.configured_features != protocol::agent_feature_none) {
                RecordUnexpectedExit(slot == ProcessSlot::explorer_shell);
            }
            state = {};
            state.process_id = process_id;
        }
        state.running = process_id != 0;
        if (!state.running) {
            state.agent_loaded = false;
            state.operation_in_progress = false;
        }
    }
    if (process_id == 0) {
        return;
    }

    auto loaded = platform::IsModuleLoaded(process_id, agent_path_);
    if (!loaded.ok()) {
        std::lock_guard lock(mutex_);
        auto& state = ProcessStateLocked(slot, process_id);
        if (state.process_id == process_id) {
            state.error = loaded.status().message();
            state.operation_in_progress = false;
        }
        return;
    }

    bool needs_configuration = false;
    {
        std::lock_guard lock(mutex_);
        auto& state = ProcessStateLocked(slot, process_id);
        if (state.process_id != process_id) {
            return;
        }
        state.agent_loaded = loaded.value();
        if (desired_features == protocol::agent_feature_none &&
            !state.agent_loaded) {
            state.error.clear();
            state.retry_after = {};
            state.operation_in_progress = false;
            return;
        }
        needs_configuration =
            (!state.agent_loaded || state.configured_process_id != process_id ||
             state.configured_features != desired_features ||
             state.configured_generation != desired_generation) &&
            std::chrono::steady_clock::now() >= state.retry_after &&
            !state.operation_in_progress;
        if (needs_configuration) {
            state.operation_in_progress = true;
            state.error.clear();
        }
    }
    if (!needs_configuration) {
        return;
    }

    protocol::AgentConfiguration configuration;
    configuration.target = target;
    configuration.feature_flags = desired_features;
    configuration.start_menu_opacity_milli = start_menu_opacity_milli;
    configuration.taskbar_opacity_milli = taskbar_opacity_milli;
    configuration.taskbar_hide_notification_center =
        taskbar_hide_notification_center ? 1 : 0;
    configuration.taskbar_hide_control_center =
        taskbar_hide_control_center ? 1 : 0;
    configuration.taskbar_hide_show_desktop =
        taskbar_hide_show_desktop ? 1 : 0;
    configuration.taskbar_capsule_enabled = taskbar_capsule_enabled ? 1 : 0;
    configuration.start_menu_hide_recommended =
        start_menu_hide_recommended ? 1 : 0;
    configuration.taskbar_background_color_enabled =
        taskbar_background_color_enabled ? 1 : 0;
    configuration.taskbar_background_color = taskbar_background_color;
    configuration.file_explorer_background_color_enabled =
        file_explorer_background_color_enabled ? 1 : 0;
    configuration.file_explorer_background_color =
        file_explorer_background_color;
    configuration.file_explorer_transition_animation =
        static_cast<std::uint8_t>(file_explorer_transition_animation);
    configuration.start_menu_background_color_enabled =
        start_menu_background_color_enabled ? 1 : 0;
    configuration.start_menu_background_color = start_menu_background_color;
    configuration.start_menu_three_panel_layout_enabled =
        start_menu_three_panel_layout_enabled ? 1 : 0;
    configuration.start_menu_hide_all_apps =
        start_menu_hide_all_apps ? 1 : 0;
    if (clock_prefix != nullptr) {
        wcsncpy_s(
            configuration.taskbar_clock_prefix,
            clock_prefix,
            _TRUNCATE);
    }
    if (explorer_title_prefix != nullptr) {
        wcsncpy_s(
            configuration.explorer_title_prefix,
            explorer_title_prefix,
            _TRUNCATE);
    }

    auto result = desired_features == protocol::agent_feature_none
                      ? injector_.ConfigureLoaded(
                            process_id,
                            agent_path_,
                            configuration)
                      : injector_.LoadAndConfigure(
                            process_id,
                            agent_path_,
                            configuration);
    bool module_loaded_after_failure = false;
    if (!result.ok()) {
        const auto loaded_after_failure =
            platform::IsModuleLoaded(process_id, agent_path_);
        module_loaded_after_failure =
            loaded_after_failure.ok() && loaded_after_failure.value();
    }
    const std::string failure_message =
        result.ok() ? std::string{} : result.status().message();
    bool write_failure = false;
    {
        std::lock_guard lock(mutex_);
        auto& state = ProcessStateLocked(slot, process_id);
        if (state.process_id != process_id) {
            return;
        }
        state.operation_in_progress = false;
        if (!result.ok()) {
            const auto now = std::chrono::steady_clock::now();
            write_failure =
                state.last_logged_error != failure_message ||
                state.last_error_logged_at ==
                    std::chrono::steady_clock::time_point{} ||
                now - state.last_error_logged_at >=
                    kRepeatedErrorLogInterval;
            if (write_failure) {
                state.last_logged_error = failure_message;
                state.last_error_logged_at = now;
            }
            state.agent_loaded = module_loaded_after_failure;
            state.error = failure_message;
            state.retry_after = now +
                                (module_loaded_after_failure
                                     ? kLoadedAgentRetryDelay
                                     : kRetryDelay);
        } else {
            state.agent_loaded = true;
            state.configured_process_id = process_id;
            state.configured_features = desired_features;
            state.configured_generation = desired_generation;
            state.error.clear();
            state.retry_after = {};
            state.last_logged_error.clear();
            state.last_error_logged_at = {};
        }
    }

    const std::string_view diagnostic_target =
        ProcessTargetName(target, desired_features);
    std::ostringstream context;
    context << "features=0x" << std::hex << std::uppercase
            << desired_features << std::dec
            << ", generation=" << desired_generation;
    if (!result.ok()) {
        if (write_failure) {
            context << ", moduleLoaded="
                    << (module_loaded_after_failure ? "true" : "false")
                    << ", error=" << failure_message;
            diagnostic_log_.Write(
                DiagnosticLevel::error,
                "engine",
                "configure-failed",
                diagnostic_target,
                process_id,
                context.str());
        }
        return;
    }
    context << ", result=success";
    diagnostic_log_.Write(
        DiagnosticLevel::info,
        "engine",
        desired_features == protocol::agent_feature_none
            ? "agent-deactivated"
            : "configure-succeeded",
        diagnostic_target,
        process_id,
        context.str());
}

protocol::TargetSnapshot EngineController::BuildSnapshot(
    const protocol::TargetId target,
    const ProcessGroupState& process) const {
    protocol::TargetSnapshot snapshot;
    snapshot.target = target;
    snapshot.enabled = settings_.Enabled(target);
    snapshot.process_running = process.running;
    snapshot.agent_loaded = process.agent_loaded;
    snapshot.process_id = process.process_id;
    snapshot.detail = process.error;

    const CompatibilityState* compatibility_state = nullptr;
    switch (target) {
        case protocol::TargetId::taskbar:
            compatibility_state = &taskbar_compatibility_;
            break;
        case protocol::TargetId::file_explorer:
            if (const auto found =
                    explorer_compatibility_.find(process.process_id);
                found != explorer_compatibility_.end()) {
                compatibility_state = &found->second;
            }
            break;
        case protocol::TargetId::start_menu:
            compatibility_state = &start_menu_compatibility_;
            break;
        default:
            break;
    }

    if (!snapshot.enabled) {
        snapshot.state = protocol::RuntimeState::disabled;
        const auto& guard_detail = target == protocol::TargetId::start_menu
                                       ? start_menu_guard_detail_
                                       : explorer_guard_detail_;
        if (!guard_detail.empty()) {
            snapshot.detail = guard_detail;
        } else if (snapshot.detail.empty()) {
            snapshot.detail = "Disabled";
        }
    } else if (
        compatibility_state != nullptr && compatibility_state->evaluated &&
        compatibility_state->process_id == process.process_id &&
        process.running &&
        !compatibility_state->supported) {
        snapshot.state = protocol::RuntimeState::incompatible;
        snapshot.detail = compatibility_state->detail;
    } else if (!process.error.empty()) {
        snapshot.state = protocol::RuntimeState::error;
    } else if (!snapshot.process_running) {
        snapshot.state = protocol::RuntimeState::stopped;
        snapshot.detail = "Target process is not running";
    } else if (process.operation_in_progress || !snapshot.agent_loaded) {
        snapshot.state = protocol::RuntimeState::injecting;
        snapshot.detail = "Waiting for agent injection";
    } else {
        snapshot.state = protocol::RuntimeState::active;
        switch (target) {
            case protocol::TargetId::taskbar:
                snapshot.detail =
                    "Clock hook and reversible Taskbar XAML rules are active";
                break;
            case protocol::TargetId::file_explorer:
                snapshot.detail = "Explorer title and surface adapters are active";
                break;
            case protocol::TargetId::start_menu:
                snapshot.detail =
                    "XAML adapter attached; root opacity is " +
                    std::to_string(settings_.start_menu_opacity_milli / 10U) +
                    "%";
                break;
            default:
                snapshot.detail = "Active";
                break;
        }
    }
    return snapshot;
}

protocol::TargetSnapshot EngineController::BuildFileExplorerSnapshot() const {
    protocol::TargetSnapshot snapshot;
    snapshot.target = protocol::TargetId::file_explorer;
    snapshot.enabled = settings_.file_explorer_enabled;

    if (!snapshot.enabled) {
        snapshot.state = protocol::RuntimeState::disabled;
        snapshot.process_running = explorer_.running;
        snapshot.agent_loaded = explorer_.agent_loaded;
        snapshot.process_id = explorer_.process_id;
        snapshot.detail = explorer_guard_detail_.empty()
            ? "Disabled"
            : explorer_guard_detail_;
        return snapshot;
    }
    if (!file_explorer_discovery_error_.empty()) {
        snapshot.state = protocol::RuntimeState::error;
        snapshot.process_running = explorer_.running;
        snapshot.process_id = explorer_.process_id;
        snapshot.detail = file_explorer_discovery_error_;
        return snapshot;
    }
    if (file_explorer_process_ids_.empty()) {
        snapshot.process_running = explorer_.running;
        snapshot.process_id = explorer_.process_id;
        snapshot.state = explorer_.running
            ? protocol::RuntimeState::active
            : protocol::RuntimeState::stopped;
        snapshot.detail = explorer_.running
            ? "Explorer adapter is armed; no folder windows are open"
            : "No Explorer process is running";
        return snapshot;
    }

    bool all_loaded = true;
    bool transition_pending = false;
    bool compatibility_pending = false;
    const CompatibilityState* incompatible = nullptr;
    std::size_t running_count = 0;
    const auto inspect = [&](const ProcessGroupState& process) {
        if (!process.running || process.process_id == 0) {
            return;
        }
        ++running_count;
        snapshot.process_running = true;
        if (snapshot.process_id == 0) {
            snapshot.process_id = process.process_id;
        }
        all_loaded = all_loaded && process.agent_loaded;
        if (snapshot.detail.empty() && !process.error.empty()) {
            snapshot.detail = process.error;
        }
        transition_pending = transition_pending ||
                             process.operation_in_progress ||
                             !process.agent_loaded ||
                             (process.configured_features &
                              protocol::agent_feature_file_explorer_title_prefix) ==
                                 0;
        const auto found = explorer_compatibility_.find(process.process_id);
        if (found == explorer_compatibility_.end() ||
            !found->second.evaluated) {
            compatibility_pending = true;
        } else if (!found->second.supported && incompatible == nullptr) {
            incompatible = &found->second;
        }
    };

    for (const std::uint32_t process_id : file_explorer_process_ids_) {
        if (process_id == explorer_.process_id) {
            inspect(explorer_);
            continue;
        }
        const auto found = explorer_auxiliary_.find(process_id);
        if (found == explorer_auxiliary_.end()) {
            snapshot.process_running = true;
            if (snapshot.process_id == 0) {
                snapshot.process_id = process_id;
            }
            all_loaded = false;
            transition_pending = true;
            compatibility_pending = true;
            ++running_count;
            continue;
        }
        inspect(found->second);
    }
    snapshot.agent_loaded = snapshot.process_running && all_loaded;

    if (incompatible != nullptr) {
        snapshot.state = protocol::RuntimeState::incompatible;
        snapshot.detail = incompatible->detail;
    } else if (!snapshot.detail.empty()) {
        snapshot.state = protocol::RuntimeState::error;
    } else if (!snapshot.process_running) {
        snapshot.state = protocol::RuntimeState::stopped;
        snapshot.detail = "No Explorer process is running";
    } else if (compatibility_pending || transition_pending || !all_loaded) {
        snapshot.state = protocol::RuntimeState::injecting;
        snapshot.detail = "Applying the Explorer adapter to all processes";
    } else {
        snapshot.state = protocol::RuntimeState::active;
        snapshot.detail = "Explorer title and surface adapters are active in " +
                          std::to_string(running_count) +
                          (running_count == 1 ? " process" : " processes");
    }
    return snapshot;
}

EngineController::CompatibilityState EngineController::EvaluateCompatibility(
    const compatibility::AdapterId adapter,
    const std::uint32_t process_id) {
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lock(mutex_);
        const CompatibilityState* cached = nullptr;
        switch (adapter) {
            case compatibility::AdapterId::taskbar_clock:
                cached = &taskbar_compatibility_;
                break;
            case compatibility::AdapterId::file_explorer_title:
                if (const auto found =
                        explorer_compatibility_.find(process_id);
                    found != explorer_compatibility_.end()) {
                    cached = &found->second;
                }
                break;
            case compatibility::AdapterId::start_menu_xaml:
                cached = &start_menu_compatibility_;
                break;
        }
        const auto refresh_interval =
            cached != nullptr && cached->supported
            ? kCompatibilityRefreshInterval
            : kPendingCompatibilityRefreshInterval;
        if (cached != nullptr && cached->evaluated &&
            cached->process_id == process_id &&
            now - cached->checked_at < refresh_interval) {
            return *cached;
        }
    }

    CompatibilityState evaluated;
    evaluated.process_id = process_id;
    evaluated.evaluated = true;
    evaluated.checked_at = now;
    try {
        auto decision = compatibility::EvaluateProcess(adapter, process_id);
        if (!decision.ok()) {
            evaluated.detail =
                "Compatibility inspection failed: " +
                decision.status().message();
        } else {
            evaluated.supported = decision.value().supported;
            evaluated.detail = decision.value().detail;
        }
    } catch (const std::exception& exception) {
        evaluated.detail =
            std::string("Compatibility inspection exception: ") +
            exception.what();
    } catch (...) {
        evaluated.detail = "Unknown compatibility inspection exception";
    }

    {
        std::lock_guard lock(mutex_);
        CompatibilityState* destination = nullptr;
        switch (adapter) {
            case compatibility::AdapterId::taskbar_clock:
                destination = &taskbar_compatibility_;
                break;
            case compatibility::AdapterId::file_explorer_title:
                explorer_compatibility_[process_id] = evaluated;
                break;
            case compatibility::AdapterId::start_menu_xaml:
                destination = &start_menu_compatibility_;
                break;
        }
        if (destination != nullptr) {
            *destination = evaluated;
        }
    }
    return evaluated;
}

void EngineController::RecordUnexpectedExit(
    const bool explorer_group) {
    // Called with mutex_ held.
    auto& crashes = explorer_group ? explorer_crashes_ : start_menu_crashes_;
    const auto now = std::chrono::steady_clock::now();
    crashes.push_back(now);
    while (!crashes.empty() && now - crashes.front() > kCrashWindow) {
        crashes.pop_front();
    }
    if (crashes.size() < kCrashLimit) {
        return;
    }

    HostSettings safe_settings = settings_;
    if (explorer_group) {
        safe_settings.taskbar_enabled = false;
        safe_settings.file_explorer_enabled = false;
    } else {
        safe_settings.start_menu_enabled = false;
    }
    auto save = settings_store_.Save(safe_settings);
    if (!save.ok()) {
        auto& state = explorer_group ? explorer_ : start_menu_;
        state.error = "Crash-loop protection could not persist safe mode: " +
                      save.status().message();
        diagnostic_log_.Write(
            DiagnosticLevel::error,
            "engine",
            "crash-loop-safe-mode-failed",
            explorer_group ? "explorer-shell" : "start-menu",
            state.process_id,
            state.error);
        return;
    }

    settings_ = safe_settings;
    crashes.clear();
    auto& detail =
        explorer_group ? explorer_guard_detail_ : start_menu_guard_detail_;
    detail = "Automatically disabled after three target crashes in 60 seconds";
    ::OutputDebugStringW(
        explorer_group
            ? L"[Metaplasia Host] Explorer crash loop detected; shell features disabled.\n"
            : L"[Metaplasia Host] Start menu crash loop detected; feature disabled.\n");
    diagnostic_log_.Write(
        DiagnosticLevel::error,
        "engine",
        "crash-loop-safe-mode",
        explorer_group ? "explorer-shell" : "start-menu",
        0,
        detail);
}

}  // namespace metaplasia::host
