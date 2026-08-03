#pragma once

#include "metaplasia/protocol/agent_abi.hpp"

#include <Windows.h>
#include <Unknwn.h>

#include <cstdint>

namespace metaplasia::agent {

// {53E737CA-78A6-48A8-87A5-754750787917}
inline constexpr CLSID kStartMenuTapClsid{
    0x53e737ca,
    0x78a6,
    0x48a8,
    {0x87, 0xa5, 0x75, 0x47, 0x50, 0x78, 0x79, 0x17}};

void SetAgentModule(HMODULE module) noexcept;

struct ShellXamlSettings final {
    std::uint32_t taskbar_opacity_milli{
        protocol::kDefaultTaskbarOpacityMilli};
    bool taskbar_hide_notification_center{false};
    bool taskbar_hide_control_center{false};
    bool taskbar_hide_show_desktop{false};
    bool taskbar_capsule_enabled{false};
    bool taskbar_background_color_enabled{false};
    std::uint32_t taskbar_background_color{
        protocol::kDefaultShellBackgroundColor};
    std::uint32_t start_menu_opacity_milli{
        protocol::kDefaultStartMenuOpacityMilli};
    bool start_menu_hide_recommended{false};
    bool start_menu_background_color_enabled{false};
    std::uint32_t start_menu_background_color{
        protocol::kDefaultShellBackgroundColor};
    bool start_menu_three_panel_layout_enabled{false};
    bool start_menu_hide_all_apps{false};
};

[[nodiscard]] protocol::AgentResult ConfigureShellXaml(
    protocol::AgentTarget target,
    bool enabled,
    const ShellXamlSettings& settings) noexcept;
[[nodiscard]] protocol::AgentResult StopShellXaml() noexcept;
[[nodiscard]] std::uint32_t StartMenuXamlLastError() noexcept;
[[nodiscard]] protocol::AgentDiagnosticStage StartMenuXamlDiagnosticStage()
    noexcept;
[[nodiscard]] std::uint32_t StartMenuXamlControllerState() noexcept;
[[nodiscard]] protocol::AgentResult CopyShellXamlDiagnostics(
    protocol::XamlDiagnosticsSnapshot* snapshot) noexcept;

[[nodiscard]] HRESULT GetStartMenuTapClassObject(
    REFCLSID class_id,
    REFIID interface_id,
    void** object) noexcept;

}  // namespace metaplasia::agent
