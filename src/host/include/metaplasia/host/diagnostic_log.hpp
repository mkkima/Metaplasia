#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string_view>

namespace metaplasia::host {

enum class DiagnosticLevel : std::uint8_t {
    info,
    warning,
    error,
};

class DiagnosticLog final {
public:
    static constexpr std::size_t kDefaultMaximumBytes = 1024U * 1024U;
    static constexpr std::size_t kDefaultBackupCount = 3;

    explicit DiagnosticLog(
        std::filesystem::path directory,
        std::size_t maximum_bytes = kDefaultMaximumBytes,
        std::size_t backup_count = kDefaultBackupCount) noexcept;

    DiagnosticLog(const DiagnosticLog&) = delete;
    DiagnosticLog& operator=(const DiagnosticLog&) = delete;

    void Write(
        DiagnosticLevel level,
        std::string_view component,
        std::string_view event,
        std::string_view target,
        std::uint32_t process_id,
        std::string_view message) noexcept;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    void RotateIfRequired(std::size_t incoming_bytes) noexcept;
    [[nodiscard]] std::filesystem::path BackupPath(
        std::size_t index) const;

    std::filesystem::path directory_;
    std::filesystem::path path_;
    std::size_t maximum_bytes_;
    std::size_t backup_count_;
    std::mutex mutex_;
};

}  // namespace metaplasia::host
