#include "metaplasia/compatibility/catalog.hpp"

#include "metaplasia/base/unique_handle.hpp"
#include "metaplasia/platform/process.hpp"
#include "metaplasia/symbols/pe_image.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

namespace metaplasia::compatibility {
namespace {

std::mutex g_external_profiles_mutex;
std::vector<CompatibilityProfile> g_external_profiles;
bool g_external_profiles_installed = false;

bool EqualsIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return std::equal(
        left.begin(),
        left.end(),
        right.begin(),
        [](const wchar_t lhs, const wchar_t rhs) {
            return std::towlower(lhs) == std::towlower(rhs);
        });
}

bool PathsEqualIgnoreCase(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept {
    return EqualsIgnoreCase(
        left.lexically_normal().native(),
        right.lexically_normal().native());
}

bool IsSafeRelativeModulePath(
    const std::filesystem::path& path) noexcept {
    if (path.empty() || path.is_absolute() || path.has_root_path()) {
        return false;
    }
    return std::none_of(
        path.begin(),
        path.end(),
        [](const std::filesystem::path& component) {
            return component.empty() || component == L"." ||
                   component == L"..";
        });
}

std::string ModuleNameForDetail(const std::wstring_view name) {
    std::string output;
    output.reserve(name.size());
    for (const wchar_t character : name) {
        output.push_back(
            character >= 0 && character <= 0x7F
                ? static_cast<char>(character)
                : '?');
    }
    return output;
}

Result<std::filesystem::path> WindowsDirectory() {
    std::array<wchar_t, 32768> path{};
    const UINT length = ::GetWindowsDirectoryW(
        path.data(),
        static_cast<UINT>(path.size()));
    if (length == 0 || length >= path.size()) {
        return Status::FromWin32(
            "GetWindowsDirectoryW",
            length == 0 ? ::GetLastError() : ERROR_INSUFFICIENT_BUFFER);
    }
    return std::filesystem::path(std::wstring_view(path.data(), length))
        .lexically_normal();
}

const std::vector<CompatibilityProfile>& CompiledProfiles() {
    // This first profile was captured and live-tested on Windows 11 25H2,
    // 10.0.26200.8875 x64. An update must add a new profile after the complete
    // enable/disable/crash-recovery matrix passes; broad build ranges are
    // intentionally forbidden.
    static const std::vector<CompatibilityProfile> profiles{
        {
            "win11-25h2-26200.8875-x64-taskbar-v1",
            AdapterId::taskbar_clock,
            {10, 0, 26200, 8875},
            {
                {
                    L"explorer.exe",
                    L"explorer.exe",
                    "8664-0BEBF481-00337000-0034690B-explorer.pdb-"
                    "EE2147B759D5A1FC288304091869502A1"},
                {
                    L"user32.dll",
                    L"System32\\user32.dll",
                    "8664-2FAA26CF-001C7000-001DB573-user32.pdb-"
                    "1622FA8BA268948EAB8F7DEE3DF9DF581"},
                {
                    L"Taskbar.dll",
                    L"System32\\Taskbar.dll",
                    "8664-A4EA993A-002FC000-003080F0-Taskbar.pdb-"
                    "E6BFF37F792CE007D4416BBD8DDEAFDA1"},
                {
                    L"Taskbar.View.dll",
                    L"SystemApps\\MicrosoftWindows.Client.Core_"
                    L"cw5n1h2txyewy\\Taskbar.View.dll",
                    "8664-6A29C5C5-00996000-0099C233-Taskbar.View.pdb-"
                    "4A99B7C5BAD94999957572CC66E119781"},
                {
                    L"twinui.pcshell.dll",
                    L"System32\\twinui.pcshell.dll",
                    "8664-15E71BEE-00996000-0099A279-twinui.pcshell.pdb-"
                    "06D692620003180BB9EE4DE2222CB6CF1"},
            }},
        {
            "win11-25h2-26200.8875-x64-explorer-v2",
            AdapterId::file_explorer_title,
            {10, 0, 26200, 8875},
            {
                {
                    L"explorer.exe",
                    L"explorer.exe",
                    "8664-0BEBF481-00337000-0034690B-explorer.pdb-"
                    "EE2147B759D5A1FC288304091869502A1"},
                {
                    L"user32.dll",
                    L"System32\\user32.dll",
                    "8664-2FAA26CF-001C7000-001DB573-user32.pdb-"
                    "1622FA8BA268948EAB8F7DEE3DF9DF581"},
                {
                    L"dwmapi.dll",
                    L"System32\\dwmapi.dll",
                    "8664-A7FC30F6-0002F000-00039A74-dwmapi.pdb-"
                    "F46145A673CC3438FCEA313CFC8664471"},
                {
                    L"Microsoft.Internal.FrameworkUdk.dll",
                    L"SystemApps\\Microsoft.WindowsAppRuntime.CBS_"
                    L"8wekyb3d8bbwe\\Microsoft.Internal.FrameworkUdk.dll",
                    "8664-2802D610-000F1000-000F73CD-"
                    "Microsoft.Internal.FrameworkUdk.pdb-"
                    "B34F54F27057CA2C71EB6F152D7679731"},
            }},
        {
            "win11-25h2-26200.8875-x64-start-xaml-v1",
            AdapterId::start_menu_xaml,
            {10, 0, 26200, 8875},
            {
                {
                    L"StartMenuExperienceHost.exe",
                    L"SystemApps\\Microsoft.Windows.StartMenuExperienceHost_"
                    L"cw5n1h2txyewy\\StartMenuExperienceHost.exe",
                    "8664-EC25B15E-00038000-00043E56-"
                    "startmenuexperiencehost.pdb-"
                    "E3E1D55BC12D6D0E8954F3C321084B1E1"},
                {
                    L"Windows.UI.Xaml.dll",
                    L"System32\\Windows.UI.Xaml.dll",
                    "8664-DE7CC7C7-01109000-01110F88-"
                    "windows.ui.xaml.pdb-"
                    "CE725F5F9540C1A2B4C5E2D97FBFC7651"},
            }},
        {
            "win11-25h2-26200.9168-x64-taskbar-v1",
            AdapterId::taskbar_clock,
            {10, 0, 26200, 9168},
            {
                {
                    L"explorer.exe",
                    L"explorer.exe",
                    "8664-77CB28FB-00334000-0033D30D-explorer.pdb-"
                    "4BF64FA877FDAC2DD1BCD616FA92B2631"},
                {
                    L"user32.dll",
                    L"System32\\user32.dll",
                    "8664-5D257817-001C6000-001D1E86-user32.pdb-"
                    "6C14B92F46F8F9BF4CF059759A9186A81"},
                {
                    L"Taskbar.dll",
                    L"System32\\Taskbar.dll",
                    "8664-AF54E51D-00302000-00305E30-Taskbar.pdb-"
                    "6C3AA9496CD5913CBF5AE8A6F3B506AA1"},
                {
                    L"Taskbar.View.dll",
                    L"SystemApps\\MicrosoftWindows.Client.Core_"
                    L"cw5n1h2txyewy\\Taskbar.View.dll",
                    "8664-6A3CD591-0098B000-00985653-Taskbar.View.pdb-"
                    "9F389A858FC9409ABBC518000E8634901"},
                {
                    L"twinui.pcshell.dll",
                    L"System32\\twinui.pcshell.dll",
                    "8664-1C8AB037-00999000-0099F555-twinui.pcshell.pdb-"
                    "FCFCF4EFC4BB468AC3C12242EB80463A1"},
            }},
        {
            "win11-25h2-26200.9168-x64-explorer-v2",
            AdapterId::file_explorer_title,
            {10, 0, 26200, 9168},
            {
                {
                    L"explorer.exe",
                    L"explorer.exe",
                    "8664-77CB28FB-00334000-0033D30D-explorer.pdb-"
                    "4BF64FA877FDAC2DD1BCD616FA92B2631"},
                {
                    L"user32.dll",
                    L"System32\\user32.dll",
                    "8664-5D257817-001C6000-001D1E86-user32.pdb-"
                    "6C14B92F46F8F9BF4CF059759A9186A81"},
                {
                    L"dwmapi.dll",
                    L"System32\\dwmapi.dll",
                    "8664-919E85B3-0002F000-00031DF5-dwmapi.pdb-"
                    "03913F6D9E7C96E9D1A6E3222247C9B11"},
                {
                    L"Microsoft.Internal.FrameworkUdk.dll",
                    L"SystemApps\\Microsoft.WindowsAppRuntime.CBS_"
                    L"8wekyb3d8bbwe\\Microsoft.Internal.FrameworkUdk.dll",
                    "8664-2802D610-000F1000-000F73CD-"
                    "Microsoft.Internal.FrameworkUdk.pdb-"
                    "B34F54F27057CA2C71EB6F152D7679731"},
            }},
        {
            "win11-25h2-26200.9168-x64-start-xaml-v1",
            AdapterId::start_menu_xaml,
            {10, 0, 26200, 9168},
            {
                {
                    L"StartMenuExperienceHost.exe",
                    L"SystemApps\\Microsoft.Windows.StartMenuExperienceHost_"
                    L"cw5n1h2txyewy\\StartMenuExperienceHost.exe",
                    "8664-B4C805A1-00038000-00043430-"
                    "startmenuexperiencehost.pdb-"
                    "19EE505E5FFD2431C0DC5C9DBBDB6BEA1"},
                {
                    L"Windows.UI.Xaml.dll",
                    L"System32\\Windows.UI.Xaml.dll",
                    "8664-7451281F-0110E000-01118268-"
                    "windows.ui.xaml.pdb-"
                    "B7398E0107885525880FA8C45B816A161"},
            }},
    };
    return profiles;
}

std::vector<ModuleRequirement> DiagnosticRequirements(
    const AdapterId adapter) {
    std::vector<ModuleRequirement> requirements;
    const auto append = [&requirements, adapter](
                            const CompatibilityProfile& profile) {
        if (profile.adapter != adapter) {
            return;
        }
        for (const auto& candidate : profile.modules) {
            const bool already_present = std::ranges::any_of(
                requirements,
                [&candidate](const ModuleRequirement& existing) {
                    return EqualsIgnoreCase(
                        existing.module_name,
                        candidate.module_name);
                });
            if (!already_present) {
                requirements.push_back(candidate);
            }
        }
    };
    for (const auto& profile : CompiledProfiles()) {
        append(profile);
    }
    {
        std::lock_guard lock(g_external_profiles_mutex);
        for (const auto& profile : g_external_profiles) {
            append(profile);
        }
    }
    return requirements;
}

struct ModuleInspection final {
    std::vector<ModuleObservation> observations;
    std::string failure;
};

Result<ModuleInspection> InspectRequiredModules(
    const AdapterId adapter,
    const std::uint32_t process_id,
    const std::span<const ModuleRequirement> requirements) {
    auto modules = platform::EnumerateProcessModules(process_id);
    if (!modules.ok()) {
        return modules.status();
    }
    UniqueHandle process(::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
        FALSE,
        process_id));
    if (!process) {
        return Status::FromWin32(
            "OpenProcess(compatibility inspection)",
            ::GetLastError());
    }

    ModuleInspection inspection;
    for (const auto& requirement : requirements) {
        for (const auto& module : modules.value()) {
            if (!EqualsIgnoreCase(module.module_name, requirement.module_name)) {
                continue;
            }
            auto identity = symbols::InspectMappedPeImage(
                process.get(),
                module.base_address,
                module.image_size,
                module.image_path);
            if (!identity.ok()) {
                inspection.failure =
                    "Unable to inspect mapped module " +
                    ModuleNameForDetail(requirement.module_name) + ": " +
                    identity.status().message();
                return inspection;
            }
            inspection.observations.push_back(ModuleObservation{
                module.module_name,
                module.image_path,
                identity.value().CompatibilityKey()});
        }
    }
    if (inspection.observations.empty()) {
        inspection.failure =
            "No diagnostic modules for adapter " +
            std::string(AdapterName(adapter)) + " are loaded in the target";
    }
    return inspection;
}

Result<std::optional<CompatibilityProfile>> FindProfile(
    const AdapterId adapter,
    const WindowsVersion& windows) {
    std::optional<CompatibilityProfile> found;
    const auto consider = [&found, adapter, &windows](
                              const CompatibilityProfile& profile)
        -> Result<void> {
        if (profile.adapter != adapter || profile.windows != windows) {
            return {};
        }
        if (found.has_value()) {
            return Status(
                ErrorCode::invalid_data,
                "Compatibility catalog contains an ambiguous profile");
        }
        found = profile;
        return {};
    };
    for (const auto& profile : CompiledProfiles()) {
        auto accepted = consider(profile);
        if (!accepted.ok()) {
            return accepted.status();
        }
    }
    {
        std::lock_guard lock(g_external_profiles_mutex);
        for (const auto& profile : g_external_profiles) {
            auto accepted = consider(profile);
            if (!accepted.ok()) {
                return accepted.status();
            }
        }
    }
    return found;
}

CompatibilityDecision Unsupported(
    const AdapterId adapter,
    const WindowsVersion& windows,
    std::string detail) {
    CompatibilityDecision decision;
    decision.adapter = adapter;
    decision.windows = windows;
    decision.detail = std::move(detail);
    return decision;
}

}  // namespace

