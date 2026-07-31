#include "metaplasia/base/windows_paths.hpp"

#include "metaplasia/base/unique_handle.hpp"

#include <Windows.h>
#include <ShlObj.h>
#include <Sddl.h>

#include <array>
#include <memory>
#include <system_error>
#include <vector>

namespace metaplasia {

Result<std::filesystem::path> ExecutablePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return Status::FromWin32("GetModuleFileNameW", ::GetLastError());
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring_view(buffer.data(), length));
        }
        if (buffer.size() >= 32768) {
            return Status(ErrorCode::invalid_data, "Executable path is too long");
        }
        buffer.resize(buffer.size() * 2);
    }
}

Result<std::filesystem::path> ExecutableDirectory() {
    auto path = ExecutablePath();
    if (!path.ok()) {
        return path.status();
    }
    return path.value().parent_path();
}

Result<std::filesystem::path> LocalAppDataDirectory() {
    PWSTR raw_path = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(
        FOLDERID_LocalAppData,
        KF_FLAG_CREATE,
        nullptr,
        &raw_path);
    if (FAILED(result)) {
        return Status(
            ErrorCode::win32_error,
            "SHGetKnownFolderPath(FOLDERID_LocalAppData) failed",
            static_cast<std::uint32_t>(result));
    }
    struct CoTaskMemoryDeleter final {
        void operator()(wchar_t* pointer) const noexcept {
            ::CoTaskMemFree(pointer);
        }
    };
    const std::unique_ptr<wchar_t, CoTaskMemoryDeleter> memory(raw_path);
    const std::filesystem::path path(memory.get());
    return path;
}

Result<std::filesystem::path> MetaplasiaDataDirectory() {
    auto base = LocalAppDataDirectory();
    if (!base.ok()) {
        return base.status();
    }

    auto path = base.value() / L"Metaplasia";
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        return Status(
            ErrorCode::win32_error,
            "Unable to create Metaplasia data directory: " + error.message(),
            static_cast<std::uint32_t>(error.value()));
    }
    return path;
}

Result<std::wstring> CurrentUserSidString() {
    UniqueHandle token;
    HANDLE raw_token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        return Status::FromWin32("OpenProcessToken", ::GetLastError());
    }
    token.reset(raw_token);

    DWORD required = 0;
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
    const DWORD size_error = ::GetLastError();
    if (required == 0 || size_error != ERROR_INSUFFICIENT_BUFFER) {
        return Status::FromWin32("GetTokenInformation(size)", size_error);
    }

    std::vector<std::byte> storage(required);
    if (!::GetTokenInformation(
            token.get(), TokenUser, storage.data(), required, &required)) {
        return Status::FromWin32("GetTokenInformation", ::GetLastError());
    }

    const auto* token_user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    LPWSTR raw_sid = nullptr;
    if (!::ConvertSidToStringSidW(token_user->User.Sid, &raw_sid)) {
        return Status::FromWin32("ConvertSidToStringSidW", ::GetLastError());
    }
    UniqueLocalMemory sid_memory(reinterpret_cast<HLOCAL>(raw_sid));
    return std::wstring(raw_sid);
}

Result<std::uint32_t> CurrentSessionId() {
    DWORD session_id = 0;
    if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session_id)) {
        return Status::FromWin32("ProcessIdToSessionId", ::GetLastError());
    }
    return static_cast<std::uint32_t>(session_id);
}

}  // namespace metaplasia
