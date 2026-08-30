#include "metaplasia/platform/process.hpp"

#include "metaplasia/base/unique_handle.hpp"

#include <TlHelp32.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace metaplasia::platform {
namespace {

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

Result<UniqueHandle> CreateModuleSnapshot(const std::uint32_t process_id) {
    // Toolhelp documents ERROR_BAD_LENGTH as a transient module-list race and
    // explicitly requires callers to retry. Explorer loads Taskbar modules in
    // bursts during sign-in, so a handful of immediate yields is not enough on
    // otherwise healthy systems. Keep the retry bounded so a broken target can
    // never stall the host monitor indefinitely.
    constexpr int kMaximumAttempts = 16;
    for (int attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        UniqueHandle snapshot(::CreateToolhelp32Snapshot(
            TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
            process_id));
        if (snapshot) {
            return snapshot;
        }
        const DWORD error = ::GetLastError();
        if (error != ERROR_BAD_LENGTH) {
            return Status::FromWin32("CreateToolhelp32Snapshot(modules)", error);
        }
        if (attempt + 1 < kMaximumAttempts) {
            // A one millisecond backoff lets the loader finish updating its
            // lists without adding meaningful latency to normal startup.
            ::Sleep(1);
        }
    }
    return Status::FromWin32(
        "CreateToolhelp32Snapshot(modules)",
        ERROR_BAD_LENGTH);
}

Result<std::vector<std::byte>> ReadTokenUser(const HANDLE token) {
    DWORD required = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (required == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return Status::FromWin32("GetTokenInformation(size)", ::GetLastError());
    }
    std::vector<std::byte> storage(required);
    if (!::GetTokenInformation(
            token,
            TokenUser,
            storage.data(),
            required,
            &required)) {
        return Status::FromWin32("GetTokenInformation", ::GetLastError());
    }
    return storage;
}

}  // namespace

Result<std::vector<ProcessInfo>> EnumerateProcesses() {
    UniqueHandle snapshot(::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return Status::FromWin32(
            "CreateToolhelp32Snapshot(processes)",
            ::GetLastError());
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Process32FirstW(snapshot.get(), &entry)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_NO_MORE_FILES) {
            return std::vector<ProcessInfo>{};
        }
        return Status::FromWin32("Process32FirstW", error);
    }

    std::vector<ProcessInfo> processes;
    do {
        ProcessInfo process;
        process.process_id = entry.th32ProcessID;
        process.image_name = entry.szExeFile;
        DWORD session_id = 0;
        if (::ProcessIdToSessionId(entry.th32ProcessID, &session_id)) {
            process.session_id = static_cast<std::uint32_t>(session_id);
        }

        UniqueHandle handle(::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE,
            entry.th32ProcessID));
        if (handle) {
            std::vector<wchar_t> path(32768);
            DWORD path_size = static_cast<DWORD>(path.size());
            if (::QueryFullProcessImageNameW(
                    handle.get(),
                    0,
                    path.data(),
                    &path_size)) {
                process.image_path =
                    std::filesystem::path(std::wstring_view(path.data(), path_size));
            }
        }
        processes.push_back(std::move(process));
    } while (::Process32NextW(snapshot.get(), &entry));

    const DWORD final_error = ::GetLastError();
    if (final_error != ERROR_NO_MORE_FILES) {
        return Status::FromWin32("Process32NextW", final_error);
    }
    return processes;
}

Result<std::vector<ProcessInfo>> FindProcessesByImageName(
    const std::wstring_view image_name,
    const std::uint32_t session_id) {
    if (image_name.empty()) {
        return Status(ErrorCode::invalid_argument, "Process image name is empty");
    }
    auto all = EnumerateProcesses();
    if (!all.ok()) {
        return all.status();
    }

    std::vector<ProcessInfo> matches;
    for (auto& process : all.value()) {
        if (process.session_id == session_id &&
            EqualsIgnoreCase(process.image_name, image_name)) {
            matches.push_back(std::move(process));
        }
    }
    return matches;
}

Result<std::vector<ProcessModuleInfo>> EnumerateProcessModules(
    const std::uint32_t process_id) {
    if (process_id == 0) {
        return Status(ErrorCode::invalid_argument, "Invalid process id");
    }
    auto snapshot = CreateModuleSnapshot(process_id);
    if (!snapshot.ok()) {
        return snapshot.status();
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Module32FirstW(snapshot.value().get(), &entry)) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_NO_MORE_FILES) {
            return std::vector<ProcessModuleInfo>{};
        }
        return Status::FromWin32("Module32FirstW", error);
    }

    std::vector<ProcessModuleInfo> modules;
    do {
        modules.push_back(ProcessModuleInfo{
            entry.szModule,
            std::filesystem::path(entry.szExePath),
            reinterpret_cast<std::uintptr_t>(entry.modBaseAddr),
            entry.modBaseSize});
    } while (::Module32NextW(snapshot.value().get(), &entry));

    const DWORD error = ::GetLastError();
    if (error != ERROR_NO_MORE_FILES) {
        return Status::FromWin32("Module32NextW", error);
    }
    return modules;
}

