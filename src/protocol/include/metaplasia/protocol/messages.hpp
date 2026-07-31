#pragma once

#include "metaplasia/base/status.hpp"
#include "metaplasia/protocol/agent_abi.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace metaplasia::protocol {

inline constexpr std::uint32_t kFrameMagic = 0x504D544D;  // "MTMP"
inline constexpr std::uint16_t kProtocolVersion = 6;
inline constexpr std::size_t kFrameHeaderSize = 16;
inline constexpr std::uint32_t kMaximumPayloadSize = 64U * 1024U;

enum class MessageKind : std::uint16_t {
    get_snapshot_request = 1,
    set_enabled_request = 2,
    get_settings_request = 3,
    set_customization_request = 4,
    get_xaml_diagnostics_request = 5,
    snapshot_response = 100,
    command_response = 101,
    error_response = 102,
    settings_response = 103,
    xaml_diagnostics_response = 104,
};

enum class TargetId : std::uint8_t {
    taskbar = 1,
    file_explorer = 2,
    start_menu = 3,
};

enum class RuntimeState : std::uint8_t {
    disabled = 0,
    stopped,
    running,
    injecting,
    active,
    error,
    incompatible,
};

struct FrameHeader final {
    MessageKind kind{MessageKind::error_response};
    std::uint32_t request_id{0};
    std::uint32_t payload_size{0};
};

struct Frame final {
    FrameHeader header;
    std::vector<std::byte> payload;
};

struct SetEnabledRequest final {
    TargetId target{TargetId::taskbar};
    bool enabled{false};
};

enum class CustomizationId : std::uint8_t {
    taskbar_clock_prefix = 1,
    file_explorer_title_prefix = 2,
    start_menu_opacity_milli = 3,
    start_menu_hide_recommended = 4,
    taskbar_opacity_milli = 5,
    taskbar_hide_notification_center = 6,
    taskbar_hide_control_center = 7,
    taskbar_hide_show_desktop = 8,
    taskbar_background_color_enabled = 9,
    taskbar_background_color = 10,
    file_explorer_background_color_enabled = 11,
    file_explorer_background_color = 12,
    start_menu_background_color_enabled = 13,
    start_menu_background_color = 14,
    file_explorer_transition_animation = 15,
    file_explorer_custom_scrollbar_enabled = 16,
    taskbar_capsule_enabled = 17,
};

struct CustomizationSettings final {
    bool taskbar_enabled{false};
    bool file_explorer_enabled{false};
    bool start_menu_enabled{false};
    std::string taskbar_clock_prefix{"M "};
    std::uint32_t taskbar_opacity_milli{kDefaultTaskbarOpacityMilli};
    bool taskbar_hide_notification_center{false};
    bool taskbar_hide_control_center{false};
    bool taskbar_hide_show_desktop{false};
    bool taskbar_capsule_enabled{false};
    bool taskbar_background_color_enabled{false};
    std::uint32_t taskbar_background_color{kDefaultShellBackgroundColor};
    std::string file_explorer_title_prefix{"Meta \xC2\xB7 "};
    bool file_explorer_background_color_enabled{false};
    std::uint32_t file_explorer_background_color{kDefaultShellBackgroundColor};
    std::uint32_t file_explorer_transition_animation{
        kDefaultExplorerTransition};
    bool file_explorer_custom_scrollbar_enabled{true};
    std::uint32_t start_menu_opacity_milli{kDefaultStartMenuOpacityMilli};
    bool start_menu_hide_recommended{false};
    bool start_menu_background_color_enabled{false};
    std::uint32_t start_menu_background_color{kDefaultShellBackgroundColor};
};

// Only the value selected by `customization` is encoded. Text customizations
// use `text_value`; the opacity customization uses `integer_value`.
struct SetCustomizationRequest final {
    CustomizationId customization{CustomizationId::taskbar_clock_prefix};
    std::string text_value;
    std::uint32_t integer_value{0};
    bool boolean_value{false};
};

