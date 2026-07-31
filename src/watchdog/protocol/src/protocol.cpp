#include "metaplasia/watchdog/protocol.hpp"

#include <limits>
#include <optional>
#include <unordered_map>

namespace metaplasia::watchdog {
namespace {

Result<std::uint32_t> ParseUint32(const std::wstring_view value) {
    if (value.empty()) {
        return Status(ErrorCode::invalid_argument, "Missing integer value");
    }
    std::uint64_t parsed = 0;
    for (const wchar_t character : value) {
        if (character < L'0' || character > L'9') {
            return Status(ErrorCode::invalid_argument, "Invalid integer value");
        }
        parsed = parsed * 10U + static_cast<unsigned int>(character - L'0');
        if (parsed > (std::numeric_limits<std::uint32_t>::max)()) {
            return Status(ErrorCode::invalid_argument, "Integer value is too large");
        }
    }
    return static_cast<std::uint32_t>(parsed);
}

bool IsLowerHexToken(const std::wstring_view token) noexcept {
    if (token.size() != kEventTokenLength) {
        return false;
    }
    for (const wchar_t character : token) {
        if (!((character >= L'0' && character <= L'9') ||
              (character >= L'a' && character <= L'f'))) {
            return false;
        }
    }
    return true;
}

Result<std::wstring_view> ReadRequired(
    const std::unordered_map<std::wstring_view, std::wstring_view>& values,
    const std::wstring_view name) {
    const auto iterator = values.find(name);
    if (iterator == values.end() || iterator->second.empty()) {
        return Status(ErrorCode::invalid_argument, "Missing watchdog argument");
    }
    return iterator->second;
}

}  // namespace

std::wstring RecoveryMutexName(const std::uint32_t session_id) {
    return L"Local\\Metaplasia.Recovery.v1." + std::to_wstring(session_id);
}

std::wstring EventNameBase(
    const std::uint32_t session_id,
    const std::uint32_t host_process_id,
    const std::wstring_view token) {
    return L"Local\\Metaplasia.Watchdog.v1." +
           std::to_wstring(session_id) + L"." +
           std::to_wstring(host_process_id) + L"." + std::wstring(token);
}

Result<void> ValidateEventNames(const LaunchArguments& arguments) {
    const std::wstring prefix =
        L"Local\\Metaplasia.Watchdog.v1." +
        std::to_wstring(arguments.session_id) + L"." +
        std::to_wstring(arguments.host_process_id) + L".";
    constexpr std::wstring_view graceful_suffix = L".graceful";
    constexpr std::wstring_view ready_suffix = L".ready";

    if (!arguments.graceful_event_name.starts_with(prefix) ||
        !arguments.graceful_event_name.ends_with(graceful_suffix) ||
        !arguments.ready_event_name.starts_with(prefix) ||
        !arguments.ready_event_name.ends_with(ready_suffix)) {
        return Status(ErrorCode::invalid_argument, "Invalid watchdog event name");
    }

    const auto graceful_token = std::wstring_view(arguments.graceful_event_name)
                                    .substr(
                                        prefix.size(),
                                        arguments.graceful_event_name.size() -
                                            prefix.size() - graceful_suffix.size());
    const auto ready_token = std::wstring_view(arguments.ready_event_name)
                                 .substr(
                                     prefix.size(),
                                     arguments.ready_event_name.size() -
                                         prefix.size() - ready_suffix.size());
    if (!IsLowerHexToken(graceful_token) || graceful_token != ready_token) {
        return Status(ErrorCode::invalid_argument, "Invalid watchdog event token");
    }
    return {};
}

Result<LaunchArguments> ParseLaunchArguments(
    const std::span<const std::wstring_view> arguments) {
    constexpr std::size_t required_argument_count = 12;
    if (arguments.size() != required_argument_count) {
        return Status(
            ErrorCode::invalid_argument,
            "Watchdog requires exactly six flag/value pairs");
    }

    std::unordered_map<std::wstring_view, std::wstring_view> values;
    for (std::size_t index = 0; index < arguments.size(); index += 2) {
        const auto name = arguments[index];
        const auto value = arguments[index + 1];
        if (name != L"--host-pid" && name != L"--session-id" &&
            name != L"--graceful-event" && name != L"--ready-event" &&
            name != L"--agent-path" && name != L"--settings-path") {
            return Status(ErrorCode::invalid_argument, "Unknown watchdog argument");
        }
        if (value.empty() || !values.emplace(name, value).second) {
            return Status(
                ErrorCode::invalid_argument,
                "Duplicate or empty watchdog argument");
        }
    }

    auto host_pid_text = ReadRequired(values, L"--host-pid");
    auto session_id_text = ReadRequired(values, L"--session-id");
    auto graceful_event = ReadRequired(values, L"--graceful-event");
    auto ready_event = ReadRequired(values, L"--ready-event");
    auto agent_path = ReadRequired(values, L"--agent-path");
    auto settings_path = ReadRequired(values, L"--settings-path");
    if (!host_pid_text.ok() || !session_id_text.ok() ||
        !graceful_event.ok() || !ready_event.ok() || !agent_path.ok() ||
        !settings_path.ok()) {
        return Status(ErrorCode::invalid_argument, "Missing watchdog argument");
    }

    auto host_pid = ParseUint32(host_pid_text.value());
    auto session_id = ParseUint32(session_id_text.value());
    if (!host_pid.ok() || host_pid.value() == 0 || !session_id.ok()) {
        return Status(ErrorCode::invalid_argument, "Invalid watchdog process identity");
    }

    LaunchArguments parsed;
    parsed.host_process_id = host_pid.value();
    parsed.session_id = session_id.value();
    parsed.graceful_event_name = graceful_event.value();
    parsed.ready_event_name = ready_event.value();
    parsed.agent_path = std::filesystem::path(agent_path.value()).lexically_normal();
    parsed.settings_path =
        std::filesystem::path(settings_path.value()).lexically_normal();
    if (!parsed.agent_path.is_absolute() ||
        !parsed.settings_path.is_absolute()) {
        return Status(ErrorCode::invalid_argument, "Watchdog paths must be absolute");
    }
    auto valid_events = ValidateEventNames(parsed);
    if (!valid_events.ok()) {
        return valid_events.status();
    }
    return parsed;
}

}  // namespace metaplasia::watchdog
