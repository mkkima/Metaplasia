#include "metaplasia/protocol/agent_abi.hpp"
#include "metaplasia/agent/explorer_window_color.hpp"
#include "metaplasia/agent/explorer_winui_adapter.hpp"
#include "metaplasia/agent/start_menu_xaml_adapter.hpp"

#include <Windows.h>
#include <MinHook.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cwchar>
#include <limits>
#include <span>
#include <string_view>

namespace {

using metaplasia::protocol::AgentConfiguration;
using metaplasia::protocol::AgentFeature;
using metaplasia::protocol::AgentResult;
using metaplasia::protocol::AgentTarget;

using GetTimeFormatExFunction = int(WINAPI*)(
    LPCWSTR locale_name,
    DWORD flags,
    const SYSTEMTIME* time,
    LPCWSTR format,
    LPWSTR output,
    int output_character_count);
using SetWindowTextWFunction = BOOL(WINAPI*)(HWND window, LPCWSTR text);

enum class AgentState : std::uint8_t {
    stopped,
    starting,
    running,
    stopping,
};

std::atomic<AgentState> g_state{AgentState::stopped};
std::atomic<std::uint32_t> g_features{0};
SRWLOCK g_configuration_lock = SRWLOCK_INIT;
SRWLOCK g_operation_lock = SRWLOCK_INIT;
std::array<wchar_t, metaplasia::protocol::kMaximumClockPrefixLength + 1>
    g_clock_prefix{};
std::array<
    wchar_t,
    metaplasia::protocol::kMaximumExplorerTitlePrefixLength + 1>
    g_explorer_title_prefix{};
GetTimeFormatExFunction g_original_get_time_format_ex = nullptr;
SetWindowTextWFunction g_original_set_window_text = nullptr;
bool g_time_hook_created = false;
bool g_explorer_title_hook_created = false;
SRWLOCK g_prefixed_windows_lock = SRWLOCK_INIT;
constexpr std::size_t kMaximumTrackedExplorerWindows = 256;
std::array<HWND, kMaximumTrackedExplorerWindows> g_prefixed_windows{};
std::size_t g_prefixed_window_count = 0;
AgentConfiguration g_applied_configuration{};
bool g_has_applied_configuration = false;

void DebugLog(const wchar_t* message) noexcept {
    ::OutputDebugStringW(L"[Metaplasia Agent] ");
    ::OutputDebugStringW(message);
    ::OutputDebugStringW(L"\n");
}

bool EqualsIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    return left.size() == right.size() &&
           _wcsnicmp(left.data(), right.data(), left.size()) == 0;
}

std::wstring_view Basename(const std::wstring_view path) noexcept {
    const auto position = path.find_last_of(L"\\/");
    return position == std::wstring_view::npos ? path : path.substr(position + 1);
}

bool IsExpectedProcess(const AgentTarget target) noexcept {
    std::array<wchar_t, 32768> path{};
    DWORD size = static_cast<DWORD>(path.size());
    if (!::QueryFullProcessImageNameW(
            ::GetCurrentProcess(),
            0,
            path.data(),
            &size)) {
        return false;
    }

    const auto name = Basename(std::wstring_view(path.data(), size));
    switch (target) {
        case AgentTarget::explorer_shell:
            return EqualsIgnoreCase(name, L"explorer.exe");
        case AgentTarget::start_menu:
            return EqualsIgnoreCase(name, L"StartMenuExperienceHost.exe");
        default:
            return false;
    }
}

