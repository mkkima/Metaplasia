#include "metaplasia/base/unique_handle.hpp"
#include "metaplasia/base/utf.hpp"
#include "metaplasia/base/windows_paths.hpp"
#include "metaplasia/host/settings_store.hpp"
#include "metaplasia/injector/injector.hpp"
#include "metaplasia/platform/process.hpp"
#include "metaplasia/protocol/agent_abi.hpp"
#include "metaplasia/watchdog/protocol.hpp"

#include <Windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using metaplasia::ErrorCode;
using metaplasia::Result;
using metaplasia::Status;
using metaplasia::UniqueHandle;
using metaplasia::watchdog::LaunchArguments;

constexpr auto kAgentDeactivationTimeout = std::chrono::seconds(3);

class OwnedMutex final {
public:
    explicit OwnedMutex(HANDLE handle) noexcept : handle_(handle) {}
    ~OwnedMutex() {
        if (handle_) {
            ::ReleaseMutex(handle_.get());
        }
    }

    OwnedMutex(const OwnedMutex&) = delete;
    OwnedMutex& operator=(const OwnedMutex&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_.get(); }

private:
    UniqueHandle handle_;
};

void Log(const std::string_view message) noexcept {
    const auto wide = metaplasia::Utf8ToWide(message);
    if (!wide.ok()) {
        ::OutputDebugStringW(L"[Metaplasia Watchdog] An unprintable error occurred.\n");
        return;
    }
    const std::wstring line =
        L"[Metaplasia Watchdog] " + wide.value() + L"\n";
    ::OutputDebugStringW(line.c_str());
}

bool PathsEqualIgnoreCase(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept {
    return _wcsicmp(
               left.lexically_normal().c_str(),
               right.lexically_normal().c_str()) == 0;
}

Result<std::filesystem::path> CanonicalRegularFile(
    const std::filesystem::path& path,
    const std::string_view description) {
    std::error_code error;
    auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) {
        return Status(
            ErrorCode::invalid_data,
            "Unable to canonicalize " + std::string(description) + ": " +
                error.message(),
            static_cast<std::uint32_t>(error.value()));
    }
    if (!std::filesystem::is_regular_file(canonical, error) || error) {
        return Status(
            ErrorCode::not_found,
            std::string(description) + " is not a regular file");
    }
    return canonical;
}

Result<void> ValidatePaths(LaunchArguments& arguments) {
    auto executable_directory = metaplasia::ExecutableDirectory();
    auto data_directory = metaplasia::MetaplasiaDataDirectory();
    if (!executable_directory.ok()) {
        return executable_directory.status();
    }
    if (!data_directory.ok()) {
        return data_directory.status();
    }

    auto actual_agent = CanonicalRegularFile(arguments.agent_path, "agent DLL");
    auto expected_agent = CanonicalRegularFile(
        executable_directory.value() / L"metaplasia-agent.dll",
        "installed agent DLL");
    if (!actual_agent.ok()) {
        return actual_agent.status();
    }
    if (!expected_agent.ok()) {
        return expected_agent.status();
    }
    if (!PathsEqualIgnoreCase(actual_agent.value(), expected_agent.value())) {
        return Status(
            ErrorCode::access_denied,
            "Watchdog agent path is outside its installation directory");
    }

    const auto expected_settings =
        (data_directory.value() / L"settings.conf").lexically_normal();
    if (!PathsEqualIgnoreCase(arguments.settings_path, expected_settings)) {
        return Status(
            ErrorCode::access_denied,
            "Watchdog settings path is outside the Metaplasia data directory");
    }
    arguments.agent_path = std::move(actual_agent).value();
    arguments.settings_path = expected_settings;
    return {};
}

