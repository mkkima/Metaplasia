#include "metaplasia/protocol/messages.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <type_traits>

namespace metaplasia::protocol {
namespace {

template <typename T, bool = std::is_enum_v<T>>
struct IntegerType final {
    using type = T;
};

template <typename T>
struct IntegerType<T, true> final {
    using type = std::underlying_type_t<T>;
};

class Writer final {
public:
    template <typename T>
        requires(std::is_integral_v<T> || std::is_enum_v<T>)
    bool Write(T value) {
        using Integer = typename IntegerType<T>::type;
        using Unsigned = std::make_unsigned_t<Integer>;
        const auto converted = static_cast<Unsigned>(value);
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            bytes_.push_back(static_cast<std::byte>(
                (converted >> (index * 8U)) & static_cast<Unsigned>(0xFFU)));
        }
        return true;
    }

    bool WriteString(const std::string_view value) {
        if (value.size() > (std::numeric_limits<std::uint16_t>::max)()) {
            return false;
        }
        Write(static_cast<std::uint16_t>(value.size()));
        const auto* begin = reinterpret_cast<const std::byte*>(value.data());
        bytes_.insert(bytes_.end(), begin, begin + value.size());
        return true;
    }

    [[nodiscard]] std::vector<std::byte> Take() && { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

class Reader final {
public:
    explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    template <typename T>
        requires(std::is_integral_v<T> || std::is_enum_v<T>)
    bool Read(T& value) {
        if (remaining() < sizeof(T)) {
            return false;
        }
        using Integer = typename IntegerType<T>::type;
        using Unsigned = std::make_unsigned_t<Integer>;
        Unsigned converted = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            converted |= static_cast<Unsigned>(
                             std::to_integer<unsigned int>(bytes_[offset_ + index]))
                         << (index * 8U);
        }
        offset_ += sizeof(T);
        value = static_cast<T>(converted);
        return true;
    }

    bool ReadString(std::string& value) {
        std::uint16_t length = 0;
        if (!Read(length) || remaining() < length) {
            return false;
        }
        const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset_);
        value.assign(begin, begin + length);
        offset_ += length;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }
    [[nodiscard]] bool finished() const noexcept { return remaining() == 0; }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{0};
};

Status InvalidPayload(const std::string_view type) {
    return Status(
        ErrorCode::invalid_data,
        "Malformed " + std::string(type) + " payload");
}

bool HasForbiddenTextByte(const std::string_view value) noexcept {
    return std::any_of(value.begin(), value.end(), [](const unsigned char byte) {
        return byte < 0x20U || byte == 0x7FU;
    });
}

bool IsValidTextValue(
    const std::string_view value,
    const std::size_t maximum_utf8_bytes) noexcept {
    return value.size() <= maximum_utf8_bytes && !HasForbiddenTextByte(value);
}

bool WriteCustomizationSettings(
    Writer& writer,
    const CustomizationSettings& settings) {
    writer.Write(static_cast<std::uint8_t>(settings.taskbar_enabled ? 1 : 0));
    writer.Write(
        static_cast<std::uint8_t>(settings.file_explorer_enabled ? 1 : 0));
    writer.Write(static_cast<std::uint8_t>(settings.start_menu_enabled ? 1 : 0));
    return writer.WriteString(settings.taskbar_clock_prefix) &&
           writer.Write(settings.taskbar_opacity_milli) &&
           writer.Write(static_cast<std::uint8_t>(
               settings.taskbar_hide_notification_center ? 1 : 0)) &&
           writer.Write(static_cast<std::uint8_t>(
               settings.taskbar_hide_control_center ? 1 : 0)) &&
           writer.Write(static_cast<std::uint8_t>(
               settings.taskbar_hide_show_desktop ? 1 : 0)) &&
           writer.Write(static_cast<std::uint8_t>(
               settings.taskbar_capsule_enabled ? 1 : 0)) &&
           writer.Write(static_cast<std::uint8_t>(
               settings.taskbar_background_color_enabled ? 1 : 0)) &&
           writer.Write(settings.taskbar_background_color) &&
           writer.WriteString(settings.file_explorer_title_prefix) &&
           writer.Write(static_cast<std::uint8_t>(
               settings.file_explorer_background_color_enabled ? 1 : 0)) &&
           writer.Write(settings.file_explorer_background_color) &&
           writer.Write(settings.file_explorer_transition_animation) &&
           writer.Write(static_cast<std::uint8_t>(
               settings.file_explorer_custom_scrollbar_enabled ? 1 : 0)) &&
           writer.Write(settings.start_menu_opacity_milli) &&
           writer.Write(static_cast<std::uint8_t>(
               settings.start_menu_hide_recommended ? 1 : 0)) &&
           writer.Write(static_cast<std::uint8_t>(
               settings.start_menu_background_color_enabled ? 1 : 0)) &&
           writer.Write(settings.start_menu_background_color);
}

bool ReadCustomizationSettings(
    Reader& reader,
    CustomizationSettings& settings) {
    std::uint8_t taskbar_enabled = 0;
    std::uint8_t file_explorer_enabled = 0;
    std::uint8_t start_menu_enabled = 0;
    std::uint8_t hide_notification_center = 0;
    std::uint8_t hide_control_center = 0;
    std::uint8_t hide_show_desktop = 0;
    std::uint8_t taskbar_capsule_enabled = 0;
    std::uint8_t taskbar_background_color_enabled = 0;
    std::uint8_t file_explorer_background_color_enabled = 0;
    std::uint8_t file_explorer_custom_scrollbar_enabled = 0;
    std::uint8_t hide_recommended = 0;
    std::uint8_t start_menu_background_color_enabled = 0;
    if (!reader.Read(taskbar_enabled) || !reader.Read(file_explorer_enabled) ||
        !reader.Read(start_menu_enabled) ||
        !reader.ReadString(settings.taskbar_clock_prefix) ||
        !reader.Read(settings.taskbar_opacity_milli) ||
        !reader.Read(hide_notification_center) ||
        !reader.Read(hide_control_center) ||
        !reader.Read(hide_show_desktop) ||
        !reader.Read(taskbar_capsule_enabled) ||
        !reader.Read(taskbar_background_color_enabled) ||
        !reader.Read(settings.taskbar_background_color) ||
        !reader.ReadString(settings.file_explorer_title_prefix) ||
        !reader.Read(file_explorer_background_color_enabled) ||
        !reader.Read(settings.file_explorer_background_color) ||
        !reader.Read(settings.file_explorer_transition_animation) ||
        !reader.Read(file_explorer_custom_scrollbar_enabled) ||
        !reader.Read(settings.start_menu_opacity_milli) ||
        !reader.Read(hide_recommended) ||
        !reader.Read(start_menu_background_color_enabled) ||
        !reader.Read(settings.start_menu_background_color) ||
        taskbar_enabled > 1 ||
        file_explorer_enabled > 1 || start_menu_enabled > 1 ||
        hide_notification_center > 1 || hide_control_center > 1 ||
        hide_show_desktop > 1 || taskbar_capsule_enabled > 1 ||
        taskbar_background_color_enabled > 1 ||
        file_explorer_background_color_enabled > 1 ||
        file_explorer_custom_scrollbar_enabled > 1 || hide_recommended > 1 ||
        start_menu_background_color_enabled > 1) {
        return false;
    }
    settings.taskbar_enabled = taskbar_enabled != 0;
    settings.file_explorer_enabled = file_explorer_enabled != 0;
    settings.start_menu_enabled = start_menu_enabled != 0;
    settings.taskbar_hide_notification_center =
        hide_notification_center != 0;
    settings.taskbar_hide_control_center = hide_control_center != 0;
    settings.taskbar_hide_show_desktop = hide_show_desktop != 0;
    settings.taskbar_capsule_enabled = taskbar_capsule_enabled != 0;
    settings.taskbar_background_color_enabled =
        taskbar_background_color_enabled != 0;
    settings.file_explorer_background_color_enabled =
        file_explorer_background_color_enabled != 0;
    settings.file_explorer_custom_scrollbar_enabled =
        file_explorer_custom_scrollbar_enabled != 0;
    settings.start_menu_hide_recommended = hide_recommended != 0;
    settings.start_menu_background_color_enabled =
        start_menu_background_color_enabled != 0;
    return IsValidCustomizationSettings(settings);
}

}  // namespace

