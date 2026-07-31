#include "metaplasia/host/settings_store.hpp"

#include "metaplasia/base/utf.hpp"
#include "metaplasia/base/unique_handle.hpp"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>

namespace metaplasia::host {
namespace {

constexpr std::size_t kMaximumSettingsSize = 64U * 1024U;
constexpr std::string_view kHexDigits = "0123456789ABCDEF";

std::string_view Trim(std::string_view value) noexcept {
    constexpr std::string_view whitespace = " \t\r\n";
    const auto begin = value.find_first_not_of(whitespace);
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = value.find_last_not_of(whitespace);
    return value.substr(begin, end - begin + 1);
}

Result<bool> ParseBoolean(const std::string_view value) {
    if (value == "0" || value == "false") {
        return false;
    }
    if (value == "1" || value == "true") {
        return true;
    }
    return Status(ErrorCode::invalid_data, "Invalid boolean in settings file");
}

Result<std::uint32_t> ParseUnsigned(const std::string_view value) {
    std::uint32_t parsed = 0;
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed,
        10);
    if (value.empty() || result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        return Status(ErrorCode::invalid_data, "Invalid unsigned integer");
    }
    return parsed;
}

std::string EncodeHex(const std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size() * 2U);
    for (const unsigned char byte : value) {
        encoded.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
        encoded.push_back(kHexDigits[byte & 0x0FU]);
    }
    return encoded;
}