std::string WindowsVersion::ToString() const {
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(build) + "." + std::to_string(revision);
}

std::string_view AdapterName(const AdapterId adapter) noexcept {
    switch (adapter) {
        case AdapterId::taskbar_clock:
            return "taskbar-clock";
        case AdapterId::file_explorer_title:
            return "file-explorer-title";
        case AdapterId::start_menu_xaml:
            return "start-menu-xaml";
    }
    return "unknown";
}

Result<WindowsVersion> QueryWindowsVersion() {
    using RtlGetVersionFunction = LONG(WINAPI*)(RTL_OSVERSIONINFOW*);
    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    const auto rtl_get_version = ntdll != nullptr
                                     ? reinterpret_cast<RtlGetVersionFunction>(
                                           ::GetProcAddress(ntdll, "RtlGetVersion"))
                                     : nullptr;
    if (rtl_get_version == nullptr) {
        return Status(
            ErrorCode::not_found,
            "RtlGetVersion is unavailable");
    }

    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    const LONG status = rtl_get_version(&version);
    if (status < 0 || version.dwMajorVersion == 0 ||
        version.dwBuildNumber == 0) {
        return Status(
            ErrorCode::win32_error,
            "RtlGetVersion failed",
            static_cast<std::uint32_t>(status));
    }

    DWORD revision = 0;
    DWORD revision_size = sizeof(revision);
    const LSTATUS registry_status = ::RegGetValueW(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
        L"UBR",
        RRF_RT_REG_DWORD,
        nullptr,
        &revision,
        &revision_size);
    if (registry_status != ERROR_SUCCESS || revision_size != sizeof(revision)) {
        return Status::FromWin32(
            "RegGetValueW(CurrentVersion\\UBR)",
            static_cast<std::uint32_t>(registry_status));
    }
    return WindowsVersion{
        version.dwMajorVersion,
        version.dwMinorVersion,
        version.dwBuildNumber,
        revision};
}

