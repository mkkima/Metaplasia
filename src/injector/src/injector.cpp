#include "metaplasia/injector/injector.hpp"
#include "metaplasia/injector/conflict_policy.hpp"
#include "metaplasia/injector/remote_image.hpp"

#include "metaplasia/base/unique_handle.hpp"
#include "metaplasia/platform/process.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace metaplasia::injector {
namespace {

Result<void> ValidateNoConflictingShellModules(
    const std::uint32_t process_id) {
    auto modules = platform::EnumerateProcessModules(process_id);
    if (!modules.ok()) {
        return modules.status();
    }
    for (const auto& module : modules.value()) {
        const auto conflict =
            IdentifyConflictingShellModule(module.module_name);
        if (!conflict.has_value()) {
            continue;
        }
        return Status(
            ErrorCode::incompatible,
            "Refusing to enable Metaplasia while " +
                std::string(ConflictingShellCustomizerName(*conflict)) +
                " is loaded in the target process");
    }
    return {};
}

[[nodiscard]] std::string_view DiagnosticStageName(
    const protocol::AgentDiagnosticStage stage) noexcept {
    using Stage = protocol::AgentDiagnosticStage;
    switch (stage) {
        case Stage::none:
            return "none";
        case Stage::initialize_xaml_diagnostics:
            return "initialize-xaml-diagnostics";
        case Stage::request_class_factory:
            return "request-class-factory";
        case Stage::create_tap_instance:
            return "create-tap-instance";
        case Stage::set_tap_site:
            return "set-tap-site";
        case Stage::query_xaml_diagnostics:
            return "query-xaml-diagnostics";
        case Stage::query_visual_tree_service:
            return "query-visual-tree-service";
        case Stage::attach_controller:
            return "attach-controller";
        case Stage::advise_visual_tree:
            return "advise-visual-tree";
        case Stage::controller_attached:
            return "controller-attached";
        case Stage::get_tap_site:
            return "get-tap-site";
        case Stage::lock_class_factory:
            return "lock-class-factory";
        case Stage::tap_site_released:
            return "tap-site-released";
        case Stage::tap_site_detached:
            return "tap-site-detached";
    }
    return "unknown";
}

class RemoteAllocation final {
public:
    RemoteAllocation(HANDLE process, void* address) noexcept
        : process_(process), address_(address) {}
    ~RemoteAllocation() {
        if (address_ != nullptr) {
            ::VirtualFreeEx(process_, address_, 0, MEM_RELEASE);
        }
    }

    RemoteAllocation(const RemoteAllocation&) = delete;
    RemoteAllocation& operator=(const RemoteAllocation&) = delete;

