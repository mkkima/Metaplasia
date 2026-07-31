#pragma once

#include "metaplasia/base/status.hpp"

#include <Windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace metaplasia::platform {

struct ProcessInfo final {
    std::uint32_t process_id{0};
    std::uint32_t session_id{0};
    std::wstring image_name;
    std::filesystem::path image_path;
};

struct ProcessModuleInfo final {
    std::wstring module_name;
    std::filesystem::path image_path;
    std::uintptr_t base_address{0};
    std::uint32_t image_size{0};
};

enum class ProcessArchitecture {
    unknown,
    x86,
    x64,
    arm64,
};

[[nodiscard]] Result<std::vector<ProcessInfo>> EnumerateProcesses();
[[nodiscard]] Result<std::vector<ProcessInfo>> FindProcessesByImageName(
    std::wstring_view image_name,
    std::uint32_t session_id);
[[nodiscard]] Result<std::vector<ProcessModuleInfo>> EnumerateProcessModules(
    std::uint32_t process_id);
[[nodiscard]] Result<ProcessModuleInfo> FindProcessModuleByName(
    std::uint32_t process_id,
    std::wstring_view module_name);
[[nodiscard]] Result<ProcessModuleInfo> FindProcessModuleByPath(
    std::uint32_t process_id,
    const std::filesystem::path& module_path);
[[nodiscard]] Result<bool> IsModuleLoaded(
    std::uint32_t process_id,
    const std::filesystem::path& module_path);
[[nodiscard]] Result<std::uintptr_t> FindRemoteModuleBase(
    std::uint32_t process_id,
    std::wstring_view module_name);
[[nodiscard]] Result<ProcessArchitecture> GetProcessArchitecture(HANDLE process);
[[nodiscard]] Result<bool> IsProcessOwnedByCurrentUser(HANDLE process);
[[nodiscard]] Result<bool> IsProtectedProcess(HANDLE process);

}  // namespace metaplasia::platform
