#include "metaplasia/watchdog/protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::vector<std::wstring> ValidArguments() {
    constexpr std::wstring_view token =
        L"0123456789abcdef0123456789abcdef";
    const auto event_base =
        metaplasia::watchdog::EventNameBase(3, 4242, token);
    return {
        L"--host-pid",
        L"4242",
        L"--session-id",
        L"3",
        L"--graceful-event",
        event_base + L".graceful",
        L"--ready-event",
        event_base + L".ready",
        L"--agent-path",
        L"C:\\Program Files\\Metaplasia\\metaplasia-agent.dll",
        L"--settings-path",
        L"C:\\Users\\Test\\AppData\\Local\\Metaplasia\\settings.conf"};
}

metaplasia::Result<metaplasia::watchdog::LaunchArguments> Parse(
    const std::vector<std::wstring>& storage) {
    std::vector<std::wstring_view> views;
    views.reserve(storage.size());
    for (const auto& argument : storage) {
        views.emplace_back(argument);
    }
    return metaplasia::watchdog::ParseLaunchArguments(views);
}

}  // namespace

int main() {
    using metaplasia::ErrorCode;

    auto arguments = ValidArguments();
    const auto parsed = Parse(arguments);
    Require(parsed.ok(), "valid watchdog arguments accepted");
    Require(parsed.value().host_process_id == 4242, "host pid parsed");
    Require(parsed.value().session_id == 3, "session id parsed");
    Require(parsed.value().agent_path.is_absolute(), "agent path remains absolute");
    Require(
        metaplasia::watchdog::RecoveryMutexName(3) ==
            L"Local\\Metaplasia.Recovery.v1.3",
        "recovery mutex is session scoped");

    auto reordered = ValidArguments();
    for (std::size_t index = 0; index < 4; ++index) {
        std::swap(reordered[index], reordered[index + 8]);
    }
    Require(Parse(reordered).ok(), "flag order is irrelevant");

    auto duplicate = ValidArguments();
    duplicate[2] = L"--host-pid";
    const auto duplicate_result = Parse(duplicate);
    Require(!duplicate_result.ok(), "duplicate argument rejected");
    Require(
        duplicate_result.status().code() == ErrorCode::invalid_argument,
        "duplicate reports invalid argument");

    auto relative_path = ValidArguments();
    relative_path[9] = L"metaplasia-agent.dll";
    Require(!Parse(relative_path).ok(), "relative agent path rejected");

    auto mismatched_token = ValidArguments();
    mismatched_token[7] =
        L"Local\\Metaplasia.Watchdog.v1.3.4242."
        L"fedcba9876543210fedcba9876543210.ready";
    Require(!Parse(mismatched_token).ok(), "mismatched event token rejected");

    auto uppercase_token = ValidArguments();
    uppercase_token[5] =
        L"Local\\Metaplasia.Watchdog.v1.3.4242."
        L"0123456789ABCDEF0123456789ABCDEF.graceful";
    Require(!Parse(uppercase_token).ok(), "non-canonical event token rejected");

    auto invalid_pid = ValidArguments();
    invalid_pid[1] = L"4294967296";
    Require(!Parse(invalid_pid).ok(), "overflowing pid rejected");

    auto unknown = ValidArguments();
    unknown[0] = L"--surprise";
    Require(!Parse(unknown).ok(), "unknown flag rejected");

    arguments.pop_back();
    Require(!Parse(arguments).ok(), "missing value rejected");

    std::cout << "Watchdog protocol tests passed\n";
    return EXIT_SUCCESS;
}