    [[nodiscard]] void* get() const noexcept { return address_; }
    [[nodiscard]] void* release() noexcept {
        return std::exchange(address_, nullptr);
    }

private:
    HANDLE process_{nullptr};
    void* address_{nullptr};
};

Result<std::filesystem::path> ValidateAgentPath(
    const std::filesystem::path& input) {
    if (input.empty() || !input.is_absolute()) {
        return Status(
            ErrorCode::invalid_argument,
            "Agent path must be absolute");
    }

    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(input, error);
    if (error) {
        return Status(
            ErrorCode::invalid_data,
            "Unable to canonicalize agent path: " + error.message(),
            static_cast<std::uint32_t>(error.value()));
    }
    if (!std::filesystem::is_regular_file(canonical, error) || error) {
        return Status(ErrorCode::not_found, "Agent DLL does not exist");
    }
    if (_wcsicmp(canonical.extension().c_str(), L".dll") != 0) {
        return Status(ErrorCode::invalid_argument, "Agent path is not a DLL");
    }
    if (canonical.native().size() >= 32767) {
        return Status(ErrorCode::invalid_argument, "Agent path is too long");
    }
    return canonical;
}

Result<DWORD> WaitForRemoteThread(
    const HANDLE thread,
    const std::chrono::milliseconds timeout,
    const std::string_view operation) {
    if (timeout.count() <= 0 ||
        timeout.count() > static_cast<long long>((std::numeric_limits<DWORD>::max)())) {
        return Status(ErrorCode::invalid_argument, "Invalid injection timeout");
    }
    const DWORD wait_result =
        ::WaitForSingleObject(thread, static_cast<DWORD>(timeout.count()));
    if (wait_result == WAIT_TIMEOUT) {
        // Do not terminate a remote thread: forcefully stopping LoadLibrary or a
        // hook transaction can corrupt the shell process. The caller reports the
        // timeout and the remote allocation intentionally remains valid until
        // process exit in this exceptional case.
        return Status(
            ErrorCode::timeout,
            std::string(operation) + " timed out");
    }
    if (wait_result != WAIT_OBJECT_0) {
        return Status::FromWin32(
            std::string(operation) + " wait",
            ::GetLastError());
    }

    DWORD exit_code = 0;
    if (!::GetExitCodeThread(thread, &exit_code)) {
        return Status::FromWin32(
            std::string(operation) + " exit code",
            ::GetLastError());
    }
    return exit_code;
}

Result<std::uintptr_t> RemoteProcedureAddress(
    const std::uint32_t process_id,
    const FARPROC local_procedure) {
    if (local_procedure == nullptr) {
        return Status(ErrorCode::invalid_argument, "Invalid local procedure");
    }

    HMODULE local_module = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(local_procedure),
            &local_module)) {
        return Status::FromWin32(
            "GetModuleHandleExW(procedure owner)",
            ::GetLastError());
    }
    std::array<wchar_t, 32768> module_path{};
    const DWORD module_path_length = ::GetModuleFileNameW(
        local_module,
        module_path.data(),
        static_cast<DWORD>(module_path.size()));
    if (module_path_length == 0 || module_path_length >= module_path.size()) {
        return Status::FromWin32(
            "GetModuleFileNameW(procedure owner)",
            module_path_length == 0 ? ::GetLastError() : ERROR_INSUFFICIENT_BUFFER);
    }
    const std::filesystem::path owner_path(
        std::wstring_view(module_path.data(), module_path_length));
    auto remote_base =
        platform::FindRemoteModuleBase(process_id, owner_path.filename().native());
    if (!remote_base.ok()) {
        return remote_base.status();
    }
    const auto local_base = reinterpret_cast<std::uintptr_t>(local_module);
    const auto local_address = reinterpret_cast<std::uintptr_t>(local_procedure);
    if (local_address < local_base) {
        return Status(ErrorCode::invalid_data, "Invalid local procedure address");
    }
    return remote_base.value() + (local_address - local_base);
}

Result<void> RemoteLoadLibrary(
    const HANDLE process,
    const std::uint32_t process_id,
    const std::filesystem::path& agent_path,
    const std::chrono::milliseconds timeout) {
    const HMODULE local_kernel32 = ::GetModuleHandleW(L"kernel32.dll");
    const FARPROC local_load_library =
        ::GetProcAddress(local_kernel32, "LoadLibraryW");
    auto remote_load_library =
        RemoteProcedureAddress(process_id, local_load_library);
    if (!remote_load_library.ok()) {
        return remote_load_library.status();
    }

    const std::wstring path = agent_path.native();
    const std::size_t byte_count = (path.size() + 1U) * sizeof(wchar_t);
    void* remote_memory = ::VirtualAllocEx(
        process,
        nullptr,
        byte_count,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (remote_memory == nullptr) {
        return Status::FromWin32("VirtualAllocEx(agent path)", ::GetLastError());
    }
    RemoteAllocation allocation(process, remote_memory);

    SIZE_T written = 0;
    if (!::WriteProcessMemory(
            process,
            remote_memory,
            path.c_str(),
            byte_count,
            &written) ||
        written != byte_count) {
        return Status::FromWin32("WriteProcessMemory(agent path)", ::GetLastError());
    }

    UniqueHandle thread(::CreateRemoteThread(
        process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_load_library.value()),
        remote_memory,
        0,
        nullptr));
    if (!thread) {
        return Status::FromWin32("CreateRemoteThread(LoadLibraryW)", ::GetLastError());
    }

    auto wait = WaitForRemoteThread(thread.get(), timeout, "Remote LoadLibraryW");
    if (!wait.ok()) {
        // The remote thread may still read the path after a timeout. Prevent the
        // RAII wrapper from freeing that memory by intentionally leaking a tiny
        // allocation in the target rather than creating a use-after-free.
        if (wait.status().code() == ErrorCode::timeout) {
            static_cast<void>(allocation.release());
        }
        return wait.status();
    }

    // A module handle is pointer-sized but GetExitCodeThread returns DWORD, so
    // it cannot be used as proof on x64. Enumerate modules instead.
    auto loaded = platform::IsModuleLoaded(process_id, agent_path);
    if (!loaded.ok()) {
        return loaded.status();
    }
    if (!loaded.value()) {
        return Status(
            ErrorCode::internal_error,
            "LoadLibraryW returned but the agent module is not present");
    }
    return {};
}

