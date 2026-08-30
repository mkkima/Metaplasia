#include "metaplasia/agent/start_menu_style.hpp"

#include <algorithm>
#include <cmath>

namespace metaplasia::agent {
namespace {

constexpr std::array<std::wstring_view, 3> kSupportedRootTypes{
    L"StartDocked.StartSizingFrame",
    L"StartMenu.StartInnerFrame",
    L"StartMenu.StartBlendedFlexFrame",
};

[[nodiscard]] bool IsTaskbarFrame(
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    return type_name == L"Taskbar.TaskbarFrame" &&
           element_name == L"TaskbarFrame";
}

[[nodiscard]] bool IsTaskbarRootGrid(
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    return type_name == L"Windows.UI.Xaml.Controls.Grid" &&
           element_name == L"RootGrid";
}

[[nodiscard]] bool IsTaskbarBackground(
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    return type_name == L"Taskbar.TaskbarBackground" &&
           element_name == L"BackgroundControl";
}

}  // namespace

bool IsSupportedStartMenuRootType(
    const std::wstring_view type_name) noexcept {
    return std::ranges::find(kSupportedRootTypes, type_name) !=
           kSupportedRootTypes.end();
}

bool IsStartMenuRecommendedElement(
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    return type_name == L"Windows.UI.Xaml.Controls.Grid" &&
           element_name == L"MoreSuggestionsRoot";
}

bool IsSupportedTaskbarOpacityElement(
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    return (type_name == L"Taskbar.TaskbarFrame" &&
            element_name == L"TaskbarFrame") ||
           (type_name == L"SystemTray.SystemTrayFrame" &&
            element_name.empty());
}

bool IsTaskbarCapsuleOutlineElement(
    const protocol::AgentTarget target,
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    return target == protocol::AgentTarget::explorer_shell &&
           type_name == L"Windows.UI.Xaml.Shapes.Rectangle" &&
           element_name == L"BackgroundStroke";
}

ShellGeometryRule IdentifyShellGeometryRule(
    const protocol::AgentTarget target,
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    if (target != protocol::AgentTarget::explorer_shell) {
        return ShellGeometryRule::none;
    }
    if (type_name == L"Taskbar.TaskbarBackground" &&
        element_name == L"BackgroundControl") {
        return ShellGeometryRule::taskbar_capsule_background;
    }
    return ShellGeometryRule::none;
}

StartMenuLayoutRule IdentifyStartMenuLayoutRule(
    const protocol::AgentTarget target,
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    if (target != protocol::AgentTarget::start_menu) {
        return StartMenuLayoutRule::none;
    }
    if (type_name == L"StartMenu.StartBlendedFlexFrame" &&
        element_name.empty()) {
        return StartMenuLayoutRule::frame;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        (element_name == L"FrameRoot" ||
         element_name == L"AnimationRoot" ||
         element_name == L"MainMenu")) {
        return StartMenuLayoutRule::frame_container;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Border" &&
        element_name == L"StartDropShadow") {
        return StartMenuLayoutRule::frame_shadow;
    }
    if (type_name == L"StartMenu.SearchBoxToggleButton" &&
        element_name == L"SearchBoxToggleButton") {
        return StartMenuLayoutRule::search_box;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        element_name == L"NavPanePlaceholder") {
        return StartMenuLayoutRule::navigation_pane;
    }
    if (type_name == L"StartDocked.NavigationPaneView" &&
        element_name == L"UserControl") {
        return StartMenuLayoutRule::navigation_content;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        element_name == L"TopLevelHeader") {
        return StartMenuLayoutRule::top_level_header;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        element_name == L"AllListHeading") {
        return StartMenuLayoutRule::all_apps_heading;
    }
    // AllAppsGrid is also the native host for the pinned Start content on
    // current Windows 11 builds. Moving or resizing it removes the pinned
    // section from the centre panel. The independent All apps panel is built
    // by the XAML adapter instead, so this native container must stay intact.
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        element_name == L"PinnedListHeaderGrid") {
        return StartMenuLayoutRule::pinned_heading;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        element_name == L"ShowMorePinnedGrid") {
        return StartMenuLayoutRule::pinned_more;
    }
    if (type_name == L"StartMenu.PinnedList" &&
        element_name == L"StartMenuPinnedList") {
        return StartMenuLayoutRule::pinned_list;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        element_name == L"TopLevelSuggestionsRoot") {
        return StartMenuLayoutRule::recommended;
    }
    return StartMenuLayoutRule::none;
}

ShellElementLayout StartMenuLayoutFor(
    const StartMenuLayoutRule rule) noexcept {
    ShellElementLayout layout;
    switch (rule) {
        case StartMenuLayoutRule::frame:
        case StartMenuLayoutRule::frame_container:
            layout.width = kStartMenuThreePanelFrameWidth;
            layout.height = kStartMenuThreePanelFrameHeight;
            layout.horizontal_alignment = ShellHorizontalAlignment::center;
            layout.vertical_alignment = ShellVerticalAlignment::bottom;
            return layout;
        case StartMenuLayoutRule::frame_shadow:
            layout.write_size = false;
            layout.write_margin = false;
            layout.write_alignment = false;
            layout.write_visibility = true;
            layout.visible = false;
            return layout;
        case StartMenuLayoutRule::search_box:
            layout.write_size = false;
            layout.write_margin = false;
            layout.write_alignment = false;
            layout.write_visibility = true;
            layout.visible = false;
            return layout;
        case StartMenuLayoutRule::navigation_pane:
            layout.width = 280.0;
            layout.height = 72.0;
            layout.margin = {0.0, 0.0, 40.0, 30.0};
            layout.horizontal_alignment = ShellHorizontalAlignment::right;
            layout.vertical_alignment = ShellVerticalAlignment::bottom;
            // The native placeholder occupies a short footer row. Cover the
            // complete parent grid so that its native user tile is not clipped.
            // XAML clamps an oversized span to the number of available rows.
            layout.reset_grid_position = true;
            layout.grid_row_span = 16;
            return layout;
        case StartMenuLayoutRule::navigation_content:
            layout.width = 280.0;
            layout.height = 72.0;
            layout.horizontal_alignment = ShellHorizontalAlignment::left;
            layout.vertical_alignment = ShellVerticalAlignment::top;
            return layout;
        case StartMenuLayoutRule::top_level_header:
            layout.width = kStartMenuThreePanelFrameWidth;
            layout.height = kStartMenuThreePanelPanelHeight;
            layout.horizontal_alignment = ShellHorizontalAlignment::left;
            layout.vertical_alignment = ShellVerticalAlignment::top;
            layout.reset_grid_position = true;
            return layout;
        case StartMenuLayoutRule::all_apps_heading:
            layout.width = 280.0;
            layout.height = -1.0;
            layout.margin = {40.0, 24.0, 0.0, 0.0};
            layout.horizontal_alignment = ShellHorizontalAlignment::left;
            layout.vertical_alignment = ShellVerticalAlignment::top;
            layout.reset_grid_position = true;
            return layout;
        case StartMenuLayoutRule::all_apps_grid:
            layout.write_size = false;
            layout.write_margin = false;
            layout.write_alignment = false;
            return layout;
        case StartMenuLayoutRule::pinned_heading:
        case StartMenuLayoutRule::pinned_more:
            layout.width = 528.0;
            layout.height = -1.0;
            layout.margin = {344.0, 24.0, 0.0, 0.0};
            layout.horizontal_alignment = ShellHorizontalAlignment::left;
            layout.vertical_alignment = ShellVerticalAlignment::top;
            layout.reset_grid_position = true;
            return layout;
        case StartMenuLayoutRule::pinned_list:
            layout.width = 528.0;
            layout.height = kStartMenuThreePanelContentHeight;
            layout.margin = {
                344.0,
                kStartMenuThreePanelContentTop,
                0.0,
                0.0};
            layout.horizontal_alignment = ShellHorizontalAlignment::left;
            layout.vertical_alignment = ShellVerticalAlignment::top;
            layout.reset_grid_position = true;
            return layout;
        case StartMenuLayoutRule::recommended:
            layout.width = 280.0;
            layout.height = kStartMenuThreePanelRecommendedHeight;
            layout.margin = {926.0, 24.0, 0.0, 0.0};
            layout.horizontal_alignment = ShellHorizontalAlignment::left;
            layout.vertical_alignment = ShellVerticalAlignment::top;
            layout.reset_grid_position = true;
            return layout;
        case StartMenuLayoutRule::none:
        default:
            layout.write_size = false;
            layout.write_margin = false;
            layout.write_alignment = false;
            return layout;
    }
}