bool IsValidTarget(const TargetId target) noexcept {
    return target == TargetId::taskbar || target == TargetId::file_explorer ||
           target == TargetId::start_menu;
}

bool IsValidRuntimeState(const RuntimeState state) noexcept {
    return state >= RuntimeState::disabled && state <= RuntimeState::incompatible;
}

bool IsValidCustomizationId(const CustomizationId customization) noexcept {
    return customization >= CustomizationId::taskbar_clock_prefix &&
           customization <= CustomizationId::taskbar_capsule_enabled;
}

bool IsValidCustomizationSettings(
    const CustomizationSettings& settings) noexcept {
    return IsValidTextValue(
               settings.taskbar_clock_prefix,
               kMaximumClockPrefixLength * 4U) &&
           IsValidTextValue(
               settings.file_explorer_title_prefix,
               kMaximumExplorerTitlePrefixLength * 4U) &&
           settings.taskbar_opacity_milli >=
               kMinimumTaskbarOpacityMilli &&
           settings.taskbar_opacity_milli <=
               kMaximumTaskbarOpacityMilli &&
           settings.start_menu_opacity_milli >=
               kMinimumStartMenuOpacityMilli &&
           settings.start_menu_opacity_milli <=
               kMaximumStartMenuOpacityMilli &&
           settings.file_explorer_transition_animation <=
               kMaximumExplorerTransition &&
           (settings.taskbar_background_color & 0xFF000000U) ==
               0xFF000000U &&
           (settings.file_explorer_background_color & 0xFF000000U) ==
               0xFF000000U &&
           (settings.start_menu_background_color & 0xFF000000U) ==
               0xFF000000U;
}

