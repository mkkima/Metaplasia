#include "metaplasia/protocol/messages.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    using namespace metaplasia::protocol;

    static_assert(kProtocolVersion == 9);
    const FrameHeader prepare_update_header{
        MessageKind::prepare_update_request,
        7,
        0};
    auto encoded_prepare_update = EncodeFrameHeader(prepare_update_header);
    Require(encoded_prepare_update.ok(), "prepare-update header encode");
    auto decoded_prepare_update =
        DecodeFrameHeader(encoded_prepare_update.value());
    Require(
        decoded_prepare_update.ok() &&
            decoded_prepare_update.value().kind ==
                MessageKind::prepare_update_request &&
            decoded_prepare_update.value().payload_size == 0,
        "prepare-update header roundtrip");

    const SetEnabledRequest command{TargetId::taskbar, true};
    auto encoded_command = EncodeSetEnabledRequest(command);
    Require(encoded_command.ok(), "set-enabled encode");
    auto decoded_command = DecodeSetEnabledRequest(encoded_command.value());
    Require(decoded_command.ok(), "set-enabled decode");
    Require(decoded_command.value().target == TargetId::taskbar, "target roundtrip");
    Require(decoded_command.value().enabled, "enabled roundtrip");

    CustomizationSettings settings;
    settings.taskbar_enabled = true;
    settings.start_menu_enabled = true;
    settings.taskbar_clock_prefix = "T ";
    settings.taskbar_opacity_milli = 820;
    settings.taskbar_hide_notification_center = true;
    settings.taskbar_hide_show_desktop = true;
    settings.taskbar_capsule_enabled = true;
    settings.taskbar_background_color_enabled = true;
    settings.taskbar_background_color = 0xFF102030U;
    settings.file_explorer_title_prefix = "Explorer \xC2\xB7 ";
    settings.file_explorer_background_color_enabled = true;
    settings.file_explorer_background_color = 0xFF405060U;
    settings.file_explorer_transition_animation =
        kExplorerTransitionScaleFade;
    settings.file_explorer_custom_scrollbar_enabled = false;
    settings.start_menu_opacity_milli = 730;
    settings.start_menu_hide_recommended = true;
    settings.start_menu_background_color_enabled = true;
    settings.start_menu_background_color = 0xFF708090U;
    settings.start_menu_three_panel_layout_enabled = true;
    settings.start_menu_hide_all_apps = true;
    auto encoded_settings = EncodeSettingsResponse(settings);
    Require(encoded_settings.ok(), "settings encode");
    auto decoded_settings = DecodeSettingsResponse(encoded_settings.value());
    Require(decoded_settings.ok(), "settings decode");
    Require(decoded_settings.value().taskbar_enabled, "settings boolean roundtrip");
    Require(
        decoded_settings.value().taskbar_opacity_milli == 820 &&
            decoded_settings.value().taskbar_hide_notification_center &&
            decoded_settings.value().taskbar_hide_show_desktop &&
            decoded_settings.value().taskbar_capsule_enabled,
        "Taskbar XAML settings roundtrip");
    Require(
        decoded_settings.value().file_explorer_title_prefix ==
            "Explorer \xC2\xB7 ",
        "settings UTF-8 roundtrip");
    Require(
        decoded_settings.value().start_menu_opacity_milli == 730,
        "settings opacity roundtrip");
    Require(
        decoded_settings.value().start_menu_hide_recommended,
        "settings visibility rule roundtrip");
    Require(
        decoded_settings.value().taskbar_background_color_enabled &&
            decoded_settings.value().taskbar_background_color == 0xFF102030U &&
            decoded_settings.value().file_explorer_background_color_enabled &&
            decoded_settings.value().file_explorer_background_color ==
                0xFF405060U &&
            decoded_settings.value().file_explorer_transition_animation ==
                kExplorerTransitionScaleFade &&
            !decoded_settings.value().file_explorer_custom_scrollbar_enabled &&
            decoded_settings.value().start_menu_background_color_enabled &&
            decoded_settings.value().start_menu_background_color ==
                0xFF708090U,
        "independent shell colors roundtrip");
    Require(
        decoded_settings.value().start_menu_three_panel_layout_enabled &&
            decoded_settings.value().start_menu_hide_all_apps,
        "three-panel Start settings roundtrip");

    const SetCustomizationRequest prefix_command{
        CustomizationId::taskbar_clock_prefix,
        "Clock ",
        0};
    auto encoded_prefix = EncodeSetCustomizationRequest(prefix_command);
    Require(encoded_prefix.ok(), "prefix command encode");
    auto decoded_prefix = DecodeSetCustomizationRequest(encoded_prefix.value());
    Require(decoded_prefix.ok(), "prefix command decode");
    Require(decoded_prefix.value().text_value == "Clock ", "prefix roundtrip");

    const SetCustomizationRequest opacity_command{
        CustomizationId::start_menu_opacity_milli,
        {},
        850};
    auto encoded_opacity = EncodeSetCustomizationRequest(opacity_command);
    Require(encoded_opacity.ok(), "opacity command encode");
    auto decoded_opacity =
        DecodeSetCustomizationRequest(encoded_opacity.value());
    Require(decoded_opacity.ok(), "opacity command decode");
    Require(decoded_opacity.value().integer_value == 850, "opacity roundtrip");

    const SetCustomizationRequest taskbar_opacity_command{
        CustomizationId::taskbar_opacity_milli,
        {},
        675};
    auto encoded_taskbar_opacity =
        EncodeSetCustomizationRequest(taskbar_opacity_command);
    Require(encoded_taskbar_opacity.ok(), "Taskbar opacity command encode");
    auto decoded_taskbar_opacity = DecodeSetCustomizationRequest(
        encoded_taskbar_opacity.value());
    Require(
        decoded_taskbar_opacity.ok() &&
            decoded_taskbar_opacity.value().integer_value == 675,
        "Taskbar opacity roundtrip");

    const SetCustomizationRequest visibility_command{
        CustomizationId::start_menu_hide_recommended,
        {},
        0,
        true};
    auto encoded_visibility =
        EncodeSetCustomizationRequest(visibility_command);
    Require(encoded_visibility.ok(), "visibility command encode");
    auto decoded_visibility = DecodeSetCustomizationRequest(
        encoded_visibility.value());
    Require(
        decoded_visibility.ok() && decoded_visibility.value().boolean_value,
        "visibility command roundtrip");

    const SetCustomizationRequest taskbar_visibility_command{
        CustomizationId::taskbar_hide_notification_center,
        {},
        0,
        true};
    auto encoded_taskbar_visibility =
        EncodeSetCustomizationRequest(taskbar_visibility_command);
    Require(
        encoded_taskbar_visibility.ok(),
        "Taskbar visibility command encode");
    auto decoded_taskbar_visibility = DecodeSetCustomizationRequest(
        encoded_taskbar_visibility.value());
    Require(
        decoded_taskbar_visibility.ok() &&
            decoded_taskbar_visibility.value().boolean_value,
        "Taskbar visibility roundtrip");

    const SetCustomizationRequest capsule_command{
        CustomizationId::taskbar_capsule_enabled,
        {},
        0,
        true};
    auto encoded_capsule =
        EncodeSetCustomizationRequest(capsule_command);
    Require(encoded_capsule.ok(), "Taskbar capsule command encode");
    auto decoded_capsule =
        DecodeSetCustomizationRequest(encoded_capsule.value());
    Require(
        decoded_capsule.ok() &&
            decoded_capsule.value().customization ==
                CustomizationId::taskbar_capsule_enabled &&
            decoded_capsule.value().boolean_value,
        "Taskbar capsule roundtrip");

    const SetCustomizationRequest color_command{
        CustomizationId::start_menu_background_color,
        {},
        0xFFABCDEFU};
    auto encoded_color = EncodeSetCustomizationRequest(color_command);
    Require(encoded_color.ok(), "shell color command encode");
    auto decoded_color = DecodeSetCustomizationRequest(encoded_color.value());
    Require(
        decoded_color.ok() &&
            decoded_color.value().integer_value == 0xFFABCDEFU,
        "shell color command roundtrip");

    const SetCustomizationRequest animation_command{
        CustomizationId::file_explorer_transition_animation,
        {},
        kExplorerTransitionSlideFade};
    auto encoded_animation =
        EncodeSetCustomizationRequest(animation_command);
    Require(encoded_animation.ok(), "Explorer animation command encode");
    auto decoded_animation =
        DecodeSetCustomizationRequest(encoded_animation.value());
    Require(
        decoded_animation.ok() &&
            decoded_animation.value().integer_value ==
                kExplorerTransitionSlideFade,
        "Explorer animation command roundtrip");

    const SetCustomizationRequest scrollbar_command{
        CustomizationId::file_explorer_custom_scrollbar_enabled,
        {},
        0,
        true};
    auto encoded_scrollbar =
        EncodeSetCustomizationRequest(scrollbar_command);
    Require(encoded_scrollbar.ok(), "Explorer scrollbar command encode");
    auto decoded_scrollbar =
        DecodeSetCustomizationRequest(encoded_scrollbar.value());
    Require(
        decoded_scrollbar.ok() && decoded_scrollbar.value().boolean_value,
        "Explorer scrollbar command roundtrip");

    const SetCustomizationRequest start_layout_command{
        CustomizationId::start_menu_three_panel_layout_enabled,
        {},
        0,
        true};
    auto encoded_start_layout =
        EncodeSetCustomizationRequest(start_layout_command);
    Require(encoded_start_layout.ok(), "Start layout command encode");
    auto decoded_start_layout = DecodeSetCustomizationRequest(
        encoded_start_layout.value());
    Require(
        decoded_start_layout.ok() &&
            decoded_start_layout.value().customization ==
                CustomizationId::start_menu_three_panel_layout_enabled &&
            decoded_start_layout.value().boolean_value,
        "Start layout command roundtrip");

    const SetCustomizationRequest hide_all_apps_command{
        CustomizationId::start_menu_hide_all_apps,
        {},
        0,
        true};
    auto encoded_hide_all_apps =
        EncodeSetCustomizationRequest(hide_all_apps_command);
    Require(encoded_hide_all_apps.ok(), "All apps command encode");
    auto decoded_hide_all_apps = DecodeSetCustomizationRequest(
        encoded_hide_all_apps.value());
    Require(
        decoded_hide_all_apps.ok() &&
            decoded_hide_all_apps.value().customization ==
                CustomizationId::start_menu_hide_all_apps &&
            decoded_hide_all_apps.value().boolean_value,
        "All apps command roundtrip");

    Require(
        !EncodeSetCustomizationRequest(
             {CustomizationId::taskbar_clock_prefix, "bad\nvalue", 0})
             .ok(),
        "control character rejected");
    Require(
        !EncodeSetCustomizationRequest(
             {CustomizationId::start_menu_opacity_milli, {}, 99})
             .ok(),
        "out-of-range opacity rejected");
    Require(
        !EncodeSetCustomizationRequest(
             {CustomizationId::taskbar_opacity_milli, {}, 1001})
             .ok(),
        "out-of-range Taskbar opacity rejected");
    Require(
        !EncodeSetCustomizationRequest(
             {CustomizationId::taskbar_background_color, {}, 0x00112233U})
             .ok(),
        "transparent shell color rejected");
    Require(
        !EncodeSetCustomizationRequest(
             {CustomizationId::file_explorer_transition_animation, {}, 4})
             .ok(),
        "unknown Explorer animation rejected");

    auto encoded_diagnostics_request =
        EncodeXamlDiagnosticsRequest(TargetId::start_menu);
    Require(encoded_diagnostics_request.ok(), "diagnostics request encode");
    auto decoded_diagnostics_request = DecodeXamlDiagnosticsRequest(
        encoded_diagnostics_request.value());
    Require(
        decoded_diagnostics_request.ok() &&
            decoded_diagnostics_request.value() == TargetId::start_menu,
        "diagnostics request roundtrip");

    XamlDiagnosticsResponse diagnostics;
    diagnostics.target = TargetId::start_menu;
    diagnostics.dropped_type_count = 2;
    diagnostics.dropped_element_count = 1;
    diagnostics.tracked_element_count = 1;
    diagnostics.types = {
        {"StartDocked.StartSizingFrame", 3},
        {"Windows.UI.Xaml.Controls.Grid", 12}};
    diagnostics.elements = {
        {0x20, 0x10, 2, 4, 0, "Root"}};
    auto encoded_diagnostics = EncodeXamlDiagnosticsResponse(diagnostics);
    Require(encoded_diagnostics.ok(), "diagnostics response encode");
    auto decoded_diagnostics = DecodeXamlDiagnosticsResponse(
        encoded_diagnostics.value());
    Require(decoded_diagnostics.ok(), "diagnostics response decode");
    Require(decoded_diagnostics.value().types.size() == 2, "diagnostics count");
    Require(
        decoded_diagnostics.value().types[1].observation_count == 12,
        "diagnostics observation roundtrip");
    Require(
        decoded_diagnostics.value().elements.size() == 1 &&
            decoded_diagnostics.value().elements[0].parent_handle == 0x10,
        "diagnostics element roundtrip");
    Require(
        decoded_diagnostics.value().tracked_element_count == 1,
        "diagnostics tracked root roundtrip");

    const std::array<TargetSnapshot, 3> snapshots{
        TargetSnapshot{
            TargetId::taskbar,
            RuntimeState::active,
            true,
            true,
            true,
            1234,
            "Active"},
        TargetSnapshot{
            TargetId::file_explorer,
            RuntimeState::disabled,
            false,
            true,
            true,
            1234,
            "Disabled"},
        TargetSnapshot{
            TargetId::start_menu,
            RuntimeState::error,
            true,
            true,
            false,
            5678,
            "Access denied"},
    };
    auto encoded_snapshots = EncodeSnapshotResponse(snapshots);
    Require(encoded_snapshots.ok(), "snapshot encode");
    auto decoded_snapshots = DecodeSnapshotResponse(encoded_snapshots.value());
    Require(decoded_snapshots.ok(), "snapshot decode");
    Require(decoded_snapshots.value().size() == snapshots.size(), "snapshot count");
    Require(decoded_snapshots.value()[2].detail == "Access denied", "detail roundtrip");

    FrameHeader header{
        MessageKind::snapshot_response,
        0x01020304,
        static_cast<std::uint32_t>(encoded_snapshots.value().size())};
    auto encoded_header = EncodeFrameHeader(header);
    Require(encoded_header.ok(), "header encode");
    Require(encoded_header.value().size() == kFrameHeaderSize, "header size");
    auto decoded_header = DecodeFrameHeader(encoded_header.value());
    Require(decoded_header.ok(), "header decode");
    Require(decoded_header.value().request_id == header.request_id, "request id");

    auto corrupted_header = encoded_header.value();
    corrupted_header[0] = std::byte{0};
    Require(!DecodeFrameHeader(corrupted_header).ok(), "corrupt magic rejected");

    const std::array<std::byte, 3> malformed{
        std::byte{0xFF}, std::byte{0x02}, std::byte{0x00}};
    Require(
        !DecodeSetEnabledRequest(malformed).ok(),
        "invalid target and boolean rejected");

    std::cout << "Protocol codec tests passed\n";
    return EXIT_SUCCESS;
}