bool IsValidConfiguration(const AgentConfiguration* configuration) noexcept {
    if (configuration == nullptr ||
        configuration->magic != metaplasia::protocol::kAgentConfigMagic ||
        configuration->version != metaplasia::protocol::kAgentAbiVersion ||
        configuration->size != sizeof(AgentConfiguration) ||
        configuration->reserved != 0 ||
        configuration->taskbar_hide_notification_center > 1 ||
        configuration->taskbar_hide_control_center > 1 ||
        configuration->taskbar_hide_show_desktop > 1 ||
        configuration->taskbar_capsule_enabled > 1 ||
        configuration->file_explorer_transition_animation >
            metaplasia::protocol::kMaximumExplorerTransition ||
        configuration->start_menu_hide_recommended > 1 ||
        configuration->taskbar_background_color_enabled > 1 ||
        configuration->file_explorer_background_color_enabled > 1 ||
        configuration->start_menu_background_color_enabled > 1) {
        return false;
    }
    if (configuration->target != AgentTarget::explorer_shell &&
        configuration->target != AgentTarget::start_menu) {
        return false;
    }
    constexpr std::uint32_t known_features =
        AgentFeature::agent_feature_taskbar_clock_prefix |
        AgentFeature::agent_feature_file_explorer_title_prefix |
        AgentFeature::agent_feature_start_menu_root_opacity |
        AgentFeature::agent_feature_taskbar_background_color |
        AgentFeature::agent_feature_file_explorer_background_color |
        AgentFeature::agent_feature_start_menu_background_color |
        AgentFeature::agent_feature_file_explorer_custom_scrollbar |
        AgentFeature::agent_feature_taskbar_capsule;
    if ((configuration->feature_flags & ~known_features) != 0) {
        return false;
    }
    if (configuration->target == AgentTarget::explorer_shell &&
        (configuration->feature_flags &
         (AgentFeature::agent_feature_start_menu_root_opacity |
          AgentFeature::agent_feature_start_menu_background_color)) != 0) {
        return false;
    }
    if (configuration->target == AgentTarget::start_menu &&
        (configuration->feature_flags &
          (AgentFeature::agent_feature_taskbar_clock_prefix |
           AgentFeature::agent_feature_file_explorer_title_prefix |
           AgentFeature::agent_feature_taskbar_capsule |
           AgentFeature::agent_feature_taskbar_background_color |
          AgentFeature::agent_feature_file_explorer_background_color |
          AgentFeature::agent_feature_file_explorer_custom_scrollbar)) != 0) {
        return false;
    }
    if ((configuration->feature_flags &
         AgentFeature::agent_feature_file_explorer_custom_scrollbar) != 0 &&
        (configuration->feature_flags &
         AgentFeature::agent_feature_file_explorer_background_color) == 0) {
        return false;
    }
    if (configuration->taskbar_opacity_milli <
            metaplasia::protocol::kMinimumTaskbarOpacityMilli ||
        configuration->taskbar_opacity_milli >
            metaplasia::protocol::kMaximumTaskbarOpacityMilli ||
        configuration->start_menu_opacity_milli <
            metaplasia::protocol::kMinimumStartMenuOpacityMilli ||
        configuration->start_menu_opacity_milli >
            metaplasia::protocol::kMaximumStartMenuOpacityMilli ||
        (configuration->taskbar_background_color & 0xFF000000U) !=
            0xFF000000U ||
        (configuration->file_explorer_background_color & 0xFF000000U) !=
            0xFF000000U ||
        (configuration->start_menu_background_color & 0xFF000000U) !=
            0xFF000000U) {
        return false;
    }
    return std::find(
               std::begin(configuration->taskbar_clock_prefix),
               std::end(configuration->taskbar_clock_prefix),
               L'\0') != std::end(configuration->taskbar_clock_prefix) &&
           std::find(
               std::begin(configuration->explorer_title_prefix),
               std::end(configuration->explorer_title_prefix),
               L'\0') != std::end(configuration->explorer_title_prefix);
}

metaplasia::agent::ShellXamlSettings ShellXamlSettingsFrom(
    const AgentConfiguration& configuration) noexcept {
    return metaplasia::agent::ShellXamlSettings{
        configuration.taskbar_opacity_milli,
        configuration.taskbar_hide_notification_center != 0,
        configuration.taskbar_hide_control_center != 0,
        configuration.taskbar_hide_show_desktop != 0,
        configuration.taskbar_capsule_enabled != 0,
        configuration.taskbar_background_color_enabled != 0,
        configuration.taskbar_background_color,
        configuration.start_menu_opacity_milli,
        configuration.start_menu_hide_recommended != 0,
        configuration.start_menu_background_color_enabled != 0,
        configuration.start_menu_background_color};
}

bool IsTaskbarCaller(const void* return_address) noexcept {
    HMODULE module = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(return_address),
            &module)) {
        return false;
    }

    std::array<wchar_t, 32768> path{};
    const DWORD length = ::GetModuleFileNameW(
        module,
        path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return false;
    }

    const auto name = Basename(std::wstring_view(path.data(), length));
    return EqualsIgnoreCase(name, L"taskbar.dll") ||
           EqualsIgnoreCase(name, L"Taskbar.View.dll") ||
           EqualsIgnoreCase(name, L"twinui.pcshell.dll");
}

bool IsExplorerWindow(const HWND window) noexcept {
    if (window == nullptr || !::IsWindow(window)) {
        return false;
    }
    DWORD process_id = 0;
    ::GetWindowThreadProcessId(window, &process_id);
    if (process_id != ::GetCurrentProcessId()) {
        return false;
    }
    std::array<wchar_t, 64> class_name{};
    const int length = ::GetClassNameW(
        window,
        class_name.data(),
        static_cast<int>(class_name.size()));
    if (length <= 0 || static_cast<std::size_t>(length) >= class_name.size()) {
        return false;
    }
    const std::wstring_view name(class_name.data(), static_cast<std::size_t>(length));
    return EqualsIgnoreCase(name, L"CabinetWClass") ||
           EqualsIgnoreCase(name, L"ExploreWClass");
}

