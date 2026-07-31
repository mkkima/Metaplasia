#pragma once

#include "metaplasia/protocol/agent_abi.hpp"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace metaplasia::agent {

inline constexpr std::uint32_t kMinimumStartMenuOpacityMilli =
    protocol::kMinimumStartMenuOpacityMilli;
inline constexpr std::uint32_t kMaximumStartMenuOpacityMilli =
    protocol::kMaximumStartMenuOpacityMilli;
inline constexpr std::uint32_t kMinimumTaskbarOpacityMilli =
    protocol::kMinimumTaskbarOpacityMilli;
inline constexpr std::uint32_t kMaximumTaskbarOpacityMilli =
    protocol::kMaximumTaskbarOpacityMilli;
inline constexpr std::size_t kMaximumTrackedShellXamlElements = 32;
inline constexpr std::size_t kMaximumTrackedTaskbarLayouts = 16;
inline constexpr double kTaskbarCapsuleMaximumWidth = 1420.0;
inline constexpr double kTaskbarCapsuleMinimumOuterMargin = 12.0;
inline constexpr double kTaskbarCapsuleFallbackOuterMargin =
    kTaskbarCapsuleMinimumOuterMargin;
inline constexpr double kTaskbarCapsuleVerticalMargin = 2.0;
inline constexpr double kTaskbarCapsuleCornerRadius = 12.0;
inline constexpr std::uint32_t kTaskbarCapsuleBackgroundColor = 0xFF151519U;
inline constexpr std::uint32_t kTaskbarCapsuleOutlineColor = 0xFF4A4A4AU;
inline constexpr double kTaskbarCapsuleOutlineThickness = 1.0;
inline constexpr double kTaskbarCapsuleOutlineInset =
    kTaskbarCapsuleOutlineThickness / 2.0;
inline constexpr double kTaskbarCapsuleOutlineCornerRadius =
    kTaskbarCapsuleCornerRadius - kTaskbarCapsuleOutlineInset;

[[nodiscard]] bool IsSupportedStartMenuRootType(
    std::wstring_view type_name) noexcept;
[[nodiscard]] bool IsStartMenuRecommendedElement(
    std::wstring_view type_name,
    std::wstring_view element_name) noexcept;
[[nodiscard]] bool IsSupportedTaskbarOpacityElement(
    std::wstring_view type_name,
    std::wstring_view element_name) noexcept;
[[nodiscard]] bool IsTaskbarCapsuleOutlineElement(
    protocol::AgentTarget target,
    std::wstring_view type_name,
    std::wstring_view element_name) noexcept;

struct ShellThickness final {
    double left{0.0};
    double top{0.0};
    double right{0.0};
    double bottom{0.0};
};

struct ShellCornerRadius final {
    double top_left{0.0};
    double top_right{0.0};
    double bottom_right{0.0};
    double bottom_left{0.0};
};

enum class ShellGeometryRule : std::uint8_t {
    none = 0,
    taskbar_capsule_layout_root,
    taskbar_capsule_background,
};

[[nodiscard]] ShellGeometryRule IdentifyShellGeometryRule(
    protocol::AgentTarget target,
    std::wstring_view type_name,
    std::wstring_view element_name) noexcept;
[[nodiscard]] double CalculateTaskbarCapsuleOuterMargin(
    double available_width) noexcept;

enum class ShellBrushProperty : std::uint8_t {
    none = 0,
    background,
    fill,
};

enum class ShellBackgroundRule : std::uint8_t {
    none = 0,
    start_menu_surface,
    taskbar_surface,
};

[[nodiscard]] ShellBackgroundRule IdentifyShellBackgroundRule(
    protocol::AgentTarget target,
    std::wstring_view type_name,
    std::wstring_view element_name) noexcept;

[[nodiscard]] ShellBrushProperty BrushPropertyFor(
    ShellBackgroundRule rule) noexcept;

enum class ShellVisibilityRule : std::uint8_t {
    none = 0,
    start_menu_recommended,
    taskbar_notification_center,
    taskbar_control_center,
    taskbar_show_desktop,
};

[[nodiscard]] ShellVisibilityRule IdentifyShellVisibilityRule(
    protocol::AgentTarget target,
    std::wstring_view type_name,
    std::wstring_view element_name) noexcept;

class ShellXamlElementAccessor {
public:
    virtual ~ShellXamlElementAccessor() = default;

