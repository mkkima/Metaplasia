#pragma once

#include <chrono>

namespace metaplasia::host {

inline constexpr auto kStartMenuInitializationGrace =
    std::chrono::seconds(5);

[[nodiscard]] constexpr bool ShouldDeferStartMenuInitialization(
    const bool process_running,
    const bool start_menu_process,
    const bool customization_requested,
    const std::chrono::steady_clock::time_point observed_at,
    const std::chrono::steady_clock::time_point now) noexcept {
    return process_running && start_menu_process && customization_requested &&
           now >= observed_at &&
           now - observed_at < kStartMenuInitializationGrace;
}

}  // namespace metaplasia::host