Result<std::vector<std::byte>> EncodeFrameHeader(const FrameHeader& header) {
    if (header.payload_size > kMaximumPayloadSize) {
        return Status(ErrorCode::invalid_argument, "Frame payload is too large");
    }

    Writer writer;
    writer.Write(kFrameMagic);
    writer.Write(kProtocolVersion);
    writer.Write(header.kind);
    writer.Write(header.request_id);
    writer.Write(header.payload_size);
    auto bytes = std::move(writer).Take();
    if (bytes.size() != kFrameHeaderSize) {
        return Status(ErrorCode::internal_error, "Invalid encoded frame header size");
    }
    return bytes;
}

Result<FrameHeader> DecodeFrameHeader(const std::span<const std::byte> bytes) {
    if (bytes.size() != kFrameHeaderSize) {
        return Status(ErrorCode::invalid_data, "Invalid frame header size");
    }

    Reader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    FrameHeader header;
    if (!reader.Read(magic) || !reader.Read(version) ||
        !reader.Read(header.kind) || !reader.Read(header.request_id) ||
        !reader.Read(header.payload_size) || !reader.finished()) {
        return Status(ErrorCode::invalid_data, "Malformed frame header");
    }
    if (magic != kFrameMagic) {
        return Status(ErrorCode::invalid_data, "Invalid frame magic");
    }
    if (version != kProtocolVersion) {
        return Status(ErrorCode::incompatible, "Unsupported protocol version");
    }
    if (header.payload_size > kMaximumPayloadSize) {
        return Status(ErrorCode::invalid_data, "Frame payload exceeds the limit");
    }
    return header;
}

Result<std::vector<std::byte>> EncodeSetEnabledRequest(
    const SetEnabledRequest& request) {
    if (!IsValidTarget(request.target)) {
        return Status(ErrorCode::invalid_argument, "Invalid target id");
    }
    Writer writer;
    writer.Write(request.target);
    writer.Write(static_cast<std::uint8_t>(request.enabled ? 1 : 0));
    return std::move(writer).Take();
}

Result<SetEnabledRequest> DecodeSetEnabledRequest(
    const std::span<const std::byte> payload) {
    Reader reader(payload);
    SetEnabledRequest request;
    std::uint8_t enabled = 0;
    if (!reader.Read(request.target) || !reader.Read(enabled) ||
        !reader.finished() || !IsValidTarget(request.target) || enabled > 1) {
        return InvalidPayload("set-enabled request");
    }
    request.enabled = enabled != 0;
    return request;
}

