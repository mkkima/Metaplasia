#include "metaplasia/injector/conflict_policy.hpp"
#include "metaplasia/injector/injector.hpp"
#include "metaplasia/injector/remote_image.hpp"
#include "metaplasia/platform/process.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    using metaplasia::injector::ConflictingShellCustomizer;
    using metaplasia::injector::ConflictingShellCustomizerName;
    using metaplasia::injector::IdentifyConflictingShellModule;

    // Exercise the exact host gateway, not the DLL's separate validator. A
    // relative DLL path is a sentinel: accepted configurations must reach path
    // validation, before any process is opened or any agent can be loaded.
    const metaplasia::injector::Injector injector;
    for (const bool allow_load : {false, true}) {
        const auto check = [&](const metaplasia::protocol::AgentConfiguration& configuration) {
            return allow_load
                ? injector.LoadAndConfigure(0, L"not-absolute.dll", configuration)
                : injector.ConfigureLoaded(0, L"not-absolute.dll", configuration);
        };
        const auto reaches_path_validation = [&](const auto& configuration) {
            const auto result = check(configuration);
            return !result.ok() &&
                result.status().code() == metaplasia::ErrorCode::invalid_argument &&
                result.status().message() == "Agent path must be absolute";
        };
        using namespace metaplasia::protocol;
        AgentConfiguration configuration;
        Require(reaches_path_validation(configuration), "host accepts the base Explorer ABI");
        configuration.feature_flags = agent_feature_file_explorer_smooth_scroll;
        Require(reaches_path_validation(configuration),
            "host accepts smooth scrolling without color or custom scrollbars");
        configuration.feature_flags |= agent_feature_taskbar_clock_prefix |
            agent_feature_taskbar_background_color | agent_feature_taskbar_capsule |
            agent_feature_file_explorer_title_prefix |
            agent_feature_file_explorer_background_color |
            agent_feature_file_explorer_custom_scrollbar;
        Require(configuration.feature_flags == 0x2DB && reaches_path_validation(configuration),
            "host accepts the reported combined Taskbar/Explorer configuration 0x2DB");
        configuration.target = AgentTarget::start_menu;
        configuration.feature_flags = agent_feature_start_menu_root_opacity |
            agent_feature_start_menu_background_color;
        Require(reaches_path_validation(configuration), "host keeps Start configuration valid");
        configuration.feature_flags |= agent_feature_file_explorer_smooth_scroll;
        auto rejected = check(configuration);
        Require(!rejected.ok() && rejected.status().message() ==
            "Agent feature flags are not valid for this target",
            "host rejects the Explorer-only feature for Start with a precise reason");
        configuration = AgentConfiguration{};
        configuration.feature_flags = 1U << 31U;
        rejected = check(configuration);
        Require(!rejected.ok() && rejected.status().message() == "Unsupported agent feature flags",
            "unknown feature bits remain rejected, without blaming the ABI version");
        configuration = AgentConfiguration{};
        --configuration.version;
        rejected = check(configuration);
        Require(!rejected.ok() && rejected.status().message() == "Invalid agent configuration ABI",
            "old ABI versions remain rejected");
    }

    Require(
        IdentifyConflictingShellModule(L"windhawk.dll") ==
            ConflictingShellCustomizer::windhawk,
        "detect Windhawk");
    Require(
        IdentifyConflictingShellModule(L"WINDHAWK.DLL") ==
            ConflictingShellCustomizer::windhawk,
        "conflict identity is case-insensitive");
    Require(
        IdentifyConflictingShellModule(L"wincorlib_orig.dll") ==
            ConflictingShellCustomizer::explorer_patcher,
        "detect ExplorerPatcher Start proxy");
    Require(
        IdentifyConflictingShellModule(L"ExplorerPatcher.amd64.dll") ==
            ConflictingShellCustomizer::explorer_patcher,
        "detect ExplorerPatcher shell module");
    Require(
        !IdentifyConflictingShellModule(L"my-windhawk.dll").has_value(),
        "reject substring heuristics");
    Require(
        !IdentifyConflictingShellModule(L"wincorlib.dll").has_value(),
        "do not reject the ordinary Windows module name");
    Require(
        ConflictingShellCustomizerName(ConflictingShellCustomizer::windhawk) ==
            "Windhawk",
        "format customizer name");

    auto modules = metaplasia::platform::EnumerateProcessModules(
        ::GetCurrentProcessId());
    Require(modules.ok(), "enumerate current-process modules");
    Require(!modules.value().empty(), "current process has modules");
    Require(
        std::ranges::all_of(modules.value(), [](const auto& module) {
            return !module.module_name.empty() &&
                   !module.image_path.empty() &&
                   module.image_path.is_absolute() &&
                   module.base_address != 0 && module.image_size != 0;
        }),
        "module snapshot contains identities and absolute paths");

    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    Require(ntdll != nullptr, "locate local ntdll");
    std::array<wchar_t, 32768> ntdll_path_buffer{};
    const DWORD ntdll_path_length = ::GetModuleFileNameW(
        ntdll,
        ntdll_path_buffer.data(),
        static_cast<DWORD>(ntdll_path_buffer.size()));
    Require(
        ntdll_path_length != 0 &&
            ntdll_path_length < ntdll_path_buffer.size(),
        "read ntdll path");
    const std::filesystem::path ntdll_path(std::wstring_view(
        ntdll_path_buffer.data(),
        ntdll_path_length));
    auto remote_ntdll = metaplasia::platform::FindProcessModuleByPath(
        ::GetCurrentProcessId(),
        ntdll_path);
    Require(remote_ntdll.ok(), "find current-process module by exact path");
    auto remote_rtl_get_version =
        metaplasia::injector::ResolveRemoteExportAddress(
            ::GetCurrentProcess(),
            remote_ntdll.value(),
            "RtlGetVersion");
    Require(remote_rtl_get_version.ok(), "resolve mapped-image export");
    Require(
        remote_rtl_get_version.value() ==
            reinterpret_cast<std::uintptr_t>(
                ::GetProcAddress(ntdll, "RtlGetVersion")),
        "mapped-image export address matches the loader");
    Require(
        !metaplasia::injector::ResolveRemoteExportAddress(
             ::GetCurrentProcess(),
             remote_ntdll.value(),
             "MetaplasiaMissingExport")
             .ok(),
        "reject a missing mapped-image export");

    auto current_architecture =
        metaplasia::platform::GetProcessArchitecture(::GetCurrentProcess());
    Require(
        current_architecture.ok() &&
            current_architecture.value() !=
                metaplasia::platform::ProcessArchitecture::unknown,
        "accept the documented current-process pseudo handle");
    Require(
        !metaplasia::platform::GetProcessArchitecture(nullptr).ok(),
        "reject a null process handle");

    std::cout << "Injector conflict-policy tests passed\n";
    return EXIT_SUCCESS;
}
