#pragma once

#include "metaplasia/base/status.hpp"
#include "metaplasia/protocol/messages.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

namespace metaplasia::host {

struct HostSettings final {
    bool taskbar_enabled{false};
    bool file_explorer_enabled{false};
    bool start_menu_enabled{false};
    std::string taskbar_clock_prefix{"M "};
    std::uint32_t taskbar_opacity_milli{
        protocol::kDefaultTaskbarOpacityMilli};
    bool taskbar_hide_notification_center{false};
    bool taskbar_hide_control_center{false};
    bool taskbar_hide_show_desktop{false};
    bool taskbar_capsule_enabled{false};
    bool taskbar_background_color_enabled{false};
    std::uint32_t taskbar_background_color{
        protocol::kDefaultShellBackgroundColor};
    std::string file_explorer_title_prefix{"Meta \xC2\xB7 "};
    bool file_explorer_background_color_enabled{false};
    std::uint32_t file_explorer_background_color{
        protocol::kDefaultShellBackgroundColor};
    std::uint32_t file_explorer_transition_animation{
        protocol::kDefaultExplorerTransition};
    bool file_explorer_custom_scrollbar_enabled{true};
    std::uint32_t start_menu_opacity_milli{
        protocol::kDefaultStartMenuOpacityMilli};
    bool start_menu_hide_recommended{false};
    bool start_menu_background_color_enabled{false};
    std::uint32_t start_menu_background_color{
        protocol::kDefaultShellBackgroundColor};

    [[nodiscard]] bool Enabled(protocol::TargetId target) const noexcept;
    [[nodiscard]] bool SetEnabled(
        protocol::TargetId target,
        bool enabled) noexcept;
    [[nodiscard]] Result<bool> SetCustomization(
        const protocol::SetCustomizationRequest& request);
    [[nodiscard]] Result<void> Validate() const;
    [[nodiscard]] protocol::CustomizationSettings ToProtocol() const;
};

class SettingsStore final {
public:
    explicit SettingsStore(std::filesystem::path path);

    [[nodiscard]] Result<HostSettings> Load() const;
    [[nodiscard]] Result<void> Save(const HostSettings& settings) const;

private:
    std::filesystem::path path_;
};

}  // namespace metaplasia::host