Result<std::uint32_t> InvokeAgentExport(
    const HANDLE process,
    const std::uint32_t process_id,
    const std::filesystem::path& agent_path,
    const char* export_name,
    const protocol::AgentConfiguration& configuration,
    const std::chrono::milliseconds timeout) {
    auto remote_module =
        platform::FindProcessModuleByPath(process_id, agent_path);
    if (!remote_module.ok()) {
        return remote_module.status();
    }
    auto remote_export = ResolveRemoteExportAddress(
        process,
        remote_module.value(),
        export_name);
    if (!remote_export.ok()) {
        return remote_export.status();
    }

    void* remote_memory = ::VirtualAllocEx(
        process,
        nullptr,
        sizeof(configuration),
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (remote_memory == nullptr) {
        return Status::FromWin32("VirtualAllocEx(agent config)", ::GetLastError());
    }
    RemoteAllocation allocation(process, remote_memory);

    SIZE_T written = 0;
    if (!::WriteProcessMemory(
            process,
            remote_memory,
            &configuration,
            sizeof(configuration),
            &written) ||
        written != sizeof(configuration)) {
        return Status::FromWin32("WriteProcessMemory(agent config)", ::GetLastError());
    }

    UniqueHandle thread(::CreateRemoteThread(
        process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_export.value()),
        remote_memory,
        0,
        nullptr));
    if (!thread) {
        return Status::FromWin32("CreateRemoteThread(agent export)", ::GetLastError());
    }
    auto exit_code = WaitForRemoteThread(thread.get(), timeout, export_name);
    if (!exit_code.ok()) {
        if (exit_code.status().code() == ErrorCode::timeout) {
            static_cast<void>(allocation.release());
        }
        return exit_code.status();
    }
    return exit_code.value();
}