bool ReservePrefixedWindow(
    const HWND window,
    bool& inserted) noexcept {
    inserted = false;
    ::AcquireSRWLockExclusive(&g_prefixed_windows_lock);
    const auto existing = std::find(
        g_prefixed_windows.begin(),
        g_prefixed_windows.begin() + g_prefixed_window_count,
        window);
    if (existing != g_prefixed_windows.begin() + g_prefixed_window_count) {
        ::ReleaseSRWLockExclusive(&g_prefixed_windows_lock);
        return true;
    }
    if (g_prefixed_window_count >= g_prefixed_windows.size()) {
        ::ReleaseSRWLockExclusive(&g_prefixed_windows_lock);
        return false;
    }
    g_prefixed_windows[g_prefixed_window_count++] = window;
    inserted = true;
    ::ReleaseSRWLockExclusive(&g_prefixed_windows_lock);
    return true;
}

void ForgetPrefixedWindow(const HWND window) noexcept {
    ::AcquireSRWLockExclusive(&g_prefixed_windows_lock);
    const auto end = g_prefixed_windows.begin() + g_prefixed_window_count;
    const auto found = std::find(g_prefixed_windows.begin(), end, window);
    if (found != end) {
        *found = *(end - 1);
        --g_prefixed_window_count;
        g_prefixed_windows[g_prefixed_window_count] = nullptr;
    }
    ::ReleaseSRWLockExclusive(&g_prefixed_windows_lock);
}

BOOL WINAPI HookedSetWindowTextW(const HWND window, const LPCWSTR text) noexcept {
    const auto original = g_original_set_window_text;
    if (original == nullptr) {
        return FALSE;
    }
    const bool explorer_window = IsExplorerWindow(window);
    const auto finish = [window, explorer_window](const BOOL result) noexcept {
        // Explorer changes its title as the final part of navigation. Apply
        // the color after SetWindowTextW returns: doing it before the call lets
        // Explorer's remaining layout work restore stock brushes for a frame.
        if (result && explorer_window) {
            metaplasia::agent::ObserveExplorerWindowForColor(window);
        }
        return result;
    };
    if ((g_features.load(std::memory_order_acquire) &
         AgentFeature::agent_feature_file_explorer_title_prefix) == 0 ||
        !explorer_window) {
        return finish(original(window, text));
    }
    if (text == nullptr) {
        ForgetPrefixedWindow(window);
        return finish(original(window, nullptr));
    }

    std::array<
        wchar_t,
        metaplasia::protocol::kMaximumExplorerTitlePrefixLength + 1>
        prefix{};
    ::AcquireSRWLockShared(&g_configuration_lock);
    prefix = g_explorer_title_prefix;
    ::ReleaseSRWLockShared(&g_configuration_lock);
    const std::size_t prefix_length = std::wcslen(prefix.data());
    if (prefix_length == 0) {
        return finish(original(window, text));
    }

    constexpr std::size_t kMaximumTitleLength = 4095;
    const std::size_t title_length =
        wcsnlen_s(text, kMaximumTitleLength + 1);
    if (title_length > kMaximumTitleLength) {
        ForgetPrefixedWindow(window);
        return finish(original(window, text));
    }
    if (title_length >= prefix_length &&
        std::equal(prefix.begin(), prefix.begin() + prefix_length, text)) {
        // This call came from outside the hook: the caller now owns the
        // already-prefixed title, so Metaplasia must not strip it on disable.
        ForgetPrefixedWindow(window);
        return finish(original(window, text));
    }
    bool inserted = false;
    if (prefix_length + title_length > kMaximumTitleLength) {
        ForgetPrefixedWindow(window);
        return finish(original(window, text));
    }
    if (!ReservePrefixedWindow(window, inserted)) {
        return finish(original(window, text));
    }

    std::array<wchar_t, kMaximumTitleLength + 1> transformed{};
    std::copy_n(prefix.data(), prefix_length, transformed.data());
    std::copy_n(
        text,
        title_length + 1,
        transformed.data() + prefix_length);
    const BOOL result = original(window, transformed.data());
    if (!result && inserted) {
        ForgetPrefixedWindow(window);
    }
    return finish(result);
}

