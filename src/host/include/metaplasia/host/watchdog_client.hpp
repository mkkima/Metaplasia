#pragma once

#include "metaplasia/base/status.hpp"
#include "metaplasia/base/unique_handle.hpp"

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace metaplasia::host {

class WatchdogClient final {
public:
    WatchdogClient() = default;
    ~WatchdogClient() = default;

    WatchdogClient(const WatchdogClient&) = delete;
    WatchdogClient& operator=(const WatchdogClient&) = delete;
    WatchdogClient(WatchdogClient&&) noexcept = default;
    WatchdogClient& operator=(WatchdogClient&&) noexcept = default;

    [[nodiscard]] static Result<WatchdogClient> Start(
        const std::filesystem::path& watchdog_path,
        const std::filesystem::path& agent_path,
        const std::filesystem::path& settings_path,
        std::uint32_t session_id,
        std::chrono::milliseconds startup_timeout);

    [[nodiscard]] HANDLE process_handle() const noexcept {
        return process_.get();
    }
    [[nodiscard]] HANDLE graceful_event_handle() const noexcept {
        return graceful_event_.get();
    }

    [[nodiscard]] Result<void> SignalGracefulShutdown() noexcept;

private:
    WatchdogClient(
        UniqueHandle process,
        UniqueHandle graceful_event,
        UniqueHandle ready_event) noexcept;

    UniqueHandle process_;
    UniqueHandle graceful_event_;
    UniqueHandle ready_event_;
};

// A watchdog owns this mutex from handshake until it finishes recovery. A new
// host waits here before reading settings, preventing a stale enabled snapshot
// from racing with the previous watchdog's safe-mode write.
[[nodiscard]] Result<void> WaitForPriorRecovery(
    std::uint32_t session_id,
    std::chrono::milliseconds timeout);

}  // namespace metaplasia::host