Result<std::vector<std::byte>> EncodeSetCustomizationRequest(
    const SetCustomizationRequest& request) {
    if (!IsValidCustomizationId(request.customization)) {
        return Status(ErrorCode::invalid_argument, "Invalid customization id");
    }

    Writer writer;
    writer.Write(request.customization);
    switch (request.customization) {
        case CustomizationId::taskbar_clock_prefix:
            if (!IsValidTextValue(
                    request.text_value,
                    kMaximumClockPrefixLength * 4U) ||
                request.integer_value != 0 || request.boolean_value) {
                return Status(
                    ErrorCode::invalid_argument,
                    "Invalid taskbar clock prefix");
            }
            writer.WriteString(request.text_value);
            break;
        case CustomizationId::file_explorer_title_prefix:
            if (!IsValidTextValue(
                    request.text_value,
                    kMaximumExplorerTitlePrefixLength * 4U) ||
                request.integer_value != 0 || request.boolean_value) {
                return Status(
                    ErrorCode::invalid_argument,
                    "Invalid File Explorer title prefix");
            }
            writer.WriteString(request.text_value);
            break;
        case CustomizationId::start_menu_opacity_milli:
            if (!request.text_value.empty() || request.boolean_value ||
                request.integer_value < kMinimumStartMenuOpacityMilli ||
                request.integer_value > kMaximumStartMenuOpacityMilli) {
                return Status(
                    ErrorCode::invalid_argument,
                    "Invalid Start menu opacity");
            }
            writer.Write(request.integer_value);
            break;
        case CustomizationId::taskbar_background_color:
        case CustomizationId::file_explorer_background_color:
        case CustomizationId::start_menu_background_color:
            if (!request.text_value.empty() || request.boolean_value ||
                (request.integer_value & 0xFF000000U) != 0xFF000000U) {
                return Status(
                    ErrorCode::invalid_argument,
                    "Invalid shell background color");
            }
            writer.Write(request.integer_value);
            break;
        case CustomizationId::taskbar_opacity_milli:
            if (!request.text_value.empty() || request.boolean_value ||
                request.integer_value < kMinimumTaskbarOpacityMilli ||
                request.integer_value > kMaximumTaskbarOpacityMilli) {
                return Status(
                    ErrorCode::invalid_argument,
                    "Invalid Taskbar opacity");
            }
            writer.Write(request.integer_value);
            break;
        case CustomizationId::file_explorer_transition_animation:
            if (!request.text_value.empty() || request.boolean_value ||
                request.integer_value > kMaximumExplorerTransition) {
                return Status(
                    ErrorCode::invalid_argument,
                    "Invalid File Explorer transition animation");
            }
            writer.Write(request.integer_value);
            break;
        case CustomizationId::start_menu_hide_recommended:
        case CustomizationId::taskbar_hide_notification_center:
        case CustomizationId::taskbar_hide_control_center:
        case CustomizationId::taskbar_hide_show_desktop:
        case CustomizationId::taskbar_capsule_enabled:
        case CustomizationId::taskbar_background_color_enabled:
        case CustomizationId::file_explorer_background_color_enabled:
        case CustomizationId::start_menu_background_color_enabled:
        case CustomizationId::file_explorer_custom_scrollbar_enabled:
            if (!request.text_value.empty() || request.integer_value != 0) {
                return Status(
                    ErrorCode::invalid_argument,
                    "Invalid boolean customization value");
            }
            writer.Write(static_cast<std::uint8_t>(
                request.boolean_value ? 1 : 0));
            break;
        default:
            return Status(ErrorCode::invalid_argument, "Invalid customization id");
    }
    return std::move(writer).Take();
}