int HexValue(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

Result<std::string> DecodeHex(const std::string_view value) {
    if ((value.size() % 2U) != 0U) {
        return Status(ErrorCode::invalid_data, "Invalid hexadecimal string");
    }
    std::string decoded;
    decoded.reserve(value.size() / 2U);
    for (std::size_t index = 0; index < value.size(); index += 2U) {
        const int high = HexValue(value[index]);
        const int low = HexValue(value[index + 1U]);
        if (high < 0 || low < 0) {
            return Status(ErrorCode::invalid_data, "Invalid hexadecimal string");
        }
        decoded.push_back(static_cast<char>((high << 4U) | low));
    }
    return decoded;
}

Result<void> ValidatePrefix(
    const std::string_view value,
    const std::size_t maximum_utf16_units,
    const std::string_view name) {
    auto wide = Utf8ToWide(value);
    if (!wide.ok()) {
        return Status(
            ErrorCode::invalid_data,
            std::string(name) + " must be valid UTF-8");
    }
    if (wide.value().size() > maximum_utf16_units) {
        return Status(
            ErrorCode::invalid_argument,
            std::string(name) + " is too long");
    }
    const auto forbidden = std::find_if(
        wide.value().begin(),
        wide.value().end(),
        [](const wchar_t value) {
            return value == L'\0' || value < L' ' || value == 0x7F;
        });
    if (forbidden != wide.value().end()) {
        return Status(
            ErrorCode::invalid_argument,
            std::string(name) + " contains a control character");
    }
    return {};
}

Result<void> WriteAll(const HANDLE file, const std::string_view contents) {
    std::size_t offset = 0;
    while (offset < contents.size()) {
        const DWORD to_write = static_cast<DWORD>((std::min)(
            contents.size() - offset,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!::WriteFile(
                file,
                contents.data() + offset,
                to_write,
                &written,
                nullptr)) {
            return Status::FromWin32("WriteFile(settings)", ::GetLastError());
        }
        if (written == 0) {
            return Status(ErrorCode::internal_error, "Short write to settings file");
        }
        offset += written;
    }
    return {};
}

}  // namespace

bool HostSettings::Enabled(const protocol::TargetId target) const noexcept {
    switch (target) {
        case protocol::TargetId::taskbar:
            return taskbar_enabled;
        case protocol::TargetId::file_explorer:
            return file_explorer_enabled;
        case protocol::TargetId::start_menu:
            return start_menu_enabled;
        default:
            return false;
    }
}

bool HostSettings::SetEnabled(
    const protocol::TargetId target,
    const bool enabled) noexcept {
    switch (target) {
        case protocol::TargetId::taskbar:
            taskbar_enabled = enabled;
            return true;
        case protocol::TargetId::file_explorer:
            file_explorer_enabled = enabled;
            return true;
        case protocol::TargetId::start_menu:
            start_menu_enabled = enabled;
            return true;
        default:
            return false;
    }
}

Result<bool> HostSettings::SetCustomization(
    const protocol::SetCustomizationRequest& request) {
    bool changed = false;
    switch (request.customization) {
        case protocol::CustomizationId::taskbar_clock_prefix:
            changed = taskbar_clock_prefix != request.text_value;
            taskbar_clock_prefix = request.text_value;
            break;
        case protocol::CustomizationId::taskbar_opacity_milli:
            changed = taskbar_opacity_milli != request.integer_value;
            taskbar_opacity_milli = request.integer_value;
            break;
        case protocol::CustomizationId::taskbar_hide_notification_center:
            changed = taskbar_hide_notification_center != request.boolean_value;
            taskbar_hide_notification_center = request.boolean_value;
            break;
        case protocol::CustomizationId::taskbar_hide_control_center:
            changed = taskbar_hide_control_center != request.boolean_value;
            taskbar_hide_control_center = request.boolean_value;
            break;
        case protocol::CustomizationId::taskbar_hide_show_desktop:
            changed = taskbar_hide_show_desktop != request.boolean_value;
            taskbar_hide_show_desktop = request.boolean_value;
            break;
        case protocol::CustomizationId::taskbar_capsule_enabled:
            changed = taskbar_capsule_enabled != request.boolean_value;
            taskbar_capsule_enabled = request.boolean_value;
            break;
        case protocol::CustomizationId::taskbar_background_color_enabled:
            changed = taskbar_background_color_enabled != request.boolean_value;
            taskbar_background_color_enabled = request.boolean_value;
            break;
        case protocol::CustomizationId::taskbar_background_color:
            changed = taskbar_background_color != request.integer_value;
            taskbar_background_color = request.integer_value;
            break;
        case protocol::CustomizationId::file_explorer_title_prefix:
            changed = file_explorer_title_prefix != request.text_value;
            file_explorer_title_prefix = request.text_value;
            break;
        case protocol::CustomizationId::file_explorer_background_color_enabled:
            changed =
                file_explorer_background_color_enabled != request.boolean_value;
            file_explorer_background_color_enabled = request.boolean_value;
            break;
        case protocol::CustomizationId::file_explorer_background_color:
            changed = file_explorer_background_color != request.integer_value;
            file_explorer_background_color = request.integer_value;
            break;
        case protocol::CustomizationId::file_explorer_transition_animation:
            changed =
                file_explorer_transition_animation != request.integer_value;
            file_explorer_transition_animation = request.integer_value;
            break;
        case protocol::CustomizationId::file_explorer_custom_scrollbar_enabled:
            changed = file_explorer_custom_scrollbar_enabled !=
                request.boolean_value;
            file_explorer_custom_scrollbar_enabled = request.boolean_value;
            break;
        case protocol::CustomizationId::start_menu_opacity_milli:
            changed = start_menu_opacity_milli != request.integer_value;
            start_menu_opacity_milli = request.integer_value;
            break;
        case protocol::CustomizationId::start_menu_hide_recommended:
            changed =
                start_menu_hide_recommended != request.boolean_value;
            start_menu_hide_recommended = request.boolean_value;
            break;
        case protocol::CustomizationId::start_menu_background_color_enabled:
            changed =
                start_menu_background_color_enabled != request.boolean_value;
            start_menu_background_color_enabled = request.boolean_value;
            break;
        case protocol::CustomizationId::start_menu_background_color:
            changed = start_menu_background_color != request.integer_value;
            start_menu_background_color = request.integer_value;
            break;
        default:
            return Status(ErrorCode::invalid_argument, "Unknown customization");
    }
    auto valid = Validate();
    if (!valid.ok()) {
        return valid.status();
    }
    return changed;
}

Result<void> HostSettings::Validate() const {
    const protocol::CustomizationSettings settings = ToProtocol();
    if (!protocol::IsValidCustomizationSettings(settings)) {
        return Status(ErrorCode::invalid_argument, "Customization settings are out of range");
    }
    constexpr std::uint32_t opaque_alpha = 0xFF000000U;
    if ((taskbar_background_color & opaque_alpha) != opaque_alpha ||
        (file_explorer_background_color & opaque_alpha) != opaque_alpha ||
        (start_menu_background_color & opaque_alpha) != opaque_alpha) {
        return Status(
            ErrorCode::invalid_argument,
            "Shell background colors must be opaque ARGB values");
    }
    auto clock = ValidatePrefix(
        taskbar_clock_prefix,
        protocol::kMaximumClockPrefixLength,
        "Taskbar clock prefix");
    if (!clock.ok()) {
        return clock.status();
    }
    return ValidatePrefix(
        file_explorer_title_prefix,
        protocol::kMaximumExplorerTitlePrefixLength,
        "File Explorer title prefix");
}

protocol::CustomizationSettings HostSettings::ToProtocol() const {
    return protocol::CustomizationSettings{
        taskbar_enabled,
        file_explorer_enabled,
        start_menu_enabled,
        taskbar_clock_prefix,
        taskbar_opacity_milli,
        taskbar_hide_notification_center,
        taskbar_hide_control_center,
        taskbar_hide_show_desktop,
        taskbar_capsule_enabled,
        taskbar_background_color_enabled,
        taskbar_background_color,
        file_explorer_title_prefix,
        file_explorer_background_color_enabled,
        file_explorer_background_color,
        file_explorer_transition_animation,
        file_explorer_custom_scrollbar_enabled,
        start_menu_opacity_milli,
        start_menu_hide_recommended,
        start_menu_background_color_enabled,
        start_menu_background_color};
}

SettingsStore::SettingsStore(std::filesystem::path path)
    : path_(std::move(path)) {}

Result<HostSettings> SettingsStore::Load() const {
    std::error_code error;
    if (!std::filesystem::exists(path_, error)) {
        if (error) {
            return Status(
                ErrorCode::win32_error,
                "Unable to inspect settings file: " + error.message(),
                static_cast<std::uint32_t>(error.value()));
        }
        return HostSettings{};
    }
    const auto file_size = std::filesystem::file_size(path_, error);
    if (error) {
        return Status(
            ErrorCode::win32_error,
            "Unable to get settings file size: " + error.message(),
            static_cast<std::uint32_t>(error.value()));
    }
    if (file_size > kMaximumSettingsSize) {
        return Status(ErrorCode::invalid_data, "Settings file is too large");
    }

    std::ifstream stream(path_, std::ios::binary);
    if (!stream) {
        return Status(ErrorCode::access_denied, "Unable to open settings file");
    }
    std::string contents{
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>()};
    if (!stream.eof() && stream.fail()) {
        return Status(ErrorCode::invalid_data, "Unable to read settings file");
    }

    HostSettings settings;
    bool version_seen = false;
    std::uint32_t settings_version = 0;
    std::unordered_map<std::string, bool> seen;
    std::istringstream lines(contents);
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        auto trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == '#') {
            continue;
        }
        const auto separator = trimmed.find('=');
        if (separator == std::string_view::npos) {
            return Status(
                ErrorCode::invalid_data,
                "Invalid settings line " + std::to_string(line_number));
        }
        const std::string key(Trim(trimmed.substr(0, separator)));
        const auto value = Trim(trimmed.substr(separator + 1));
        const bool empty_hex_value =
            (key == "taskbar_clock_prefix_utf8_hex" ||
             key == "file_explorer_title_prefix_utf8_hex") &&
            value.empty();
        if (key.empty() || (!empty_hex_value && value.empty()) ||
            seen.contains(key)) {
            return Status(
                ErrorCode::invalid_data,
                "Invalid or duplicate settings key on line " +
                    std::to_string(line_number));
        }
        seen.emplace(key, true);

        if (key == "version") {
            version_seen = true;
            auto parsed = ParseUnsigned(value);
            if (!parsed.ok() || parsed.value() < 1U || parsed.value() > 7U) {
                return Status(
                    ErrorCode::incompatible,
                    "Unsupported settings format version");
            }
            settings_version = parsed.value();
            continue;
        }

        if (key == "taskbar_clock_prefix_utf8_hex") {
            auto decoded = DecodeHex(value);
            if (!decoded.ok()) {
                return Status(
                    decoded.status().code(),
                    decoded.status().message() + " on line " +
                        std::to_string(line_number));
            }
            settings.taskbar_clock_prefix = std::move(decoded).value();
            continue;
        }
        if (key == "file_explorer_title_prefix_utf8_hex") {
            auto decoded = DecodeHex(value);
            if (!decoded.ok()) {
                return Status(
                    decoded.status().code(),
                    decoded.status().message() + " on line " +
                        std::to_string(line_number));
            }
            settings.file_explorer_title_prefix = std::move(decoded).value();
            continue;
        }
        if (key == "taskbar_opacity_milli") {
            auto parsed = ParseUnsigned(value);
            if (!parsed.ok()) {
                return Status(
                    parsed.status().code(),
                    parsed.status().message() + " on line " +
                        std::to_string(line_number));
            }
            settings.taskbar_opacity_milli = parsed.value();
            continue;
        }
        if (key == "taskbar_hide_notification_center" ||
            key == "taskbar_hide_control_center" ||
            key == "taskbar_hide_show_desktop" ||
            key == "taskbar_capsule_enabled" ||
            key == "taskbar_background_color_enabled") {
            auto parsed = ParseBoolean(value);
            if (!parsed.ok()) {
                return Status(
                    parsed.status().code(),
                    parsed.status().message() + " on line " +
                        std::to_string(line_number));
            }
            if (key == "taskbar_hide_notification_center") {
                settings.taskbar_hide_notification_center = parsed.value();
            } else if (key == "taskbar_hide_control_center") {
                settings.taskbar_hide_control_center = parsed.value();
            } else if (key == "taskbar_hide_show_desktop") {
                settings.taskbar_hide_show_desktop = parsed.value();
            } else if (key == "taskbar_capsule_enabled") {
                settings.taskbar_capsule_enabled = parsed.value();
            } else {
                settings.taskbar_background_color_enabled = parsed.value();
            }
            continue;
        }
        if (key == "taskbar_background_color" ||
            key == "file_explorer_background_color" ||
            key == "start_menu_background_color") {
            auto parsed = ParseUnsigned(value);
            if (!parsed.ok()) {
                return Status(
                    parsed.status().code(),
                    parsed.status().message() + " on line " +
                        std::to_string(line_number));
            }
            if (key == "taskbar_background_color") {
                settings.taskbar_background_color = parsed.value();
            } else if (key == "file_explorer_background_color") {
                settings.file_explorer_background_color = parsed.value();
            } else {
                settings.start_menu_background_color = parsed.value();
            }
            continue;
        }
        if (key == "file_explorer_background_color_enabled" ||
            key == "start_menu_background_color_enabled") {
            auto parsed = ParseBoolean(value);
            if (!parsed.ok()) {
                return Status(
                    parsed.status().code(),
                    parsed.status().message() + " on line " +
                        std::to_string(line_number));
            }
            if (key == "file_explorer_background_color_enabled") {
                settings.file_explorer_background_color_enabled =
                    parsed.value();
            } else {
                settings.start_menu_background_color_enabled = parsed.value();
            }
            continue;
        }
        if (key == "file_explorer_transition_animation") {
            auto parsed = ParseUnsigned(value);
            if (!parsed.ok()) {
                return Status(
                    parsed.status().code(),
                    parsed.status().message() + " on line " +
                        std::to_string(line_number));
            }
            settings.file_explorer_transition_animation = parsed.value();
            continue;
        }
        if (key == "file_explorer_custom_scrollbar_enabled") {
            auto parsed = ParseBoolean(value);
            if (!parsed.ok()) {
                return Status(
                    parsed.status().code(),
                    parsed.status().message() + " on line " +
                        std::to_string(line_number));
            }
            settings.file_explorer_custom_scrollbar_enabled = parsed.value();
            continue;
        }
        if (key == "start_menu_opacity_milli") {
            auto parsed = ParseUnsigned(value);
            if (!parsed.ok()) {
                return Status(
                    parsed.status().code(),
                    parsed.status().message() + " on line " +
                        std::to_string(line_number));
            }
            settings.start_menu_opacity_milli = parsed.value();
            continue;
        }
        if (key == "start_menu_hide_recommended") {
            auto parsed = ParseBoolean(value);
            if (!parsed.ok()) {
                return Status(
                    parsed.status().code(),
                    parsed.status().message() + " on line " +
                        std::to_string(line_number));
            }
            settings.start_menu_hide_recommended = parsed.value();
            continue;
        }

        if (key != "taskbar_enabled" && key != "file_explorer_enabled" &&
            key != "start_menu_enabled") {
            continue;
        }

        auto parsed = ParseBoolean(value);
        if (!parsed.ok()) {
            return Status(
                parsed.status().code(),
                parsed.status().message() + " on line " +
                    std::to_string(line_number));
        }
        if (key == "taskbar_enabled") {
            settings.taskbar_enabled = parsed.value();
        } else if (key == "file_explorer_enabled") {
            settings.file_explorer_enabled = parsed.value();
        } else if (key == "start_menu_enabled") {
            settings.start_menu_enabled = parsed.value();
        }
        // Unknown keys are ignored so a newer writer can remain readable by
        // this version. Saving with this version intentionally emits only the
        // keys it understands.
    }
    if (!version_seen) {
        return Status(ErrorCode::invalid_data, "Settings version is missing");
    }
    if (settings_version == 1U) {
        // Version 1 contained only enable flags. The in-memory defaults migrate
        // its three customization values without rewriting the file on read.
        settings.taskbar_clock_prefix = "M ";
        settings.file_explorer_title_prefix = "Meta \xC2\xB7 ";
        settings.start_menu_opacity_milli =
            protocol::kDefaultStartMenuOpacityMilli;
        settings.start_menu_hide_recommended = false;
    }
    if (settings_version < 3U) {
        // Version 3 introduced Taskbar XAML settings. Older files migrate to
        // visually neutral defaults without being rewritten during Load().
        settings.taskbar_opacity_milli =
            protocol::kDefaultTaskbarOpacityMilli;
        settings.taskbar_hide_notification_center = false;
        settings.taskbar_hide_control_center = false;
        settings.taskbar_hide_show_desktop = false;
    }
    if (settings_version < 4U) {
        settings.taskbar_background_color_enabled = false;
        settings.taskbar_background_color =
            protocol::kDefaultShellBackgroundColor;
        settings.file_explorer_background_color_enabled = false;
        settings.file_explorer_background_color =
            protocol::kDefaultShellBackgroundColor;
        settings.start_menu_background_color_enabled = false;
        settings.start_menu_background_color =
            protocol::kDefaultShellBackgroundColor;
    }
    if (settings_version < 5U) {
        settings.file_explorer_transition_animation =
            protocol::kDefaultExplorerTransition;
    }
    if (settings_version < 6U) {
        // Version 5 always applied Metaplasia's Explorer scrollbar whenever
        // the custom Explorer color was active. Preserve that behavior.
        settings.file_explorer_custom_scrollbar_enabled = true;
    }
    if (settings_version < 7U) {
        settings.taskbar_capsule_enabled = false;
    }
    auto valid = settings.Validate();
    if (!valid.ok()) {
        return valid.status();
    }
    return settings;
}