CompatibilityDecision EvaluateProfile(
    const CompatibilityProfile& profile,
    const WindowsVersion& windows,
    const std::vector<ModuleObservation>& observations,
    const std::filesystem::path& windows_directory) {
    CompatibilityDecision decision;
    decision.adapter = profile.adapter;
    decision.windows = windows;
    decision.profile_id = profile.id;
    decision.modules = observations;

    if (profile.id.empty() || profile.modules.empty() ||
        windows_directory.empty() || !windows_directory.is_absolute()) {
        decision.detail = "Compatibility profile is invalid";
        return decision;
    }
    if (windows != profile.windows) {
        decision.detail =
            "Profile requires Windows " + profile.windows.ToString();
        return decision;
    }

    for (const auto& requirement : profile.modules) {
        if (requirement.module_name.empty() ||
            !IsSafeRelativeModulePath(
                requirement.path_relative_to_windows) ||
            !EqualsIgnoreCase(
                requirement.path_relative_to_windows.filename().native(),
                requirement.module_name) ||
            requirement.compatibility_key.empty()) {
            decision.detail = "Compatibility profile contains an invalid module";
            return decision;
        }
        std::vector<const ModuleObservation*> matches;
        for (const auto& observation : observations) {
            if (EqualsIgnoreCase(
                    observation.module_name,
                    requirement.module_name)) {
                matches.push_back(&observation);
            }
        }
        if (matches.empty()) {
            decision.detail =
                "Required module is not loaded: " +
                ModuleNameForDetail(requirement.module_name);
            return decision;
        }
        if (matches.size() != 1) {
            decision.detail =
                "Required module identity is ambiguous: " +
                ModuleNameForDetail(requirement.module_name);
            return decision;
        }
        const auto expected_path =
            (windows_directory / requirement.path_relative_to_windows)
                .lexically_normal();
        if (!PathsEqualIgnoreCase(matches.front()->path, expected_path)) {
            decision.detail =
                "Required module has an unexpected path: " +
                ModuleNameForDetail(requirement.module_name);
            return decision;
        }
        if (matches.front()->compatibility_key !=
            requirement.compatibility_key) {
            decision.detail =
                "Required module identity is not approved: " +
                ModuleNameForDetail(requirement.module_name);
            return decision;
        }
    }

    decision.supported = true;
    decision.detail = "Approved by exact compatibility profile " + profile.id;
    return decision;
}