double CalculateTaskbarCapsuleOuterMargin(
    const double available_width) noexcept {
    if (!std::isfinite(available_width) ||
        available_width <= kTaskbarCapsuleMinimumOuterMargin * 2.0) {
        return kTaskbarCapsuleFallbackOuterMargin;
    }
    return (std::max)(
        kTaskbarCapsuleMinimumOuterMargin,
        (available_width - kTaskbarCapsuleMaximumWidth) / 2.0);
}

ShellBackgroundRule IdentifyShellBackgroundRule(
    const protocol::AgentTarget target,
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    if (target == protocol::AgentTarget::start_menu &&
        type_name == L"Windows.UI.Xaml.Controls.Border" &&
        (element_name == L"AcrylicBorder" ||
         element_name == L"AcrylicOverlay")) {
        return ShellBackgroundRule::start_menu_surface;
    }
    if (target == protocol::AgentTarget::explorer_shell &&
        type_name == L"Windows.UI.Xaml.Shapes.Rectangle" &&
        element_name == L"BackgroundFill") {
        return ShellBackgroundRule::taskbar_surface;
    }
    return ShellBackgroundRule::none;
}

ShellBrushProperty BrushPropertyFor(
    const ShellBackgroundRule rule) noexcept {
    switch (rule) {
        case ShellBackgroundRule::start_menu_surface:
            return ShellBrushProperty::background;
        case ShellBackgroundRule::taskbar_surface:
            return ShellBrushProperty::fill;
        default:
            return ShellBrushProperty::none;
    }
}

ShellVisibilityRule IdentifyShellVisibilityRule(
    const protocol::AgentTarget target,
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    if (target == protocol::AgentTarget::start_menu) {
        return IsStartMenuRecommendedElement(type_name, element_name)
            ? ShellVisibilityRule::start_menu_recommended
            : ShellVisibilityRule::none;
    }
    if (target != protocol::AgentTarget::explorer_shell) {
        return ShellVisibilityRule::none;
    }
    if (type_name == L"SystemTray.OmniButton") {
        if (element_name == L"NotificationCenterButton") {
            return ShellVisibilityRule::taskbar_notification_center;
        }
        if (element_name == L"ControlCenterButton") {
            return ShellVisibilityRule::taskbar_control_center;
        }
    }
    if (type_name == L"SystemTray.Stack" &&
        element_name == L"ShowDesktopStack") {
        return ShellVisibilityRule::taskbar_show_desktop;
    }
    return ShellVisibilityRule::none;
}

ShellXamlStyle::ShellXamlStyle(
    ShellXamlElementAccessor& accessor,
    const protocol::AgentTarget target) noexcept
    : accessor_(accessor), target_(target) {}

bool ShellXamlStyle::Configure(
    const bool enabled,
    const std::uint32_t taskbar_opacity_milli,
    const bool taskbar_hide_notification_center,
    const bool taskbar_hide_control_center,
    const bool taskbar_hide_show_desktop,
    const bool taskbar_capsule_enabled,
    const bool taskbar_background_color_enabled,
    const std::uint32_t taskbar_background_color,
    const std::uint32_t start_menu_opacity_milli,
    const bool hide_recommended,
    const bool start_menu_background_color_enabled,
    const std::uint32_t start_menu_background_color,
    const bool start_menu_three_panel_layout_enabled,
    const bool start_menu_hide_all_apps) noexcept {
    if ((target_ != protocol::AgentTarget::explorer_shell &&
         target_ != protocol::AgentTarget::start_menu) ||
        taskbar_opacity_milli < kMinimumTaskbarOpacityMilli ||
        taskbar_opacity_milli > kMaximumTaskbarOpacityMilli ||
        start_menu_opacity_milli < kMinimumStartMenuOpacityMilli ||
        start_menu_opacity_milli > kMaximumStartMenuOpacityMilli) {
        return false;
    }
    taskbar_opacity_milli_.store(
        taskbar_opacity_milli,
        std::memory_order_release);
    taskbar_hide_notification_center_.store(
        taskbar_hide_notification_center,
        std::memory_order_release);
    taskbar_hide_control_center_.store(
        taskbar_hide_control_center,
        std::memory_order_release);
    taskbar_hide_show_desktop_.store(
        taskbar_hide_show_desktop,
        std::memory_order_release);
    taskbar_capsule_enabled_.store(
        taskbar_capsule_enabled,
        std::memory_order_release);
    taskbar_background_color_enabled_.store(
        taskbar_background_color_enabled,
        std::memory_order_release);
    taskbar_background_color_.store(
        taskbar_background_color,
        std::memory_order_release);
    start_menu_opacity_milli_.store(
        start_menu_opacity_milli,
        std::memory_order_release);
    hide_recommended_.store(hide_recommended, std::memory_order_release);
    start_menu_background_color_enabled_.store(
        start_menu_background_color_enabled,
        std::memory_order_release);
    start_menu_background_color_.store(
        start_menu_background_color,
        std::memory_order_release);
    start_menu_three_panel_layout_enabled_.store(
        start_menu_three_panel_layout_enabled,
        std::memory_order_release);
    start_menu_hide_all_apps_.store(
        start_menu_hide_all_apps,
        std::memory_order_release);
    enabled_.store(enabled, std::memory_order_release);
    return true;
}

