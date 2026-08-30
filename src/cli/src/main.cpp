#include "metaplasia/base/windows_paths.hpp"
#include "metaplasia/base/utf.hpp"
#include "metaplasia/compatibility/catalog.hpp"
#include "metaplasia/platform/named_pipe.hpp"
#include "metaplasia/platform/process.hpp"
#include "metaplasia/protocol/messages.hpp"
#include "metaplasia/symbols/pe_image.hpp"
#include "metaplasia/symbols/symbol_resolver.hpp"
#include "metaplasia/trust/authenticode.hpp"

#include <Windows.h>

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using metaplasia::protocol::MessageKind;
using metaplasia::protocol::RuntimeState;
using metaplasia::protocol::TargetId;
using metaplasia::protocol::XamlStyleStage;
using metaplasia::protocol::XamlStyleState;

const char* TargetName(const TargetId target) noexcept {
    switch (target) {
        case TargetId::taskbar:
            return "taskbar";
        case TargetId::file_explorer:
            return "file-explorer";
        case TargetId::start_menu:
            return "start-menu";
        default:
            return "unknown";
    }
}

const char* StateName(const RuntimeState state) noexcept {
    switch (state) {
        case RuntimeState::disabled:
            return "disabled";
        case RuntimeState::stopped:
            return "stopped";
        case RuntimeState::running:
            return "running";
        case RuntimeState::injecting:
            return "injecting";
        case RuntimeState::active:
            return "active";
        case RuntimeState::error:
            return "error";
        case RuntimeState::incompatible:
            return "incompatible";
        default:
            return "unknown";
    }
}

const char* XamlStyleStateName(const XamlStyleState state) noexcept {
    switch (state) {
        case XamlStyleState::inactive:
            return "inactive";
        case XamlStyleState::waiting_for_visual_tree:
            return "waiting";
        case XamlStyleState::applying:
            return "applying";
        case XamlStyleState::active:
            return "active";
        case XamlStyleState::failed:
            return "failed";
        default:
            return "unknown";
    }
}

const char* XamlStyleStageName(const XamlStyleStage stage) noexcept {
    switch (stage) {
        case XamlStyleStage::none:
            return "none";
        case XamlStyleStage::observe_visual_tree:
            return "observe-visual-tree";
        case XamlStyleStage::verify_scene_relation:
            return "verify-scene-relation";
        case XamlStyleStage::create_frame_envelope:
            return "create-frame-envelope";
        case XamlStyleStage::create_panel_surface:
            return "create-panel-surface";
        case XamlStyleStage::attach_recommended:
            return "attach-recommended";
        case XamlStyleStage::create_all_apps:
            return "create-all-apps";
        case XamlStyleStage::apply_element_layout:
            return "apply-element-layout";
        case XamlStyleStage::apply_element_style:
            return "apply-element-style";
        case XamlStyleStage::rollback_scene:
            return "rollback-scene";
        default:
            return "unknown";
    }
}

std::optional<TargetId> ParseTarget(const std::wstring_view value) noexcept {
    if (value == L"taskbar") {
        return TargetId::taskbar;
    }
    if (value == L"file-explorer") {
        return TargetId::file_explorer;
    }
    if (value == L"start-menu") {
        return TargetId::start_menu;
    }
    return std::nullopt;
}

