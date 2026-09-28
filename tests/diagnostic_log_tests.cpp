#include "metaplasia/host/diagnostic_log.hpp"

#include <Windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
}

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    const auto directory = std::filesystem::temp_directory_path() /
        (L"MetaplasiaDiagnosticLogTests-" +
         std::to_wstring(::GetCurrentProcessId()));
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);

    {
        metaplasia::host::DiagnosticLog log(directory, 4096, 2);
        log.Write(
            metaplasia::host::DiagnosticLevel::error,
            "engine",
            "configure-failed",
            "taskbar",
            42,
            "quoted \"message\"\nsecond line");
        const std::string first = ReadFile(log.path());
        Require(
            first.find("\"schema\":1") != std::string::npos,
            "schema field");
        Require(
            first.find("\"level\":\"error\"") != std::string::npos,
            "level field");
        Require(
            first.find("\"processId\":42") != std::string::npos,
            "process ID field");
        Require(
            first.find("quoted \\\"message\\\"\\nsecond line") !=
                std::string::npos,
            "JSON escaping");

        const std::string payload(3900, 'x');
        log.Write(
            metaplasia::host::DiagnosticLevel::info,
            "host",
            "large-entry",
            "host",
            0,
            payload);
        log.Write(
            metaplasia::host::DiagnosticLevel::warning,
            "host",
            "rotate",
            "host",
            0,
            payload);
        Require(
            std::filesystem::is_regular_file(log.path()),
            "current log after rotation");
        Require(
            std::filesystem::is_regular_file(
                std::filesystem::path(log.path().wstring() + L".1")),
            "rotated log backup");
    }

    {
        metaplasia::host::DiagnosticLog watchdog_log(
            directory,
            4096,
            1,
            L"watchdog.log");
        watchdog_log.Write(
            metaplasia::host::DiagnosticLevel::info,
            "watchdog",
            "startup",
            "host",
            73,
            "Watchdog startup began");
        Require(
            watchdog_log.path().filename() == L"watchdog.log",
            "custom log filename");
        Require(
            std::filesystem::is_regular_file(watchdog_log.path()),
            "custom log file");
    }

    {
        metaplasia::host::DiagnosticLog rejected_name(
            directory,
            4096,
            1,
            L"..");
        Require(
            rejected_name.path().filename() == L"host.log" &&
                rejected_name.path().parent_path() == directory,
            "reject a diagnostic log filename outside the fixed allowlist");
    }

    std::filesystem::remove_all(directory, cleanup_error);
    return 0;
}