HRESULT ShellXamlStyle::OnElementAdded(
    const std::uint64_t handle,
    const std::wstring_view type_name,
    const std::wstring_view element_name,
    const std::uint64_t parent_handle) noexcept {
    const bool owns_opacity =
        (target_ == protocol::AgentTarget::start_menu &&
         IsSupportedStartMenuRootType(type_name)) ||
        (target_ == protocol::AgentTarget::explorer_shell &&
         IsSupportedTaskbarOpacityElement(type_name, element_name));
    const ShellVisibilityRule visibility_rule =
        IdentifyShellVisibilityRule(target_, type_name, element_name);
    const ShellBackgroundRule background_rule =
        IdentifyShellBackgroundRule(target_, type_name, element_name);
    const bool owns_taskbar_capsule_outline =
        IsTaskbarCapsuleOutlineElement(
            target_,
            type_name,
            element_name);
    const ShellGeometryRule geometry_rule =
        IdentifyShellGeometryRule(target_, type_name, element_name);
    const StartMenuLayoutRule start_menu_layout_rule =
        IdentifyStartMenuLayoutRule(target_, type_name, element_name);
    const bool enabled = enabled_.load(std::memory_order_acquire);
    if (handle == 0 || !enabled) {
        return S_FALSE;
    }

    TaskbarLayoutRelation* taskbar_layout = nullptr;
    const HRESULT observe_result = ObserveTaskbarLayoutRelation(
        handle,
        type_name,
        element_name,
        parent_handle,
        taskbar_layout);
    if (FAILED(observe_result)) {
        return observe_result;
    }
    const HRESULT start_menu_observe_result = ObserveStartMenuLayoutRelation(
        handle,
        type_name,
        element_name,
        parent_handle);
    if (FAILED(start_menu_observe_result)) {
        return start_menu_observe_result;
    }

    if (!owns_opacity && visibility_rule == ShellVisibilityRule::none &&
        background_rule == ShellBackgroundRule::none &&
        !owns_taskbar_capsule_outline &&
        geometry_rule == ShellGeometryRule::none &&
        start_menu_layout_rule == StartMenuLayoutRule::none) {
        if (taskbar_layout != nullptr) {
            const HRESULT refresh_result =
                RefreshTaskbarLayout(*taskbar_layout);
            if (FAILED(refresh_result)) {
                return refresh_result;
            }
        }
        if (start_menu_observe_result == S_OK ||
            StartMenuSceneNeedsRetry()) {
            const HRESULT refresh_result = RefreshStartMenuLayout();
            if (FAILED(refresh_result)) {
                return refresh_result;
            }
        }
        return S_FALSE;
    }

    if (TrackedElement* existing = Find(handle); existing != nullptr) {
        HRESULT result = S_OK;
        if (existing->owns_opacity) {
            result = accessor_.WriteOpacity(handle, DesiredOpacity());
        }
        if (SUCCEEDED(result) &&
            existing->visibility_rule != ShellVisibilityRule::none) {
            result = accessor_.WriteVisibility(
                handle,
                ShouldHide(existing->visibility_rule)
                    ? false
                    : existing->original_visible);
        }
        if (SUCCEEDED(result) &&
            existing->background_rule != ShellBackgroundRule::none) {
            const auto property = BrushPropertyFor(existing->background_rule);
            result = BackgroundEnabled(existing->background_rule)
                ? accessor_.WriteBrushColor(
                      handle,
                      property,
                      DesiredBackgroundColor(existing->background_rule))
                : accessor_.RestoreBrush(
                      handle,
                      property,
                      existing->original_brush);
        }
        if (SUCCEEDED(result) &&
            existing->owns_taskbar_capsule_outline) {
            result = ApplyTaskbarCapsuleOutline(*existing, true);
        }
        if (SUCCEEDED(result) &&
            existing->geometry_rule != ShellGeometryRule::none) {
            result = ApplyGeometry(*existing, true);
        }
        if (SUCCEEDED(result) &&
            existing->start_menu_layout_rule != StartMenuLayoutRule::none) {
            result = ApplyStartMenuLayout(*existing, true);
        }
        if (SUCCEEDED(result) && taskbar_layout != nullptr &&
            taskbar_layout->frame_handle == handle &&
            taskbar_layout->layout_root_handle != 0) {
            result =
                EnsureTaskbarLayoutRoot(taskbar_layout->layout_root_handle);
        }
        if (SUCCEEDED(result) && taskbar_layout != nullptr) {
            const HRESULT refresh_result =
                RefreshTaskbarLayout(*taskbar_layout);
            if (FAILED(refresh_result)) {
                result = refresh_result;
            }
        }
        if (SUCCEEDED(result) &&
            (start_menu_observe_result == S_OK ||
             StartMenuSceneNeedsRetry())) {
            result = RefreshStartMenuLayout();
        }
        return result;
    }

    double original_opacity = 1.0;
    if (owns_opacity) {
        const HRESULT read_result =
            accessor_.ReadOpacity(handle, original_opacity);
        if (FAILED(read_result)) {
            return read_result;
        }
        if (!std::isfinite(original_opacity) || original_opacity < 0.0 ||
            original_opacity > 1.0) {
            return E_UNEXPECTED;
        }
    }
    bool original_visible = true;
    if (visibility_rule != ShellVisibilityRule::none) {
        const HRESULT read_result =
            accessor_.ReadVisibility(handle, original_visible);
        if (FAILED(read_result)) {
            return read_result;
        }
    }
    std::uint64_t original_brush = 0;
    if (background_rule != ShellBackgroundRule::none) {
        const HRESULT read_result = accessor_.CaptureBrush(
            handle,
            BrushPropertyFor(background_rule),
            original_brush);
        if (FAILED(read_result)) {
            return read_result;
        }
    }
    std::uint64_t original_taskbar_capsule_outline = 0;
    if (owns_taskbar_capsule_outline) {
        const HRESULT read_result =
            accessor_.CaptureTaskbarCapsuleOutline(
                handle,
                original_taskbar_capsule_outline);
        if (FAILED(read_result)) {
            accessor_.ReleaseBrushSnapshot(original_brush);
            return read_result;
        }
    }
    ShellThickness original_margin;
    ShellCornerRadius original_corner_radius;
    double capsule_outer_margin = kTaskbarCapsuleFallbackOuterMargin;
    if (geometry_rule != ShellGeometryRule::none) {
        const HRESULT margin_result =
            accessor_.ReadMargin(handle, original_margin);
        if (FAILED(margin_result)) {
            accessor_.ReleaseBrushSnapshot(original_brush);
            accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
                original_taskbar_capsule_outline);
            return margin_result;
        }
        const bool margin_is_finite =
            std::isfinite(original_margin.left) &&
            std::isfinite(original_margin.top) &&
            std::isfinite(original_margin.right) &&
            std::isfinite(original_margin.bottom);
        if (!margin_is_finite) {
            accessor_.ReleaseBrushSnapshot(original_brush);
            accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
                original_taskbar_capsule_outline);
            return E_UNEXPECTED;
        }
        if (geometry_rule ==
            ShellGeometryRule::taskbar_capsule_background) {
            const HRESULT radius_result =
                accessor_.ReadCornerRadius(handle, original_corner_radius);
            if (FAILED(radius_result)) {
                accessor_.ReleaseBrushSnapshot(original_brush);
                accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
                    original_taskbar_capsule_outline);
                return radius_result;
            }
            const bool radius_is_finite =
                std::isfinite(original_corner_radius.top_left) &&
                std::isfinite(original_corner_radius.top_right) &&
                std::isfinite(original_corner_radius.bottom_right) &&
                std::isfinite(original_corner_radius.bottom_left);
            if (!radius_is_finite) {
                accessor_.ReleaseBrushSnapshot(original_brush);
                accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
                    original_taskbar_capsule_outline);
                return E_UNEXPECTED;
            }
        }

        if (geometry_rule ==
            ShellGeometryRule::taskbar_capsule_background) {
            static_cast<void>(TryTaskbarCapsuleOuterMargin(
                parent_handle,
                capsule_outer_margin));
        }
    }
    std::uint64_t original_element_layout = 0;
    if (start_menu_layout_rule != StartMenuLayoutRule::none) {
        const HRESULT layout_result = accessor_.CaptureElementLayout(
            handle,
            original_element_layout);
        if (FAILED(layout_result)) {
            accessor_.ReleaseBrushSnapshot(original_brush);
            accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
                original_taskbar_capsule_outline);
            return layout_result;
        }
    }

    TrackedElement* slot = FindEmpty();
    if (slot == nullptr) {
        accessor_.ReleaseBrushSnapshot(original_brush);
        accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
            original_taskbar_capsule_outline);
        accessor_.ReleaseElementLayoutSnapshot(original_element_layout);
        return HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
    }
    slot->handle = handle;
    slot->original_opacity = original_opacity;
    slot->original_visible = original_visible;
    slot->owns_opacity = owns_opacity;
    slot->visibility_rule = visibility_rule;
    slot->background_rule = background_rule;
    slot->original_brush = original_brush;
    slot->owns_taskbar_capsule_outline =
        owns_taskbar_capsule_outline;
    slot->original_taskbar_capsule_outline =
        original_taskbar_capsule_outline;
    slot->geometry_rule = geometry_rule;
    slot->start_menu_layout_rule = start_menu_layout_rule;
    slot->original_element_layout = original_element_layout;
    slot->original_margin = original_margin;
    slot->original_corner_radius = original_corner_radius;
    slot->capsule_outer_margin = capsule_outer_margin;
    tracked_count_.fetch_add(1, std::memory_order_release);

    HRESULT write_result = S_OK;
    if (owns_opacity) {
        write_result = accessor_.WriteOpacity(handle, DesiredOpacity());
    }
    if (SUCCEEDED(write_result) &&
        visibility_rule != ShellVisibilityRule::none &&
        ShouldHide(visibility_rule)) {
        write_result = accessor_.WriteVisibility(handle, false);
        if (FAILED(write_result) && owns_opacity) {
            const HRESULT restore_result =
                accessor_.WriteOpacity(handle, original_opacity);
            if (FAILED(restore_result)) {
                write_result = restore_result;
            }
        }
    }
    if (SUCCEEDED(write_result) &&
        background_rule != ShellBackgroundRule::none &&
        BackgroundEnabled(background_rule)) {
        write_result = accessor_.WriteBrushColor(
            handle,
            BrushPropertyFor(background_rule),
            DesiredBackgroundColor(background_rule));
    }
    if (SUCCEEDED(write_result) && owns_taskbar_capsule_outline) {
        write_result = ApplyTaskbarCapsuleOutline(*slot, true);
    }
    if (SUCCEEDED(write_result) &&
        geometry_rule != ShellGeometryRule::none) {
        write_result = ApplyGeometry(*slot, true);
    }
    if (SUCCEEDED(write_result) &&
        start_menu_layout_rule != StartMenuLayoutRule::none) {
        write_result = ApplyStartMenuLayout(*slot, true);
    }
    if (FAILED(write_result)) {
        if (owns_opacity) {
            static_cast<void>(
                accessor_.WriteOpacity(handle, original_opacity));
        }
        if (visibility_rule != ShellVisibilityRule::none) {
            static_cast<void>(
                accessor_.WriteVisibility(handle, original_visible));
        }
        if (background_rule != ShellBackgroundRule::none) {
            static_cast<void>(accessor_.RestoreBrush(
                handle,
                BrushPropertyFor(background_rule),
                original_brush));
        }
        if (owns_taskbar_capsule_outline) {
            static_cast<void>(
                accessor_.RestoreTaskbarCapsuleOutline(
                    handle,
                    original_taskbar_capsule_outline));
        }
        if (geometry_rule != ShellGeometryRule::none) {
            static_cast<void>(
                accessor_.WriteMargin(handle, original_margin));
            if (geometry_rule ==
                ShellGeometryRule::taskbar_capsule_background) {
                static_cast<void>(accessor_.WriteCornerRadius(
                    handle,
                    original_corner_radius));
            }
        }
        if (start_menu_layout_rule != StartMenuLayoutRule::none) {
            static_cast<void>(accessor_.RestoreElementLayout(
                handle,
                original_element_layout));
        }
        accessor_.ReleaseBrushSnapshot(original_brush);
        accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
            original_taskbar_capsule_outline);
        accessor_.ReleaseElementLayoutSnapshot(original_element_layout);
        *slot = {};
        tracked_count_.fetch_sub(1, std::memory_order_release);
    }
    if (SUCCEEDED(write_result) && taskbar_layout != nullptr &&
        taskbar_layout->frame_handle == handle &&
        taskbar_layout->layout_root_handle != 0) {
        write_result =
            EnsureTaskbarLayoutRoot(taskbar_layout->layout_root_handle);
    }
    if (SUCCEEDED(write_result) && taskbar_layout != nullptr) {
        const HRESULT refresh_result =
            RefreshTaskbarLayout(*taskbar_layout);
        if (FAILED(refresh_result)) {
            write_result = refresh_result;
        }
    }
    if (SUCCEEDED(write_result) &&
        (start_menu_observe_result == S_OK ||
         StartMenuSceneNeedsRetry())) {
        write_result = RefreshStartMenuLayout();
    }
    return write_result;
}

