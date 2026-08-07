#pragma once

#include "metaplasia/base/status.hpp"
#include "metaplasia/compatibility/catalog.hpp"
#include "metaplasia/host/diagnostic_log.hpp"
#include "metaplasia/host/settings_store.hpp"
#include "metaplasia/injector/injector.hpp"
#include "metaplasia/platform/process.hpp"
#include "metaplasia/protocol/agent_abi.hpp"
#include "metaplasia/protocol/messages.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace metaplasia::host {

class EngineController final {
public:
    EngineController(
        std::filesystem::path agent_path,
        SettingsStore settings_store,
        HostSettings initial_settings,
        std::uint32_t session_id,
        DiagnosticLog& diagnostic_log,
        injector::AgentTrustPolicy agent_trust_policy);
    ~EngineController();

    EngineController(const EngineController&) = delete;
    EngineController& operator=(const EngineController&) = delete;

    void Start();
    void Stop() noexcept;

    [[nodiscard]] std::vector<protocol::TargetSnapshot> Snapshot() const;
    [[nodiscard]] protocol::CustomizationSettings Settings() const;
    [[nodiscard]] Result<protocol::XamlDiagnosticsResponse> XamlDiagnostics(
        protocol::TargetId target) const;
    [[nodiscard]] Result<void> SetEnabled(
        protocol::TargetId target,
        bool enabled);
    [[nodiscard]] Result<void> SetCustomization(
        const protocol::SetCustomizationRequest& request);

private:
    struct ProcessGroupState final {
        std::uint32_t process_id{0};
        bool running{false};
        bool agent_loaded{false};
        bool operation_in_progress{false};
        std::uint32_t configured_features{0};
        std::uint32_t configured_process_id{0};
        std::uint64_t configured_generation{0};
        std::chrono::steady_clock::time_point retry_after{};
        std::chrono::steady_clock::time_point last_error_logged_at{};
        std::string last_logged_error;
        std::string error;
    };

    struct CompatibilityState final {
        std::uint32_t process_id{0};
        bool evaluated{false};
        bool supported{false};
        std::chrono::steady_clock::time_point checked_at{};
        std::string detail;
    };

    enum class ProcessSlot : std::uint8_t {
        explorer_shell,
        explorer_auxiliary,
        start_menu,
    };

    void MonitorLoop(std::stop_token stop_token) noexcept;
    void ExplorerWindowEventLoop(std::stop_token stop_token) noexcept;
    void RequestWake() noexcept;
    static void CALLBACK ExplorerWindowEventCallback(
        HWINEVENTHOOK hook,
        DWORD event,
        HWND window,
        LONG object_id,
        LONG child_id,
        DWORD event_thread_id,
        DWORD event_time) noexcept;
    void Reconcile();
    void ReconcileExplorer(
        const std::vector<platform::ProcessInfo>& processes);
    void ReconcileStartMenu(
        const std::vector<platform::ProcessInfo>& processes);
    void ReconcileProcess(
        ProcessSlot slot,
        std::uint32_t process_id,
        protocol::AgentTarget target,
        std::uint32_t desired_features,
        std::uint64_t desired_generation,
        const wchar_t* clock_prefix,
        const wchar_t* explorer_title_prefix,
        std::uint32_t taskbar_opacity_milli,
        bool taskbar_hide_notification_center,
        bool taskbar_hide_control_center,
        bool taskbar_hide_show_desktop,
        bool taskbar_capsule_enabled,
        bool taskbar_background_color_enabled,
        std::uint32_t taskbar_background_color,
        bool file_explorer_background_color_enabled,
        std::uint32_t file_explorer_background_color,
        std::uint32_t file_explorer_transition_animation,
        std::uint32_t start_menu_opacity_milli,
        bool start_menu_hide_recommended,
        bool start_menu_background_color_enabled,
        std::uint32_t start_menu_background_color,
        bool start_menu_three_panel_layout_enabled,
        bool start_menu_hide_all_apps);
    void DeactivateLoadedAgents() noexcept;
    [[nodiscard]] CompatibilityState EvaluateCompatibility(
        compatibility::AdapterId adapter,
        std::uint32_t process_id);

    [[nodiscard]] protocol::TargetSnapshot BuildSnapshot(
        protocol::TargetId target,
        const ProcessGroupState& process) const;
    [[nodiscard]] protocol::TargetSnapshot BuildFileExplorerSnapshot() const;
    [[nodiscard]] ProcessGroupState& ProcessStateLocked(
        ProcessSlot slot,
        std::uint32_t process_id);
    void RecordUnexpectedExit(bool explorer_group);

    std::filesystem::path agent_path_;
    SettingsStore settings_store_;
    std::uint32_t session_id_{0};
    DiagnosticLog& diagnostic_log_;

    mutable std::mutex mutex_;
    std::condition_variable wake_condition_;
    HostSettings settings_;
    ProcessGroupState explorer_;
    std::unordered_map<std::uint32_t, ProcessGroupState>
        explorer_auxiliary_;
    std::vector<std::uint32_t> file_explorer_process_ids_;
    std::string file_explorer_discovery_error_;
    ProcessGroupState start_menu_;
    CompatibilityState taskbar_compatibility_;
    std::unordered_map<std::uint32_t, CompatibilityState>
        explorer_compatibility_;
    CompatibilityState start_menu_compatibility_;
    std::deque<std::chrono::steady_clock::time_point> explorer_crashes_;
    std::deque<std::chrono::steady_clock::time_point> start_menu_crashes_;
    std::string explorer_guard_detail_;
    std::string start_menu_guard_detail_;
    std::uint64_t explorer_configuration_generation_{1};
    std::uint64_t start_menu_configuration_generation_{1};
    bool wake_requested_{false};
    std::jthread monitor_thread_;
    std::jthread explorer_window_event_thread_;
    std::atomic<DWORD> explorer_window_event_thread_id_{0};
    injector::Injector injector_;
};

}  // namespace metaplasia::host
