#pragma once

#include <Windows.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string_view>

namespace metaplasia::agent {

// InitializeXamlDiagnosticsEx can accept a connection before the target XAML
// service is ready to create the TAP site. Keep retries bounded and move each
// retry to a distinct system-XAML diagnostics endpoint so one abandoned
// connection cannot permanently poison the process.
class XamlDiagnosticsInitialization final {
public:
    [[nodiscard]] std::optional<std::wstring_view> BeginAttempt() noexcept {
        if (reuse_result_ || attempt_count_ >= kEndpoints.size()) {
            return std::nullopt;
        }
        return kEndpoints[attempt_count_++];
    }

    void RecordApiResult(
        const HRESULT result,
        const bool retry_allowed) noexcept {
        result_ = result;
        waiting_for_site_ = SUCCEEDED(result);
        reuse_result_ = waiting_for_site_ || !retry_allowed ||
                        attempt_count_ >= kEndpoints.size();
    }

    [[nodiscard]] bool RecordSiteTimeout(
        const bool retry_allowed) noexcept {
        if (!waiting_for_site_) {
            return false;
        }
        result_ = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        waiting_for_site_ = false;
        reuse_result_ = !retry_allowed ||
                        attempt_count_ >= kEndpoints.size();
        return !reuse_result_;
    }

    [[nodiscard]] bool reuse_result() const noexcept {
        return reuse_result_;
    }

    [[nodiscard]] HRESULT result() const noexcept { return result_; }

    [[nodiscard]] std::size_t attempt_count() const noexcept {
        return attempt_count_;
    }

    [[nodiscard]] bool waiting_for_site() const noexcept {
        return waiting_for_site_;
    }

private:
    static constexpr std::array<std::wstring_view, 4> kEndpoints{
        L"VisualDiagConnection1",
        L"VisualDiagConnection2",
        L"VisualDiagConnection3",
        L"VisualDiagConnection4"};

    std::size_t attempt_count_{0};
    HRESULT result_{E_UNEXPECTED};
    bool waiting_for_site_{false};
    bool reuse_result_{false};
};

}  // namespace metaplasia::agent