BOOL CALLBACK PrefixExistingExplorerWindow(
    const HWND window,
    LPARAM) noexcept {
    if (!IsExplorerWindow(window)) {
        return TRUE;
    }
    std::array<wchar_t, 4096> title{};
    const int length = ::GetWindowTextW(
        window,
        title.data(),
        static_cast<int>(title.size()));
    if (length > 0 && static_cast<std::size_t>(length) < title.size()) {
        HookedSetWindowTextW(window, title.data());
    }
    return TRUE;
}

void PrefixExistingExplorerWindows() noexcept {
    ::EnumWindows(&PrefixExistingExplorerWindow, 0);
}

bool RestoreTrackedExplorerWindows(
    const std::span<const wchar_t> prefix) noexcept {
    std::array<HWND, kMaximumTrackedExplorerWindows> windows{};
    std::size_t count = 0;
    ::AcquireSRWLockExclusive(&g_prefixed_windows_lock);
    count = g_prefixed_window_count;
    std::copy_n(g_prefixed_windows.begin(), count, windows.begin());
    g_prefixed_windows.fill(nullptr);
    g_prefixed_window_count = 0;
    ::ReleaseSRWLockExclusive(&g_prefixed_windows_lock);

    const auto original = g_original_set_window_text;
    if (original == nullptr || prefix.empty() || prefix.front() == L'\0') {
        return count == 0;
    }
    bool restored = true;
    const std::size_t prefix_length = wcsnlen_s(
        prefix.data(),
        prefix.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (!IsExplorerWindow(windows[index])) {
            continue;
        }
        std::array<wchar_t, 4096> title{};
        ::SetLastError(ERROR_SUCCESS);
        const int length = ::GetWindowTextW(
            windows[index],
            title.data(),
            static_cast<int>(title.size()));
        const DWORD read_error = ::GetLastError();
        if (length == 0 && read_error != ERROR_SUCCESS) {
            bool inserted = false;
            static_cast<void>(ReservePrefixedWindow(windows[index], inserted));
            restored = false;
            continue;
        }
        if (static_cast<std::size_t>(length) < prefix_length ||
            !std::equal(
                prefix.begin(),
                prefix.begin() + prefix_length,
                title.begin())) {
            continue;
        }
        if (!original(windows[index], title.data() + prefix_length)) {
            bool inserted = false;
            static_cast<void>(ReservePrefixedWindow(windows[index], inserted));
            restored = false;
        }
    }
    return restored;
}

int WINAPI HookedGetTimeFormatEx(
    LPCWSTR locale_name,
    const DWORD flags,
    const SYSTEMTIME* time,
    LPCWSTR format,
    LPWSTR output,
    const int output_character_count) noexcept {
    const auto original = g_original_get_time_format_ex;
    if (original == nullptr ||
        (g_features.load(std::memory_order_acquire) &
         AgentFeature::agent_feature_taskbar_clock_prefix) == 0 ||
        !IsTaskbarCaller(_ReturnAddress())) {
        return original != nullptr
                   ? original(
                         locale_name,
                         flags,
                         time,
                         format,
                         output,
                         output_character_count)
                   : 0;
    }

    std::array<wchar_t, metaplasia::protocol::kMaximumClockPrefixLength + 1>
        prefix{};
    ::AcquireSRWLockShared(&g_configuration_lock);
    prefix = g_clock_prefix;
    ::ReleaseSRWLockShared(&g_configuration_lock);
    const std::size_t prefix_length = std::wcslen(prefix.data());
    if (prefix_length == 0) {
        return original(
            locale_name,
            flags,
            time,
            format,
            output,
            output_character_count);
    }

    const int original_required =
        original(locale_name, flags, time, format, nullptr, 0);
    if (original_required <= 0) {
        return original_required;
    }
    if (prefix_length >
        static_cast<std::size_t>((std::numeric_limits<int>::max)() -
                                 original_required)) {
        ::SetLastError(ERROR_ARITHMETIC_OVERFLOW);
        return 0;
    }
    const int total_required =
        static_cast<int>(prefix_length) + original_required;
    if (output_character_count == 0) {
        return total_required;
    }
    if (output == nullptr || output_character_count < total_required) {
        ::SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return 0;
    }

    std::copy_n(prefix.data(), prefix_length, output);
    const int written = original(
        locale_name,
        flags,
        time,
        format,
        output + prefix_length,
        output_character_count - static_cast<int>(prefix_length));
    if (written == 0) {
        output[0] = L'\0';
        return 0;
    }
    return static_cast<int>(prefix_length) + written;
}