    [[nodiscard]] virtual HRESULT ReadOpacity(
        std::uint64_t handle,
        double& opacity) noexcept = 0;
    [[nodiscard]] virtual HRESULT WriteOpacity(
        std::uint64_t handle,
        double opacity) noexcept = 0;
    [[nodiscard]] virtual HRESULT ReadVisibility(
        std::uint64_t handle,
        bool& visible) noexcept = 0;
    [[nodiscard]] virtual HRESULT WriteVisibility(
        std::uint64_t handle,
        bool visible) noexcept = 0;
    [[nodiscard]] virtual HRESULT ReadActualWidth(
        std::uint64_t handle,
        double& width) noexcept = 0;
    [[nodiscard]] virtual HRESULT ReadMargin(
        std::uint64_t handle,
        ShellThickness& margin) noexcept = 0;
    [[nodiscard]] virtual HRESULT WriteMargin(
        std::uint64_t handle,
        const ShellThickness& margin) noexcept = 0;
    [[nodiscard]] virtual HRESULT ReadCornerRadius(
        std::uint64_t handle,
        ShellCornerRadius& radius) noexcept = 0;
    [[nodiscard]] virtual HRESULT WriteCornerRadius(
        std::uint64_t handle,
        const ShellCornerRadius& radius) noexcept = 0;
    [[nodiscard]] virtual HRESULT CaptureBrush(
        std::uint64_t handle,
        ShellBrushProperty property,
        std::uint64_t& snapshot) noexcept = 0;
    [[nodiscard]] virtual HRESULT WriteBrushColor(
        std::uint64_t handle,
        ShellBrushProperty property,
        std::uint32_t argb) noexcept = 0;
    [[nodiscard]] virtual HRESULT RestoreBrush(
        std::uint64_t handle,
        ShellBrushProperty property,
        std::uint64_t snapshot) noexcept = 0;
    virtual void ReleaseBrushSnapshot(std::uint64_t snapshot) noexcept = 0;
    [[nodiscard]] virtual HRESULT CaptureTaskbarCapsuleOutline(
        std::uint64_t handle,
        std::uint64_t& snapshot) noexcept = 0;
    [[nodiscard]] virtual HRESULT WriteTaskbarCapsuleOutline(
        std::uint64_t handle,
        std::uint32_t argb,
        double thickness,
        double corner_radius,
        double inset) noexcept = 0;
    [[nodiscard]] virtual HRESULT RestoreTaskbarCapsuleOutline(
        std::uint64_t handle,
        std::uint64_t snapshot) noexcept = 0;
    virtual void ReleaseTaskbarCapsuleOutlineSnapshot(
        std::uint64_t snapshot) noexcept = 0;
};

// This target-aware property engine contains no XAML or COM lifetime logic.
// Methods that mutate the ownership table run on the target XAML UI thread;
// Configure() is thread-safe for the injector's short-lived worker thread.
class ShellXamlStyle final {
public:
    ShellXamlStyle(
        ShellXamlElementAccessor& accessor,
        protocol::AgentTarget target) noexcept;

    [[nodiscard]] bool Configure(
        bool enabled,
        std::uint32_t taskbar_opacity_milli,
        bool taskbar_hide_notification_center,
        bool taskbar_hide_control_center,
        bool taskbar_hide_show_desktop,
        bool taskbar_capsule_enabled,
        bool taskbar_background_color_enabled,
        std::uint32_t taskbar_background_color,
        std::uint32_t start_menu_opacity_milli,
        bool hide_recommended,
        bool start_menu_background_color_enabled,
        std::uint32_t start_menu_background_color) noexcept;

    // Source-compatible neutral-color overload for callers that only manage
    // the original opacity and visibility settings.
    [[nodiscard]] bool Configure(
        bool enabled,
        std::uint32_t taskbar_opacity_milli,
        bool taskbar_hide_notification_center,
        bool taskbar_hide_control_center,
        bool taskbar_hide_show_desktop,
        std::uint32_t start_menu_opacity_milli,
        bool hide_recommended) noexcept {
        return Configure(
            enabled,
            taskbar_opacity_milli,
            taskbar_hide_notification_center,
            taskbar_hide_control_center,
            taskbar_hide_show_desktop,
            false,
            false,
            protocol::kDefaultShellBackgroundColor,
            start_menu_opacity_milli,
            hide_recommended,
            false,
            protocol::kDefaultShellBackgroundColor);
    }

    [[nodiscard]] HRESULT OnElementAdded(
        std::uint64_t handle,
        std::wstring_view type_name,
        std::wstring_view element_name = {},
        std::uint64_t parent_handle = 0) noexcept;
    void OnElementRemoved(std::uint64_t handle) noexcept;
    [[nodiscard]] HRESULT ApplyDesiredToTrackedElements() noexcept;