void ShellXamlStyle::OnElementRemoved(const std::uint64_t handle) noexcept {
    if (start_menu_layout_mutation_in_progress_ &&
        handle == start_menu_layout_.recommended_handle) {
        return;
    }
    ForgetTaskbarLayoutHandle(handle);
    ForgetStartMenuLayoutHandle(handle);
    Forget(handle);
}

HRESULT ShellXamlStyle::ApplyDesiredToTrackedElements() noexcept {
    const bool enabled = enabled_.load(std::memory_order_acquire);
    const double desired_opacity = DesiredOpacity();
    HRESULT first_failure = S_OK;

    const bool three_panel_enabled =
        enabled && target_ == protocol::AgentTarget::start_menu &&
        start_menu_three_panel_layout_enabled_.load(
            std::memory_order_acquire);
    if (three_panel_enabled) {
        const HRESULT result = RefreshStartMenuLayout();
        if (FAILED(result)) {
            first_failure = result;
        }
    }

    for (auto& element : tracked_) {
        if (element.handle == 0) {
            continue;
        }
        HRESULT result = S_OK;
        if (element.owns_opacity) {
            result = accessor_.WriteOpacity(
                element.handle,
                enabled ? desired_opacity : element.original_opacity);
        }
        if (SUCCEEDED(result) &&
            element.visibility_rule != ShellVisibilityRule::none) {
            result = accessor_.WriteVisibility(
                element.handle,
                enabled && ShouldHide(element.visibility_rule)
                    ? false
                    : element.original_visible);
        }
        if (SUCCEEDED(result) &&
            element.background_rule != ShellBackgroundRule::none) {
            const auto property = BrushPropertyFor(element.background_rule);
            result = enabled && BackgroundEnabled(element.background_rule)
                ? accessor_.WriteBrushColor(
                      element.handle,
                      property,
                      DesiredBackgroundColor(element.background_rule))
                : accessor_.RestoreBrush(
                      element.handle,
                      property,
                      element.original_brush);
        }
        if (SUCCEEDED(result) &&
            element.owns_taskbar_capsule_outline) {
            result = ApplyTaskbarCapsuleOutline(element, enabled);
        }
        if (SUCCEEDED(result) &&
            element.geometry_rule != ShellGeometryRule::none) {
            result = ApplyGeometry(element, enabled);
        }
        if (SUCCEEDED(result) &&
            element.start_menu_layout_rule != StartMenuLayoutRule::none) {
            result = ApplyStartMenuLayout(element, enabled);
        }
        constexpr HRESULT element_not_found =
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (SUCCEEDED(result) || result == element_not_found) {
            if (!enabled || result == element_not_found) {
                accessor_.ReleaseBrushSnapshot(element.original_brush);
                accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
                    element.original_taskbar_capsule_outline);
                accessor_.ReleaseElementLayoutSnapshot(
                    element.original_element_layout);
                element = {};
                tracked_count_.fetch_sub(1, std::memory_order_release);
            }
        } else if (SUCCEEDED(first_failure)) {
            first_failure = result;
        }
    }
    const HRESULT layout_result = RefreshStartMenuLayout();
    if (FAILED(layout_result) && SUCCEEDED(first_failure)) {
        first_failure = layout_result;
    }
    if (!enabled && tracked_count_.load(std::memory_order_acquire) == 0) {
        taskbar_layouts_.fill({});
        start_menu_layout_ = {};
    }
    return first_failure;
}

bool ShellXamlStyle::BackgroundEnabled(
    const ShellBackgroundRule rule) const noexcept {
    switch (rule) {
        case ShellBackgroundRule::start_menu_surface:
            return start_menu_background_color_enabled_.load(
                std::memory_order_acquire);
        case ShellBackgroundRule::taskbar_surface:
            return taskbar_background_color_enabled_.load(
                       std::memory_order_acquire) ||
                   taskbar_capsule_enabled_.load(
                       std::memory_order_acquire);
        default:
            return false;
    }
}

std::uint32_t ShellXamlStyle::DesiredBackgroundColor(
    const ShellBackgroundRule rule) const noexcept {
    switch (rule) {
        case ShellBackgroundRule::start_menu_surface:
            return start_menu_background_color_.load(std::memory_order_acquire);
        case ShellBackgroundRule::taskbar_surface:
            return taskbar_background_color_enabled_.load(
                       std::memory_order_acquire)
                ? taskbar_background_color_.load(std::memory_order_acquire)
                : kTaskbarCapsuleBackgroundColor;
        default:
            return protocol::kDefaultShellBackgroundColor;
    }
}

HRESULT ShellXamlStyle::ApplyTaskbarCapsuleOutline(
    const TrackedElement& element,
    const bool enabled) noexcept {
    if (!element.owns_taskbar_capsule_outline) {
        return S_OK;
    }
    const bool capsule_enabled =
        enabled &&
        taskbar_capsule_enabled_.load(std::memory_order_acquire);
    return capsule_enabled
        ? accessor_.WriteTaskbarCapsuleOutline(
              element.handle,
              kTaskbarCapsuleOutlineColor,
              kTaskbarCapsuleOutlineThickness,
              kTaskbarCapsuleOutlineCornerRadius,
              kTaskbarCapsuleOutlineInset)
        : accessor_.RestoreTaskbarCapsuleOutline(
              element.handle,
              element.original_taskbar_capsule_outline);
}