AgentResult EnsureTimeHook(const bool required) noexcept {
    if (required && !g_time_hook_created) {
        const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        if (kernel32 == nullptr) {
            return AgentResult::hook_failed;
        }
        const FARPROC target = ::GetProcAddress(kernel32, "GetTimeFormatEx");
        if (target == nullptr) {
            return AgentResult::hook_failed;
        }
        const MH_STATUS create_status = ::MH_CreateHook(
            reinterpret_cast<LPVOID>(target),
            reinterpret_cast<LPVOID>(&HookedGetTimeFormatEx),
            reinterpret_cast<LPVOID*>(&g_original_get_time_format_ex));
        if (create_status != MH_OK && create_status != MH_ERROR_ALREADY_CREATED) {
            return AgentResult::hook_failed;
        }
        const MH_STATUS enable_status = ::MH_EnableHook(
            reinterpret_cast<LPVOID>(target));
        if (enable_status != MH_OK && enable_status != MH_ERROR_ENABLED) {
            ::MH_RemoveHook(reinterpret_cast<LPVOID>(target));
            g_original_get_time_format_ex = nullptr;
            return AgentResult::hook_failed;
        }
        g_time_hook_created = true;
        DebugLog(L"Taskbar time hook enabled");
    } else if (!required && g_time_hook_created) {
        const HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        const FARPROC target = kernel32 != nullptr
                                   ? ::GetProcAddress(kernel32, "GetTimeFormatEx")
                                   : nullptr;
        if (target == nullptr) {
            return AgentResult::hook_failed;
        }
        const MH_STATUS disable_status =
            ::MH_DisableHook(reinterpret_cast<LPVOID>(target));
        if (disable_status != MH_OK &&
            disable_status != MH_ERROR_DISABLED) {
            return AgentResult::hook_failed;
        }
        const MH_STATUS remove_status =
            ::MH_RemoveHook(reinterpret_cast<LPVOID>(target));
        if (remove_status != MH_OK &&
            remove_status != MH_ERROR_NOT_CREATED) {
            return AgentResult::hook_failed;
        }
        g_time_hook_created = false;
        g_original_get_time_format_ex = nullptr;
        DebugLog(L"Taskbar time hook disabled");
    }
    return AgentResult::success;
}

AgentResult EnsureExplorerTitleHook(const bool required) noexcept {
    if (required && !g_explorer_title_hook_created) {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        if (user32 == nullptr) {
            return AgentResult::hook_failed;
        }
        const FARPROC target = ::GetProcAddress(user32, "SetWindowTextW");
        if (target == nullptr) {
            return AgentResult::hook_failed;
        }
        const MH_STATUS create_status = ::MH_CreateHook(
            reinterpret_cast<LPVOID>(target),
            reinterpret_cast<LPVOID>(&HookedSetWindowTextW),
            reinterpret_cast<LPVOID*>(&g_original_set_window_text));
        if (create_status != MH_OK && create_status != MH_ERROR_ALREADY_CREATED) {
            return AgentResult::hook_failed;
        }
        const MH_STATUS enable_status =
            ::MH_EnableHook(reinterpret_cast<LPVOID>(target));
        if (enable_status != MH_OK && enable_status != MH_ERROR_ENABLED) {
            ::MH_RemoveHook(reinterpret_cast<LPVOID>(target));
            g_original_set_window_text = nullptr;
            return AgentResult::hook_failed;
        }
        g_explorer_title_hook_created = true;
        DebugLog(L"Explorer window-title hook enabled");
    } else if (!required && g_explorer_title_hook_created) {
        const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        const FARPROC target = user32 != nullptr
                                   ? ::GetProcAddress(user32, "SetWindowTextW")
                                   : nullptr;
        if (target == nullptr) {
            return AgentResult::hook_failed;
        }
        const MH_STATUS disable_status =
            ::MH_DisableHook(reinterpret_cast<LPVOID>(target));
        if (disable_status != MH_OK &&
            disable_status != MH_ERROR_DISABLED) {
            return AgentResult::hook_failed;
        }
        const MH_STATUS remove_status =
            ::MH_RemoveHook(reinterpret_cast<LPVOID>(target));
        if (remove_status != MH_OK &&
            remove_status != MH_ERROR_NOT_CREATED) {
            return AgentResult::hook_failed;
        }
        g_explorer_title_hook_created = false;
        g_original_set_window_text = nullptr;
        DebugLog(L"Explorer window-title hook disabled");
    }
    return AgentResult::success;
}