Result<void> ValidateHostProcess(
    const HANDLE process,
    const LaunchArguments& arguments) {
    DWORD process_session = 0;
    if (!::ProcessIdToSessionId(arguments.host_process_id, &process_session)) {
        return Status::FromWin32(
            "ProcessIdToSessionId(watchdog host)",
            ::GetLastError());
    }
    if (process_session != arguments.session_id) {
        return Status(ErrorCode::access_denied, "Watchdog host session mismatch");
    }

    auto same_user = metaplasia::platform::IsProcessOwnedByCurrentUser(process);
    if (!same_user.ok()) {
        return same_user.status();
    }
    if (!same_user.value()) {
        return Status(
            ErrorCode::access_denied,
            "Watchdog host belongs to another user");
    }

    std::array<wchar_t, 32768> image_path{};
    DWORD image_path_size = static_cast<DWORD>(image_path.size());
    if (!::QueryFullProcessImageNameW(
            process,
            0,
            image_path.data(),
            &image_path_size)) {
        return Status::FromWin32(
            "QueryFullProcessImageNameW(watchdog host)",
            ::GetLastError());
    }
    const std::filesystem::path host_path(
        std::wstring_view(image_path.data(), image_path_size));
    auto executable_directory = metaplasia::ExecutableDirectory();
    if (!executable_directory.ok()) {
        return executable_directory.status();
    }
    const auto expected_host =
        (executable_directory.value() / L"metaplasia-host.exe")
            .lexically_normal();
    if (!PathsEqualIgnoreCase(host_path, expected_host)) {
        return Status(
            ErrorCode::access_denied,
            "Watchdog host is not the sibling Metaplasia host executable");
    }
    return {};
}

struct RecoveryReport final {
    std::size_t loaded_agents{0};
    std::size_t deactivated_agents{0};
    std::size_t failed_agents{0};
    bool settings_reset{false};
};

Result<RecoveryReport> Recover(const LaunchArguments& arguments) {
    RecoveryReport report;
    metaplasia::host::SettingsStore settings_store(arguments.settings_path);
    const auto save = settings_store.Save(metaplasia::host::HostSettings{});
    report.settings_reset = save.ok();
    if (!save.ok()) {
        Log("Unable to persist safe-mode settings: " + save.status().message());
    }

    auto processes = metaplasia::platform::EnumerateProcesses();
    if (!processes.ok()) {
        return processes.status();
    }

    metaplasia::injector::Injector injector;
    for (const auto& process : processes.value()) {
        if (process.session_id != arguments.session_id) {
            continue;
        }

        metaplasia::protocol::AgentTarget target{};
        if (_wcsicmp(process.image_name.c_str(), L"explorer.exe") == 0) {
            target = metaplasia::protocol::AgentTarget::explorer_shell;
        } else if (
            _wcsicmp(
                process.image_name.c_str(),
                L"StartMenuExperienceHost.exe") == 0) {
            target = metaplasia::protocol::AgentTarget::start_menu;
        } else {
            continue;
        }

        auto loaded = metaplasia::platform::IsModuleLoaded(
            process.process_id,
            arguments.agent_path);
        if (!loaded.ok()) {
            // Shell processes can exit while the snapshot is being traversed.
            // A vanished process cannot retain active customizations.
            UniqueHandle still_running(::OpenProcess(
                SYNCHRONIZE,
                FALSE,
                process.process_id));
            if (still_running &&
                ::WaitForSingleObject(still_running.get(), 0) == WAIT_TIMEOUT) {
                ++report.failed_agents;
                Log(
                    "Unable to inspect a running shell process: " +
                    loaded.status().message());
            }
            continue;
        }
        if (!loaded.value()) {
            continue;
        }

        ++report.loaded_agents;
        metaplasia::protocol::AgentConfiguration configuration;
        configuration.target = target;
        configuration.feature_flags =
            metaplasia::protocol::agent_feature_none;
        const auto deactivated = injector.ConfigureLoaded(
            process.process_id,
            arguments.agent_path,
            configuration,
            kAgentDeactivationTimeout);
        if (!deactivated.ok()) {
            ++report.failed_agents;
            Log(
                "Unable to deactivate a loaded agent: " +
                deactivated.status().message());
            continue;
        }
        ++report.deactivated_agents;
    }

    if (!report.settings_reset) {
        return Status(
            ErrorCode::internal_error,
            "Recovery could not persist safe-mode settings");
    }
    if (report.failed_agents != 0) {
        return Status(
            ErrorCode::internal_error,
            "Recovery could not deactivate every loaded agent");
    }
    return report;
}