HRESULT ShellXamlStyle::ApplyGeometry(
    const TrackedElement& element,
    const bool enabled) noexcept {
    if (element.geometry_rule == ShellGeometryRule::none) {
        return S_OK;
    }

    const bool capsule_enabled =
        enabled &&
        taskbar_capsule_enabled_.load(std::memory_order_acquire);
    ShellThickness margin = element.original_margin;
    if (capsule_enabled) {
        if (element.geometry_rule ==
            ShellGeometryRule::taskbar_capsule_layout_root) {
            margin.right += element.capsule_outer_margin;
        } else if (
            element.geometry_rule ==
            ShellGeometryRule::taskbar_capsule_background) {
            margin.left += element.capsule_outer_margin;
            margin.top += kTaskbarCapsuleVerticalMargin;
            margin.right += element.capsule_outer_margin;
            margin.bottom += kTaskbarCapsuleVerticalMargin;
        }
    }

    HRESULT result = accessor_.WriteMargin(element.handle, margin);
    if (FAILED(result) ||
        element.geometry_rule !=
            ShellGeometryRule::taskbar_capsule_background) {
        return result;
    }
    const ShellCornerRadius radius = capsule_enabled
        ? ShellCornerRadius{
              kTaskbarCapsuleCornerRadius,
              kTaskbarCapsuleCornerRadius,
              kTaskbarCapsuleCornerRadius,
              kTaskbarCapsuleCornerRadius}
        : element.original_corner_radius;
    result = accessor_.WriteCornerRadius(element.handle, radius);
    if (FAILED(result)) {
        static_cast<void>(accessor_.WriteMargin(
            element.handle,
            element.original_margin));
    }
    return result;
}

HRESULT ShellXamlStyle::ApplyStartMenuLayout(
    const TrackedElement& element,
    const bool enabled) noexcept {
    if (element.start_menu_layout_rule == StartMenuLayoutRule::none) {
        return S_OK;
    }
    const bool layout_enabled =
        enabled && target_ == protocol::AgentTarget::start_menu &&
        start_menu_three_panel_layout_enabled_.load(
            std::memory_order_acquire) &&
        start_menu_layout_.scene_active;
    if (!layout_enabled) {
        return accessor_.RestoreElementLayout(
            element.handle,
            element.original_element_layout);
    }
    if (element.handle != start_menu_layout_.frame_handle) {
        bool belongs_to_active_frame = false;
        const HRESULT relation_result = accessor_.IsDescendantOf(
            element.handle,
            start_menu_layout_.frame_handle,
            belongs_to_active_frame);
        if (FAILED(relation_result)) {
            return relation_result;
        }
        if (!belongs_to_active_frame) {
            return accessor_.RestoreElementLayout(
                element.handle,
                element.original_element_layout);
        }
    }
    return accessor_.WriteElementLayout(
        element.handle,
        StartMenuLayoutFor(element.start_menu_layout_rule));
}

HRESULT ShellXamlStyle::ObserveStartMenuLayoutRelation(
    const std::uint64_t handle,
    const std::wstring_view type_name,
    const std::wstring_view element_name,
    const std::uint64_t parent_handle) noexcept {
    if (target_ != protocol::AgentTarget::start_menu || handle == 0) {
        return S_FALSE;
    }
    if (start_menu_layout_mutation_in_progress_ &&
        handle == start_menu_layout_.recommended_handle) {
        return S_FALSE;
    }
    const auto remember_candidate = [](
                                        auto& candidates,
                                        const std::uint64_t candidate_handle,
                                        const std::uint64_t candidate_parent) {
        for (auto& candidate : candidates) {
            if (candidate.handle == candidate_handle ||
                candidate.handle == 0) {
                candidate = {candidate_handle, candidate_parent};
                return;
            }
        }
        std::rotate(
            candidates.begin(),
            candidates.begin() + 1,
            candidates.end());
        candidates.back() = {candidate_handle, candidate_parent};
    };
    const auto find_child = [](
                                const auto& candidates,
                                const std::uint64_t parent) {
        for (const auto& candidate : candidates) {
            if (candidate.parent_handle == parent) {
                return candidate.handle;
            }
        }
        return std::uint64_t{0};
    };
    const auto resolve_main_surface = [&]() {
        if (start_menu_layout_.main_menu_handle == 0) {
            return;
        }
        const std::uint64_t previous_border =
            start_menu_layout_.acrylic_border_handle;
        const std::uint64_t previous_content =
            start_menu_layout_.main_content_handle;
        const std::uint64_t previous_overlay =
            start_menu_layout_.acrylic_overlay_handle;
        start_menu_layout_.acrylic_border_handle = find_child(
            start_menu_layout_.acrylic_border_candidates,
            start_menu_layout_.main_menu_handle);
        start_menu_layout_.main_content_handle = find_child(
            start_menu_layout_.main_content_candidates,
            start_menu_layout_.main_menu_handle);
        if (start_menu_layout_.main_content_handle != 0) {
            start_menu_layout_.acrylic_overlay_handle = find_child(
                start_menu_layout_.acrylic_overlay_candidates,
                start_menu_layout_.main_content_handle);
        }
        if (start_menu_layout_.scene_active &&
            (previous_border != start_menu_layout_.acrylic_border_handle ||
             previous_content != start_menu_layout_.main_content_handle ||
             previous_overlay !=
                 start_menu_layout_.acrylic_overlay_handle)) {
            ReleaseStartMenuSceneSnapshots(true);
        }
    };
    if (type_name == L"StartMenu.StartBlendedFlexFrame" &&
        element_name.empty()) {
        if (start_menu_layout_.frame_handle != 0 &&
            start_menu_layout_.frame_handle != handle) {
            ReleaseStartMenuSceneSnapshots(true);
        }
        start_menu_layout_.frame_handle = handle;
        return S_OK;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        element_name == L"MainMenu") {
        if (start_menu_layout_.main_menu_handle != 0 &&
            start_menu_layout_.main_menu_handle != handle) {
            ReleaseStartMenuSceneSnapshots(true);
        }
        start_menu_layout_.main_menu_handle = handle;
        resolve_main_surface();
        return S_OK;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Border" &&
        element_name == L"AcrylicBorder") {
        remember_candidate(
            start_menu_layout_.acrylic_border_candidates,
            handle,
            parent_handle);
        resolve_main_surface();
        return S_OK;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Border" &&
        element_name == L"AcrylicOverlay") {
        remember_candidate(
            start_menu_layout_.acrylic_overlay_candidates,
            handle,
            parent_handle);
        resolve_main_surface();
        return S_OK;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        element_name == L"MainContent") {
        remember_candidate(
            start_menu_layout_.main_content_candidates,
            handle,
            parent_handle);
        resolve_main_surface();
        return S_OK;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        element_name == L"TopLevelHeader") {
        start_menu_layout_.top_level_header_handle = handle;
        return S_OK;
    }
    if (type_name == L"Windows.UI.Xaml.Controls.Grid" &&
        element_name == L"TopLevelSuggestionsRoot" && parent_handle != 0) {
        if (start_menu_layout_.recommended_handle != 0 &&
            start_menu_layout_.recommended_handle != handle &&
            start_menu_layout_.recommended_snapshot != 0) {
            const bool mutation_was_in_progress =
                start_menu_layout_mutation_in_progress_;
            start_menu_layout_mutation_in_progress_ = true;
            accessor_.ReleaseStartMenuRecommendedSnapshot(
                start_menu_layout_.recommended_snapshot);
            start_menu_layout_mutation_in_progress_ =
                mutation_was_in_progress;
            start_menu_layout_.recommended_snapshot = 0;
        }
        start_menu_layout_.recommended_handle = handle;
        start_menu_layout_.recommended_parent_handle = parent_handle;
        return S_OK;
    }
    return S_FALSE;
}

bool ShellXamlStyle::StartMenuSceneNeedsRetry() const noexcept {
    if (target_ != protocol::AgentTarget::start_menu ||
        !enabled_.load(std::memory_order_acquire) ||
        !start_menu_three_panel_layout_enabled_.load(
            std::memory_order_acquire)) {
        return false;
    }
    if (!start_menu_layout_.scene_active) {
        return start_menu_layout_.frame_handle != 0 &&
               start_menu_layout_.main_menu_handle != 0 &&
               start_menu_layout_.acrylic_border_handle != 0 &&
               start_menu_layout_.acrylic_overlay_handle != 0;
    }
    return start_menu_layout_.all_apps_snapshot == 0 ||
           (start_menu_layout_.recommended_handle != 0 &&
            start_menu_layout_.recommended_parent_handle != 0 &&
            start_menu_layout_.recommended_snapshot == 0);
}

