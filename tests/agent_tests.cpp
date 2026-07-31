#include "metaplasia/protocol/agent_abi.hpp"
#include "metaplasia/agent/start_menu_xaml_adapter.hpp"

#include <Windows.h>
#include <ocidl.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

using metaplasia::protocol::AgentConfiguration;
using metaplasia::protocol::AgentEntryPoint;
using metaplasia::protocol::AgentFeature;
using metaplasia::protocol::AgentResult;
using metaplasia::protocol::AgentTarget;

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

LRESULT CALLBACK TestWindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) {
    return ::DefWindowProcW(window, message, wparam, lparam);
}

std::wstring WindowTitle(const HWND window) {
    std::array<wchar_t, 512> title{};
    const int length = ::GetWindowTextW(
        window,
        title.data(),
        static_cast<int>(title.size()));
    Require(length >= 0 && static_cast<std::size_t>(length) < title.size(), "read title");
    return std::wstring(title.data(), static_cast<std::size_t>(length));
}

AgentConfiguration ExplorerConfiguration(
    const std::uint32_t features,
    const wchar_t* prefix) {
    AgentConfiguration configuration;
    configuration.target = AgentTarget::explorer_shell;
    configuration.feature_flags = features;
    wcsncpy_s(configuration.explorer_title_prefix, prefix, _TRUNCATE);
    return configuration;
}

}  // namespace