Result<protocol::XamlDiagnosticsSnapshot> InvokeXamlDiagnosticsExport(
    const HANDLE process,
    const std::uint32_t process_id,
    const std::filesystem::path& agent_path,
    const protocol::AgentTarget target,
    const std::chrono::milliseconds timeout) {
    auto remote_module =
        platform::FindProcessModuleByPath(process_id, agent_path);
    if (!remote_module.ok()) {
        return remote_module.status();
    }
    auto remote_export = ResolveRemoteExportAddress(
        process,
        remote_module.value(),
        protocol::kAgentGetXamlDiagnosticsExport);
    if (!remote_export.ok()) {
        return remote_export.status();
    }

    protocol::XamlDiagnosticsSnapshot snapshot;
    snapshot.target = target;
    void* remote_memory = ::VirtualAllocEx(
        process,
        nullptr,
        sizeof(snapshot),
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
    if (remote_memory == nullptr) {
        return Status::FromWin32(
            "VirtualAllocEx(XAML diagnostics)",
            ::GetLastError());
    }
    RemoteAllocation allocation(process, remote_memory);

    SIZE_T transferred = 0;
    if (!::WriteProcessMemory(
            process,
            remote_memory,
            &snapshot,
            sizeof(snapshot),
            &transferred) ||
        transferred != sizeof(snapshot)) {
        return Status::FromWin32(
            "WriteProcessMemory(XAML diagnostics)",
            ::GetLastError());
    }

    UniqueHandle thread(::CreateRemoteThread(
        process,
        nullptr,
        0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(remote_export.value()),
        remote_memory,
        0,
        nullptr));
    if (!thread) {
        return Status::FromWin32(
            "CreateRemoteThread(XAML diagnostics)",
            ::GetLastError());
    }
    auto exit_code = WaitForRemoteThread(
        thread.get(),
        timeout,
        protocol::kAgentGetXamlDiagnosticsExport);
    if (!exit_code.ok()) {
        if (exit_code.status().code() == ErrorCode::timeout) {
            static_cast<void>(allocation.release());
        }
        return exit_code.status();
    }
    if (exit_code.value() !=
        static_cast<DWORD>(protocol::AgentResult::success)) {
        return Status(
            ErrorCode::not_found,
            "Loaded agent has no active XAML diagnostics snapshot",
            exit_code.value());
    }

    transferred = 0;
    if (!::ReadProcessMemory(
            process,
            remote_memory,
            &snapshot,
            sizeof(snapshot),
            &transferred) ||
        transferred != sizeof(snapshot)) {
        return Status::FromWin32(
            "ReadProcessMemory(XAML diagnostics)",
            ::GetLastError());
    }
    if (snapshot.magic != protocol::kXamlDiagnosticsMagic ||
        snapshot.version != protocol::kXamlDiagnosticsVersion ||
        snapshot.size != sizeof(snapshot) || snapshot.target != target ||
        snapshot.reserved != 0 || snapshot.reserved_state != 0 ||
        snapshot.type_count > protocol::kMaximumXamlDiagnosticTypes ||
        snapshot.element_count > protocol::kMaximumXamlDiagnosticElements) {
        return Status(
            ErrorCode::invalid_data,
            "Agent returned an invalid XAML diagnostics snapshot");
    }
    for (std::uint32_t index = 0; index < snapshot.type_count; ++index) {
        const auto& entry = snapshot.types[index];
        if (entry.observation_count == 0 ||
            std::find(
                std::begin(entry.type_name),
                std::end(entry.type_name),
                L'\0') == std::end(entry.type_name)) {
            return Status(
                ErrorCode::invalid_data,
                "Agent returned an invalid XAML type entry");
        }
    }
    for (std::uint32_t index = 0; index < snapshot.element_count; ++index) {
        const auto& entry = snapshot.elements[index];
        if (entry.handle == 0 || entry.reserved != 0 ||
            entry.type_index >= snapshot.type_count ||
            std::find(
                std::begin(entry.name),
                std::end(entry.name),
                L'\0') == std::end(entry.name)) {
            return Status(
                ErrorCode::invalid_data,
                "Agent returned an invalid XAML element entry");
        }
    }
    return snapshot;
}

}  // namespace

Result<void> Injector::ValidateTarget(
    const HANDLE process,
    const std::uint32_t process_id,
    const protocol::AgentTarget target) const {
    if (process_id == 0 || process_id == ::GetCurrentProcessId()) {
        return Status(ErrorCode::invalid_argument, "Invalid target process id");
    }

    const wchar_t* expected_image = nullptr;
    switch (target) {
        case protocol::AgentTarget::explorer_shell:
            expected_image = L"explorer.exe";
            break;
        case protocol::AgentTarget::start_menu:
            expected_image = L"StartMenuExperienceHost.exe";
            break;
        default:
            return Status(ErrorCode::invalid_argument, "Invalid agent target");
    }

    std::array<wchar_t, 32768> image_path{};
    DWORD image_path_size = static_cast<DWORD>(image_path.size());
    if (!::QueryFullProcessImageNameW(
            process,
            0,
            image_path.data(),
            &image_path_size)) {
        return Status::FromWin32(
            "QueryFullProcessImageNameW(injection target)",
            ::GetLastError());
    }
    const std::wstring_view full_image(image_path.data(), image_path_size);
    const auto separator = full_image.find_last_of(L"\\/");
    const auto image_name = separator == std::wstring_view::npos
                                ? full_image
                                : full_image.substr(separator + 1);
    if (_wcsicmp(std::wstring(image_name).c_str(), expected_image) != 0) {
        return Status(
            ErrorCode::incompatible,
            "Target process image does not match the requested agent target");
    }

    DWORD target_session = 0;
    DWORD current_session = 0;
    if (!::ProcessIdToSessionId(process_id, &target_session) ||
        !::ProcessIdToSessionId(::GetCurrentProcessId(), &current_session)) {
        return Status::FromWin32(
            "ProcessIdToSessionId(injection target)",
            ::GetLastError());
    }
    if (target_session != current_session) {
        return Status(
            ErrorCode::access_denied,
            "Refusing to inject into a process from another session");
    }

    auto same_user = platform::IsProcessOwnedByCurrentUser(process);
    if (!same_user.ok()) {
        return same_user.status();
    }
    if (!same_user.value()) {
        return Status(
            ErrorCode::access_denied,
            "Refusing to inject into a process owned by another user");
    }

    auto protected_process = platform::IsProtectedProcess(process);
    if (!protected_process.ok()) {
        return protected_process.status();
    }
    if (protected_process.value()) {
        return Status(
            ErrorCode::access_denied,
            "Refusing to inject into a protected process");
    }

    auto target_architecture = platform::GetProcessArchitecture(process);
    if (!target_architecture.ok()) {
        return target_architecture.status();
    }
    auto current_architecture =
        platform::GetProcessArchitecture(::GetCurrentProcess());
    if (!current_architecture.ok()) {
        return current_architecture.status();
    }
    if (target_architecture.value() != current_architecture.value()) {
        return Status(
            ErrorCode::incompatible,
            "Target and injector architectures do not match");
    }
    return {};
}