void ShellXamlStyle::ReleaseStartMenuSceneSnapshots(
    const bool release_frame_envelope) noexcept {
    start_menu_layout_.scene_active = false;
    const bool mutation_was_in_progress =
        start_menu_layout_mutation_in_progress_;
    start_menu_layout_mutation_in_progress_ = true;
    if (start_menu_layout_.all_apps_snapshot != 0) {
        accessor_.ReleaseStartMenuAllAppsSnapshot(
            start_menu_layout_.all_apps_snapshot);
        start_menu_layout_.all_apps_snapshot = 0;
    }
    if (start_menu_layout_.recommended_snapshot != 0) {
        accessor_.ReleaseStartMenuRecommendedSnapshot(
            start_menu_layout_.recommended_snapshot);
        start_menu_layout_.recommended_snapshot = 0;
    }
    if (start_menu_layout_.panel_surface_snapshot != 0) {
        accessor_.ReleaseStartMenuThreePanelSurfaceSnapshot(
            start_menu_layout_.panel_surface_snapshot);
        start_menu_layout_.panel_surface_snapshot = 0;
    }
    if (release_frame_envelope &&
        start_menu_layout_.frame_envelope_snapshot != 0) {
        accessor_.ReleaseStartMenuFrameEnvelopeSnapshot(
            start_menu_layout_.frame_envelope_snapshot);
        start_menu_layout_.frame_envelope_snapshot = 0;
    }
    start_menu_layout_mutation_in_progress_ = mutation_was_in_progress;
    for (auto& element : tracked_) {
        if (element.handle != 0 &&
            element.start_menu_layout_rule != StartMenuLayoutRule::none) {
            static_cast<void>(accessor_.RestoreElementLayout(
                element.handle,
                element.original_element_layout));
        }
    }
}

HRESULT ShellXamlStyle::RefreshStartMenuLayout() noexcept {
    if (target_ != protocol::AgentTarget::start_menu) {
        return S_OK;
    }
    if (start_menu_layout_mutation_in_progress_) {
        return S_OK;
    }
    const bool layout_enabled =
        enabled_.load(std::memory_order_acquire) &&
        start_menu_three_panel_layout_enabled_.load(
            std::memory_order_acquire);
    if (!layout_enabled) {
        start_menu_layout_.scene_active = false;
        HRESULT first_failure = S_OK;
        const auto preserve_first_failure =
            [&first_failure](const HRESULT candidate) noexcept {
                if (FAILED(candidate) && SUCCEEDED(first_failure)) {
                    first_failure = candidate;
                }
            };
        if (start_menu_layout_.all_apps_snapshot != 0) {
            start_menu_layout_mutation_in_progress_ = true;
            const HRESULT restore_result = accessor_.RestoreStartMenuAllApps(
                start_menu_layout_.all_apps_snapshot);
            start_menu_layout_mutation_in_progress_ = false;
            preserve_first_failure(restore_result);
            if (SUCCEEDED(restore_result)) {
                accessor_.ReleaseStartMenuAllAppsSnapshot(
                    start_menu_layout_.all_apps_snapshot);
                start_menu_layout_.all_apps_snapshot = 0;
            }
        }
        if (start_menu_layout_.recommended_snapshot != 0) {
            start_menu_layout_mutation_in_progress_ = true;
            const HRESULT restore_result =
                accessor_.RestoreStartMenuRecommended(
                    start_menu_layout_.recommended_snapshot);
            start_menu_layout_mutation_in_progress_ = false;
            preserve_first_failure(restore_result);
            if (SUCCEEDED(restore_result)) {
                accessor_.ReleaseStartMenuRecommendedSnapshot(
                    start_menu_layout_.recommended_snapshot);
                start_menu_layout_.recommended_snapshot = 0;
            }
        }
        if (start_menu_layout_.recommended_snapshot == 0 &&
            start_menu_layout_.panel_surface_snapshot != 0) {
            const HRESULT restore_result =
                accessor_.RestoreStartMenuThreePanelSurface(
                    start_menu_layout_.panel_surface_snapshot);
            preserve_first_failure(restore_result);
            if (SUCCEEDED(restore_result)) {
                accessor_.ReleaseStartMenuThreePanelSurfaceSnapshot(
                    start_menu_layout_.panel_surface_snapshot);
                start_menu_layout_.panel_surface_snapshot = 0;
            }
        }
        if (start_menu_layout_.panel_surface_snapshot == 0 &&
            start_menu_layout_.frame_envelope_snapshot != 0) {
            const HRESULT restore_result =
                accessor_.RestoreStartMenuFrameEnvelope(
                    start_menu_layout_.frame_envelope_snapshot);
            preserve_first_failure(restore_result);
            if (SUCCEEDED(restore_result)) {
                accessor_.ReleaseStartMenuFrameEnvelopeSnapshot(
                    start_menu_layout_.frame_envelope_snapshot);
                start_menu_layout_.frame_envelope_snapshot = 0;
            }
        }
        return first_failure;
    }

    const bool has_surface_dependencies =
        start_menu_layout_.frame_handle != 0 &&
        start_menu_layout_.main_menu_handle != 0 &&
        start_menu_layout_.acrylic_border_handle != 0 &&
        start_menu_layout_.acrylic_overlay_handle != 0;
    if (!has_surface_dependencies) {
        if (start_menu_layout_.frame_envelope_snapshot != 0 ||
            start_menu_layout_.panel_surface_snapshot != 0 ||
            start_menu_layout_.scene_active) {
            ReleaseStartMenuSceneSnapshots(true);
        }
        return S_OK;
    }

    bool surface_belongs_to_frame = false;
    HRESULT result = accessor_.IsDescendantOf(
        start_menu_layout_.main_menu_handle,
        start_menu_layout_.frame_handle,
        surface_belongs_to_frame);
    if (FAILED(result)) {
        ReleaseStartMenuSceneSnapshots(true);
        return result;
    }
    if (!surface_belongs_to_frame) {
        ReleaseStartMenuSceneSnapshots(true);
        return S_OK;
    }

    if (start_menu_layout_.frame_envelope_snapshot == 0) {
        result = accessor_.CreateStartMenuFrameEnvelope(
            start_menu_layout_.frame_handle,
            start_menu_layout_.frame_envelope_snapshot);
        if (FAILED(result)) {
            ReleaseStartMenuSceneSnapshots(true);
            return result;
        }
    }

    if (start_menu_layout_.panel_surface_snapshot == 0) {
        result = accessor_.CreateStartMenuThreePanelSurface(
            start_menu_layout_.main_menu_handle,
            start_menu_layout_.acrylic_border_handle,
            start_menu_layout_.acrylic_overlay_handle,
            start_menu_layout_.panel_surface_snapshot);
    } else {
        result = accessor_.UpdateStartMenuThreePanelSurface(
            start_menu_layout_.panel_surface_snapshot);
    }
    if (FAILED(result)) {
        ReleaseStartMenuSceneSnapshots(true);
        return result;
    }

    if (start_menu_layout_.recommended_snapshot == 0 &&
        start_menu_layout_.recommended_handle != 0 &&
        start_menu_layout_.recommended_parent_handle != 0) {
        bool recommendation_belongs_to_surface = false;
        result = accessor_.IsDescendantOf(
            start_menu_layout_.recommended_parent_handle,
            start_menu_layout_.main_menu_handle,
            recommendation_belongs_to_surface);
        if (FAILED(result)) {
            ReleaseStartMenuSceneSnapshots(true);
            return result;
        }
        if (recommendation_belongs_to_surface) {
            start_menu_layout_mutation_in_progress_ = true;
            result = accessor_.AttachStartMenuRecommended(
                start_menu_layout_.recommended_handle,
                start_menu_layout_.recommended_parent_handle,
                start_menu_layout_.main_menu_handle,
                start_menu_layout_.recommended_snapshot);
            start_menu_layout_mutation_in_progress_ = false;
            if (FAILED(result)) {
                ReleaseStartMenuSceneSnapshots(true);
                return result;
            }
        } else {
            // The recommendation root belongs to a retired Start tree. Forget
            // it so ordinary visual-tree traffic does not continuously retry
            // the active scene; a new root will be observed independently.
            start_menu_layout_.recommended_handle = 0;
            start_menu_layout_.recommended_parent_handle = 0;
        }
    }

    if (start_menu_layout_.all_apps_snapshot == 0) {
        start_menu_layout_mutation_in_progress_ = true;
        result = accessor_.CreateStartMenuAllAppsPanel(
            start_menu_layout_.panel_surface_snapshot,
            !start_menu_hide_all_apps_.load(std::memory_order_acquire),
            start_menu_layout_.all_apps_snapshot);
        start_menu_layout_mutation_in_progress_ = false;
    } else {
        result = accessor_.UpdateStartMenuAllAppsVisibility(
            start_menu_layout_.all_apps_snapshot,
            !start_menu_hide_all_apps_.load(std::memory_order_acquire));
    }
    if (FAILED(result)) {
        ReleaseStartMenuSceneSnapshots(true);
        return result;
    }

    const bool was_active = start_menu_layout_.scene_active;
    start_menu_layout_.scene_active = true;
    if (!was_active) {
        for (auto& element : tracked_) {
            if (element.handle == 0 ||
                element.start_menu_layout_rule ==
                    StartMenuLayoutRule::none) {
                continue;
            }
            result = ApplyStartMenuLayout(element, true);
            if (FAILED(result)) {
                ReleaseStartMenuSceneSnapshots(true);
                return result;
            }
        }
    }
    return S_OK;
}