Result<SetCustomizationRequest> DecodeSetCustomizationRequest(
    const std::span<const std::byte> payload) {
    Reader reader(payload);
    SetCustomizationRequest request;
    if (!reader.Read(request.customization) ||
        !IsValidCustomizationId(request.customization)) {
        return InvalidPayload("set-customization request");
    }
    switch (request.customization) {
        case CustomizationId::taskbar_clock_prefix:
            if (!reader.ReadString(request.text_value) ||
                !IsValidTextValue(
                    request.text_value,
                    kMaximumClockPrefixLength * 4U)) {
                return InvalidPayload("set-customization request");
            }
            break;
        case CustomizationId::file_explorer_title_prefix:
            if (!reader.ReadString(request.text_value) ||
                !IsValidTextValue(
                    request.text_value,
                    kMaximumExplorerTitlePrefixLength * 4U)) {
                return InvalidPayload("set-customization request");
            }
            break;
        case CustomizationId::start_menu_opacity_milli:
            if (!reader.Read(request.integer_value) ||
                request.integer_value < kMinimumStartMenuOpacityMilli ||
                request.integer_value > kMaximumStartMenuOpacityMilli) {
                return InvalidPayload("set-customization request");
            }
            break;
        case CustomizationId::taskbar_opacity_milli:
            if (!reader.Read(request.integer_value) ||
                request.integer_value < kMinimumTaskbarOpacityMilli ||
                request.integer_value > kMaximumTaskbarOpacityMilli) {
                return InvalidPayload("set-customization request");
            }
            break;
        case CustomizationId::file_explorer_transition_animation:
            if (!reader.Read(request.integer_value) ||
                request.integer_value > kMaximumExplorerTransition) {
                return InvalidPayload("set-customization request");
            }
            break;
        case CustomizationId::taskbar_background_color:
        case CustomizationId::file_explorer_background_color:
        case CustomizationId::start_menu_background_color:
            if (!reader.Read(request.integer_value) ||
                (request.integer_value & 0xFF000000U) != 0xFF000000U) {
                return InvalidPayload("set-customization request");
            }
            break;
        case CustomizationId::start_menu_hide_recommended:
        case CustomizationId::taskbar_hide_notification_center:
        case CustomizationId::taskbar_hide_control_center:
        case CustomizationId::taskbar_hide_show_desktop:
        case CustomizationId::taskbar_capsule_enabled:
        case CustomizationId::taskbar_background_color_enabled:
        case CustomizationId::file_explorer_background_color_enabled:
        case CustomizationId::start_menu_background_color_enabled:
        case CustomizationId::file_explorer_custom_scrollbar_enabled: {
            std::uint8_t value = 0;
            if (!reader.Read(value) || value > 1) {
                return InvalidPayload("set-customization request");
            }
            request.boolean_value = value != 0;
            break;
        }
        default:
            return InvalidPayload("set-customization request");
    }
    if (!reader.finished()) {
        return InvalidPayload("set-customization request");
    }
    return request;
}

Result<std::vector<std::byte>> EncodeSettingsResponse(
    const CustomizationSettings& settings) {
    if (!IsValidCustomizationSettings(settings)) {
        return Status(ErrorCode::invalid_argument, "Invalid customization settings");
    }
    Writer writer;
    if (!WriteCustomizationSettings(writer, settings)) {
        return Status(ErrorCode::invalid_argument, "Settings payload is too large");
    }
    return std::move(writer).Take();
}

Result<CustomizationSettings> DecodeSettingsResponse(
    const std::span<const std::byte> payload) {
    Reader reader(payload);
    CustomizationSettings settings;
    if (!ReadCustomizationSettings(reader, settings) || !reader.finished()) {
        return InvalidPayload("settings response");
    }
    return settings;
}

Result<std::vector<std::byte>> EncodeXamlDiagnosticsRequest(
    const TargetId target) {
    if (!IsValidTarget(target)) {
        return Status(ErrorCode::invalid_argument, "Invalid diagnostics target");
    }
    Writer writer;
    writer.Write(target);
    return std::move(writer).Take();
}

Result<TargetId> DecodeXamlDiagnosticsRequest(
    const std::span<const std::byte> payload) {
    Reader reader(payload);
    TargetId target{};
    if (!reader.Read(target) || !reader.finished() || !IsValidTarget(target)) {
        return InvalidPayload("XAML diagnostics request");
    }
    return target;
}