Result<ProcessModuleInfo> FindProcessModuleByName(
    const std::uint32_t process_id,
    const std::wstring_view module_name) {
    if (process_id == 0 || module_name.empty()) {
        return Status(ErrorCode::invalid_argument, "Invalid module lookup arguments");
    }
    auto modules = EnumerateProcessModules(process_id);
    if (!modules.ok()) {
        return modules.status();
    }
    for (auto& module : modules.value()) {
        if (EqualsIgnoreCase(module.module_name, module_name)) {
            return std::move(module);
        }
    }
    return Status(ErrorCode::not_found, "Remote module was not found");
}

Result<ProcessModuleInfo> FindProcessModuleByPath(
    const std::uint32_t process_id,
    const std::filesystem::path& module_path) {
    if (process_id == 0 || module_path.empty() || !module_path.is_absolute()) {
        return Status(ErrorCode::invalid_argument, "Invalid module lookup arguments");
    }
    auto modules = EnumerateProcessModules(process_id);
    if (!modules.ok()) {
        return modules.status();
    }
    const auto expected = module_path.lexically_normal().native();
    for (auto& module : modules.value()) {
        if (EqualsIgnoreCase(
                module.image_path.lexically_normal().native(),
                expected)) {
            return std::move(module);
        }
    }
    return Status(ErrorCode::not_found, "Remote module was not found");
}

Result<bool> IsModuleLoaded(
    const std::uint32_t process_id,
    const std::filesystem::path& module_path) {
    if (process_id == 0 || module_path.empty()) {
        return Status(ErrorCode::invalid_argument, "Invalid module lookup arguments");
    }
    auto snapshot = CreateModuleSnapshot(process_id);
    if (!snapshot.ok()) {
        return snapshot.status();
    }

    const auto expected = module_path.lexically_normal().native();
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!::Module32FirstW(snapshot.value().get(), &entry)) {
        return Status::FromWin32("Module32FirstW", ::GetLastError());
    }
    do {
        const std::filesystem::path current(entry.szExePath);
        if (EqualsIgnoreCase(current.lexically_normal().native(), expected)) {
            return true;
        }
    } while (::Module32NextW(snapshot.value().get(), &entry));

    const DWORD error = ::GetLastError();
    if (error != ERROR_NO_MORE_FILES) {
        return Status::FromWin32("Module32NextW", error);
    }
    return false;
}

Result<std::uintptr_t> FindRemoteModuleBase(
    const std::uint32_t process_id,
    const std::wstring_view module_name) {
    auto module = FindProcessModuleByName(process_id, module_name);
    if (!module.ok()) {
        return module.status();
    }
    return module.value().base_address;
}

Result<ProcessArchitecture> GetProcessArchitecture(const HANDLE process) {
    // GetCurrentProcess() is the documented pseudo handle whose bit pattern is
    // also INVALID_HANDLE_VALUE. IsWow64Process2 accepts that pseudo handle, so
    // only null is unconditionally invalid here.
    if (process == nullptr) {
        return Status(ErrorCode::invalid_argument, "Invalid process handle");
    }

    USHORT process_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    USHORT native_machine = IMAGE_FILE_MACHINE_UNKNOWN;
    if (!::IsWow64Process2(process, &process_machine, &native_machine)) {
        return Status::FromWin32("IsWow64Process2", ::GetLastError());
    }
    const USHORT effective = process_machine == IMAGE_FILE_MACHINE_UNKNOWN
                                 ? native_machine
                                 : process_machine;
    switch (effective) {
        case IMAGE_FILE_MACHINE_I386:
            return ProcessArchitecture::x86;
        case IMAGE_FILE_MACHINE_AMD64:
            return ProcessArchitecture::x64;
        case IMAGE_FILE_MACHINE_ARM64:
            return ProcessArchitecture::arm64;
        default:
            return ProcessArchitecture::unknown;
    }
}

Result<bool> IsProcessOwnedByCurrentUser(const HANDLE process) {
    HANDLE raw_target_token = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &raw_target_token)) {
        return Status::FromWin32("OpenProcessToken(target)", ::GetLastError());
    }
    UniqueHandle target_token(raw_target_token);

    HANDLE raw_current_token = nullptr;
    if (!::OpenProcessToken(
            ::GetCurrentProcess(),
            TOKEN_QUERY,
            &raw_current_token)) {
        return Status::FromWin32("OpenProcessToken(current)", ::GetLastError());
    }
    UniqueHandle current_token(raw_current_token);

    auto target_user = ReadTokenUser(target_token.get());
    if (!target_user.ok()) {
        return target_user.status();
    }
    auto current_user = ReadTokenUser(current_token.get());
    if (!current_user.ok()) {
        return current_user.status();
    }

    const auto* target =
        reinterpret_cast<const TOKEN_USER*>(target_user.value().data());
    const auto* current =
        reinterpret_cast<const TOKEN_USER*>(current_user.value().data());
    return ::EqualSid(target->User.Sid, current->User.Sid) != FALSE;
}

Result<bool> IsProtectedProcess(const HANDLE process) {
    PROCESS_PROTECTION_LEVEL_INFORMATION protection{};
    if (!::GetProcessInformation(
            process,
            ProcessProtectionLevelInfo,
            &protection,
            sizeof(protection))) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_INVALID_PARAMETER) {
            return false;
        }
        return Status::FromWin32(
            "GetProcessInformation(ProcessProtectionLevelInfo)",
            error);
    }
    return protection.ProtectionLevel != PROTECTION_LEVEL_NONE;
}

}  // namespace metaplasia::platform