void ShellXamlStyle::ForgetStartMenuLayoutHandle(
    const std::uint64_t handle) noexcept {
    if (handle == 0 || target_ != protocol::AgentTarget::start_menu) {
        return;
    }
    const bool surface_dependency =
        handle == start_menu_layout_.main_menu_handle ||
        handle == start_menu_layout_.acrylic_border_handle ||
        handle == start_menu_layout_.acrylic_overlay_handle ||
        handle == start_menu_layout_.main_content_handle;
    const bool all_apps_dependency =
        surface_dependency ||
        handle == start_menu_layout_.top_level_header_handle;
    const bool recommended_dependency =
        handle == start_menu_layout_.main_menu_handle ||
        handle == start_menu_layout_.recommended_handle ||
        handle == start_menu_layout_.recommended_parent_handle;
    const bool frame_dependency =
        handle == start_menu_layout_.frame_handle;
    if (frame_dependency || surface_dependency) {
        ReleaseStartMenuSceneSnapshots(true);
    } else if (all_apps_dependency &&
               start_menu_layout_.all_apps_snapshot != 0) {
        start_menu_layout_mutation_in_progress_ = true;
        accessor_.ReleaseStartMenuAllAppsSnapshot(
            start_menu_layout_.all_apps_snapshot);
        start_menu_layout_mutation_in_progress_ = false;
        start_menu_layout_.all_apps_snapshot = 0;
    }
    if (!frame_dependency && !surface_dependency &&
        recommended_dependency &&
        start_menu_layout_.recommended_snapshot != 0) {
        start_menu_layout_mutation_in_progress_ = true;
        accessor_.ReleaseStartMenuRecommendedSnapshot(
            start_menu_layout_.recommended_snapshot);
        start_menu_layout_mutation_in_progress_ = false;
        start_menu_layout_.recommended_snapshot = 0;
    }
    if (handle == start_menu_layout_.main_menu_handle) {
        start_menu_layout_.main_menu_handle = 0;
    }
    if (handle == start_menu_layout_.frame_handle) {
        start_menu_layout_.frame_handle = 0;
    }
    if (handle == start_menu_layout_.acrylic_border_handle) {
        start_menu_layout_.acrylic_border_handle = 0;
    }
    if (handle == start_menu_layout_.acrylic_overlay_handle) {
        start_menu_layout_.acrylic_overlay_handle = 0;
    }
    if (handle == start_menu_layout_.main_content_handle) {
        start_menu_layout_.main_content_handle = 0;
    }
    const auto forget_candidate = [handle](auto& candidates) {
        for (auto& candidate : candidates) {
            if (candidate.handle == handle ||
                candidate.parent_handle == handle) {
                candidate = {};
            }
        }
    };
    forget_candidate(start_menu_layout_.acrylic_border_candidates);
    forget_candidate(start_menu_layout_.acrylic_overlay_candidates);
    forget_candidate(start_menu_layout_.main_content_candidates);
    if (handle == start_menu_layout_.top_level_header_handle) {
        start_menu_layout_.top_level_header_handle = 0;
    }
    if (handle == start_menu_layout_.recommended_handle) {
        start_menu_layout_.recommended_handle = 0;
    }
    if (handle == start_menu_layout_.recommended_parent_handle) {
        start_menu_layout_.recommended_parent_handle = 0;
    }
}

HRESULT ShellXamlStyle::EnsureTaskbarLayoutRoot(
    const std::uint64_t handle) noexcept {
    if (handle == 0) {
        return E_INVALIDARG;
    }
    if (TrackedElement* existing = Find(handle); existing != nullptr) {
        if (existing->geometry_rule !=
            ShellGeometryRule::taskbar_capsule_layout_root) {
            return E_UNEXPECTED;
        }
        return ApplyGeometry(*existing, true);
    }

    ShellThickness original_margin;
    HRESULT result = accessor_.ReadMargin(handle, original_margin);
    if (FAILED(result)) {
        return result;
    }
    if (!std::isfinite(original_margin.left) ||
        !std::isfinite(original_margin.top) ||
        !std::isfinite(original_margin.right) ||
        !std::isfinite(original_margin.bottom)) {
        return E_UNEXPECTED;
    }

    double available_width = 0.0;
    double capsule_outer_margin = kTaskbarCapsuleFallbackOuterMargin;
    if (SUCCEEDED(accessor_.ReadActualWidth(handle, available_width))) {
        capsule_outer_margin =
            CalculateTaskbarCapsuleOuterMargin(available_width);
    }

    TrackedElement* slot = FindEmpty();
    if (slot == nullptr) {
        return HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
    }
    slot->handle = handle;
    slot->geometry_rule =
        ShellGeometryRule::taskbar_capsule_layout_root;
    slot->original_margin = original_margin;
    slot->capsule_outer_margin = capsule_outer_margin;
    tracked_count_.fetch_add(1, std::memory_order_release);

    result = ApplyGeometry(*slot, true);
    if (FAILED(result)) {
        *slot = {};
        tracked_count_.fetch_sub(1, std::memory_order_release);
    }
    return result;
}

HRESULT ShellXamlStyle::ObserveTaskbarLayoutRelation(
    const std::uint64_t handle,
    const std::wstring_view type_name,
    const std::wstring_view element_name,
    const std::uint64_t parent_handle,
    TaskbarLayoutRelation*& relation) noexcept {
    relation = nullptr;
    if (target_ != protocol::AgentTarget::explorer_shell ||
        parent_handle == 0) {
        return S_FALSE;
    }

    const bool is_frame = IsTaskbarFrame(type_name, element_name);
    const bool is_root_grid = IsTaskbarRootGrid(type_name, element_name);
    const bool is_background =
        IsTaskbarBackground(type_name, element_name);
    if (!is_frame && !is_root_grid && !is_background) {
        return S_FALSE;
    }

    const auto merge_relations = [](
                                     TaskbarLayoutRelation& destination,
                                     const TaskbarLayoutRelation& source)
        noexcept -> bool {
        const auto compatible = [](const std::uint64_t left,
                                   const std::uint64_t right) noexcept {
            return left == 0 || right == 0 || left == right;
        };
        if (!compatible(
                destination.frame_handle,
                source.frame_handle) ||
            !compatible(
                destination.layout_root_handle,
                source.layout_root_handle) ||
            !compatible(
                destination.root_grid_handle,
                source.root_grid_handle) ||
            !compatible(
                destination.background_handle,
                source.background_handle)) {
            return false;
        }
        if (destination.frame_handle == 0) {
            destination.frame_handle = source.frame_handle;
        }
        if (destination.layout_root_handle == 0) {
            destination.layout_root_handle =
                source.layout_root_handle;
        }
        if (destination.root_grid_handle == 0) {
            destination.root_grid_handle = source.root_grid_handle;
        }
        if (destination.background_handle == 0) {
            destination.background_handle =
                source.background_handle;
        }
        return true;
    };

    if (is_frame) {
        relation = FindTaskbarLayoutByFrame(handle);
        if (relation == nullptr) {
            relation = FindEmptyTaskbarLayout();
        }
        if (relation == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
        }
        if ((relation->frame_handle != 0 &&
             relation->frame_handle != handle) ||
            (relation->layout_root_handle != 0 &&
             relation->layout_root_handle != parent_handle)) {
            relation = nullptr;
            return E_UNEXPECTED;
        }
        relation->frame_handle = handle;
        relation->layout_root_handle = parent_handle;
        return S_OK;
    }

    if (is_root_grid) {
        TaskbarLayoutRelation* by_grid =
            FindTaskbarLayoutByRootGrid(handle);
        TaskbarLayoutRelation* by_frame =
            FindTaskbarLayoutByFrame(parent_handle);
        relation = by_frame != nullptr ? by_frame : by_grid;
        if (relation == nullptr) {
            // RootGrid is a common name outside the Taskbar. Keep it only
            // after an exact Taskbar ancestor or descendant identifies the
            // layout, otherwise unrelated XAML trees could exhaust the
            // bounded relation table.
            return S_FALSE;
        }
        if (by_grid != nullptr && by_frame != nullptr &&
            by_grid != by_frame) {
            if (!merge_relations(*by_frame, *by_grid)) {
                relation = nullptr;
                return E_UNEXPECTED;
            }
            *by_grid = {};
            relation = by_frame;
        }
        if ((relation->frame_handle != 0 &&
             relation->frame_handle != parent_handle) ||
            (relation->root_grid_handle != 0 &&
             relation->root_grid_handle != handle)) {
            relation = nullptr;
            return E_UNEXPECTED;
        }
        relation->frame_handle = parent_handle;
        relation->root_grid_handle = handle;
        return S_OK;
    }

    TaskbarLayoutRelation* by_background =
        FindTaskbarLayoutByBackground(handle);
    TaskbarLayoutRelation* by_grid =
        FindTaskbarLayoutByRootGrid(parent_handle);
    relation = by_grid != nullptr ? by_grid : by_background;
    if (relation == nullptr) {
        relation = FindEmptyTaskbarLayout();
    }
    if (relation == nullptr) {
        return HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
    }
    if (by_background != nullptr && by_grid != nullptr &&
        by_background != by_grid) {
        if (!merge_relations(*by_grid, *by_background)) {
            relation = nullptr;
            return E_UNEXPECTED;
        }
        *by_background = {};
        relation = by_grid;
    }
    if ((relation->root_grid_handle != 0 &&
         relation->root_grid_handle != parent_handle) ||
        (relation->background_handle != 0 &&
         relation->background_handle != handle)) {
        relation = nullptr;
        return E_UNEXPECTED;
    }
    relation->root_grid_handle = parent_handle;
    relation->background_handle = handle;
    return S_OK;
}

