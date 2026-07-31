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
    const std::uint32_t start_menu_background_color) noexcept {
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

    if (!owns_opacity && visibility_rule == ShellVisibilityRule::none &&
        background_rule == ShellBackgroundRule::none &&
        !owns_taskbar_capsule_outline &&
        geometry_rule == ShellGeometryRule::none) {
        if (taskbar_layout != nullptr) {
            const HRESULT refresh_result =
                RefreshTaskbarLayout(*taskbar_layout);
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

    TrackedElement* slot = FindEmpty();
    if (slot == nullptr) {
        accessor_.ReleaseBrushSnapshot(original_brush);
        accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
            original_taskbar_capsule_outline);
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
        accessor_.ReleaseBrushSnapshot(original_brush);
        accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
            original_taskbar_capsule_outline);
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
    return write_result;
}

void ShellXamlStyle::OnElementRemoved(const std::uint64_t handle) noexcept {
    ForgetTaskbarLayoutHandle(handle);
    Forget(handle);
}

HRESULT ShellXamlStyle::ApplyDesiredToTrackedElements() noexcept {
    const bool enabled = enabled_.load(std::memory_order_acquire);
    const double desired_opacity = DesiredOpacity();
    HRESULT first_failure = S_OK;

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
        constexpr HRESULT element_not_found =
            HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        if (SUCCEEDED(result) || result == element_not_found) {
            if (!enabled || result == element_not_found) {
                accessor_.ReleaseBrushSnapshot(element.original_brush);
                accessor_.ReleaseTaskbarCapsuleOutlineSnapshot(
                    element.original_taskbar_capsule_outline);
                element = {};
                tracked_count_.fetch_sub(1, std::memory_order_release);
            }
        } else if (SUCCEEDED(first_failure)) {
            first_failure = result;
        }
    }
    if (!enabled && tracked_count_.load(std::memory_order_acquire) == 0) {
        taskbar_layouts_.fill({});
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
        *element = {};
        tracked_count_.fetch_sub(1, std::memory_order_release);
    }
}

}  // namespace metaplasia::agent