void PrintUsage() {
    std::cout
        << "Metaplasia CLI\n\n"
        << "  metaplasia-cli snapshot\n"
        << "  metaplasia-cli settings\n"
        << "  metaplasia-cli xaml-types <taskbar|start-menu>\n"
        << "  metaplasia-cli set taskbar-clock-prefix <text>\n"
        << "  metaplasia-cli set taskbar-opacity <10-100>\n"
        << "  metaplasia-cli set taskbar-color <#RRGGBB>\n"
        << "  metaplasia-cli set taskbar-color-enabled <true|false>\n"
        << "  metaplasia-cli set taskbar-hide-notification-center <true|false>\n"
        << "  metaplasia-cli set taskbar-hide-control-center <true|false>\n"
        << "  metaplasia-cli set taskbar-hide-show-desktop <true|false>\n"
        << "  metaplasia-cli set taskbar-capsule-enabled <true|false>\n"
        << "  metaplasia-cli set explorer-title-prefix <text>\n"
        << "  metaplasia-cli set explorer-color <#RRGGBB>\n"
        << "  metaplasia-cli set explorer-color-enabled <true|false>\n"
        << "  metaplasia-cli set explorer-animation <none|fade|slide|scale>\n"
        << "  metaplasia-cli set explorer-custom-scrollbar-enabled <true|false>\n"
        << "  metaplasia-cli set start-menu-opacity <10-100>\n"
        << "  metaplasia-cli set start-menu-color <#RRGGBB>\n"
        << "  metaplasia-cli set start-menu-color-enabled <true|false>\n"
        << "  metaplasia-cli set start-menu-hide-recommended <true|false>\n"
        << "  metaplasia-cli set start-menu-hide-all-apps <true|false>\n"
        << "  metaplasia-cli set start-menu-three-panel-layout-enabled <true|false>\n"
        << "  metaplasia-cli enable <taskbar|file-explorer|start-menu> --confirm\n"
        << "  metaplasia-cli disable <taskbar|file-explorer|start-menu>\n"
        << "  metaplasia-cli module-info <absolute-module-path>\n"
        << "  metaplasia-cli authenticode-info <absolute-file-path>\n"
        << "  metaplasia-cli component-trust\n"
        << "  metaplasia-cli compatibility-report\n"
        << "  metaplasia-cli resolve-symbol <absolute-module-path> <symbol-name>\n";
}

std::optional<std::uint32_t> ParseOpacityPercent(
    const std::wstring_view value) noexcept {
    if (value.empty()) {
        return std::nullopt;
    }
    std::uint32_t percent = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        const std::uint32_t digit =
            static_cast<std::uint32_t>(character - L'0');
        if (percent > 100U || percent * 10U + digit > 100U) {
            return std::nullopt;
        }
        percent = percent * 10U + digit;
    }
    if (percent < 10U || percent > 100U) {
        return std::nullopt;
    }
    return percent * 10U;
}

std::optional<std::uint32_t> ParseExplorerAnimation(
    const std::wstring_view value) noexcept {
    if (value == L"none") {
        return metaplasia::protocol::kExplorerTransitionNone;
    }
    if (value == L"fade") {
        return metaplasia::protocol::kExplorerTransitionFade;
    }
    if (value == L"slide") {
        return metaplasia::protocol::kExplorerTransitionSlideFade;
    }
    if (value == L"scale") {
        return metaplasia::protocol::kExplorerTransitionScaleFade;
    }
    return std::nullopt;
}

const char* ExplorerAnimationName(const std::uint32_t value) noexcept {
    switch (value) {
        case metaplasia::protocol::kExplorerTransitionNone:
            return "none";
        case metaplasia::protocol::kExplorerTransitionFade:
            return "fade";
        case metaplasia::protocol::kExplorerTransitionSlideFade:
            return "slide";
        case metaplasia::protocol::kExplorerTransitionScaleFade:
            return "scale";
        default:
            return "unknown";
    }
}

std::optional<std::uint32_t> ParseOpaqueColor(
    const std::wstring_view value) noexcept {
    if (value.size() != 7 || value.front() != L'#') {
        return std::nullopt;
    }
    std::uint32_t rgb = 0;
    for (std::size_t index = 1; index < value.size(); ++index) {
        const wchar_t character = value[index];
        std::uint32_t digit = 0;
        if (character >= L'0' && character <= L'9') {
            digit = static_cast<std::uint32_t>(character - L'0');
        } else if (character >= L'a' && character <= L'f') {
            digit = static_cast<std::uint32_t>(character - L'a') + 10U;
        } else if (character >= L'A' && character <= L'F') {
            digit = static_cast<std::uint32_t>(character - L'A') + 10U;
        } else {
            return std::nullopt;
        }
        rgb = (rgb << 4U) | digit;
    }
    return 0xFF000000U | rgb;
}

std::optional<bool> ParseBooleanValue(
    const std::wstring_view value) noexcept {
    if (value == L"true" || value == L"1") {
        return true;
    }
    if (value == L"false" || value == L"0") {
        return false;
    }
    return std::nullopt;
}

constexpr bool AllowUnsignedDevelopment() noexcept {
#if defined(METAPLASIA_DEVELOPMENT_TRUST)
    return true;
#else
    return false;
#endif
}