    [[nodiscard]] std::size_t tracked_count() const noexcept;

private:
    struct TrackedElement final {
        std::uint64_t handle{0};
        double original_opacity{1.0};
        bool original_visible{true};
        bool owns_opacity{false};
        ShellVisibilityRule visibility_rule{ShellVisibilityRule::none};
        ShellBackgroundRule background_rule{ShellBackgroundRule::none};
        std::uint64_t original_brush{0};
        bool owns_taskbar_capsule_outline{false};
        std::uint64_t original_taskbar_capsule_outline{0};
        ShellGeometryRule geometry_rule{ShellGeometryRule::none};
        ShellThickness original_margin{};
        ShellCornerRadius original_corner_radius{};
        double capsule_outer_margin{kTaskbarCapsuleFallbackOuterMargin};
    };

    struct TaskbarLayoutRelation final {
        std::uint64_t frame_handle{0};
        std::uint64_t layout_root_handle{0};
        std::uint64_t root_grid_handle{0};
        std::uint64_t background_handle{0};
    };

    [[nodiscard]] double DesiredOpacity() const noexcept;
    [[nodiscard]] bool ShouldHide(ShellVisibilityRule rule) const noexcept;
    [[nodiscard]] bool BackgroundEnabled(ShellBackgroundRule rule) const noexcept;
    [[nodiscard]] std::uint32_t DesiredBackgroundColor(
        ShellBackgroundRule rule) const noexcept;
    [[nodiscard]] HRESULT ApplyTaskbarCapsuleOutline(
        const TrackedElement& element,
        bool enabled) noexcept;
    [[nodiscard]] HRESULT ApplyGeometry(
        const TrackedElement& element,
        bool enabled) noexcept;
    [[nodiscard]] HRESULT EnsureTaskbarLayoutRoot(
        std::uint64_t handle) noexcept;
    [[nodiscard]] HRESULT ObserveTaskbarLayoutRelation(
        std::uint64_t handle,
        std::wstring_view type_name,
        std::wstring_view element_name,
        std::uint64_t parent_handle,
        TaskbarLayoutRelation*& relation) noexcept;
    [[nodiscard]] HRESULT RefreshTaskbarLayout(
        TaskbarLayoutRelation& relation) noexcept;
    [[nodiscard]] bool TryTaskbarCapsuleOuterMargin(
        std::uint64_t root_grid_handle,
        double& outer_margin) noexcept;
    [[nodiscard]] TaskbarLayoutRelation* FindTaskbarLayoutByFrame(
        std::uint64_t handle) noexcept;
    [[nodiscard]] TaskbarLayoutRelation* FindTaskbarLayoutByRootGrid(
        std::uint64_t handle) noexcept;
    [[nodiscard]] TaskbarLayoutRelation* FindTaskbarLayoutByBackground(
        std::uint64_t handle) noexcept;
    [[nodiscard]] TaskbarLayoutRelation* FindEmptyTaskbarLayout() noexcept;
    void ForgetTaskbarLayoutHandle(std::uint64_t handle) noexcept;
    [[nodiscard]] TrackedElement* Find(std::uint64_t handle) noexcept;
    [[nodiscard]] TrackedElement* FindEmpty() noexcept;
    void Forget(std::uint64_t handle) noexcept;

    ShellXamlElementAccessor& accessor_;
    const protocol::AgentTarget target_;
    std::atomic<bool> enabled_{false};
    std::atomic<std::uint32_t> taskbar_opacity_milli_{
        kMaximumTaskbarOpacityMilli};
    std::atomic<bool> taskbar_hide_notification_center_{false};
    std::atomic<bool> taskbar_hide_control_center_{false};
    std::atomic<bool> taskbar_hide_show_desktop_{false};
    std::atomic<bool> taskbar_capsule_enabled_{false};
    std::atomic<bool> taskbar_background_color_enabled_{false};
    std::atomic<std::uint32_t> taskbar_background_color_{
        protocol::kDefaultShellBackgroundColor};
    std::atomic<std::uint32_t> start_menu_opacity_milli_{
        kMaximumStartMenuOpacityMilli};
    std::atomic<bool> hide_recommended_{false};
    std::atomic<bool> start_menu_background_color_enabled_{false};
    std::atomic<std::uint32_t> start_menu_background_color_{
        protocol::kDefaultShellBackgroundColor};
    std::atomic<std::size_t> tracked_count_{0};
    std::array<TrackedElement, kMaximumTrackedShellXamlElements> tracked_{};
    std::array<TaskbarLayoutRelation, kMaximumTrackedTaskbarLayouts>
        taskbar_layouts_{};
};

}  // namespace metaplasia::agent