Result<void> SettingsStore::Save(const HostSettings& settings) const {
    auto valid = settings.Validate();
    if (!valid.ok()) {
        return valid.status();
    }
    std::error_code error;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return Status(
                ErrorCode::win32_error,
                "Unable to create settings directory: " + error.message(),
                static_cast<std::uint32_t>(error.value()));
        }
    }

    const std::string contents =
        "version=7\n"
        "taskbar_enabled=" +
        std::string(settings.taskbar_enabled ? "1\n" : "0\n") +
        "file_explorer_enabled=" +
        std::string(settings.file_explorer_enabled ? "1\n" : "0\n") +
        "start_menu_enabled=" +
        std::string(settings.start_menu_enabled ? "1\n" : "0\n") +
        "taskbar_clock_prefix_utf8_hex=" +
        EncodeHex(settings.taskbar_clock_prefix) + "\n" +
        "taskbar_opacity_milli=" +
        std::to_string(settings.taskbar_opacity_milli) + "\n" +
        "taskbar_hide_notification_center=" +
        std::string(
            settings.taskbar_hide_notification_center ? "1\n" : "0\n") +
        "taskbar_hide_control_center=" +
        std::string(settings.taskbar_hide_control_center ? "1\n" : "0\n") +
        "taskbar_hide_show_desktop=" +
        std::string(settings.taskbar_hide_show_desktop ? "1\n" : "0\n") +
        "taskbar_capsule_enabled=" +
        std::string(settings.taskbar_capsule_enabled ? "1\n" : "0\n") +
        "taskbar_background_color_enabled=" +
        std::string(settings.taskbar_background_color_enabled ? "1\n" : "0\n") +
        "taskbar_background_color=" +
        std::to_string(settings.taskbar_background_color) + "\n" +
        "file_explorer_title_prefix_utf8_hex=" +
        EncodeHex(settings.file_explorer_title_prefix) + "\n" +
        "file_explorer_background_color_enabled=" +
        std::string(
            settings.file_explorer_background_color_enabled ? "1\n" : "0\n") +
        "file_explorer_background_color=" +
        std::to_string(settings.file_explorer_background_color) + "\n" +
        "file_explorer_transition_animation=" +
        std::to_string(settings.file_explorer_transition_animation) + "\n" +
        "file_explorer_custom_scrollbar_enabled=" +
        std::string(
            settings.file_explorer_custom_scrollbar_enabled ? "1\n" : "0\n") +
        "start_menu_opacity_milli=" +
        std::to_string(settings.start_menu_opacity_milli) + "\n" +
        "start_menu_hide_recommended=" +
        std::string(settings.start_menu_hide_recommended ? "1\n" : "0\n") +
        "start_menu_background_color_enabled=" +
        std::string(
            settings.start_menu_background_color_enabled ? "1\n" : "0\n") +
        "start_menu_background_color=" +
        std::to_string(settings.start_menu_background_color) + "\n";

    static std::atomic<std::uint64_t> temporary_sequence{0};
    const auto temporary =
        path_.native() + L".tmp." + std::to_wstring(::GetCurrentProcessId()) +
        L"." + std::to_wstring(temporary_sequence.fetch_add(
                     1,
                     std::memory_order_relaxed));
    UniqueHandle file(::CreateFileW(
        temporary.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!file) {
        return Status::FromWin32("CreateFileW(settings temp)", ::GetLastError());
    }
    auto write = WriteAll(file.get(), contents);
    if (!write.ok()) {
        file.reset();
        ::DeleteFileW(temporary.c_str());
        return write.status();
    }
    if (!::FlushFileBuffers(file.get())) {
        const auto status =
            Status::FromWin32("FlushFileBuffers(settings)", ::GetLastError());
        file.reset();
        ::DeleteFileW(temporary.c_str());
        return status;
    }
    file.reset();

    if (!::MoveFileExW(
            temporary.c_str(),
            path_.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto status =
            Status::FromWin32("MoveFileExW(settings)", ::GetLastError());
        ::DeleteFileW(temporary.c_str());
        return status;
    }
    return {};
}

}  // namespace metaplasia::host