int Run(const int argc, wchar_t* argv[]) {
    std::vector<std::wstring_view> raw_arguments;
    if (argc > 1) {
        raw_arguments.reserve(static_cast<std::size_t>(argc - 1));
        for (int index = 1; index < argc; ++index) {
            raw_arguments.emplace_back(argv[index]);
        }
    }
    auto parsed = metaplasia::watchdog::ParseLaunchArguments(raw_arguments);
    if (!parsed.ok()) {
        Log(parsed.status().message());
        return 10;
    }
    auto arguments = std::move(parsed).value();
    auto paths_valid = ValidatePaths(arguments);
    if (!paths_valid.ok()) {
        Log(paths_valid.status().message());
        return 11;
    }

    UniqueHandle host_process(::OpenProcess(
        SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        arguments.host_process_id));
    if (!host_process) {
        Log("Unable to open the host process");
        return 12;
    }
    auto host_valid = ValidateHostProcess(host_process.get(), arguments);
    if (!host_valid.ok()) {
        Log(host_valid.status().message());
        return 13;
    }

    const auto recovery_mutex_name =
        metaplasia::watchdog::RecoveryMutexName(arguments.session_id);
    OwnedMutex recovery_mutex(
        ::CreateMutexW(nullptr, TRUE, recovery_mutex_name.c_str()));
    const DWORD recovery_mutex_error = ::GetLastError();
    if (recovery_mutex.get() == nullptr) {
        Log("Unable to create the recovery mutex");
        return 14;
    }
    if (recovery_mutex_error == ERROR_ALREADY_EXISTS) {
        Log("Another watchdog already owns the recovery lease");
        return 15;
    }

    UniqueHandle graceful_event(::OpenEventW(
        SYNCHRONIZE,
        FALSE,
        arguments.graceful_event_name.c_str()));
    UniqueHandle ready_event(::OpenEventW(
        EVENT_MODIFY_STATE,
        FALSE,
        arguments.ready_event_name.c_str()));
    if (!graceful_event || !ready_event) {
        Log("Unable to open watchdog handshake events");
        return 16;
    }
    if (!::SetEvent(ready_event.get())) {
        Log("Unable to signal watchdog readiness");
        return 17;
    }

    const std::array<HANDLE, 2> wait_handles{
        graceful_event.get(),
        host_process.get()};
    const DWORD wait = ::WaitForMultipleObjects(
        static_cast<DWORD>(wait_handles.size()),
        wait_handles.data(),
        FALSE,
        INFINITE);
    if (wait == WAIT_OBJECT_0) {
        return 0;
    }
    if (wait != WAIT_OBJECT_0 + 1U) {
        Log("Watchdog wait failed");
        return 18;
    }

    // Graceful shutdown wins a simultaneous race with process termination.
    if (::WaitForSingleObject(graceful_event.get(), 0) == WAIT_OBJECT_0) {
        return 0;
    }
    Log("Host exited unexpectedly; entering safe mode");
    auto recovery = Recover(arguments);
    if (!recovery.ok()) {
        Log(recovery.status().message());
        return 20;
    }
    Log(
        "Recovery completed; deactivated " +
        std::to_string(recovery.value().deactivated_agents) +
        " loaded agent(s)");
    return 0;
}

}  // namespace

int wmain(const int argc, wchar_t* argv[]) {
    try {
        return Run(argc, argv);
    } catch (const std::exception& exception) {
        Log(std::string("Unhandled watchdog exception: ") + exception.what());
        return 30;
    } catch (...) {
        Log("Unknown watchdog exception");
        return 31;
    }
}