Result<std::vector<std::byte>> EncodeXamlDiagnosticsResponse(
    const XamlDiagnosticsResponse& response) {
    if (!IsValidTarget(response.target) ||
        response.types.size() > kMaximumXamlDiagnosticTypes ||
        response.elements.size() > kMaximumXamlDiagnosticElements) {
        return Status(ErrorCode::invalid_argument, "Invalid XAML diagnostics");
    }
    Writer writer;
    writer.Write(response.target);
    writer.Write(response.dropped_type_count);
    writer.Write(response.dropped_element_count);
    writer.Write(response.tracked_element_count);
    writer.Write(static_cast<std::uint16_t>(response.types.size()));
    for (const auto& type : response.types) {
        if (type.observation_count == 0 ||
            !IsValidTextValue(
                type.type_name,
                kMaximumXamlDiagnosticTypeNameLength * 4U) ||
            type.type_name.empty() || !writer.WriteString(type.type_name)) {
            return Status(
                ErrorCode::invalid_argument,
                "Invalid XAML diagnostics type");
        }
        writer.Write(type.observation_count);
    }
    writer.Write(static_cast<std::uint16_t>(response.elements.size()));
    for (const auto& element : response.elements) {
        if (element.handle == 0 || element.type_index >= response.types.size() ||
            !IsValidTextValue(
                element.name,
                kMaximumXamlDiagnosticElementNameLength * 4U)) {
            return Status(
                ErrorCode::invalid_argument,
                "Invalid XAML diagnostics element");
        }
        writer.Write(element.handle);
        writer.Write(element.parent_handle);
        writer.Write(element.child_index);
        writer.Write(element.child_count);
        writer.Write(element.type_index);
        if (!writer.WriteString(element.name)) {
            return Status(
                ErrorCode::invalid_argument,
                "XAML element name is too large");
        }
    }
    auto payload = std::move(writer).Take();
    if (payload.size() > kMaximumPayloadSize) {
        return Status(
            ErrorCode::invalid_argument,
            "XAML diagnostics payload is too large");
    }
    return payload;
}

Result<XamlDiagnosticsResponse> DecodeXamlDiagnosticsResponse(
    const std::span<const std::byte> payload) {
    Reader reader(payload);
    XamlDiagnosticsResponse response;
    std::uint16_t count = 0;
    if (!reader.Read(response.target) ||
        !reader.Read(response.dropped_type_count) ||
        !reader.Read(response.dropped_element_count) ||
        !reader.Read(response.tracked_element_count) || !reader.Read(count) ||
        !IsValidTarget(response.target) ||
        count > kMaximumXamlDiagnosticTypes) {
        return InvalidPayload("XAML diagnostics response");
    }
    response.types.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        XamlTypeObservation type;
        if (!reader.ReadString(type.type_name) ||
            !reader.Read(type.observation_count) ||
            type.observation_count == 0 || type.type_name.empty() ||
            !IsValidTextValue(
                type.type_name,
                kMaximumXamlDiagnosticTypeNameLength * 4U)) {
            return InvalidPayload("XAML diagnostics response");
        }
        response.types.push_back(std::move(type));
    }
    std::uint16_t element_count = 0;
    if (!reader.Read(element_count) ||
        element_count > kMaximumXamlDiagnosticElements) {
        return InvalidPayload("XAML diagnostics response");
    }
    response.elements.reserve(element_count);
    for (std::uint16_t index = 0; index < element_count; ++index) {
        XamlElementObservation element;
        if (!reader.Read(element.handle) ||
            !reader.Read(element.parent_handle) ||
            !reader.Read(element.child_index) ||
            !reader.Read(element.child_count) ||
            !reader.Read(element.type_index) ||
            !reader.ReadString(element.name) || element.handle == 0 ||
            element.type_index >= response.types.size() ||
            !IsValidTextValue(
                element.name,
                kMaximumXamlDiagnosticElementNameLength * 4U)) {
            return InvalidPayload("XAML diagnostics response");
        }
        response.elements.push_back(std::move(element));
    }
    if (!reader.finished()) {
        return InvalidPayload("XAML diagnostics response");
    }
    return response;
}