HRESULT ShellXamlStyle::RefreshTaskbarLayout(
    TaskbarLayoutRelation& relation) noexcept {
    if (relation.layout_root_handle == 0 ||
        relation.background_handle == 0) {
        return S_FALSE;
    }

    TrackedElement* layout_root = Find(relation.layout_root_handle);
    TrackedElement* background = Find(relation.background_handle);
    if (layout_root == nullptr || background == nullptr) {
        return S_FALSE;
    }
    if (layout_root->geometry_rule !=
            ShellGeometryRule::taskbar_capsule_layout_root ||
        background->geometry_rule !=
            ShellGeometryRule::taskbar_capsule_background) {
        return E_UNEXPECTED;
    }

    background->capsule_outer_margin =
        layout_root->capsule_outer_margin;
    return ApplyGeometry(*background, true);
}

bool ShellXamlStyle::TryTaskbarCapsuleOuterMargin(
    const std::uint64_t root_grid_handle,
    double& outer_margin) noexcept {
    if (root_grid_handle == 0) {
        return false;
    }
    const TaskbarLayoutRelation* relation =
        FindTaskbarLayoutByRootGrid(root_grid_handle);
    if (relation == nullptr || relation->layout_root_handle == 0) {
        return false;
    }
    TrackedElement* layout_root = Find(relation->layout_root_handle);
    if (layout_root == nullptr ||
        layout_root->geometry_rule !=
            ShellGeometryRule::taskbar_capsule_layout_root) {
        return false;
    }
    outer_margin = layout_root->capsule_outer_margin;
    return true;
}

ShellXamlStyle::TaskbarLayoutRelation*
ShellXamlStyle::FindTaskbarLayoutByFrame(
    const std::uint64_t handle) noexcept {
    if (handle == 0) {
        return nullptr;
    }
    const auto found = std::ranges::find(
        taskbar_layouts_,
        handle,
        &TaskbarLayoutRelation::frame_handle);
    return found == taskbar_layouts_.end() ? nullptr : &*found;
}

ShellXamlStyle::TaskbarLayoutRelation*
ShellXamlStyle::FindTaskbarLayoutByRootGrid(
    const std::uint64_t handle) noexcept {
    if (handle == 0) {
        return nullptr;
    }
    const auto found = std::ranges::find(
        taskbar_layouts_,
        handle,
        &TaskbarLayoutRelation::root_grid_handle);
    return found == taskbar_layouts_.end() ? nullptr : &*found;
}

ShellXamlStyle::TaskbarLayoutRelation*
ShellXamlStyle::FindTaskbarLayoutByBackground(
    const std::uint64_t handle) noexcept {
    if (handle == 0) {
        return nullptr;
    }
    const auto found = std::ranges::find(
        taskbar_layouts_,
        handle,
        &TaskbarLayoutRelation::background_handle);
    return found == taskbar_layouts_.end() ? nullptr : &*found;
}

ShellXamlStyle::TaskbarLayoutRelation*
ShellXamlStyle::FindEmptyTaskbarLayout() noexcept {
    const auto found = std::ranges::find_if(
        taskbar_layouts_,
        [](const TaskbarLayoutRelation& relation) noexcept {
            return relation.frame_handle == 0 &&
                   relation.layout_root_handle == 0 &&
                   relation.root_grid_handle == 0 &&
                   relation.background_handle == 0;
        });
    return found == taskbar_layouts_.end() ? nullptr : &*found;
}

void ShellXamlStyle::ForgetTaskbarLayoutHandle(
    const std::uint64_t handle) noexcept {
    if (handle == 0) {
        return;
    }
    for (auto& relation : taskbar_layouts_) {
        if (relation.frame_handle == handle) {
            relation.frame_handle = 0;
        }
        if (relation.layout_root_handle == handle) {
            relation.layout_root_handle = 0;
        }
        if (relation.root_grid_handle == handle) {
            relation.root_grid_handle = 0;
            relation.background_handle = 0;
        } else if (relation.background_handle == handle) {
            relation.background_handle = 0;
        }
        if (relation.frame_handle == 0 &&
            relation.root_grid_handle == 0 &&
            relation.background_handle == 0) {
            relation = {};
        }
    }
}

std::size_t ShellXamlStyle::tracked_count() const noexcept {
    return tracked_count_.load(std::memory_order_acquire);
}

double ShellXamlStyle::DesiredOpacity() const noexcept {
    return static_cast<double>(
               (target_ == protocol::AgentTarget::start_menu
                    ? start_menu_opacity_milli_.load(std::memory_order_acquire)
                    : taskbar_opacity_milli_.load(std::memory_order_acquire))) /
           1000.0;
}

bool ShellXamlStyle::ShouldHide(
    const ShellVisibilityRule rule) const noexcept {
    switch (rule) {
        case ShellVisibilityRule::start_menu_recommended:
            return hide_recommended_.load(std::memory_order_acquire);
        case ShellVisibilityRule::taskbar_notification_center:
            return taskbar_hide_notification_center_.load(
                std::memory_order_acquire);
        case ShellVisibilityRule::taskbar_control_center:
            return taskbar_hide_control_center_.load(
                std::memory_order_acquire);
        case ShellVisibilityRule::taskbar_show_desktop:
            return taskbar_hide_show_desktop_.load(
                std::memory_order_acquire);
        default:
            return false;
    }
}

ShellXamlStyle::TrackedElement* ShellXamlStyle::Find(
    const std::uint64_t handle) noexcept {
    const auto found = std::ranges::find(
        tracked_,
        handle,
        &TrackedElement::handle);
    return found == tracked_.end() ? nullptr : &*found;
}

ShellXamlStyle::TrackedElement* ShellXamlStyle::FindEmpty() noexcept {
    return Find(0);
}

void ShellXamlStyle::Forget(const std::uint64_t handle) noexcept {
    if (TrackedElement* element = Find(handle); element != nullptr) {
        accessor_.ReleaseBrushSnapshot(element->original_brush);
        accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
            element->original_taskbar_capsule_outline);
        accessor_.ReleaseElementLayoutSnapshot(
            element->original_element_layout);
        *element = {};
        tracked_count_.fetch_sub(1, std::memory_order_release);
    }
}

}  // namespace metaplasia::agent
