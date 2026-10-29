#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace metaplasia::agent {

// Percent-based policy is separate from HWND/COM ownership and is deterministic
// under tests. No screenshots, content snapshots, or synthetic pointer input.
[[nodiscard]] inline std::optional<double> SmoothWheelPercent(
    const int delta, const std::uint32_t lines, const double view_percent,
    const double viewport_pixels, const double row_pixels) noexcept {
    if (delta == 0 || delta % 120 != 0 || lines == 0 ||
        !std::isfinite(view_percent) || view_percent <= 0 || view_percent >= 100 ||
        !std::isfinite(viewport_pixels) || viewport_pixels <= 0 ||
        !std::isfinite(row_pixels) || row_pixels <= 0) {
        return std::nullopt;
    }
    const double pixels = lines == (std::numeric_limits<std::uint32_t>::max)()
        ? (std::max)(row_pixels, viewport_pixels - row_pixels)
        : row_pixels * static_cast<double>(lines);
    const double range = viewport_pixels * (100.0 / view_percent - 1.0);
    if (!std::isfinite(range) || range <= 0) { return std::nullopt; }
    return std::clamp(-static_cast<double>(delta) / 120.0 * pixels / range * 100.0,
                      -100.0, 100.0);
}

class SmoothScrollAnimation final {
public:
    static constexpr std::uint64_t kDurationMilliseconds = 160;

    [[nodiscard]] double Position(const std::uint64_t now) const noexcept {
        const auto elapsed = now >= started_ ? now - started_ : 0;
        const double t = std::clamp(static_cast<double>(elapsed) /
                                   static_cast<double>(kDurationMilliseconds), 0.0, 1.0);
        const double remaining = 1.0 - t;
        return start_ + (target_ - start_) * (1.0 - remaining * remaining * remaining);
    }

    [[nodiscard]] bool Active(const std::uint64_t now) const noexcept {
        return active_ && (now < started_ || now - started_ < kDurationMilliseconds);
    }

    [[nodiscard]] bool Retarget(const double actual, const double delta,
                                const std::uint64_t now) noexcept {
        if (!std::isfinite(actual) || actual < 0 || actual > 100 ||
            !std::isfinite(delta) || delta == 0) { return false; }
        const double current = Active(now) ? Position(now) : actual;
        const double pending = target_ - current;
        // Reversing the wheel stops the old pending direction immediately.
        const double base = Active(now) && pending * delta > 0 ? target_ : current;
        start_ = current;
        target_ = std::clamp(base + delta, 0.0, 100.0);
        started_ = now;
        active_ = std::abs(target_ - start_) > 0.000001;
        return active_;
    }

    void Cancel() noexcept { active_ = false; }
    [[nodiscard]] double Target() const noexcept { return target_; }

private:
    double start_{0};
    double target_{0};
    std::uint64_t started_{0};
    bool active_{false};
};

} // namespace metaplasia::agent