AgentResult ApplyConfiguration(const AgentConfiguration& configuration) noexcept {
    if (configuration.target == AgentTarget::start_menu) {
        const bool start_menu_opacity_required =
            (configuration.feature_flags &
             AgentFeature::agent_feature_start_menu_root_opacity) != 0;
        const bool start_menu_xaml_required =
            start_menu_opacity_required ||
            (configuration.feature_flags &
             AgentFeature::agent_feature_start_menu_background_color) != 0;
        const AgentResult result = metaplasia::agent::ConfigureShellXaml(
            configuration.target,
            start_menu_xaml_required,
            ShellXamlSettingsFrom(configuration));
        if (result == AgentResult::success) {
            g_applied_configuration = configuration;
            g_has_applied_configuration = true;
            g_features.store(
                configuration.feature_flags,
                std::memory_order_release);
        }
        return result;
    }

    const std::uint32_t old_features =
        g_features.load(std::memory_order_acquire);
    AgentConfiguration old_configuration;
    if (g_has_applied_configuration) {
        old_configuration = g_applied_configuration;
    }
    old_configuration.target = configuration.target;
    const bool old_clock_hook_required =
        (old_features & AgentFeature::agent_feature_taskbar_clock_prefix) != 0;
    const bool clock_hook_required =
        (configuration.feature_flags &
         AgentFeature::agent_feature_taskbar_clock_prefix) != 0;
    const bool old_explorer_hook_required =
        (old_features &
         AgentFeature::agent_feature_file_explorer_title_prefix) != 0;
    const bool explorer_hook_required =
        (configuration.feature_flags &
         AgentFeature::agent_feature_file_explorer_title_prefix) != 0;
    const bool old_explorer_color_required =
        (old_features &
         AgentFeature::agent_feature_file_explorer_background_color) != 0;
    const bool explorer_color_required =
        (configuration.feature_flags &
         AgentFeature::agent_feature_file_explorer_background_color) != 0;
    const bool old_explorer_scrollbar_required =
        (old_features &
         AgentFeature::agent_feature_file_explorer_custom_scrollbar) != 0;
    const bool explorer_scrollbar_required =
        (configuration.feature_flags &
         AgentFeature::agent_feature_file_explorer_custom_scrollbar) != 0;
    std::array<
        wchar_t,
        metaplasia::protocol::kMaximumExplorerTitlePrefixLength + 1>
        old_explorer_prefix{};
    ::AcquireSRWLockShared(&g_configuration_lock);
    old_explorer_prefix = g_explorer_title_prefix;
    ::ReleaseSRWLockShared(&g_configuration_lock);
    const bool explorer_prefix_changed =
        !std::equal(
            old_explorer_prefix.begin(),
            old_explorer_prefix.end(),
            std::begin(configuration.explorer_title_prefix));
    const bool titles_restored =
        old_explorer_hook_required &&
        (!explorer_hook_required || explorer_prefix_changed);
    if (titles_restored) {
        g_features.store(
            old_features &
                ~static_cast<std::uint32_t>(
                    AgentFeature::agent_feature_file_explorer_title_prefix),
            std::memory_order_release);
        if (!RestoreTrackedExplorerWindows(old_explorer_prefix)) {
            g_features.store(old_features, std::memory_order_release);
            PrefixExistingExplorerWindows();
            return AgentResult::hook_failed;
        }
    }

    const AgentResult time_result = EnsureTimeHook(clock_hook_required);
    if (time_result != AgentResult::success) {
        if (titles_restored) {
            g_features.store(old_features, std::memory_order_release);
            PrefixExistingExplorerWindows();
        }
        return time_result;
    }
    const AgentResult explorer_result =
        EnsureExplorerTitleHook(explorer_hook_required);
    if (explorer_result != AgentResult::success) {
        static_cast<void>(EnsureTimeHook(old_clock_hook_required));
        if (titles_restored) {
            g_features.store(old_features, std::memory_order_release);
            PrefixExistingExplorerWindows();
        }
        return explorer_result;
    }

    const AgentResult explorer_color_result =
        metaplasia::agent::ConfigureExplorerWindowColor(
            explorer_color_required,
            configuration.file_explorer_background_color,
            configuration.file_explorer_transition_animation,
            explorer_scrollbar_required);
    if (explorer_color_result != AgentResult::success) {
        static_cast<void>(EnsureExplorerTitleHook(old_explorer_hook_required));
        static_cast<void>(EnsureTimeHook(old_clock_hook_required));
        static_cast<void>(metaplasia::agent::ConfigureExplorerWindowColor(
            old_explorer_color_required,
            old_configuration.file_explorer_background_color,
            old_configuration.file_explorer_transition_animation,
            old_explorer_scrollbar_required));
        if (titles_restored) {
            g_features.store(old_features, std::memory_order_release);
            PrefixExistingExplorerWindows();
        }
        return explorer_color_result;
    }

    // Apply Taskbar XAML only after both hooks have reached the new state. If
    // the adapter rejects the transition, return every component to the last
    // committed configuration before reporting failure.
    const bool shell_xaml_required =
        clock_hook_required ||
        (configuration.feature_flags &
         (AgentFeature::agent_feature_taskbar_background_color |
          AgentFeature::agent_feature_taskbar_capsule)) != 0;
    const AgentResult xaml_result = metaplasia::agent::ConfigureShellXaml(
        configuration.target,
        shell_xaml_required,
        ShellXamlSettingsFrom(configuration));
    if (xaml_result != AgentResult::success) {
        const AgentResult explorer_rollback =
            EnsureExplorerTitleHook(old_explorer_hook_required);
        const AgentResult time_rollback =
            EnsureTimeHook(old_clock_hook_required);
        static_cast<void>(metaplasia::agent::ConfigureShellXaml(
            old_configuration.target,
            old_clock_hook_required ||
                (old_features &
                 (AgentFeature::agent_feature_taskbar_background_color |
                  AgentFeature::agent_feature_taskbar_capsule)) != 0,
            ShellXamlSettingsFrom(old_configuration)));
        const AgentResult color_rollback =
            metaplasia::agent::ConfigureExplorerWindowColor(
                old_explorer_color_required,
                old_configuration.file_explorer_background_color,
                old_configuration.file_explorer_transition_animation,
                old_explorer_scrollbar_required);
        g_features.store(old_features, std::memory_order_release);
        if (titles_restored) {
            PrefixExistingExplorerWindows();
        }
        return explorer_rollback == AgentResult::success &&
                       time_rollback == AgentResult::success &&
                       color_rollback == AgentResult::success
            ? xaml_result
            : AgentResult::hook_failed;
    }

    ::AcquireSRWLockExclusive(&g_configuration_lock);
    std::copy(
        std::begin(configuration.taskbar_clock_prefix),
        std::end(configuration.taskbar_clock_prefix),
        g_clock_prefix.begin());
    std::copy(
        std::begin(configuration.explorer_title_prefix),
        std::end(configuration.explorer_title_prefix),
        g_explorer_title_prefix.begin());
    ::ReleaseSRWLockExclusive(&g_configuration_lock);
    g_applied_configuration = configuration;
    g_has_applied_configuration = true;
    g_features.store(configuration.feature_flags, std::memory_order_release);
    if (explorer_hook_required) {
        PrefixExistingExplorerWindows();
    }
    return AgentResult::success;
}

}  // namespace

