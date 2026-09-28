#include "metaplasia/host/diagnostic_log.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>

namespace metaplasia::host {
namespace {

constexpr std::size_t kMaximumFieldBytes = 128;
constexpr std::size_t kMaximumMessageBytes = 4096;

std::string_view LevelName(const DiagnosticLevel level) noexcept {
    switch (level) {
        case DiagnosticLevel::info:
            return "info";
        case DiagnosticLevel::warning:
            return "warning";
        case DiagnosticLevel::error:
            return "error";
        default:
            return "error";
    }
}

std::string TimestampUtc() {
    SYSTEMTIME time{};
    ::GetSystemTime(&time);
    std::array<char, 32> buffer{};
    const int written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        time.wYear,
        time.wMonth,
        time.wDay,
        time.wHour,
        time.wMinute,
        time.wSecond,
        time.wMilliseconds);
    return written > 0 && static_cast<std::size_t>(written) < buffer.size()
               ? std::string(buffer.data(), static_cast<std::size_t>(written))
               : std::string("1970-01-01T00:00:00.000Z");
}

void AppendJsonString(
    std::string& output,
    const std::string_view value,
    const std::size_t maximum_bytes) {
    output.push_back('"');
    const std::size_t length = (std::min)(value.size(), maximum_bytes);
    for (std::size_t index = 0; index < length; ++index) {
        const auto character = static_cast<unsigned char>(value[index]);
        switch (character) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (character < 0x20U) {
                    constexpr char digits[] = "0123456789ABCDEF";
                    output += "\\u00";
                    output.push_back(digits[(character >> 4U) & 0x0FU]);
                    output.push_back(digits[character & 0x0FU]);
                } else {
                    output.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    if (value.size() > length) {
        output += "...";
    }
    output.push_back('"');
}

std::string BuildLine(
    const DiagnosticLevel level,
    const std::string_view component,
    const std::string_view event,
    const std::string_view target,
    const std::uint32_t process_id,
    const std::string_view message) {
    std::string output;
    output.reserve((std::min)(message.size(), kMaximumMessageBytes) + 256U);
    output += "{\"schema\":1,\"timestamp\":";
    AppendJsonString(output, TimestampUtc(), 32);
    output += ",\"level\":";
    AppendJsonString(output, LevelName(level), 16);
    output += ",\"component\":";
    AppendJsonString(output, component, kMaximumFieldBytes);
    output += ",\"event\":";
    AppendJsonString(output, event, kMaximumFieldBytes);
    output += ",\"target\":";
    AppendJsonString(output, target, kMaximumFieldBytes);
    output += ",\"processId\":";
    output += std::to_string(process_id);
    output += ",\"message\":";
    AppendJsonString(output, message, kMaximumMessageBytes);
    output += "}\n";
    return output;
}

}  // namespace

DiagnosticLog::DiagnosticLog(
    std::filesystem::path directory,
    const std::size_t maximum_bytes,
    const std::size_t backup_count,
    std::filesystem::path file_name) noexcept
    : directory_(std::move(directory)),
      path_(directory_ /
            (file_name == L"watchdog.log"
                 ? std::filesystem::path(L"watchdog.log")
                 : std::filesystem::path(L"host.log"))),
      maximum_bytes_((std::max)(maximum_bytes, std::size_t{4096})),
      backup_count_((std::min)(backup_count, std::size_t{16})) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
}

void DiagnosticLog::Write(
    const DiagnosticLevel level,
    const std::string_view component,
    const std::string_view event,
    const std::string_view target,
    const std::uint32_t process_id,
    const std::string_view message) noexcept {
    try {
        const std::string line = BuildLine(
            level,
            component,
            event,
            target,
            process_id,
            message);
        std::lock_guard lock(mutex_);
        std::error_code directory_error;
        std::filesystem::create_directories(directory_, directory_error);
        if (directory_error) {
            return;
        }
        RotateIfRequired(line.size());
        std::ofstream stream(path_, std::ios::binary | std::ios::app);
        if (!stream.is_open()) {
            return;
        }
        stream.write(line.data(), static_cast<std::streamsize>(line.size()));
        stream.flush();
    } catch (...) {
        // Diagnostics are fail-open: logging must never stop shell recovery.
    }
}

void DiagnosticLog::RotateIfRequired(
    const std::size_t incoming_bytes) noexcept {
    std::error_code error;
    const std::uintmax_t current_bytes =
        std::filesystem::exists(path_, error) && !error
            ? std::filesystem::file_size(path_, error)
            : 0;
    if (error ||
        (current_bytes < maximum_bytes_ &&
         incoming_bytes <= maximum_bytes_ - current_bytes)) {
        return;
    }

    if (backup_count_ == 0) {
        std::filesystem::remove(path_, error);
        return;
    }

    std::filesystem::remove(BackupPath(backup_count_), error);
    for (std::size_t index = backup_count_; index > 1; --index) {
        error.clear();
        const auto source = BackupPath(index - 1);
        if (!std::filesystem::exists(source, error) || error) {
            continue;
        }
        error.clear();
        std::filesystem::rename(source, BackupPath(index), error);
    }
    error.clear();
    if (std::filesystem::exists(path_, error) && !error) {
        error.clear();
        std::filesystem::rename(path_, BackupPath(1), error);
        if (error) {
            // Keep the current log intact if rotation is temporarily blocked
            // (for example by an antivirus scanner). Losing diagnostics is
            // worse than allowing the file to exceed its soft size limit.
            return;
        }
    }
}

std::filesystem::path DiagnosticLog::BackupPath(
    const std::size_t index) const {
    auto backup = path_;
    backup += L"." + std::to_wstring(index);
    return backup;
}

}  // namespace metaplasia::host
