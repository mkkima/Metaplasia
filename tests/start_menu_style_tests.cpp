#include "metaplasia/agent/start_menu_style.hpp"

#include <Windows.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>

namespace {

using metaplasia::agent::ShellXamlElementAccessor;
using metaplasia::agent::ShellXamlStyle;
using metaplasia::agent::kStartMenuThreePanelFrameHeight;
using metaplasia::agent::kStartMenuThreePanelFrameWidth;

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

class FakeAccessor final : public ShellXamlElementAccessor {
public:
    struct OutlineState final {
        bool visible{true};
        bool hit_test_visible{true};
        bool use_layout_rounding{true};
        double width{320.0};
        double height{1.0};
        metaplasia::agent::ShellThickness margin{};
        bool fill_present{true};
        std::uint32_t stroke_color{0xFF778899U};
        double stroke_thickness{0.0};
        double radius_x{0.0};
        double radius_y{0.0};
    };

    struct Element final {
        std::uint64_t handle{0};
        double opacity{1.0};
        bool visible{true};
        bool fail_read{false};
        bool fail_write{false};
        std::size_t writes{0};
        std::size_t visibility_writes{0};
        double actual_width{1920.0};
        metaplasia::agent::ShellThickness margin{};
        metaplasia::agent::ShellCornerRadius corner_radius{};
        std::size_t geometry_writes{0};
        std::uint64_t brush{0x1234};
        std::size_t brush_writes{0};
        OutlineState outline{};
        std::size_t outline_writes{0};
        metaplasia::agent::ShellElementLayout layout{};
        bool layout_active{false};
        std::size_t layout_writes{0};
    };

    struct LayoutSnapshot final {
        bool visible{true};
        bool layout_active{false};
        metaplasia::agent::ShellElementLayout layout{};
    };

    Element& Add(const std::uint64_t handle, const double opacity = 1.0) {
        for (auto& element : elements_) {
            if (element.handle == 0) {
                element.handle = handle;
                element.opacity = opacity;
                return element;
            }
        }
        std::abort();
    }

    Element* Find(const std::uint64_t handle) noexcept {
        for (auto& element : elements_) {
            if (element.handle == handle) {
                return &element;
            }
        }
        return nullptr;
    }