extern "C" __declspec(dllexport) DWORD WINAPI MetaplasiaAgentStart(
    void* raw_configuration) noexcept {
    const auto* configuration =
        static_cast<const AgentConfiguration*>(raw_configuration);
    if (!IsValidConfiguration(configuration)) {
        return static_cast<DWORD>(AgentResult::invalid_configuration);
    }
    if (!IsExpectedProcess(configuration->target)) {
        return static_cast<DWORD>(AgentResult::incompatible_process);
    }

    AgentState expected = AgentState::stopped;
    if (!g_state.compare_exchange_strong(expected, AgentState::starting)) {
        return static_cast<DWORD>(
            expected == AgentState::running ? AgentResult::success
                                            : AgentResult::busy);
    }

    const MH_STATUS initialize_status = ::MH_Initialize();
    if (initialize_status != MH_OK &&
        initialize_status != MH_ERROR_ALREADY_INITIALIZED) {
        g_state.store(AgentState::stopped, std::memory_order_release);
        return static_cast<DWORD>(AgentResult::initialization_failed);
    }

    const AgentResult result = ApplyConfiguration(*configuration);
    if (result != AgentResult::success) {
        ::MH_Uninitialize();
        g_state.store(AgentState::stopped, std::memory_order_release);
        return static_cast<DWORD>(result);
    }
    g_state.store(AgentState::running, std::memory_order_release);
    DebugLog(L"Agent initialized");
    return static_cast<DWORD>(AgentResult::success);
}

extern "C" __declspec(dllexport) DWORD WINAPI MetaplasiaAgentConfigure(
    void* raw_configuration) noexcept {
    const auto* configuration =
        static_cast<const AgentConfiguration*>(raw_configuration);
    if (!IsValidConfiguration(configuration)) {
        return static_cast<DWORD>(AgentResult::invalid_configuration);
    }
    if (!IsExpectedProcess(configuration->target)) {
        return static_cast<DWORD>(AgentResult::incompatible_process);
    }
    ::AcquireSRWLockExclusive(&g_operation_lock);
    if (g_state.load(std::memory_order_acquire) != AgentState::running) {
        ::ReleaseSRWLockExclusive(&g_operation_lock);
        return static_cast<DWORD>(AgentResult::not_initialized);
    }
    const auto result = ApplyConfiguration(*configuration);
    ::ReleaseSRWLockExclusive(&g_operation_lock);
    return static_cast<DWORD>(result);
}

