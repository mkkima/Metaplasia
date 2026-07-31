#pragma once

#include "metaplasia/base/status.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace metaplasia::watchdog {

inline constexpr std::size_t kEventTokenLength = 32;

struct LaunchArguments final {
    std::uint32_t host_process_id{0};
    std::uint32_t session_id{0};
    std::wstring graceful_event_name;
    std::wstring ready_event_name;
    std::filesystem::path agent_path;
    std::filesystem::path settings_path;
};

[[nodiscard]] std::wstring RecoveryMutexName(std::uint32_t session_id);

[[nodiscard]] std::wstring EventNameBase(
    std::uint32_t session_id,
    std::uint32_t host_process_id,
    std::wstring_view token);

[[nodiscard]] Result<void> ValidateEventNames(
    const LaunchArguments& arguments);

// Parses flag/value pairs without relying on the CRT command-line quoting
// implementation. The executable name is not included in `arguments`.
[[nodiscard]] Result<LaunchArguments> ParseLaunchArguments(
    std::span<const std::wstring_view> arguments);

}  // namespace metaplasia::watchdog