Result<void> Injector::ValidateAgentTrust(
    const std::filesystem::path& agent_path) const {
    auto signature = trust::VerifyAuthenticode(agent_path);
    if (!signature.ok()) {
        return signature.status();
    }
    const bool publisher_is_pinned = std::any_of(
        trust_policy_.expected_publisher.begin(),
        trust_policy_.expected_publisher.end(),
        [](const std::uint8_t value) { return value != 0; });
    if (publisher_is_pinned) {
        if (signature.value().state != trust::SignatureState::trusted ||
            signature.value().publisher_thumbprint !=
                trust_policy_.expected_publisher) {
            return Status(
                ErrorCode::access_denied,
                "Agent Authenticode publisher does not match the trusted host set");
        }
        return {};
    }
    if (trust_policy_.allow_unsigned_development &&
        signature.value().state == trust::SignatureState::unsigned_file) {
        return {};
    }
    return Status(
        ErrorCode::access_denied,
        "Agent is not trusted for a new load operation");
}

Result<InjectionResult> Injector::LoadAndConfigure(
    const std::uint32_t process_id,
    const std::filesystem::path& agent_path,
    const protocol::AgentConfiguration& configuration,
    const std::chrono::milliseconds timeout) const {
    return ConfigureInternal(
        process_id,
        agent_path,
        configuration,
        timeout,
        true);
}

Result<InjectionResult> Injector::ConfigureLoaded(
    const std::uint32_t process_id,
    const std::filesystem::path& agent_path,
    const protocol::AgentConfiguration& configuration,
    const std::chrono::milliseconds timeout) const {
    return ConfigureInternal(
        process_id,
        agent_path,
        configuration,
        timeout,
        false);
}

Result<protocol::XamlDiagnosticsSnapshot>
Injector::QueryLoadedXamlDiagnostics(
    const std::uint32_t process_id,
    const std::filesystem::path& agent_path,
    const protocol::AgentTarget target,
    const std::chrono::milliseconds timeout) const {
    auto canonical_path = ValidateAgentPath(agent_path);
    if (!canonical_path.ok()) {
        return canonical_path.status();
    }

    UniqueHandle process(::OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        process_id));
    if (!process) {
        return Status::FromWin32(
            "OpenProcess(XAML diagnostics target)",
            ::GetLastError());
    }
    auto target_validation = ValidateTarget(process.get(), process_id, target);
    if (!target_validation.ok()) {
        return target_validation.status();
    }
    auto loaded =
        platform::IsModuleLoaded(process_id, canonical_path.value());
    if (!loaded.ok()) {
        return loaded.status();
    }
    if (!loaded.value()) {
        return Status(
            ErrorCode::not_found,
            "Agent is not loaded; diagnostics never injects it");
    }
    return InvokeXamlDiagnosticsExport(
        process.get(),
        process_id,
        canonical_path.value(),
        target,
        timeout);
}