metaplasia::Result<metaplasia::compatibility::ProfilePackLoadResult>
InitializeExternalProfilePack() {
    auto executable_path = metaplasia::ExecutablePath();
    auto executable_directory = metaplasia::ExecutableDirectory();
    if (!executable_path.ok()) {
        return executable_path.status();
    }
    if (!executable_directory.ok()) {
        return executable_directory.status();
    }
    auto cli_signature =
        metaplasia::trust::VerifyAuthenticode(executable_path.value());
    if (!cli_signature.ok()) {
        return cli_signature.status();
    }
    metaplasia::trust::PublisherThumbprint publisher{};
    if (cli_signature.value().state ==
        metaplasia::trust::SignatureState::trusted) {
        publisher = cli_signature.value().publisher_thumbprint;
    }
    return metaplasia::compatibility::LoadExternalProfilePack(
        executable_directory.value() /
            L"metaplasia-compatibility-pack.dll",
        publisher,
        AllowUnsignedDevelopment());
}

const metaplasia::platform::ProcessInfo* SelectLowestPid(
    const std::vector<metaplasia::platform::ProcessInfo>& processes) noexcept {
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

const metaplasia::platform::ProcessInfo* SelectExplorerShell(
    const std::vector<metaplasia::platform::ProcessInfo>& processes) noexcept {
    const HWND taskbar = ::FindWindowW(L"Shell_TrayWnd", nullptr);
    if (taskbar != nullptr) {
        DWORD taskbar_pid = 0;
        ::GetWindowThreadProcessId(taskbar, &taskbar_pid);
        const auto match = std::find_if(
            processes.begin(),
            processes.end(),
            [taskbar_pid](const auto& process) {
                return process.process_id == taskbar_pid;
            });
        if (match != processes.end()) {
            return &*match;
        }
    }
    return SelectLowestPid(processes);
}

bool PrintCompatibilityDecision(
    const metaplasia::compatibility::AdapterId adapter,
    const std::uint32_t process_id) {
    const auto name = metaplasia::compatibility::AdapterName(adapter);
    if (process_id == 0) {
        std::cout << name << ": target-not-running\n";
        return true;
    }
    auto decision =
        metaplasia::compatibility::EvaluateProcess(adapter, process_id);
    if (!decision.ok()) {
        std::cout << name << ": error, pid=" << process_id
                  << ", detail=" << decision.status().message() << '\n';
        return false;
    }
    std::cout << name << ": "
              << (decision.value().supported ? "supported" : "incompatible")
              << ", windows=" << decision.value().windows.ToString()
              << ", pid=" << process_id << ", profile="
              << (decision.value().profile_id.empty()
                      ? "none"
                      : decision.value().profile_id)
              << ", detail=" << decision.value().detail << '\n';
    for (const auto& module : decision.value().modules) {
        std::wcout << L"  " << module.module_name << L" -> "
                   << module.path.c_str() << L'\n';
        std::cout << "    key=" << module.compatibility_key << '\n';
    }
    return true;
}

int PrintProtocolError(const metaplasia::protocol::Frame& response) {
    if (response.header.kind != MessageKind::error_response) {
        std::cerr << "Unexpected response from host\n";
        return 4;
    }
    auto error =
        metaplasia::protocol::DecodeErrorResponse(response.payload);
    std::cerr << (error.ok() ? error.value().detail : "Malformed error response")
              << '\n';
    return 5;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    if (argc < 2) {
        PrintUsage();
        return 2;
    }

    const std::wstring_view action(argv[1]);
    if (action == L"module-info") {
        if (argc != 3) {
            PrintUsage();
            return 2;
        }
        auto identity = metaplasia::symbols::InspectPeImage(argv[2]);
        if (!identity.ok()) {
            std::cerr << identity.status().message() << '\n';
            return 5;
        }
        std::wcout << L"path: " << identity.value().path.c_str() << L'\n';
        std::cout << "compatibility-key: "
                  << identity.value().CompatibilityKey() << '\n'
                  << "machine: 0x" << std::uppercase << std::hex
                  << identity.value().machine << '\n'
                  << "timestamp: 0x" << std::setw(8) << std::setfill('0')
                  << identity.value().timestamp << '\n'
                  << "image-size: 0x" << std::setw(8)
                  << identity.value().image_size << '\n'
                  << "checksum: 0x" << std::setw(8)
                  << identity.value().checksum << std::dec << '\n';
        if (identity.value().pdb.has_value()) {
            std::cout << "pdb: " << identity.value().pdb->file_name << '\n'
                      << "pdb-key: "
                      << identity.value().pdb->SymbolServerKey() << '\n';
        } else {
            std::cout << "pdb: unavailable\n";
        }
        return 0;
    }

    if (action == L"authenticode-info") {
        if (argc != 3) {
            PrintUsage();
            return 2;
        }
        auto signature = metaplasia::trust::VerifyAuthenticode(argv[2]);
        if (!signature.ok()) {
            std::cerr << signature.status().message() << '\n';
            return 5;
        }
        const char* state = "invalid";
        if (signature.value().state ==
            metaplasia::trust::SignatureState::trusted) {
            state = "trusted";
        } else if (
            signature.value().state ==
            metaplasia::trust::SignatureState::unsigned_file) {
            state = "unsigned";
        }
        std::cout << "authenticode: " << state << ", native=0x"
                  << std::hex << std::uppercase
                  << signature.value().native_status << std::dec
                  << ", publisher-sha256="
                  << metaplasia::trust::HexThumbprint(
                         signature.value().publisher_thumbprint)
                  << ", detail=" << signature.value().detail << '\n';
        if (!signature.value().publisher_name.empty()) {
            std::wcout << L"publisher: "
                       << signature.value().publisher_name << L'\n';
        }
        return 0;
    }

    if (action == L"resolve-symbol") {
        if (argc != 4) {
            PrintUsage();
            return 2;
        }
        auto data_directory = metaplasia::MetaplasiaDataDirectory();
        if (!data_directory.ok()) {
            std::cerr << data_directory.status().message() << '\n';
            return 5;
        }
        metaplasia::symbols::SymbolResolver resolver(
            data_directory.value() / L"symbols");
        auto symbol = resolver.Resolve(argv[2], argv[3]);
        if (!symbol.ok()) {
            std::cerr << symbol.status().message();
            if (symbol.status().native_code() != 0) {
                std::cerr << " (native=" << symbol.status().native_code() << ')';
            }
            std::cerr << '\n';
            return 5;
        }
        std::wcout << L"symbol: " << symbol.value().name << L'\n';
        std::cout << "module-key: "
                  << symbol.value().module.CompatibilityKey() << '\n'
                  << "rva: 0x" << std::uppercase << std::hex
                  << symbol.value().rva << '\n'
                  << "size: 0x" << symbol.value().size << std::dec << '\n'
                  << "code-sha256: "
                  << metaplasia::symbols::HexDigest(
                         symbol.value().code_fingerprint)
                  << '\n';
        return 0;
    }

    if (action == L"compatibility-report") {
        if (argc != 2) {
            PrintUsage();
            return 2;
        }
        auto pack = InitializeExternalProfilePack();
        if (!pack.ok()) {
            std::cerr << "external-pack: rejected, detail="
                      << pack.status().message() << '\n';
            return 5;
        }
        std::cout << "external-pack: "
                  << (pack.value().loaded ? "loaded" : "not-loaded")
                  << ", profiles=" << pack.value().profile_count
                  << ", detail=" << pack.value().detail << '\n';
        auto session = metaplasia::CurrentSessionId();
        auto processes = metaplasia::platform::EnumerateProcesses();
        if (!session.ok() || !processes.ok()) {
            std::cerr << (session.ok() ? processes.status().message()
                                       : session.status().message())
                      << '\n';
            return 5;
        }
        std::vector<metaplasia::platform::ProcessInfo> explorer_processes;
        std::vector<metaplasia::platform::ProcessInfo> start_processes;
        for (const auto& process : processes.value()) {
            if (process.session_id != session.value()) {
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
        const auto* explorer = SelectExplorerShell(explorer_processes);
        const auto* start = SelectLowestPid(start_processes);
        bool report_ok = PrintCompatibilityDecision(
            metaplasia::compatibility::AdapterId::taskbar_clock,
            explorer != nullptr ? explorer->process_id : 0);
        report_ok = PrintCompatibilityDecision(
                        metaplasia::compatibility::AdapterId::file_explorer_title,
                        explorer != nullptr ? explorer->process_id : 0) &&
                    report_ok;
        report_ok = PrintCompatibilityDecision(
                        metaplasia::compatibility::AdapterId::start_menu_xaml,
                        start != nullptr ? start->process_id : 0) &&
                    report_ok;
        return report_ok ? 0 : 5;
    }

    if (action == L"component-trust") {
        if (argc != 2) {
            PrintUsage();
            return 2;
        }
        auto executable_path = metaplasia::ExecutablePath();
        auto executable_directory = metaplasia::ExecutableDirectory();
        if (!executable_path.ok() || !executable_directory.ok()) {
            std::cerr << (executable_path.ok()
                              ? executable_directory.status().message()
                              : executable_path.status().message())
                      << '\n';
            return 5;
        }
        auto component_trust = metaplasia::trust::VerifyComponentFiles(
            {
                {"cli", executable_path.value()},
                {"host", executable_directory.value() /
                             L"metaplasia-host.exe"},
                {"watchdog", executable_directory.value() /
                                 L"metaplasia-watchdog.exe"},
                {"agent", executable_directory.value() /
                              L"metaplasia-agent.dll"},
            },
            AllowUnsignedDevelopment());
        if (!component_trust.ok()) {
            std::cerr << component_trust.status().message() << '\n';
            return 5;
        }
        std::cout << "component-trust: "
                  << (component_trust.value().accepted ? "accepted" : "rejected")
                  << ", mode="
                  << (component_trust.value().development_override
                          ? "debug-unsigned"
                          : "authenticode")
                  << ", publisher="
                  << metaplasia::trust::HexThumbprint(
                         component_trust.value().publisher_thumbprint)
                  << ", detail=" << component_trust.value().detail << '\n';
        return component_trust.value().accepted ? 0 : 6;
    }

    auto pipe_name = metaplasia::platform::HostPipeName();
    if (!pipe_name.ok()) {
        std::cerr << pipe_name.status().message() << '\n';
        return 3;
    }
    metaplasia::platform::NamedPipeClient client(pipe_name.value());

    if (action == L"snapshot") {
        metaplasia::protocol::Frame request;
        request.header.kind = MessageKind::get_snapshot_request;
        request.header.request_id = 1;
        auto response = client.Transact(request, std::chrono::seconds(2));
        if (!response.ok()) {
            std::cerr << response.status().message() << '\n';
            return 3;
        }
        if (response.value().header.kind != MessageKind::snapshot_response) {
            return PrintProtocolError(response.value());
        }
        auto snapshots = metaplasia::protocol::DecodeSnapshotResponse(
            response.value().payload);
        if (!snapshots.ok()) {
            std::cerr << snapshots.status().message() << '\n';
            return 5;
        }
        for (const auto& snapshot : snapshots.value()) {
            std::cout << TargetName(snapshot.target) << ": "
                      << StateName(snapshot.state) << ", enabled="
                      << (snapshot.enabled ? "true" : "false")
                      << ", pid=" << snapshot.process_id << ", agent="
                      << (snapshot.agent_loaded ? "loaded" : "not-loaded")
                      << ", detail=" << snapshot.detail << '\n';
        }
        return 0;
    }

    if (action == L"settings") {
        if (argc != 2) {
            PrintUsage();
            return 2;
        }
        metaplasia::protocol::Frame request;
        request.header.kind = MessageKind::get_settings_request;
        request.header.request_id = 1;
        auto response = client.Transact(request, std::chrono::seconds(2));
        if (!response.ok()) {
            std::cerr << response.status().message() << '\n';
            return 3;
        }
        if (response.value().header.kind != MessageKind::settings_response) {
            return PrintProtocolError(response.value());
        }
        auto settings = metaplasia::protocol::DecodeSettingsResponse(
            response.value().payload);
        if (!settings.ok()) {
            std::cerr << settings.status().message() << '\n';
            return 5;
        }
        std::cout
            << "taskbar-enabled="
            << (settings.value().taskbar_enabled ? "true" : "false")
            << '\n'
            << "taskbar-clock-prefix="
            << settings.value().taskbar_clock_prefix << '\n'
            << "taskbar-opacity-percent="
            << settings.value().taskbar_opacity_milli / 10U << '\n'
            << "taskbar-color-enabled="
            << (settings.value().taskbar_background_color_enabled
                    ? "true"
                    : "false")
            << '\n'
            << "taskbar-color=#" << std::hex << std::uppercase
            << std::setw(6) << std::setfill('0')
            << (settings.value().taskbar_background_color & 0x00FFFFFFU)
            << std::dec << std::nouppercase << std::setfill(' ') << '\n'
            << "taskbar-hide-notification-center="
            << (settings.value().taskbar_hide_notification_center
                    ? "true"
                    : "false")
            << '\n'
            << "taskbar-hide-control-center="
            << (settings.value().taskbar_hide_control_center
                    ? "true"
                    : "false")
            << '\n'
            << "taskbar-hide-show-desktop="
            << (settings.value().taskbar_hide_show_desktop
                    ? "true"
                    : "false")
            << '\n'
            << "taskbar-capsule-enabled="
            << (settings.value().taskbar_capsule_enabled ? "true" : "false")
            << '\n'
            << "file-explorer-enabled="
            << (settings.value().file_explorer_enabled ? "true" : "false")
            << '\n'
            << "file-explorer-title-prefix="
            << settings.value().file_explorer_title_prefix << '\n'
            << "file-explorer-color-enabled="
            << (settings.value().file_explorer_background_color_enabled
                    ? "true"
                    : "false")
            << '\n'
            << "file-explorer-color=#" << std::hex << std::uppercase
            << std::setw(6) << std::setfill('0')
            << (settings.value().file_explorer_background_color & 0x00FFFFFFU)
            << std::dec << std::nouppercase << std::setfill(' ') << '\n'
            << "file-explorer-animation="
            << ExplorerAnimationName(
                   settings.value().file_explorer_transition_animation)
            << '\n'
            << "file-explorer-custom-scrollbar-enabled="
            << (settings.value().file_explorer_custom_scrollbar_enabled
                    ? "true"
                    : "false")
            << '\n'
            << "start-menu-enabled="
            << (settings.value().start_menu_enabled ? "true" : "false")
            << '\n'
            << "start-menu-opacity-percent="
            << settings.value().start_menu_opacity_milli / 10U << '\n'
            << "start-menu-color-enabled="
            << (settings.value().start_menu_background_color_enabled
                    ? "true"
                    : "false")
            << '\n'
            << "start-menu-color=#" << std::hex << std::uppercase
            << std::setw(6) << std::setfill('0')
            << (settings.value().start_menu_background_color & 0x00FFFFFFU)
            << std::dec << std::nouppercase << std::setfill(' ') << '\n'
            << "start-menu-hide-recommended="
            << (settings.value().start_menu_hide_recommended
                    ? "true"
                    : "false")
            << '\n'
            << "start-menu-three-panel-layout-enabled="
            << (settings.value().start_menu_three_panel_layout_enabled
                    ? "true"
                    : "false")
            << '\n'
            << "start-menu-hide-all-apps="
            << (settings.value().start_menu_hide_all_apps ? "true" : "false")
            << '\n';
        return 0;
    }

    if (action == L"xaml-types") {
        if (argc != 3) {
            PrintUsage();
            return 2;
        }
        const auto target = ParseTarget(argv[2]);
        if (!target.has_value()) {
            std::cerr << "Unknown target\n";
            return 2;
        }
        auto payload =
            metaplasia::protocol::EncodeXamlDiagnosticsRequest(*target);
        if (!payload.ok()) {
            std::cerr << payload.status().message() << '\n';
            return 2;
        }
        metaplasia::protocol::Frame request;
        request.header.kind = MessageKind::get_xaml_diagnostics_request;
        request.header.request_id = 1;
        request.payload = std::move(payload).value();
        auto response = client.Transact(request, std::chrono::seconds(5));
        if (!response.ok()) {
            std::cerr << response.status().message() << '\n';
            return 3;
        }
        if (response.value().header.kind !=
            MessageKind::xaml_diagnostics_response) {
            return PrintProtocolError(response.value());
        }
        auto diagnostics =
            metaplasia::protocol::DecodeXamlDiagnosticsResponse(
                response.value().payload);
        if (!diagnostics.ok()) {
            std::cerr << diagnostics.status().message() << '\n';
            return 5;
        }
        std::cout << "target=" << TargetName(diagnostics.value().target)
                  << ", types=" << diagnostics.value().types.size()
                  << ", dropped-types="
                  << diagnostics.value().dropped_type_count
                  << ", elements=" << diagnostics.value().elements.size()
                  << ", dropped-elements="
                  << diagnostics.value().dropped_element_count
                  << ", styled-elements="
                  << diagnostics.value().tracked_element_count
                  << ", style-state="
                  << XamlStyleStateName(diagnostics.value().style_state)
                  << ", style-stage="
                  << XamlStyleStageName(diagnostics.value().style_stage)
                  << ", dependencies=0x" << std::hex << std::uppercase
                  << diagnostics.value().scene_dependencies
                  << ", native=0x" << std::setw(8) << std::setfill('0')
                  << diagnostics.value().last_style_error << std::dec
                  << std::nouppercase << std::setfill(' ')
                  << ", attempts="
                  << diagnostics.value().style_apply_attempt_count
                  << ", successes="
                  << diagnostics.value().style_apply_success_count
                  << ", failures="
                  << diagnostics.value().style_apply_failure_count << '\n';
        for (std::size_t index = 0;
             index < diagnostics.value().types.size();
             ++index) {
            const auto& type = diagnostics.value().types[index];
            std::cout << index << '\t' << type.observation_count << '\t'
                      << type.type_name
                      << '\n';
        }
        for (const auto& element : diagnostics.value().elements) {
            std::cout << "element\t0x" << std::hex << std::uppercase
                      << element.handle << "\tparent=0x"
                      << element.parent_handle << std::dec
                      << "\tindex=" << element.child_index
                      << "\tchildren=" << element.child_count << "\ttype="
                      << diagnostics.value().types[element.type_index].type_name;
            if (!element.name.empty()) {
                std::cout << "\tname=" << element.name;
            }
            std::cout << '\n';
        }
        return 0;
    }

    if (action == L"set") {
        if (argc != 4) {
            PrintUsage();
            return 2;
        }
        const std::wstring_view setting(argv[2]);
        metaplasia::protocol::SetCustomizationRequest command;
        if (setting == L"taskbar-clock-prefix" ||
            setting == L"explorer-title-prefix") {
            auto value = metaplasia::WideToUtf8(argv[3]);
            if (!value.ok()) {
                std::cerr << value.status().message() << '\n';
                return 2;
            }
            command.customization = setting == L"taskbar-clock-prefix"
                ? metaplasia::protocol::CustomizationId::taskbar_clock_prefix
                : metaplasia::protocol::CustomizationId::
                      file_explorer_title_prefix;
            command.text_value = std::move(value).value();
        } else if (setting == L"taskbar-opacity" ||
                   setting == L"start-menu-opacity") {
            const auto opacity = ParseOpacityPercent(argv[3]);
            if (!opacity.has_value()) {
                std::cerr << "Opacity must be an integer from 10 to 100\n";
                return 2;
            }
            command.customization =
                setting == L"taskbar-opacity"
                ? metaplasia::protocol::CustomizationId::
                      taskbar_opacity_milli
                : metaplasia::protocol::CustomizationId::
                      start_menu_opacity_milli;
            command.integer_value = *opacity;
        } else if (setting == L"explorer-animation") {
            const auto animation = ParseExplorerAnimation(argv[3]);
            if (!animation.has_value()) {
                std::cerr << "Animation must be none, fade, slide, or scale\n";
                return 2;
            }
            command.customization = metaplasia::protocol::CustomizationId::
                file_explorer_transition_animation;
            command.integer_value = *animation;
        } else if (setting == L"taskbar-color" ||
                   setting == L"explorer-color" ||
                   setting == L"start-menu-color") {
            const auto color = ParseOpaqueColor(argv[3]);
            if (!color.has_value()) {
                std::cerr << "Color must use the #RRGGBB format\n";
                return 2;
            }
            if (setting == L"taskbar-color") {
                command.customization = metaplasia::protocol::CustomizationId::
                    taskbar_background_color;
            } else if (setting == L"explorer-color") {
                command.customization = metaplasia::protocol::CustomizationId::
                    file_explorer_background_color;
            } else {
                command.customization = metaplasia::protocol::CustomizationId::
                    start_menu_background_color;
            }
            command.integer_value = *color;
        } else if (
            setting == L"start-menu-hide-recommended" ||
            setting == L"taskbar-hide-notification-center" ||
            setting == L"taskbar-hide-control-center" ||
            setting == L"taskbar-hide-show-desktop" ||
            setting == L"taskbar-capsule-enabled" ||
            setting == L"taskbar-color-enabled" ||
            setting == L"explorer-color-enabled" ||
            setting == L"explorer-custom-scrollbar-enabled" ||
            setting == L"start-menu-color-enabled" ||
            setting == L"start-menu-three-panel-layout-enabled" ||
            setting == L"start-menu-hide-all-apps") {
            const auto hidden = ParseBooleanValue(argv[3]);
            if (!hidden.has_value()) {
                std::cerr << "Visibility setting must be true or false\n";
                return 2;
            }
            if (setting == L"start-menu-hide-recommended") {
                command.customization =
                    metaplasia::protocol::CustomizationId::
                        start_menu_hide_recommended;
            } else if (setting == L"taskbar-hide-notification-center") {
                command.customization =
                    metaplasia::protocol::CustomizationId::
                        taskbar_hide_notification_center;
            } else if (setting == L"taskbar-hide-control-center") {
                command.customization =
                    metaplasia::protocol::CustomizationId::
                        taskbar_hide_control_center;
            } else if (setting == L"taskbar-hide-show-desktop") {
                command.customization =
                    metaplasia::protocol::CustomizationId::
                        taskbar_hide_show_desktop;
            } else if (setting == L"taskbar-capsule-enabled") {
                command.customization =
                    metaplasia::protocol::CustomizationId::
                        taskbar_capsule_enabled;
            } else if (setting == L"taskbar-color-enabled") {
                command.customization = metaplasia::protocol::CustomizationId::
                    taskbar_background_color_enabled;
            } else if (setting == L"explorer-color-enabled") {
                command.customization = metaplasia::protocol::CustomizationId::
                    file_explorer_background_color_enabled;
            } else if (setting == L"explorer-custom-scrollbar-enabled") {
                command.customization = metaplasia::protocol::CustomizationId::
                    file_explorer_custom_scrollbar_enabled;
            } else if (
                setting == L"start-menu-three-panel-layout-enabled") {
                command.customization = metaplasia::protocol::CustomizationId::
                    start_menu_three_panel_layout_enabled;
            } else if (setting == L"start-menu-hide-all-apps") {
                command.customization = metaplasia::protocol::CustomizationId::
                    start_menu_hide_all_apps;
            } else {
                command.customization = metaplasia::protocol::CustomizationId::
                    start_menu_background_color_enabled;
            }
            command.boolean_value = *hidden;
        } else {
            std::cerr << "Unknown customization\n";
            return 2;
        }

        auto payload =
            metaplasia::protocol::EncodeSetCustomizationRequest(command);
        if (!payload.ok()) {
            std::cerr << payload.status().message() << '\n';
            return 2;
        }
        metaplasia::protocol::Frame request;
        request.header.kind = MessageKind::set_customization_request;
        request.header.request_id = 1;
        request.payload = std::move(payload).value();
        auto response = client.Transact(request, std::chrono::seconds(2));
        if (!response.ok()) {
            std::cerr << response.status().message() << '\n';
            return 3;
        }
        if (response.value().header.kind != MessageKind::command_response) {
            return PrintProtocolError(response.value());
        }
        auto result = metaplasia::protocol::DecodeCommandResponse(
            response.value().payload);
        if (!result.ok() || !result.value().accepted) {
            std::cerr << (result.ok() ? result.value().detail
                                     : result.status().message())
                      << '\n';
            return 5;
        }
        std::cout << result.value().detail << '\n';
        return 0;
    }

    if (action != L"enable" && action != L"disable") {
        PrintUsage();
        return 2;
    }
    if (argc < 3) {
        PrintUsage();
        return 2;
    }
    auto target = ParseTarget(argv[2]);
    if (!target.has_value()) {
        std::cerr << "Unknown target\n";
        return 2;
    }
    const bool enabled = action == L"enable";
    if (enabled && (argc < 4 || std::wstring_view(argv[3]) != L"--confirm")) {
        std::cerr
            << "Enabling native shell hooks requires the explicit --confirm flag\n";
        return 2;
    }

    auto payload = metaplasia::protocol::EncodeSetEnabledRequest(
        {*target, enabled});
    if (!payload.ok()) {
        std::cerr << payload.status().message() << '\n';
        return 5;
    }
    metaplasia::protocol::Frame request;
    request.header.kind = MessageKind::set_enabled_request;
    request.header.request_id = 1;
    request.payload = std::move(payload).value();
    auto response = client.Transact(request, std::chrono::seconds(2));
    if (!response.ok()) {
        std::cerr << response.status().message() << '\n';
        return 3;
    }
    if (response.value().header.kind != MessageKind::command_response) {
        return PrintProtocolError(response.value());
    }
    auto command =
        metaplasia::protocol::DecodeCommandResponse(response.value().payload);
    if (!command.ok() || !command.value().accepted) {
        std::cerr << (command.ok() ? command.value().detail
                                  : command.status().message())
                  << '\n';
        return 5;
    }
    std::cout << command.value().detail << '\n';
    return 0;
}
