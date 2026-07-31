#include "metaplasia/host/watchdog_client.hpp"

#include "metaplasia/watchdog/protocol.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <array>
#include <limits>
#include <string>
#include <vector>

namespace metaplasia::host {
namespace {

constexpr std::chrono::milliseconds kFailedStartupShutdownTimeout =
    std::chrono::seconds(1);

Result<std::wstring> GenerateEventToken() {
    std::array<unsigned char, 16> random{};
    const NTSTATUS status = ::BCryptGenRandom(
        nullptr,
        random.data(),
        static_cast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return Status(
            ErrorCode::win32_error,
            "BCryptGenRandom failed",
            static_cast<std::uint32_t>(status));
    }

    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring token;
    token.reserve(random.size() * 2U);
    for (const auto byte : random) {
        token.push_back(hex[(byte >> 4U) & 0x0FU]);
        token.push_back(hex[byte & 0x0FU]);
    }
    return token;
}

std::wstring QuoteCommandLineArgument(const std::wstring_view argument) {
    std::wstring quoted;
    quoted.reserve(argument.size() + 2U);
    quoted.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2U + 1U, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    // Backslashes immediately before the closing quote must be doubled.
    quoted.append(backslashes * 2U, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

void AppendArgument(std::wstring& command_line, const std::wstring_view value) {
    if (!command_line.empty()) {
        command_line.push_back(L' ');
    }
    command_line += QuoteCommandLineArgument(value);
}

DWORD TimeoutToDword(const std::chrono::milliseconds timeout) noexcept {
    if (timeout.count() <= 0) {
        return 0;
    }
    return static_cast<DWORD>((std::min)(
        static_cast<unsigned long long>(timeout.count()),
        static_cast<unsigned long long>((std::numeric_limits<DWORD>::max)() - 1U)));
}

void StopFailedWatchdog(HANDLE process, HANDLE graceful_event) noexcept {
    if (graceful_event != nullptr) {
        ::SetEvent(graceful_event);
    }
    if (process == nullptr ||
        ::WaitForSingleObject(
            process,
            TimeoutToDword(kFailedStartupShutdownTimeout)) == WAIT_OBJECT_0) {
        return;
    }
    // This process was created by us and never completed its readiness
    // handshake. It must not remain orphaned after a failed host startup.
    ::TerminateProcess(process, ERROR_CANCELLED);
    ::WaitForSingleObject(process, TimeoutToDword(kFailedStartupShutdownTimeout));
}

}  // namespace

WatchdogClient::WatchdogClient(
    UniqueHandle process,
    UniqueHandle graceful_event,
    UniqueHandle ready_event) noexcept
    : process_(std::move(process)),
      graceful_event_(std::move(graceful_event)),
      ready_event_(std::move(ready_event)) {}

Result<WatchdogClient> WatchdogClient::Start(
    const std::filesystem::path& watchdog_path,
    const std::filesystem::path& agent_path,
    const std::filesystem::path& settings_path,
    const std::uint32_t session_id,
    const std::chrono::milliseconds startup_timeout) {
    if (!watchdog_path.is_absolute() || !agent_path.is_absolute() ||
        !settings_path.is_absolute() || startup_timeout.count() <= 0) {
        return Status(ErrorCode::invalid_argument, "Invalid watchdog launch arguments");
    }
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(watchdog_path, filesystem_error) ||
        filesystem_error) {
        return Status(ErrorCode::not_found, "Watchdog executable was not found");
    }

    auto token = GenerateEventToken();
    if (!token.ok()) {
        return token.status();
    }
    const auto event_base = watchdog::EventNameBase(
        session_id,
        ::GetCurrentProcessId(),
        token.value());
    const std::wstring graceful_name = event_base + L".graceful";
    const std::wstring ready_name = event_base + L".ready";

    UniqueHandle graceful_event(
        ::CreateEventW(nullptr, TRUE, FALSE, graceful_name.c_str()));
    const DWORD graceful_error = ::GetLastError();
    if (!graceful_event) {
        return Status::FromWin32("CreateEventW(watchdog graceful)", graceful_error);
    }
    if (graceful_error == ERROR_ALREADY_EXISTS) {
        return Status(
            ErrorCode::already_exists,
            "Watchdog graceful event unexpectedly already exists");
    }
    UniqueHandle ready_event(::CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
    const DWORD ready_error = ::GetLastError();
    if (!ready_event) {
        return Status::FromWin32("CreateEventW(watchdog ready)", ready_error);
    }
    if (ready_error == ERROR_ALREADY_EXISTS) {
        return Status(
            ErrorCode::already_exists,
            "Watchdog ready event unexpectedly already exists");
    }

    std::wstring command_line;
    AppendArgument(command_line, watchdog_path.native());
    AppendArgument(command_line, L"--host-pid");
    AppendArgument(command_line, std::to_wstring(::GetCurrentProcessId()));
    AppendArgument(command_line, L"--session-id");
    AppendArgument(command_line, std::to_wstring(session_id));
    AppendArgument(command_line, L"--graceful-event");
    AppendArgument(command_line, graceful_name);
    AppendArgument(command_line, L"--ready-event");
    AppendArgument(command_line, ready_name);
    AppendArgument(command_line, L"--agent-path");
    AppendArgument(command_line, agent_path.native());
    AppendArgument(command_line, L"--settings-path");
    AppendArgument(command_line, settings_path.native());
    std::vector<wchar_t> mutable_command_line(
        command_line.begin(),
        command_line.end());
    mutable_command_line.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_information{};
    if (!::CreateProcessW(
            watchdog_path.c_str(),
            mutable_command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            watchdog_path.parent_path().c_str(),
            &startup,
            &process_information)) {
        return Status::FromWin32("CreateProcessW(watchdog)", ::GetLastError());
    }
    UniqueHandle process(process_information.hProcess);
    UniqueHandle thread(process_information.hThread);

    const std::array<HANDLE, 2> startup_handles{
        ready_event.get(),
        process.get()};
    const DWORD wait = ::WaitForMultipleObjects(
        static_cast<DWORD>(startup_handles.size()),
        startup_handles.data(),
        FALSE,
        TimeoutToDword(startup_timeout));
    const DWORD wait_error = wait == WAIT_FAILED ? ::GetLastError() : ERROR_SUCCESS;
    if (wait != WAIT_OBJECT_0) {
        StopFailedWatchdog(process.get(), graceful_event.get());
        if (wait == WAIT_OBJECT_0 + 1U) {
            DWORD exit_code = 0;
            ::GetExitCodeProcess(process.get(), &exit_code);
            return Status(
                ErrorCode::internal_error,
                "Watchdog exited before readiness",
                exit_code);
        }
        if (wait == WAIT_TIMEOUT) {
            return Status(ErrorCode::timeout, "Watchdog readiness timed out");
        }
        return Status::FromWin32("WaitForMultipleObjects(watchdog)", wait_error);
    }

    return WatchdogClient(
        std::move(process),
        std::move(graceful_event),
        std::move(ready_event));
}

Result<void> WatchdogClient::SignalGracefulShutdown() noexcept {
    if (!graceful_event_) {
        return Status(ErrorCode::invalid_argument, "Watchdog is not running");
    }
    if (!::SetEvent(graceful_event_.get())) {
        return Status::FromWin32(
            "SetEvent(watchdog graceful)",
            ::GetLastError());
    }
    return {};
}

Result<void> WaitForPriorRecovery(
    const std::uint32_t session_id,
    const std::chrono::milliseconds timeout) {
    const auto mutex_name = watchdog::RecoveryMutexName(session_id);
    UniqueHandle mutex(::CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
    if (!mutex) {
        return Status::FromWin32("CreateMutexW(recovery probe)", ::GetLastError());
    }
    const DWORD wait = ::WaitForSingleObject(mutex.get(), TimeoutToDword(timeout));
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        if (wait == WAIT_TIMEOUT) {
            return Status(ErrorCode::timeout, "Previous recovery did not finish");
        }
        return Status::FromWin32("WaitForSingleObject(recovery probe)", ::GetLastError());
    }
    if (!::ReleaseMutex(mutex.get())) {
        return Status::FromWin32("ReleaseMutex(recovery probe)", ::GetLastError());
    }
    return {};
}

}  // namespace metaplasia::host