Result<InjectionResult> Injector::ConfigureInternal(
    const std::uint32_t process_id,
    const std::filesystem::path& agent_path,
    const protocol::AgentConfiguration& configuration,
    const std::chrono::milliseconds timeout,
    const bool allow_load) const {
    constexpr std::uint32_t known_features =
        protocol::agent_feature_taskbar_clock_prefix |
        protocol::agent_feature_file_explorer_title_prefix |
        protocol::agent_feature_start_menu_root_opacity |
        protocol::agent_feature_taskbar_background_color |
        protocol::agent_feature_file_explorer_background_color |
        protocol::agent_feature_start_menu_background_color |
        protocol::agent_feature_file_explorer_custom_scrollbar |
        protocol::agent_feature_taskbar_capsule |
        protocol::agent_feature_start_menu_three_panel_layout;
    const bool scrollbar_features_valid =
        (configuration.feature_flags &
         protocol::agent_feature_file_explorer_custom_scrollbar) == 0 ||
        (configuration.feature_flags &
         protocol::agent_feature_file_explorer_background_color) != 0;
    const bool target_features_valid =
        (configuration.target == protocol::AgentTarget::explorer_shell &&
         (configuration.feature_flags &
          (protocol::agent_feature_start_menu_root_opacity |
           protocol::agent_feature_start_menu_background_color |
           protocol::agent_feature_start_menu_three_panel_layout)) == 0) ||
        (configuration.target == protocol::AgentTarget::start_menu &&
         (configuration.feature_flags &
           (protocol::agent_feature_taskbar_clock_prefix |
            protocol::agent_feature_file_explorer_title_prefix |
            protocol::agent_feature_taskbar_capsule |
            protocol::agent_feature_taskbar_background_color |
           protocol::agent_feature_file_explorer_background_color |
           protocol::agent_feature_file_explorer_custom_scrollbar)) == 0);
    if (configuration.magic != protocol::kAgentConfigMagic ||
        configuration.version != protocol::kAgentAbiVersion ||
        configuration.size != sizeof(configuration) ||
        configuration.reserved != 0 ||
        configuration.taskbar_hide_notification_center > 1 ||
        configuration.taskbar_hide_control_center > 1 ||
        configuration.taskbar_hide_show_desktop > 1 ||
        configuration.taskbar_capsule_enabled > 1 ||
        configuration.file_explorer_transition_animation >
            protocol::kMaximumExplorerTransition ||
        configuration.start_menu_hide_recommended > 1 ||
        configuration.taskbar_background_color_enabled > 1 ||
        configuration.file_explorer_background_color_enabled > 1 ||
        configuration.start_menu_background_color_enabled > 1 ||
        configuration.start_menu_three_panel_layout_enabled > 1 ||
        configuration.start_menu_hide_all_apps > 1 ||
        std::ranges::any_of(
            configuration.reserved_tail,
            [](const std::uint8_t value) { return value != 0; }) ||
        (configuration.feature_flags & ~known_features) != 0 ||
        !scrollbar_features_valid ||
        !target_features_valid ||
        configuration.taskbar_opacity_milli <
            protocol::kMinimumTaskbarOpacityMilli ||
        configuration.taskbar_opacity_milli >
            protocol::kMaximumTaskbarOpacityMilli ||
        configuration.start_menu_opacity_milli <
            protocol::kMinimumStartMenuOpacityMilli ||
        configuration.start_menu_opacity_milli >
            protocol::kMaximumStartMenuOpacityMilli ||
        (configuration.taskbar_background_color & 0xFF000000U) !=
            0xFF000000U ||
        (configuration.file_explorer_background_color & 0xFF000000U) !=
            0xFF000000U ||
        (configuration.start_menu_background_color & 0xFF000000U) !=
            0xFF000000U ||
        std::find(
            std::begin(configuration.taskbar_clock_prefix),
            std::end(configuration.taskbar_clock_prefix),
            L'\0') == std::end(configuration.taskbar_clock_prefix) ||
        std::find(
            std::begin(configuration.explorer_title_prefix),
            std::end(configuration.explorer_title_prefix),
            L'\0') == std::end(configuration.explorer_title_prefix)) {
        return Status(ErrorCode::invalid_argument, "Invalid agent configuration ABI");
    }

    auto canonical_path = ValidateAgentPath(agent_path);
    if (!canonical_path.ok()) {
        return canonical_path.status();
    }

    UniqueHandle process(::OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_LIMITED_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
        FALSE,
        process_id));
    if (!process) {
        return Status::FromWin32("OpenProcess(injection target)", ::GetLastError());
    }
    auto target_validation =
        ValidateTarget(process.get(), process_id, configuration.target);
    if (!target_validation.ok()) {
        return target_validation.status();
    }

    // Never block an empty configuration: restoring our own changes must stay
    // possible even if another customizer was loaded after Metaplasia.
    if (configuration.feature_flags != protocol::agent_feature_none) {
        auto conflict_validation =
            ValidateNoConflictingShellModules(process_id);
        if (!conflict_validation.ok()) {
            return conflict_validation.status();
        }
    }

    auto already_loaded =
        platform::IsModuleLoaded(process_id, canonical_path.value());
    if (!already_loaded.ok()) {
        return already_loaded.status();
    }

    InjectionResult result;
    result.newly_loaded = !already_loaded.value();
    if (result.newly_loaded && !allow_load) {
        return Status(
            ErrorCode::not_found,
            "Agent is not loaded; configure-only operation will not load it");
    }
    if (result.newly_loaded) {
        auto agent_trust = ValidateAgentTrust(canonical_path.value());
        if (!agent_trust.ok()) {
            return agent_trust.status();
        }
        auto load = RemoteLoadLibrary(
            process.get(),
            process_id,
            canonical_path.value(),
            timeout);
        if (!load.ok()) {
            return load.status();
        }
    }

    auto agent_result = InvokeAgentExport(
        process.get(),
        process_id,
        canonical_path.value(),
        result.newly_loaded ? protocol::kAgentStartExport
                            : protocol::kAgentConfigureExport,
        configuration,
        timeout);
    if (!agent_result.ok()) {
        return agent_result.status();
    }
    if (!result.newly_loaded &&
        agent_result.value() ==
            static_cast<std::uint32_t>(protocol::AgentResult::not_initialized)) {
        agent_result = InvokeAgentExport(
            process.get(),
            process_id,
            canonical_path.value(),
            protocol::kAgentStartExport,
            configuration,
            timeout);
        if (!agent_result.ok()) {
            return agent_result.status();
        }
    }
    result.agent_result =
        static_cast<protocol::AgentResult>(agent_result.value());
    if (result.agent_result != protocol::AgentResult::success) {
        std::string native_detail;
        auto native_error = InvokeAgentExport(
            process.get(),
            process_id,
            canonical_path.value(),
            protocol::kAgentGetLastErrorExport,
            configuration,
            timeout);
        if (native_error.ok() && native_error.value() != S_OK) {
            std::ostringstream formatted;
            formatted << " (native=0x" << std::hex << std::uppercase
                      << native_error.value() << ')';
            native_detail = formatted.str();
        }
        std::string stage_detail;
        auto diagnostic_stage = InvokeAgentExport(
            process.get(),
            process_id,
            canonical_path.value(),
            protocol::kAgentGetDiagnosticStageExport,
            configuration,
            timeout);
        if (diagnostic_stage.ok()) {
            const auto stage = static_cast<protocol::AgentDiagnosticStage>(
                diagnostic_stage.value());
            stage_detail = " (stage=";
            stage_detail += DiagnosticStageName(stage);
            stage_detail += ')';
        }
        std::string state_detail;
        auto start_menu_state = InvokeAgentExport(
            process.get(),
            process_id,
            canonical_path.value(),
            protocol::kAgentGetStartMenuStateExport,
            configuration,
            timeout);
        if (start_menu_state.ok()) {
            std::ostringstream formatted;
            formatted << " (state=0x" << std::hex << std::uppercase
                      << start_menu_state.value() << ')';
            state_detail = formatted.str();
        }
        return Status(
            ErrorCode::internal_error,
                "Agent rejected configuration with code " +
                std::to_string(static_cast<std::uint32_t>(result.agent_result)) +
                native_detail + stage_detail + state_detail);
    }
    return result;
}

}  // namespace metaplasia::injector