Result<std::vector<std::byte>> EncodeSnapshotResponse(
    const std::span<const TargetSnapshot> snapshots) {
    if (snapshots.size() > 16) {
        return Status(ErrorCode::invalid_argument, "Too many target snapshots");
    }

    for (const auto& snapshot : snapshots) {
        if (!IsValidTarget(snapshot.target) ||
            !IsValidRuntimeState(snapshot.state) ||
            snapshot.detail.size() >
                (std::numeric_limits<std::uint16_t>::max)()) {
            return Status(ErrorCode::invalid_argument, "Invalid target snapshot");
        }
    }

    Writer stable_writer;
    stable_writer.Write(static_cast<std::uint8_t>(snapshots.size()));
    for (const auto& snapshot : snapshots) {
        stable_writer.Write(snapshot.target);
        stable_writer.Write(snapshot.state);
        stable_writer.Write(static_cast<std::uint8_t>(snapshot.enabled ? 1 : 0));
        stable_writer.Write(
            static_cast<std::uint8_t>(snapshot.process_running ? 1 : 0));
        stable_writer.Write(static_cast<std::uint8_t>(snapshot.agent_loaded ? 1 : 0));
        stable_writer.Write(snapshot.process_id);
        if (!stable_writer.WriteString(snapshot.detail)) {
            return Status(ErrorCode::invalid_argument, "Snapshot detail is too large");
        }
    }
    return std::move(stable_writer).Take();
}

Result<std::vector<TargetSnapshot>> DecodeSnapshotResponse(
    const std::span<const std::byte> payload) {
    Reader reader(payload);
    std::uint8_t count = 0;
    if (!reader.Read(count) || count > 16) {
        return InvalidPayload("snapshot response");
    }

    std::vector<TargetSnapshot> snapshots;
    snapshots.reserve(count);
    for (std::uint8_t index = 0; index < count; ++index) {
        TargetSnapshot snapshot;
        std::uint8_t enabled = 0;
        std::uint8_t running = 0;
        std::uint8_t loaded = 0;
        if (!reader.Read(snapshot.target) || !reader.Read(snapshot.state) ||
            !reader.Read(enabled) || !reader.Read(running) ||
            !reader.Read(loaded) || !reader.Read(snapshot.process_id) ||
            !reader.ReadString(snapshot.detail) ||
            !IsValidTarget(snapshot.target) ||
            !IsValidRuntimeState(snapshot.state) || enabled > 1 || running > 1 ||
            loaded > 1) {
            return InvalidPayload("snapshot response");
        }
        snapshot.enabled = enabled != 0;
        snapshot.process_running = running != 0;
        snapshot.agent_loaded = loaded != 0;
        snapshots.push_back(std::move(snapshot));
    }
    if (!reader.finished()) {
        return InvalidPayload("snapshot response");
    }
    return snapshots;
}

Result<std::vector<std::byte>> EncodeCommandResponse(
    const CommandResponse& response) {
    Writer writer;
    writer.Write(static_cast<std::uint8_t>(response.accepted ? 1 : 0));
    if (!writer.WriteString(response.detail)) {
        return Status(ErrorCode::invalid_argument, "Response detail is too large");
    }
    return std::move(writer).Take();
}

Result<CommandResponse> DecodeCommandResponse(
    const std::span<const std::byte> payload) {
    Reader reader(payload);
    CommandResponse response;
    std::uint8_t accepted = 0;
    if (!reader.Read(accepted) || accepted > 1 ||
        !reader.ReadString(response.detail) || !reader.finished()) {
        return InvalidPayload("command response");
    }
    response.accepted = accepted != 0;
    return response;
}

Result<std::vector<std::byte>> EncodeErrorResponse(
    const ErrorResponse& response) {
    Writer writer;
    writer.Write(response.code);
    writer.Write(response.native_code);
    if (!writer.WriteString(response.detail)) {
        return Status(ErrorCode::invalid_argument, "Error detail is too large");
    }
    return std::move(writer).Take();
}

Result<ErrorResponse> DecodeErrorResponse(
    const std::span<const std::byte> payload) {
    Reader reader(payload);
    ErrorResponse response;
    if (!reader.Read(response.code) || !reader.Read(response.native_code) ||
        !reader.ReadString(response.detail) || !reader.finished() ||
        response.code == ErrorCode::ok) {
        return InvalidPayload("error response");
    }
    return response;
}

}  // namespace metaplasia::protocol