int wmain(const int argc, wchar_t** argv) {
    Require(argc == 2, "agent path argument");
    const std::filesystem::path agent_path(argv[1]);
    Require(agent_path.is_absolute(), "absolute agent path");

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &TestWindowProcedure;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"CabinetWClass";
    const ATOM class_atom = ::RegisterClassW(&window_class);
    Require(class_atom != 0, "register Explorer test window class");

    const HWND window = ::CreateWindowExW(
        0,
        window_class.lpszClassName,
        L"Folder",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        480,
        nullptr,
        nullptr,
        window_class.hInstance,
        nullptr);
    Require(window != nullptr, "create Explorer test window");
    const HWND naturally_prefixed_window = ::CreateWindowExW(
        0,
        window_class.lpszClassName,
        L"Test · Native",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        480,
        nullptr,
        nullptr,
        window_class.hInstance,
        nullptr);
    Require(
        naturally_prefixed_window != nullptr,
        "create naturally prefixed Explorer test window");
    const HWND externally_owned_window = ::CreateWindowExW(
        0,
        window_class.lpszClassName,
        L"External",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        480,
        nullptr,
        nullptr,
        window_class.hInstance,
        nullptr);
    Require(
        externally_owned_window != nullptr,
        "create Explorer ownership test window");

    const HMODULE agent = ::LoadLibraryW(agent_path.c_str());
    Require(agent != nullptr, "load agent DLL");
    const auto start = reinterpret_cast<AgentEntryPoint>(
        ::GetProcAddress(agent, metaplasia::protocol::kAgentStartExport));
    const auto configure = reinterpret_cast<AgentEntryPoint>(
        ::GetProcAddress(agent, metaplasia::protocol::kAgentConfigureExport));
    const auto stop = reinterpret_cast<AgentEntryPoint>(
        ::GetProcAddress(agent, metaplasia::protocol::kAgentStopExport));
    const auto get_last_error = reinterpret_cast<AgentEntryPoint>(
        ::GetProcAddress(
            agent,
            metaplasia::protocol::kAgentGetLastErrorExport));
    const auto get_diagnostic_stage = reinterpret_cast<AgentEntryPoint>(
        ::GetProcAddress(
            agent,
            metaplasia::protocol::kAgentGetDiagnosticStageExport));
    const auto get_start_menu_state = reinterpret_cast<AgentEntryPoint>(
        ::GetProcAddress(
            agent,
            metaplasia::protocol::kAgentGetStartMenuStateExport));
    Require(
        start != nullptr && configure != nullptr && stop != nullptr &&
            get_last_error != nullptr && get_diagnostic_stage != nullptr &&
            get_start_menu_state != nullptr,
        "agent exports");
    Require(get_last_error(nullptr) == S_OK, "initial adapter error is clear");
    Require(
        get_diagnostic_stage(nullptr) ==
            static_cast<std::uint32_t>(
                metaplasia::protocol::AgentDiagnosticStage::none),
        "initial adapter diagnostic stage is clear");
    Require(
        get_start_menu_state(nullptr) == 0,
        "initial Start Menu controller state is clear");

    const auto get_class_object = reinterpret_cast<LPFNGETCLASSOBJECT>(
        ::GetProcAddress(agent, "DllGetClassObject"));
    Require(get_class_object != nullptr, "TAP class-object export");
    const auto can_unload_now = reinterpret_cast<HRESULT(WINAPI*)()>(
        ::GetProcAddress(agent, "DllCanUnloadNow"));
    Require(
        can_unload_now != nullptr && can_unload_now() == S_FALSE,
        "TAP module remains loaded while callbacks can be active");
    void* class_object = nullptr;
    Require(
        get_class_object(
            CLSID_NULL,
            __uuidof(IClassFactory),
            &class_object) == CLASS_E_CLASSNOTAVAILABLE &&
            class_object == nullptr,
        "reject unknown TAP class id");
    Require(
        get_class_object(
            metaplasia::agent::kStartMenuTapClsid,
            __uuidof(IClassFactory),
            &class_object) == S_OK &&
            class_object != nullptr,
        "create TAP class factory");
    auto* class_factory = static_cast<IClassFactory*>(class_object);
    void* tap_object = nullptr;
    Require(
        class_factory->CreateInstance(
            class_factory,
            __uuidof(IObjectWithSite),
            &tap_object) == CLASS_E_NOAGGREGATION &&
            tap_object == nullptr,
        "reject TAP COM aggregation");
    Require(
        class_factory->CreateInstance(
            nullptr,
            __uuidof(IObjectWithSite),
            &tap_object) == S_OK &&
            tap_object != nullptr,
        "create TAP site object");
    auto* tap_site = static_cast<IObjectWithSite*>(tap_object);
    void* missing_site = nullptr;
    Require(
        tap_site->GetSite(__uuidof(IUnknown), &missing_site) ==
                MK_E_NOSTORAGE &&
            missing_site == nullptr,
        "report missing diagnostics site");
    Require(
        FAILED(tap_site->SetSite(class_factory)),
        "reject a site without XAML diagnostics interfaces");
    Require(tap_site->SetSite(nullptr) == S_OK, "clear an empty TAP site");
    tap_site->Release();
    class_factory->Release();

    auto invalid_configuration = ExplorerConfiguration(
        AgentFeature::agent_feature_file_explorer_title_prefix,
        L"");
    std::fill(
        std::begin(invalid_configuration.explorer_title_prefix),
        std::end(invalid_configuration.explorer_title_prefix),
        L'X');
    Require(
        start(&invalid_configuration) ==
            static_cast<std::uint32_t>(AgentResult::invalid_configuration),
        "reject unterminated agent ABI string");

    invalid_configuration = ExplorerConfiguration(
        AgentFeature::agent_feature_none,
        L"");
    invalid_configuration.start_menu_opacity_milli = 0;
    Require(
        start(&invalid_configuration) ==
            static_cast<std::uint32_t>(AgentResult::invalid_configuration),
        "reject invalid numeric field even when its feature is disabled");

    invalid_configuration = ExplorerConfiguration(
        AgentFeature::agent_feature_none,
        L"");
    invalid_configuration.taskbar_opacity_milli = 0;
    Require(
        start(&invalid_configuration) ==
            static_cast<std::uint32_t>(AgentResult::invalid_configuration),
        "reject invalid Taskbar opacity while disabled");

    invalid_configuration = ExplorerConfiguration(
        AgentFeature::agent_feature_none,
        L"");
    invalid_configuration.taskbar_hide_notification_center = 2;
    Require(
        start(&invalid_configuration) ==
            static_cast<std::uint32_t>(AgentResult::invalid_configuration),
        "reject non-boolean Taskbar visibility field");

    invalid_configuration = ExplorerConfiguration(
        AgentFeature::agent_feature_none,
        L"");
    invalid_configuration.taskbar_capsule_enabled = 2;
    Require(
        start(&invalid_configuration) ==
            static_cast<std::uint32_t>(AgentResult::invalid_configuration),
        "reject non-boolean Taskbar capsule field");

    auto configuration = ExplorerConfiguration(
        AgentFeature::agent_feature_file_explorer_title_prefix,
        L"Test · ");
    const auto start_result = start(&configuration);
    if (start_result != static_cast<std::uint32_t>(AgentResult::success)) {
        std::cerr << "Agent start result: " << start_result << '\n';
    }
    Require(
        start_result == static_cast<std::uint32_t>(AgentResult::success),
        "start Explorer title hook");
    Require(WindowTitle(window) == L"Test · Folder", "prefix existing window");
    Require(
        WindowTitle(naturally_prefixed_window) == L"Test · Native",
        "do not claim an existing matching prefix");

    Require(::SetWindowTextW(window, L"Other") != FALSE, "set hooked title");
    Require(WindowTitle(window) == L"Test · Other", "prefix updated title");

    configuration = ExplorerConfiguration(
        AgentFeature::agent_feature_file_explorer_title_prefix,
        L"Next · ");
    Require(
        configure(&configuration) ==
            static_cast<std::uint32_t>(AgentResult::success),
        "reconfigure Explorer title prefix");
    Require(WindowTitle(window) == L"Next · Other", "replace owned prefix");
    Require(
        WindowTitle(naturally_prefixed_window) == L"Next · Test · Native",
        "apply changed prefix without losing original text");
    Require(
        ::SetWindowTextW(externally_owned_window, L"Next · External") != FALSE,
        "set an externally owned prefixed title");
    Require(
        WindowTitle(externally_owned_window) == L"Next · External",
        "preserve externally supplied prefix");

    configuration = ExplorerConfiguration(
        AgentFeature::agent_feature_none,
        L"");
    Require(
        configure(&configuration) ==
            static_cast<std::uint32_t>(AgentResult::success),
        "disable Explorer title hook");
    Require(WindowTitle(window) == L"Other", "restore original title");
    Require(
        WindowTitle(naturally_prefixed_window) == L"Test · Native",
        "preserve an unowned original prefix");
    Require(
        WindowTitle(externally_owned_window) == L"Next · External",
        "do not strip an externally owned prefix on disable");

    Require(
        stop(nullptr) == static_cast<std::uint32_t>(AgentResult::success),
        "stop agent");
    ::FreeLibrary(agent);
    ::DestroyWindow(window);
    ::DestroyWindow(naturally_prefixed_window);
    ::DestroyWindow(externally_owned_window);
    ::UnregisterClassW(window_class.lpszClassName, window_class.hInstance);

    std::cout << "Agent hook tests passed\n";
    return EXIT_SUCCESS;
}