Result<CompatibilityDecision> EvaluateProcess(
    const AdapterId adapter,
    const std::uint32_t process_id) {
    if (process_id == 0) {
        return Status(ErrorCode::invalid_argument, "Invalid compatibility target PID");
    }
    auto windows = QueryWindowsVersion();
    if (!windows.ok()) {
        return windows.status();
    }
    auto selected_profile = FindProfile(adapter, windows.value());
    if (!selected_profile.ok()) {
        return selected_profile.status();
    }
    if (!selected_profile.value().has_value()) {
        auto decision = Unsupported(
            adapter,
            windows.value(),
            "No approved compatibility profile for Windows " +
                windows.value().ToString());
        const auto requirements = DiagnosticRequirements(adapter);
        auto inspection =
            InspectRequiredModules(adapter, process_id, requirements);
        if (!inspection.ok()) {
            decision.detail +=
                "; mapped-module observation failed: " +
                inspection.status().message();
        } else {
            if (!inspection.value().failure.empty()) {
                decision.detail += "; " + inspection.value().failure;
            }
            decision.modules = std::move(inspection).value().observations;
        }
        return decision;
    }
    const CompatibilityProfile profile =
        std::move(selected_profile).value().value();
    auto windows_directory = WindowsDirectory();
    if (!windows_directory.ok()) {
        return windows_directory.status();
    }
    auto inspection =
        InspectRequiredModules(adapter, process_id, profile.modules);
    if (!inspection.ok()) {
        return inspection.status();
    }
    if (!inspection.value().failure.empty()) {
        auto decision = Unsupported(
            adapter,
            windows.value(),
            inspection.value().failure);
        decision.profile_id = profile.id;
        decision.modules = std::move(inspection).value().observations;
        return decision;
    }
    return EvaluateProfile(
        profile,
        windows.value(),
        inspection.value().observations,
        windows_directory.value());
}

Result<void> InstallVerifiedExternalProfiles(
    std::vector<CompatibilityProfile> profiles) {
    if (profiles.empty()) {
        return Status(
            ErrorCode::invalid_argument,
            "External compatibility profile set is empty");
    }
    std::lock_guard lock(g_external_profiles_mutex);
    if (g_external_profiles_installed) {
        return Status(
            ErrorCode::already_exists,
            "External compatibility profiles were already initialized");
    }
    for (const auto& external : profiles) {
        const auto compiled_duplicate = std::find_if(
            CompiledProfiles().begin(),
            CompiledProfiles().end(),
            [&external](const CompatibilityProfile& compiled) {
                return compiled.adapter == external.adapter &&
                       compiled.windows == external.windows;
            });
        if (compiled_duplicate != CompiledProfiles().end()) {
            return Status(
                ErrorCode::already_exists,
                "External profile attempts to override a compiled profile");
        }
    }
    g_external_profiles = std::move(profiles);
    g_external_profiles_installed = true;
    return {};
}

}  // namespace metaplasia::compatibility