extern "C" __declspec(dllexport) DWORD WINAPI MetaplasiaAgentStop(
    void*) noexcept {
    AgentState expected = AgentState::running;
    if (!g_state.compare_exchange_strong(expected, AgentState::stopping)) {
        return static_cast<DWORD>(
            expected == AgentState::stopped ? AgentResult::success
                                            : AgentResult::busy);
    }

    ::AcquireSRWLockExclusive(&g_operation_lock);
    const AgentResult explorer_color_result =
        metaplasia::agent::StopExplorerWindowColor();
    if (explorer_color_result != AgentResult::success) {
        g_state.store(AgentState::running, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&g_operation_lock);
        return static_cast<DWORD>(explorer_color_result);
    }
    const AgentResult start_menu_result =
        metaplasia::agent::StopShellXaml();
    if (start_menu_result != AgentResult::success) {
        g_state.store(AgentState::running, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&g_operation_lock);
        return static_cast<DWORD>(start_menu_result);
    }
    std::array<
        wchar_t,
        metaplasia::protocol::kMaximumExplorerTitlePrefixLength + 1>
        explorer_prefix{};
    ::AcquireSRWLockShared(&g_configuration_lock);
    explorer_prefix = g_explorer_title_prefix;
    ::ReleaseSRWLockShared(&g_configuration_lock);
    g_features.store(AgentFeature::agent_feature_none, std::memory_order_release);
    if (!RestoreTrackedExplorerWindows(explorer_prefix)) {
        g_state.store(AgentState::running, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&g_operation_lock);
        return static_cast<DWORD>(AgentResult::hook_failed);
    }
    const AgentResult explorer_result = EnsureExplorerTitleHook(false);
    const AgentResult time_result = EnsureTimeHook(false);
    if (explorer_result != AgentResult::success ||
        time_result != AgentResult::success) {
        g_state.store(AgentState::running, std::memory_order_release);
        ::ReleaseSRWLockExclusive(&g_operation_lock);
        return static_cast<DWORD>(AgentResult::hook_failed);
    }
    ::MH_Uninitialize();
    g_applied_configuration = {};
    g_has_applied_configuration = false;
    g_state.store(AgentState::stopped, std::memory_order_release);
    ::ReleaseSRWLockExclusive(&g_operation_lock);
    DebugLog(L"Agent stopped");
    return static_cast<DWORD>(AgentResult::success);
}

extern "C" __declspec(dllexport) DWORD WINAPI MetaplasiaAgentGetLastError(
    void*) noexcept {
    return metaplasia::agent::StartMenuXamlLastError();
}

extern "C" __declspec(dllexport) DWORD WINAPI
MetaplasiaAgentGetDiagnosticStage(void*) noexcept {
    return static_cast<DWORD>(
        metaplasia::agent::StartMenuXamlDiagnosticStage());
}

extern "C" __declspec(dllexport) DWORD WINAPI
MetaplasiaAgentGetStartMenuState(void*) noexcept {
    return metaplasia::agent::StartMenuXamlControllerState();
}

extern "C" __declspec(dllexport) DWORD WINAPI
MetaplasiaAgentGetXamlDiagnostics(void* raw_snapshot) noexcept {
    return static_cast<DWORD>(
        metaplasia::agent::CopyShellXamlDiagnostics(
            static_cast<
                metaplasia::protocol::XamlDiagnosticsSnapshot*>(
                raw_snapshot)));
}

STDAPI DllGetClassObject(
    REFCLSID class_id,
    REFIID interface_id,
    void** object) {
    const HRESULT shell_xaml_result =
        metaplasia::agent::GetStartMenuTapClassObject(
        class_id,
        interface_id,
        object);
    return shell_xaml_result != CLASS_E_CLASSNOTAVAILABLE
        ? shell_xaml_result
        : metaplasia::agent::GetExplorerWinUiTapClassObject(
              class_id,
              interface_id,
              object);
}

STDAPI DllCanUnloadNow() {
    // The agent owns process-wide hooks and XAML callbacks. It is unloaded only
    // as part of a controlled target-process restart, never by COM while those
    // callbacks may still be in flight.
    return S_FALSE;
}

BOOL WINAPI DllMain(const HINSTANCE instance, const DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        metaplasia::agent::SetAgentModule(instance);
        ::DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