    HRESULT ReadOpacity(
        const std::uint64_t handle,
        double& opacity) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_read) {
            return E_FAIL;
        }
        opacity = element->opacity;
        return S_OK;
    }

    HRESULT WriteOpacity(
        const std::uint64_t handle,
        const double opacity) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_write) {
            return E_FAIL;
        }
        element->opacity = opacity;
        ++element->writes;
        return S_OK;
    }

    HRESULT ReadVisibility(
        const std::uint64_t handle,
        bool& visible) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_read) {
            return E_FAIL;
        }
        visible = element->visible;
        return S_OK;
    }

    HRESULT WriteVisibility(
        const std::uint64_t handle,
        const bool visible) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_write) {
            return E_FAIL;
        }
        element->visible = visible;
        ++element->visibility_writes;
        return S_OK;
    }

    HRESULT ReadActualWidth(
        const std::uint64_t handle,
        double& width) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_read) {
            return E_FAIL;
        }
        width = element->actual_width;
        return S_OK;
    }

    HRESULT ReadMargin(
        const std::uint64_t handle,
        metaplasia::agent::ShellThickness& margin) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_read) {
            return E_FAIL;
        }
        margin = element->margin;
        return S_OK;
    }

    HRESULT WriteMargin(
        const std::uint64_t handle,
        const metaplasia::agent::ShellThickness& margin) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_write) {
            return E_FAIL;
        }
        element->margin = margin;
        ++element->geometry_writes;
        return S_OK;
    }

    HRESULT ReadCornerRadius(
        const std::uint64_t handle,
        metaplasia::agent::ShellCornerRadius& radius) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_read) {
            return E_FAIL;
        }
        radius = element->corner_radius;
        return S_OK;
    }

    HRESULT WriteCornerRadius(
        const std::uint64_t handle,
        const metaplasia::agent::ShellCornerRadius& radius) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_write) {
            return E_FAIL;
        }
        element->corner_radius = radius;
        ++element->geometry_writes;
        return S_OK;
    }

    HRESULT CaptureBrush(
        const std::uint64_t handle,
        metaplasia::agent::ShellBrushProperty,
        std::uint64_t& snapshot) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_read) {
            return E_FAIL;
        }
        snapshot = element->brush;
        return S_OK;
    }

    HRESULT WriteBrushColor(
        const std::uint64_t handle,
        metaplasia::agent::ShellBrushProperty,
        const std::uint32_t argb) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_write) {
            return E_FAIL;
        }
        element->brush = argb;
        ++element->brush_writes;
        return S_OK;
    }

    HRESULT RestoreBrush(
        const std::uint64_t handle,
        metaplasia::agent::ShellBrushProperty,
        const std::uint64_t snapshot) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_write) {
            return E_FAIL;
        }
        element->brush = snapshot;
        ++element->brush_writes;
        return S_OK;
    }

    void ReleaseBrushSnapshot(std::uint64_t) noexcept override {}

    HRESULT CaptureTaskbarCapsuleOutline(
        const std::uint64_t handle,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_read) {
            return E_FAIL;
        }
        auto* captured = new (std::nothrow) OutlineState(element->outline);
        if (captured == nullptr) {
            return E_OUTOFMEMORY;
        }
        snapshot = reinterpret_cast<std::uint64_t>(captured);
        return S_OK;
    }

    HRESULT WriteTaskbarCapsuleOutline(
        const std::uint64_t handle,
        const std::uint32_t argb,
        const double thickness,
        const double corner_radius,
        const double inset) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_write) {
            return E_FAIL;
        }
        element->outline.visible = true;
        element->outline.hit_test_visible = false;
        element->outline.use_layout_rounding = false;
        element->outline.width =
            (std::numeric_limits<double>::quiet_NaN)();
        element->outline.height =
            (std::numeric_limits<double>::quiet_NaN)();
        element->outline.margin = {inset, inset, inset, inset};
        element->outline.fill_present = false;
        element->outline.stroke_color = argb;
        element->outline.stroke_thickness = thickness;
        element->outline.radius_x = corner_radius;
        element->outline.radius_y = corner_radius;
        ++element->outline_writes;
        return S_OK;
    }

    HRESULT RestoreTaskbarCapsuleOutline(
        const std::uint64_t handle,
        const std::uint64_t snapshot) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_write || snapshot == 0) {
            return E_FAIL;
        }
        element->outline =
            *reinterpret_cast<const OutlineState*>(snapshot);
        ++element->outline_writes;
        return S_OK;
    }

    void ReleaseTaskbarCapsuleOutlineSnapshot(
        const std::uint64_t snapshot) noexcept override {
        delete reinterpret_cast<OutlineState*>(snapshot);
    }

    HRESULT CaptureElementLayout(
        const std::uint64_t handle,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_read) {
            return E_FAIL;
        }
        auto* captured = new (std::nothrow) LayoutSnapshot{
            element->visible,
            element->layout_active,
            element->layout};
        if (captured == nullptr) {
            return E_OUTOFMEMORY;
        }
        snapshot = reinterpret_cast<std::uint64_t>(captured);
        return S_OK;
    }

    HRESULT WriteElementLayout(
        const std::uint64_t handle,
        const metaplasia::agent::ShellElementLayout& layout) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_write) {
            return E_FAIL;
        }
        element->layout = layout;
        element->layout_active = true;
        if (layout.write_visibility) {
            element->visible = layout.visible;
        }
        ++element->layout_writes;
        return S_OK;
    }

    HRESULT RestoreElementLayout(
        const std::uint64_t handle,
        const std::uint64_t snapshot) noexcept override {
        Element* element = Find(handle);
        if (element == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (element->fail_write || snapshot == 0) {
            return E_FAIL;
        }
        const auto* captured =
            reinterpret_cast<const LayoutSnapshot*>(snapshot);
        element->visible = captured->visible;
        element->layout_active = captured->layout_active;
        element->layout = captured->layout;
        ++element->layout_writes;
        return S_OK;
    }

    void ReleaseElementLayoutSnapshot(
        const std::uint64_t snapshot) noexcept override {
        delete reinterpret_cast<LayoutSnapshot*>(snapshot);
    }

    HRESULT CreateStartMenuFrameEnvelope(
        const std::uint64_t frame_handle,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        if (Find(frame_handle) == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        if (frame_envelope_failures_remaining != 0) {
            --frame_envelope_failures_remaining;
            return E_FAIL;
        }
        auto* token = new (std::nothrow) std::uint8_t(1);
        if (token == nullptr) {
            return E_OUTOFMEMORY;
        }
        snapshot = reinterpret_cast<std::uint64_t>(token);
        frame_envelope_active = true;
        ++frame_envelope_writes;
        return S_OK;
    }

    HRESULT RestoreStartMenuFrameEnvelope(
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        frame_envelope_active = false;
        ++frame_envelope_writes;
        return S_OK;
    }

    void ReleaseStartMenuFrameEnvelopeSnapshot(
        const std::uint64_t snapshot) noexcept override {
        frame_envelope_active = false;
        delete reinterpret_cast<std::uint8_t*>(snapshot);
    }

    HRESULT CreateStartMenuThreePanelSurface(
        const std::uint64_t main_menu_handle,
        const std::uint64_t acrylic_border_handle,
        const std::uint64_t acrylic_overlay_handle,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        if (Find(main_menu_handle) == nullptr ||
            Find(acrylic_border_handle) == nullptr ||
            Find(acrylic_overlay_handle) == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        auto* token = new (std::nothrow) std::uint8_t(1);
        if (token == nullptr) {
            return E_OUTOFMEMORY;
        }
        snapshot = reinterpret_cast<std::uint64_t>(token);
        last_surface_main_menu = main_menu_handle;
        last_surface_acrylic_border = acrylic_border_handle;
        last_surface_acrylic_overlay = acrylic_overlay_handle;
        surface_active = true;
        ++surface_writes;
        return S_OK;
    }

    HRESULT UpdateStartMenuThreePanelSurface(
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0 || !surface_active) {
            return E_INVALIDARG;
        }
        ++surface_writes;
        return S_OK;
    }

    HRESULT RestoreStartMenuThreePanelSurface(
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        surface_active = false;
        ++surface_writes;
        return S_OK;
    }

    void ReleaseStartMenuThreePanelSurfaceSnapshot(
        const std::uint64_t snapshot) noexcept override {
        surface_active = false;
        delete reinterpret_cast<std::uint8_t*>(snapshot);
    }

    HRESULT AttachStartMenuRecommended(
        const std::uint64_t recommended_handle,
        const std::uint64_t original_parent_handle,
        const std::uint64_t destination_panel_handle,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        if (Find(recommended_handle) == nullptr ||
            Find(original_parent_handle) == nullptr ||
            Find(destination_panel_handle) == nullptr) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        auto* token = new (std::nothrow) std::uint8_t(1);
        if (token == nullptr) {
            return E_OUTOFMEMORY;
        }
        snapshot = reinterpret_cast<std::uint64_t>(token);
        recommended_attached = true;
        ++recommended_writes;
        return S_OK;
    }

    HRESULT RestoreStartMenuRecommended(
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        recommended_attached = false;
        ++recommended_writes;
        return S_OK;
    }

    void ReleaseStartMenuRecommendedSnapshot(
        const std::uint64_t snapshot) noexcept override {
        recommended_attached = false;
        delete reinterpret_cast<std::uint8_t*>(snapshot);
    }

    HRESULT CreateStartMenuAllAppsPanel(
        const std::uint64_t panel_surface_snapshot,
        const bool visible,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        if (panel_surface_snapshot == 0 || !surface_active) {
            return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        }
        auto* token = new (std::nothrow) std::uint8_t(1);
        if (token == nullptr) {
            return E_OUTOFMEMORY;
        }
        snapshot = reinterpret_cast<std::uint64_t>(token);
        all_apps_attached = true;
        all_apps_visible = visible;
        ++all_apps_writes;
        return S_OK;
    }

    HRESULT UpdateStartMenuAllAppsVisibility(
        const std::uint64_t snapshot,
        const bool visible) noexcept override {
        if (snapshot == 0 || !all_apps_attached) {
            return E_INVALIDARG;
        }
        all_apps_visible = visible;
        ++all_apps_writes;
        return S_OK;
    }

    HRESULT RestoreStartMenuAllApps(
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        all_apps_attached = false;
        all_apps_visible = false;
        ++all_apps_writes;
        return S_OK;
    }

    void ReleaseStartMenuAllAppsSnapshot(
        const std::uint64_t snapshot) noexcept override {
        all_apps_attached = false;
        all_apps_visible = false;
        delete reinterpret_cast<std::uint8_t*>(snapshot);
    }

    bool surface_active{false};
    bool all_apps_attached{false};
    bool all_apps_visible{false};
    bool recommended_attached{false};
    bool frame_envelope_active{false};
    std::size_t surface_writes{0};
    std::size_t all_apps_writes{0};
    std::size_t recommended_writes{0};
    std::size_t frame_envelope_writes{0};
    std::size_t frame_envelope_failures_remaining{0};
    std::uint64_t last_surface_main_menu{0};
    std::uint64_t last_surface_acrylic_border{0};
    std::uint64_t last_surface_acrylic_overlay{0};

private:
    std::array<Element, 48> elements_{};
};

bool Near(const double left, const double right) noexcept {
    return std::abs(left - right) < 0.000001;
}

}  // namespace