struct XamlTypeObservation final {
    std::string type_name;
    std::uint32_t observation_count{0};
};

struct XamlElementObservation final {
    std::uint64_t handle{0};
    std::uint64_t parent_handle{0};
    std::uint32_t child_index{0};
    std::uint32_t child_count{0};
    std::uint16_t type_index{0};
    std::string name;
};

struct XamlDiagnosticsResponse final {
    TargetId target{TargetId::start_menu};
    std::uint32_t dropped_type_count{0};
    std::uint32_t dropped_element_count{0};
    std::uint32_t tracked_element_count{0};
    std::vector<XamlTypeObservation> types;
    std::vector<XamlElementObservation> elements;
};

struct TargetSnapshot final {
    TargetId target{TargetId::taskbar};
    RuntimeState state{RuntimeState::disabled};
    bool enabled{false};
    bool process_running{false};
    bool agent_loaded{false};
    std::uint32_t process_id{0};
    std::string detail;
};

struct CommandResponse final {
    bool accepted{false};
    std::string detail;
};

struct ErrorResponse final {
    ErrorCode code{ErrorCode::internal_error};
    std::uint32_t native_code{0};
    std::string detail;
};

[[nodiscard]] bool IsValidTarget(TargetId target) noexcept;
[[nodiscard]] bool IsValidRuntimeState(RuntimeState state) noexcept;
[[nodiscard]] bool IsValidCustomizationId(CustomizationId customization) noexcept;
[[nodiscard]] bool IsValidCustomizationSettings(
    const CustomizationSettings& settings) noexcept;

[[nodiscard]] Result<std::vector<std::byte>> EncodeFrameHeader(
    const FrameHeader& header);
[[nodiscard]] Result<FrameHeader> DecodeFrameHeader(
    std::span<const std::byte> bytes);

[[nodiscard]] Result<std::vector<std::byte>> EncodeSetEnabledRequest(
    const SetEnabledRequest& request);
[[nodiscard]] Result<SetEnabledRequest> DecodeSetEnabledRequest(
    std::span<const std::byte> payload);

[[nodiscard]] Result<std::vector<std::byte>> EncodeSetCustomizationRequest(
    const SetCustomizationRequest& request);
[[nodiscard]] Result<SetCustomizationRequest> DecodeSetCustomizationRequest(
    std::span<const std::byte> payload);

[[nodiscard]] Result<std::vector<std::byte>> EncodeSettingsResponse(
    const CustomizationSettings& settings);
[[nodiscard]] Result<CustomizationSettings> DecodeSettingsResponse(
    std::span<const std::byte> payload);

[[nodiscard]] Result<std::vector<std::byte>> EncodeXamlDiagnosticsRequest(
    TargetId target);
[[nodiscard]] Result<TargetId> DecodeXamlDiagnosticsRequest(
    std::span<const std::byte> payload);
[[nodiscard]] Result<std::vector<std::byte>> EncodeXamlDiagnosticsResponse(
    const XamlDiagnosticsResponse& response);
[[nodiscard]] Result<XamlDiagnosticsResponse> DecodeXamlDiagnosticsResponse(
    std::span<const std::byte> payload);

[[nodiscard]] Result<std::vector<std::byte>> EncodeSnapshotResponse(
    std::span<const TargetSnapshot> snapshots);
[[nodiscard]] Result<std::vector<TargetSnapshot>> DecodeSnapshotResponse(
    std::span<const std::byte> payload);

[[nodiscard]] Result<std::vector<std::byte>> EncodeCommandResponse(
    const CommandResponse& response);
[[nodiscard]] Result<CommandResponse> DecodeCommandResponse(
    std::span<const std::byte> payload);

[[nodiscard]] Result<std::vector<std::byte>> EncodeErrorResponse(
    const ErrorResponse& response);
[[nodiscard]] Result<ErrorResponse> DecodeErrorResponse(
    std::span<const std::byte> payload);

}  // namespace metaplasia::protocol