int main() {
    using metaplasia::agent::IsSupportedStartMenuRootType;
    using metaplasia::agent::IsStartMenuRecommendedElement;
    using metaplasia::agent::IdentifyStartMenuLayoutRule;
    using metaplasia::agent::IdentifyShellBackgroundRule;
    using metaplasia::agent::StartMenuLayoutRule;
    using metaplasia::agent::ShellBackgroundRule;
    using metaplasia::agent::kMaximumStartMenuOpacityMilli;
    using metaplasia::agent::kMaximumTrackedShellXamlElements;
    using metaplasia::agent::kMinimumStartMenuOpacityMilli;

    Require(
        IsSupportedStartMenuRootType(L"StartDocked.StartSizingFrame"),
        "accept current Start root type");
    Require(
        IsSupportedStartMenuRootType(L"StartMenu.StartInnerFrame"),
        "accept legacy Start root type");
    Require(
        IsSupportedStartMenuRootType(L"StartMenu.StartBlendedFlexFrame"),
        "accept current blended Start root type");
    Require(
        !IsSupportedStartMenuRootType(L"startdocked.startsizingframe"),
        "type allowlist is exact");
    Require(
        !IsSupportedStartMenuRootType(L"StartDocked.LauncherFrame"),
        "reject unapproved nested type");
    Require(
        IsStartMenuRecommendedElement(
            L"Windows.UI.Xaml.Controls.Grid",
            L"MoreSuggestionsRoot"),
        "accept exact Recommended root identity");
    Require(
        !IsStartMenuRecommendedElement(
            L"Windows.UI.Xaml.Controls.Grid",
            L"MoreSuggestionsContainer"),
        "reject nested suggestions container");
    Require(
        IdentifyShellBackgroundRule(
            metaplasia::protocol::AgentTarget::start_menu,
            L"Windows.UI.Xaml.Controls.Border",
            L"AcrylicBorder") == ShellBackgroundRule::start_menu_surface,
        "identify exact Start acrylic surface");
    Require(
        IdentifyShellBackgroundRule(
            metaplasia::protocol::AgentTarget::start_menu,
            L"Windows.UI.Xaml.Controls.Grid",
            L"AcrylicBorder") == ShellBackgroundRule::none,
        "reject mismatched Start surface type");
    Require(
        IdentifyStartMenuLayoutRule(
            metaplasia::protocol::AgentTarget::start_menu,
            L"StartMenu.PinnedList",
            L"StartMenuPinnedList") == StartMenuLayoutRule::pinned_list,
        "identify the exact native pinned-list container");
    Require(
        IdentifyStartMenuLayoutRule(
            metaplasia::protocol::AgentTarget::start_menu,
            L"Windows.UI.Xaml.Controls.Grid",
            L"MainMenu") == StartMenuLayoutRule::frame_container,
        "identify an exact internal frame container");
    Require(
        IdentifyStartMenuLayoutRule(
            metaplasia::protocol::AgentTarget::start_menu,
            L"Windows.UI.Xaml.Controls.Border",
            L"StartDropShadow") == StartMenuLayoutRule::frame_shadow,
        "identify the native outer Start shadow");
    Require(
        IdentifyStartMenuLayoutRule(
            metaplasia::protocol::AgentTarget::start_menu,
            L"Windows.UI.Xaml.Controls.Grid",
            L"NavPanePlaceholder") == StartMenuLayoutRule::navigation_pane,
        "position the footer through its clipping parent");
    Require(
        IdentifyStartMenuLayoutRule(
            metaplasia::protocol::AgentTarget::start_menu,
            L"StartDocked.NavigationPaneView",
            L"UserControl") == StartMenuLayoutRule::navigation_content,
        "size the native footer inside its clipping parent");
    Require(
        IdentifyStartMenuLayoutRule(
            metaplasia::protocol::AgentTarget::start_menu,
            L"Windows.UI.Xaml.Controls.GridView",
            L"AllAppsGrid") == StartMenuLayoutRule::none,
        "leave the shared native pinned-content grid intact");
    Require(
        IdentifyStartMenuLayoutRule(
            metaplasia::protocol::AgentTarget::explorer_shell,
            L"StartMenu.PinnedList",
            L"StartMenuPinnedList") == StartMenuLayoutRule::none,
        "never apply Start layout rules inside Explorer");
    const auto navigation_pane_layout =
        metaplasia::agent::StartMenuLayoutFor(
            StartMenuLayoutRule::navigation_pane);
    const auto navigation_content_layout =
        metaplasia::agent::StartMenuLayoutFor(
            StartMenuLayoutRule::navigation_content);
    Require(
        Near(navigation_pane_layout.width, 280.0) &&
            Near(navigation_pane_layout.height, 72.0) &&
            Near(navigation_pane_layout.margin.bottom, 30.0) &&
            navigation_pane_layout.reset_grid_position &&
            navigation_pane_layout.grid_row == 0 &&
            navigation_pane_layout.grid_row_span == 16 &&
            navigation_pane_layout.vertical_alignment ==
                metaplasia::agent::ShellVerticalAlignment::bottom,
        "reserve and unclip enough footer height for the native user tile");
    Require(
        Near(navigation_content_layout.width, 280.0) &&
            Near(navigation_content_layout.height, 72.0),
        "keep the native footer content inside its expanded clipping parent");

    FakeAccessor accessor;
    ShellXamlStyle style(
        accessor,
        metaplasia::protocol::AgentTarget::start_menu);
    Require(
        !style.Configure(
            true,
            1000,
            false,
            false,
            false,
            kMinimumStartMenuOpacityMilli - 1,
            false),
        "reject too-low opacity");
    Require(
        !style.Configure(
            true,
            1000,
            false,
            false,
            false,
            kMaximumStartMenuOpacityMilli + 1,
            false),
        "reject too-high opacity");
    Require(
        style.Configure(true, 1000, false, false, false, 940, false),
        "accept valid opacity");

    auto& root = accessor.Add(1, 0.82);
    Require(
        style.OnElementAdded(1, L"StartDocked.LauncherFrame") == S_FALSE,
        "ignore non-root visual");
    Require(root.writes == 0, "ignored visual is untouched");
    Require(
        style.OnElementAdded(1, L"StartDocked.StartSizingFrame") == S_OK,
        "style supported root");
    Require(Near(root.opacity, 0.94), "apply configured opacity");
    Require(style.tracked_count() == 1, "track styled root");

    Require(
        style.Configure(true, 1000, false, false, false, 730, false),
        "accept updated opacity");
    Require(
        style.ApplyDesiredToTrackedElements() == S_OK,
        "apply opacity update");
    Require(Near(root.opacity, 0.73), "update tracked root");
    Require(
        style.OnElementAdded(1, L"StartDocked.StartSizingFrame") == S_OK,
        "handle duplicate add idempotently");

    Require(
        style.Configure(false, 1000, false, false, false, 730, false),
        "disable style");
    Require(
        style.ApplyDesiredToTrackedElements() == S_OK,
        "restore original opacity");
    Require(Near(root.opacity, 0.82), "restore exact original value");
    Require(style.tracked_count() == 0, "forget restored root");

    Require(
        style.Configure(true, 1000, false, false, false, 900, false),
        "re-enable style");
    auto& failing = accessor.Add(2, 0.67);
    failing.fail_write = true;
    Require(
        style.OnElementAdded(2, L"StartMenu.StartInnerFrame") == E_FAIL,
        "surface element write failure");
    Require(Near(failing.opacity, 0.67), "failed write preserves element");
    Require(style.tracked_count() == 0, "failed write is not tracked");

    auto& recommended = accessor.Add(3);
    Require(
        style.Configure(true, 1000, false, false, false, 900, true),
        "enable Recommended visibility rule");
    Require(
        style.OnElementAdded(
            3,
            L"Windows.UI.Xaml.Controls.Grid",
            L"MoreSuggestionsRoot") == S_OK,
        "track and collapse Recommended root");
    Require(!recommended.visible, "collapse Recommended root");
    Require(
        style.Configure(true, 1000, false, false, false, 900, false),
        "disable Recommended visibility rule");
    Require(
        style.ApplyDesiredToTrackedElements() == S_OK,
        "restore Recommended root while adapter remains active");
    Require(recommended.visible, "restore original Recommended visibility");
    Require(
        style.Configure(false, 1000, false, false, false, 900, false),
        "disable adapter after Recommended test");
    Require(
        style.ApplyDesiredToTrackedElements() == S_OK,
        "release Recommended ownership on disable");
    Require(style.tracked_count() == 0, "forget restored Recommended root");

    FakeAccessor start_color_accessor;
    ShellXamlStyle start_color_style(
        start_color_accessor,
        metaplasia::protocol::AgentTarget::start_menu);
    Require(
        start_color_style.Configure(
            true,
            1000,
            false,
            false,
            false,
            false,
            false,
            0xFF000000U,
            1000,
            false,
            true,
            0xFF123456U),
        "configure independent Start color");
    auto& start_surface = start_color_accessor.Add(4);
    Require(
        start_color_style.OnElementAdded(
            4,
            L"Windows.UI.Xaml.Controls.Border",
            L"AcrylicBorder") == S_OK,
        "style Start acrylic surface");
    Require(
        start_surface.brush == 0xFF123456U,
        "apply selected Start color");
    Require(
        start_color_style.Configure(
            true,
            1000,
            false,
            false,
            false,
            false,
            false,
            0xFF000000U,
            1000,
            false,
            false,
            0xFF654321U),
        "disable Start color without disabling the target");
    Require(
        start_color_style.ApplyDesiredToTrackedElements() == S_OK,
        "restore Start brush live");
    Require(start_surface.brush == 0x1234U, "restore exact native Start brush");
    Require(
        start_color_style.Configure(
            false,
            1000,
            false,
            false,
            false,
            false,
            false,
            0xFF000000U,
            1000,
            false,
            false,
            0xFF000000U) &&
            start_color_style.ApplyDesiredToTrackedElements() == S_OK,
        "release Start brush ownership");

    FakeAccessor layout_accessor;
    ShellXamlStyle layout_style(
        layout_accessor,
        metaplasia::protocol::AgentTarget::start_menu);
    for (const std::uint64_t handle :
         {500U, 501U, 502U, 503U, 504U, 505U, 506U, 507U,
           508U, 509U, 510U, 511U, 512U, 513U, 514U, 515U,
           516U, 517U, 518U, 519U, 520U, 521U}) {
        layout_accessor.Add(handle);
    }
    Require(
        layout_style.Configure(
            true,
            1000,
            false,
            false,
            false,
            false,
            false,
            0xFF000000U,
            1000,
            false,
            false,
            0xFF000000U,
            true),
        "enable the reversible three-panel Start layout");
    Require(
        layout_style.OnElementAdded(
            500,
            L"Windows.UI.Xaml.Controls.Grid",
            L"MainMenu") == S_OK,
        "expand and observe the exact panel host");
    Require(
        layout_style.OnElementAdded(
            501,
            L"Windows.UI.Xaml.Controls.Border",
            L"AcrylicBorder",
            500) == S_OK &&
        layout_style.OnElementAdded(
            515,
            L"Windows.UI.Xaml.Controls.Grid",
            L"MainContent",
            500) == S_FALSE &&
        layout_style.OnElementAdded(
            502,
            L"Windows.UI.Xaml.Controls.Border",
            L"AcrylicOverlay",
            515) == S_OK,
        "observe both native acrylic layers before the frame is ready");
    Require(
        !layout_accessor.surface_active &&
            !layout_accessor.all_apps_attached &&
            !layout_accessor.frame_envelope_active,
        "do not construct a clipped three-panel scene without its envelope");
    Require(
        layout_style.OnElementAdded(
            518,
            L"Windows.UI.Xaml.Controls.Border",
            L"AcrylicBorder",
            519) == S_OK &&
        layout_style.OnElementAdded(
            520,
            L"Windows.UI.Xaml.Controls.Grid",
            L"MainContent",
            519) == S_FALSE &&
        layout_style.OnElementAdded(
            521,
            L"Windows.UI.Xaml.Controls.Border",
            L"AcrylicOverlay",
            520) == S_OK,
        "observe duplicate companion surfaces without selecting them");
    Require(
        layout_style.OnElementAdded(
            503,
            L"Windows.UI.Xaml.Controls.Grid",
            L"TopLevelHeader") == S_OK,
        "track the shared three-column content host");
    layout_accessor.frame_envelope_failures_remaining = 1;
    Require(
        FAILED(layout_style.OnElementAdded(
            506,
            L"StartMenu.StartBlendedFlexFrame")),
        "surface a transient frame-envelope failure");
    Require(
        !layout_accessor.surface_active &&
            !layout_accessor.frame_envelope_active,
        "keep all panels detached after the failed envelope attempt");
    Require(
        layout_style.OnElementAdded(
            504,
            L"Windows.UI.Xaml.Controls.Grid",
            L"UnrelatedVisual") == S_FALSE,
        "retry the incomplete scene on the next visual-tree event");
    Require(
        layout_accessor.surface_active && layout_accessor.all_apps_attached &&
            !layout_accessor.recommended_attached &&
            layout_accessor.frame_envelope_active,
        "create only the scene parts whose native dependencies exist");
    Require(
        layout_accessor.last_surface_main_menu == 500 &&
            layout_accessor.last_surface_acrylic_border == 501 &&
            layout_accessor.last_surface_acrylic_overlay == 502,
        "bind the three-panel surface only to the MainMenu descendants");
    Require(
        layout_style.OnElementAdded(
            507,
            L"StartMenu.SearchBoxToggleButton",
            L"SearchBoxToggleButton") == S_OK &&
        layout_style.OnElementAdded(
            508,
            L"Windows.UI.Xaml.Controls.Grid",
            L"PinnedListHeaderGrid") == S_OK &&
        layout_style.OnElementAdded(
            509,
            L"Windows.UI.Xaml.Controls.Grid",
            L"ShowMorePinnedGrid") == S_OK &&
        layout_style.OnElementAdded(
            510,
            L"StartMenu.PinnedList",
            L"StartMenuPinnedList") == S_OK &&
        layout_style.OnElementAdded(
            511,
            L"Windows.UI.Xaml.Controls.Grid",
            L"TopLevelSuggestionsRoot",
            503) == S_OK &&
        layout_style.OnElementAdded(
            512,
            L"Windows.UI.Xaml.Controls.Grid",
            L"NavPanePlaceholder") == S_OK &&
        layout_style.OnElementAdded(
            513,
            L"Windows.UI.Xaml.Controls.Grid",
            L"FrameRoot") == S_OK &&
        layout_style.OnElementAdded(
            514,
            L"Windows.UI.Xaml.Controls.Grid",
            L"AnimationRoot") == S_OK &&
        layout_style.OnElementAdded(
            516,
            L"Windows.UI.Xaml.Controls.Border",
            L"StartDropShadow") == S_OK &&
        layout_style.OnElementAdded(
            517,
            L"StartDocked.NavigationPaneView",
            L"UserControl") == S_OK,
        "position native Start sections without replacing their controls");
    Require(
        layout_accessor.surface_active && layout_accessor.all_apps_attached &&
            layout_accessor.recommended_attached &&
            layout_accessor.frame_envelope_active,
        "activate the expanded frame envelope, panel surfaces, and "
        "independent All apps list");
    Require(
        layout_accessor.Find(506)->layout_active &&
            Near(
                layout_accessor.Find(506)->layout.width,
                kStartMenuThreePanelFrameWidth) &&
            Near(
                layout_accessor.Find(506)->layout.height,
                kStartMenuThreePanelFrameHeight) &&
            layout_accessor.Find(500)->layout_active &&
            layout_accessor.Find(513)->layout_active &&
            !layout_accessor.Find(507)->visible &&
            !layout_accessor.Find(516)->visible &&
            layout_accessor.Find(517)->layout_active &&
            layout_accessor.Find(510)->layout_active &&
            layout_accessor.Find(511)->layout_active,
        "apply the reference geometry to frame, pinned, and Recommended");
    Require(
        layout_accessor.all_apps_visible,
        "show the custom All apps list by default");
    Require(
        layout_style.Configure(
            true,
            1000,
            false,
            false,
            false,
            false,
            false,
            0xFF000000U,
            1000,
            false,
            false,
            0xFF000000U,
            true,
            true) &&
            layout_style.ApplyDesiredToTrackedElements() == S_OK,
        "hide All apps content without changing three-panel geometry");
    Require(
        layout_accessor.surface_active && layout_accessor.all_apps_attached &&
            !layout_accessor.all_apps_visible &&
            layout_accessor.recommended_attached &&
            layout_accessor.frame_envelope_active,
        "keep every three-panel resource attached while All apps is empty");
    Require(
        layout_style.Configure(
            true,
            1000,
            false,
            false,
            false,
            false,
            false,
            0xFF000000U,
            1000,
            false,
            false,
            0xFF000000U,
            true,
            false) &&
            layout_style.ApplyDesiredToTrackedElements() == S_OK &&
            layout_accessor.all_apps_visible,
        "restore All apps content live without rebuilding the layout");
    Require(
        layout_style.Configure(
            true,
            1000,
            false,
            false,
            false,
            false,
            false,
            0xFF000000U,
            1000,
            false,
            false,
            0xFF000000U,
            false) &&
        layout_style.ApplyDesiredToTrackedElements() == S_OK,
        "disable the three-panel layout while Start remains injected");
    Require(
        !layout_accessor.surface_active && !layout_accessor.all_apps_attached &&
            !layout_accessor.recommended_attached &&
            !layout_accessor.frame_envelope_active &&
            layout_accessor.Find(507)->visible &&
            layout_accessor.Find(516)->visible &&
            !layout_accessor.Find(517)->layout_active &&
            !layout_accessor.Find(510)->layout_active &&
            !layout_accessor.Find(513)->layout_active,
        "restore exact native ownership when the layout switch is off");
    Require(
        layout_style.Configure(
            true,
            1000,
            false,
            false,
            false,
            false,
            false,
            0xFF000000U,
            1000,
            false,
            false,
            0xFF000000U,
            true) &&
        layout_style.ApplyDesiredToTrackedElements() == S_OK &&
        layout_accessor.surface_active &&
        layout_accessor.all_apps_attached &&
        layout_accessor.recommended_attached &&
        layout_accessor.frame_envelope_active,
        "recreate every three-panel resource after a live re-enable");
    layout_style.OnElementRemoved(506);
    Require(
        !layout_accessor.surface_active && !layout_accessor.all_apps_attached &&
            !layout_accessor.recommended_attached &&
            !layout_accessor.frame_envelope_active,
        "tear down the complete injected scene when Start recycles its root");
    layout_accessor.Add(522);
    Require(
        layout_style.OnElementAdded(
            522,
            L"StartMenu.StartBlendedFlexFrame") == S_OK &&
            !layout_accessor.surface_active &&
            !layout_accessor.all_apps_attached &&
            !layout_accessor.recommended_attached &&
            layout_accessor.frame_envelope_active,
        "wait for confirmed descendants after the recycled root returns");
    const auto reobserve_native_start_descendants = [&]() noexcept {
        return
            layout_style.OnElementAdded(
                500,
                L"Windows.UI.Xaml.Controls.Grid",
                L"MainMenu") == S_OK &&
            layout_style.OnElementAdded(
                501,
                L"Windows.UI.Xaml.Controls.Border",
                L"AcrylicBorder",
                500) == S_OK &&
            layout_style.OnElementAdded(
                515,
                L"Windows.UI.Xaml.Controls.Grid",
                L"MainContent",
                500) == S_FALSE &&
            layout_style.OnElementAdded(
                502,
                L"Windows.UI.Xaml.Controls.Border",
                L"AcrylicOverlay",
                515) == S_OK &&
            layout_style.OnElementAdded(
                503,
                L"Windows.UI.Xaml.Controls.Grid",
                L"TopLevelHeader") == S_OK &&
            layout_style.OnElementAdded(
                511,
                L"Windows.UI.Xaml.Controls.Grid",
                L"TopLevelSuggestionsRoot",
                503) == S_OK;
    };
    Require(
        reobserve_native_start_descendants() &&
            layout_accessor.surface_active &&
            layout_accessor.all_apps_attached &&
            layout_accessor.recommended_attached &&
            layout_accessor.frame_envelope_active,
        "rebind and rebuild the complete scene from the recycled tree");
    layout_accessor.Add(523);
    Require(
        layout_style.OnElementAdded(
            523,
            L"StartMenu.StartBlendedFlexFrame") == S_OK &&
            !layout_accessor.surface_active &&
            !layout_accessor.all_apps_attached &&
            !layout_accessor.recommended_attached &&
            layout_accessor.frame_envelope_active,
        "replace an overlapping old root without retaining its descendants");
    Require(
        reobserve_native_start_descendants() &&
            layout_accessor.surface_active &&
            layout_accessor.all_apps_attached &&
            layout_accessor.recommended_attached &&
            layout_accessor.frame_envelope_active,
        "rebuild against the replacement root and confirmed descendants");
    layout_style.OnElementRemoved(522);
    Require(
        layout_accessor.surface_active && layout_accessor.all_apps_attached &&
            layout_accessor.recommended_attached &&
            layout_accessor.frame_envelope_active,
        "ignore the late removal of an already replaced Start root");
    Require(
        layout_style.Configure(
            false,
            1000,
            false,
            false,
            false,
            false,
            false,
            0xFF000000U,
            1000,
            false,
            false,
            0xFF000000U,
            true) &&
        layout_style.ApplyDesiredToTrackedElements() == S_OK,
        "release all Start layout snapshots on target disable");

    Require(
        style.Configure(true, 1000, false, false, false, 900, false),
        "re-enable for capacity test");

    for (std::size_t index = 0;
         index < kMaximumTrackedShellXamlElements;
         ++index) {
        const std::uint64_t handle = 10 + index;
        accessor.Add(handle);
        Require(
            style.OnElementAdded(
                handle,
                L"StartDocked.StartSizingFrame") == S_OK,
            "fill bounded tracker");
    }
    auto& overflow = accessor.Add(100);
    Require(
        FAILED(style.OnElementAdded(100, L"StartDocked.StartSizingFrame")),
        "reject tracker overflow");
    Require(overflow.writes == 0, "overflow element remains untouched");

    style.OnElementRemoved(10);
    Require(
        style.tracked_count() == kMaximumTrackedShellXamlElements - 1,
        "forget removed visual without restoring a dead object");

    using metaplasia::agent::IdentifyShellVisibilityRule;
    using metaplasia::agent::IdentifyShellGeometryRule;
    using metaplasia::agent::IsSupportedTaskbarOpacityElement;
    using metaplasia::agent::ShellGeometryRule;
    using metaplasia::agent::ShellVisibilityRule;
    using metaplasia::protocol::AgentTarget;
    Require(
        IsSupportedTaskbarOpacityElement(
            L"Taskbar.TaskbarFrame",
            L"TaskbarFrame"),
        "accept exact Taskbar root identity");
    Require(
        !IsSupportedTaskbarOpacityElement(
            L"Taskbar.TaskbarFrame",
            L"RootGrid"),
        "reject Taskbar root with an unexpected name");
    Require(
        IdentifyShellVisibilityRule(
            AgentTarget::explorer_shell,
            L"SystemTray.OmniButton",
            L"NotificationCenterButton") ==
            ShellVisibilityRule::taskbar_notification_center,
        "identify notification-center button");
    Require(
        IdentifyShellVisibilityRule(
            AgentTarget::start_menu,
            L"SystemTray.OmniButton",
            L"NotificationCenterButton") == ShellVisibilityRule::none,
        "do not apply Taskbar identities to Start");
    Require(
        metaplasia::agent::IsTaskbarCapsuleOutlineElement(
            AgentTarget::explorer_shell,
            L"Windows.UI.Xaml.Shapes.Rectangle",
            L"BackgroundStroke"),
        "identify the exact native Taskbar outline element");
    Require(
        !metaplasia::agent::IsTaskbarCapsuleOutlineElement(
            AgentTarget::start_menu,
            L"Windows.UI.Xaml.Shapes.Rectangle",
            L"BackgroundStroke"),
        "do not apply the Taskbar outline rule to Start");
    Require(
        IdentifyShellBackgroundRule(
            AgentTarget::explorer_shell,
            L"Windows.UI.Xaml.Shapes.Rectangle",
            L"BackgroundFill") == ShellBackgroundRule::taskbar_surface,
        "identify exact Taskbar background fill");
    Require(
        IdentifyShellGeometryRule(
            AgentTarget::explorer_shell,
            L"Taskbar.TaskbarBackground",
            L"BackgroundControl") ==
            ShellGeometryRule::taskbar_capsule_background,
        "identify exact capsule background");
    Require(
        IdentifyShellGeometryRule(
            AgentTarget::explorer_shell,
            L"SystemTray.SystemTrayFrame",
            {}) == ShellGeometryRule::none,
        "do not rely on the system-tray frame margin");
    Require(
        Near(
            metaplasia::agent::CalculateTaskbarCapsuleOuterMargin(1920.0),
            250.0),
        "match the reference capsule width at 1920 pixels");
    Require(
        Near(
            metaplasia::agent::CalculateTaskbarCapsuleOuterMargin(1280.0),
            metaplasia::agent::kTaskbarCapsuleMinimumOuterMargin),
        "preserve a safe capsule width on narrow displays");
    Require(
        Near(
            metaplasia::agent::CalculateTaskbarCapsuleOuterMargin(
                std::numeric_limits<double>::quiet_NaN()),
            metaplasia::agent::kTaskbarCapsuleMinimumOuterMargin),
        "use the safe minimum margin before layout width is available");

    FakeAccessor taskbar_accessor;
    ShellXamlStyle taskbar_style(
        taskbar_accessor,
        AgentTarget::explorer_shell);
    Require(
        taskbar_style.Configure(
            true,
            720,
            true,
            false,
            true,
            true,
            false,
            0xFF203040U,
            1000,
            false,
            false,
            0xFF000000U),
        "configure Taskbar XAML rules");
    auto& taskbar_container = taskbar_accessor.Add(199);
    taskbar_container.actual_width = 1920.0;
    taskbar_container.margin = {3.0, 2.0, 5.0, 4.0};
    auto& taskbar_root = taskbar_accessor.Add(200, 0.91);
    auto& tray_root = taskbar_accessor.Add(201, 0.88);
    tray_root.margin = {0.0, 0.0, 11.0, 0.0};
    auto& notification = taskbar_accessor.Add(202);
    auto& control = taskbar_accessor.Add(203);
    auto& show_desktop = taskbar_accessor.Add(204);
    auto& taskbar_surface = taskbar_accessor.Add(205);
    auto& capsule_background = taskbar_accessor.Add(206);
    taskbar_accessor.Add(207);
    auto& capsule_background_stroke = taskbar_accessor.Add(212);
    capsule_background.margin = {7.0, 1.0, 9.0, 2.0};
    capsule_background.corner_radius = {1.0, 2.0, 3.0, 4.0};
    for (std::size_t index = 0;
         index <
         metaplasia::agent::kMaximumTrackedTaskbarLayouts + 2;
         ++index) {
        Require(
            taskbar_style.OnElementAdded(
                1000 + index,
                L"Windows.UI.Xaml.Controls.Grid",
                L"RootGrid",
                2000 + index) == S_FALSE,
            "ignore an unrelated RootGrid without consuming layout capacity");
    }
    Require(
        taskbar_style.OnElementAdded(
            206,
            L"Taskbar.TaskbarBackground",
            L"BackgroundControl",
            207) == S_OK,
        "accept the capsule background before its ancestors");
    Require(
        taskbar_style.OnElementAdded(
            207,
            L"Windows.UI.Xaml.Controls.Grid",
            L"RootGrid",
            200) == S_FALSE,
        "associate a late Taskbar RootGrid without property ownership");
    Require(
        taskbar_style.OnElementAdded(
            200,
            L"Taskbar.TaskbarFrame",
            L"TaskbarFrame",
            199) == S_OK,
        "resolve bottom-up Taskbar events and shared layout geometry");
    Require(
        taskbar_style.OnElementAdded(
            201,
            L"SystemTray.SystemTrayFrame",
            {},
            199) == S_OK,
        "style system-tray root");
    Require(
        taskbar_style.OnElementAdded(
            202,
            L"SystemTray.OmniButton",
            L"NotificationCenterButton") == S_OK,
        "own notification-center visibility");
    Require(
        taskbar_style.OnElementAdded(
            203,
            L"SystemTray.OmniButton",
            L"ControlCenterButton") == S_OK,
        "own control-center visibility");
    Require(
        taskbar_style.OnElementAdded(
            204,
            L"SystemTray.Stack",
            L"ShowDesktopStack") == S_OK,
        "own show-desktop visibility");
    Require(
        taskbar_style.OnElementAdded(
            205,
            L"Windows.UI.Xaml.Shapes.Rectangle",
            L"BackgroundFill") == S_OK,
        "own Taskbar background fill");
    Require(
        taskbar_style.OnElementAdded(
            212,
            L"Windows.UI.Xaml.Shapes.Rectangle",
            L"BackgroundStroke") == S_OK,
        "own the native Taskbar capsule outline properties");
    Require(
        Near(taskbar_root.opacity, 0.72) && Near(tray_root.opacity, 0.72),
        "apply Taskbar opacity to both visual roots");
    Require(!notification.visible, "collapse notification center");
    Require(control.visible, "preserve enabled control center");
    Require(!show_desktop.visible, "collapse show-desktop area");
    Require(
        capsule_background_stroke.outline.visible &&
            !capsule_background_stroke.outline.hit_test_visible &&
            !capsule_background_stroke.outline.use_layout_rounding &&
            std::isnan(capsule_background_stroke.outline.width) &&
            std::isnan(capsule_background_stroke.outline.height) &&
            !capsule_background_stroke.outline.fill_present &&
            capsule_background_stroke.outline.stroke_color ==
                metaplasia::agent::kTaskbarCapsuleOutlineColor &&
            Near(
                capsule_background_stroke.outline.stroke_thickness,
                metaplasia::agent::kTaskbarCapsuleOutlineThickness) &&
            Near(
                capsule_background_stroke.outline.radius_x,
                metaplasia::agent::kTaskbarCapsuleOutlineCornerRadius) &&
            Near(
                capsule_background_stroke.outline.margin.left,
                metaplasia::agent::kTaskbarCapsuleOutlineInset),
        "expand the native stroke into a non-interactive capsule outline");
    Require(
        taskbar_surface.brush ==
            metaplasia::agent::kTaskbarCapsuleBackgroundColor,
        "apply the reference dark capsule color");
    Require(
        Near(taskbar_container.margin.left, 3.0) &&
            Near(taskbar_container.margin.top, 2.0) &&
            Near(taskbar_container.margin.right, 255.0) &&
            Near(taskbar_container.margin.bottom, 4.0),
        "constrain the shared layout at the system-tray edge");
    Require(
        Near(capsule_background.margin.left, 257.0) &&
            Near(capsule_background.margin.top, 3.0) &&
            Near(capsule_background.margin.right, 259.0) &&
            Near(capsule_background.margin.bottom, 4.0),
        "place both rounded background edges inside the layout bounds");
    Require(
        Near(capsule_background.corner_radius.top_left, 12.0) &&
            Near(capsule_background.corner_radius.bottom_right, 12.0),
        "apply capsule corner radius");
    Require(
        Near(tray_root.margin.right, 11.0),
        "leave the system-tray margin under Windows ownership");

    auto& secondary_container = taskbar_accessor.Add(208);
    secondary_container.actual_width = 1280.0;
    taskbar_accessor.Add(209, 0.95);
    taskbar_accessor.Add(210);
    auto& secondary_background = taskbar_accessor.Add(211);
    Require(
        taskbar_style.OnElementAdded(
            209,
            L"Taskbar.TaskbarFrame",
            L"TaskbarFrame",
            208) == S_OK,
        "style a secondary Taskbar layout");
    Require(
        taskbar_style.OnElementAdded(
            210,
            L"Windows.UI.Xaml.Controls.Grid",
            L"RootGrid",
            209) == S_FALSE,
        "associate the secondary Taskbar RootGrid");
    Require(
        taskbar_style.OnElementAdded(
            211,
            L"Taskbar.TaskbarBackground",
            L"BackgroundControl",
            210) == S_OK,
        "style the secondary Taskbar background");
    Require(
        Near(secondary_container.margin.right, 12.0) &&
            Near(secondary_background.margin.left, 12.0) &&
            Near(secondary_background.margin.right, 12.0) &&
            Near(capsule_background.margin.left, 257.0) &&
            Near(capsule_background.margin.right, 259.0),
        "keep per-monitor capsule margins isolated");

    Require(
        taskbar_style.Configure(
            true,
            830,
            false,
            true,
            false,
            false,
            true,
            0xFF506070U,
            1000,
            false,
            false,
            0xFF000000U),
        "reconfigure Taskbar rules");
    Require(
        taskbar_style.ApplyDesiredToTrackedElements() == S_OK,
        "apply Taskbar reconfiguration");
    Require(notification.visible, "restore notification center live");
    Require(!control.visible, "collapse control center live");
    Require(show_desktop.visible, "restore show desktop live");
    Require(
        capsule_background_stroke.outline.visible &&
            capsule_background_stroke.outline.hit_test_visible &&
            capsule_background_stroke.outline.use_layout_rounding &&
            Near(capsule_background_stroke.outline.width, 320.0) &&
            Near(capsule_background_stroke.outline.height, 1.0) &&
            capsule_background_stroke.outline.fill_present &&
            capsule_background_stroke.outline.stroke_color ==
                0xFF778899U &&
            Near(
                capsule_background_stroke.outline.stroke_thickness,
                0.0),
        "restore the exact native outline when capsule mode is turned off");
    Require(
        Near(taskbar_root.opacity, 0.83) && Near(tray_root.opacity, 0.83),
        "update Taskbar opacity live");
    Require(
        taskbar_surface.brush == 0xFF506070U,
        "update Taskbar color live");
    Require(
        Near(capsule_background.margin.left, 7.0) &&
            Near(capsule_background.margin.top, 1.0) &&
            Near(capsule_background.margin.right, 9.0) &&
            Near(capsule_background.margin.bottom, 2.0) &&
            Near(capsule_background.corner_radius.top_left, 1.0) &&
            Near(capsule_background.corner_radius.bottom_right, 3.0) &&
            Near(taskbar_container.margin.left, 3.0) &&
            Near(taskbar_container.margin.top, 2.0) &&
            Near(taskbar_container.margin.right, 5.0) &&
            Near(taskbar_container.margin.bottom, 4.0) &&
            Near(secondary_container.margin.right, 0.0) &&
            Near(secondary_background.margin.left, 0.0) &&
            Near(secondary_background.margin.right, 0.0),
        "restore native geometry when capsule mode is turned off");

    Require(
        taskbar_style.Configure(
            false,
            830,
            false,
            true,
            false,
            false,
            false,
            0xFF506070U,
            1000,
            false,
            false,
            0xFF000000U),
        "disable Taskbar rules");
    Require(
        taskbar_style.ApplyDesiredToTrackedElements() == S_OK,
        "restore all Taskbar properties");
    Require(
        Near(taskbar_root.opacity, 0.91) && Near(tray_root.opacity, 0.88),
        "restore exact Taskbar opacity values");
    Require(
        notification.visible && control.visible && show_desktop.visible,
        "restore exact Taskbar visibility values");
    Require(
        capsule_background_stroke.outline.visible &&
            Near(capsule_background_stroke.outline.height, 1.0),
        "preserve the native outline after releasing ownership");
    Require(
        taskbar_surface.brush == 0x1234U,
        "restore exact native Taskbar brush");
    Require(taskbar_style.tracked_count() == 0, "release Taskbar ownership");

    std::cout << "Shell XAML style tests passed\n";
    return EXIT_SUCCESS;
}
