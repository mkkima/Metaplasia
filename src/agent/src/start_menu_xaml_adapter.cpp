#include "metaplasia/agent/start_menu_xaml_adapter.hpp"

#include "metaplasia/agent/start_menu_style.hpp"

#include <Windows.h>
#include <roapi.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <windows.ui.core.h>
#include <windows.ui.xaml.h>
#include <windows.ui.xaml.controls.h>
#include <windows.ui.xaml.media.h>
#include <windows.ui.xaml.shapes.h>
#include <xamlom.h>
#include <wrl.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>

namespace metaplasia::agent {
namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::ChainInterfaces;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

using UiElement = ABI::Windows::UI::Xaml::IUIElement;
using FrameworkElement = ABI::Windows::UI::Xaml::IFrameworkElement;
using XamlVisibility = ABI::Windows::UI::Xaml::Visibility;
using XamlThickness = ABI::Windows::UI::Xaml::Thickness;
using XamlCornerRadius = ABI::Windows::UI::Xaml::CornerRadius;
using XamlHorizontalAlignment =
    ABI::Windows::UI::Xaml::HorizontalAlignment;
using XamlVerticalAlignment =
    ABI::Windows::UI::Xaml::VerticalAlignment;
using XamlBorder = ABI::Windows::UI::Xaml::Controls::IBorder;
using XamlControl7 = ABI::Windows::UI::Xaml::Controls::IControl7;
using XamlBrush = ABI::Windows::UI::Xaml::Media::IBrush;
using XamlSolidColorBrush =
    ABI::Windows::UI::Xaml::Media::ISolidColorBrush;
using XamlSolidColorBrushFactory =
    ABI::Windows::UI::Xaml::Media::ISolidColorBrushFactory;
using XamlShape = ABI::Windows::UI::Xaml::Shapes::IShape;
using XamlRectangle = ABI::Windows::UI::Xaml::Shapes::IRectangle;
using CoreDispatcher = ABI::Windows::UI::Core::ICoreDispatcher;
using DispatchedHandler = ABI::Windows::UI::Core::IDispatchedHandler;
using AsyncAction = ABI::Windows::Foundation::IAsyncAction;

constexpr DWORD kDispatcherTimeoutMilliseconds = 2000;
constexpr DWORD kVisualTreeSubscriptionTimeoutMilliseconds = 5000;
constexpr wchar_t kVisualDiagnosticsEndpoint[] = L"VisualDiagConnection1";

struct TaskbarCapsuleOutlineSnapshot final {
    double width{0.0};
    double height{0.0};
    XamlHorizontalAlignment horizontal_alignment{};
    XamlVerticalAlignment vertical_alignment{};
    XamlThickness margin{};
    XamlVisibility visibility{};
    boolean is_hit_test_visible{false};
    boolean use_layout_rounding{true};
    double stroke_thickness{0.0};
    double radius_x{0.0};
    double radius_y{0.0};
    ComPtr<XamlBrush> fill;
    ComPtr<XamlBrush> stroke;
};

std::atomic<HMODULE> g_agent_module{nullptr};
std::atomic<protocol::AgentTarget> g_desired_target{
    protocol::AgentTarget::start_menu};
std::atomic<bool> g_desired_enabled{false};
std::atomic<std::uint32_t> g_desired_taskbar_opacity_milli{
    protocol::kDefaultTaskbarOpacityMilli};
std::atomic<bool> g_desired_taskbar_hide_notification_center{false};
std::atomic<bool> g_desired_taskbar_hide_control_center{false};
std::atomic<bool> g_desired_taskbar_hide_show_desktop{false};
std::atomic<bool> g_desired_taskbar_capsule_enabled{false};
std::atomic<bool> g_desired_taskbar_background_color_enabled{false};
std::atomic<std::uint32_t> g_desired_taskbar_background_color{
    protocol::kDefaultShellBackgroundColor};
std::atomic<std::uint32_t> g_desired_opacity_milli{
    kMaximumStartMenuOpacityMilli};
std::atomic<bool> g_desired_hide_recommended{false};
std::atomic<bool> g_desired_start_menu_background_color_enabled{false};
std::atomic<std::uint32_t> g_desired_start_menu_background_color{
    protocol::kDefaultShellBackgroundColor};
std::atomic<std::uint32_t> g_last_adapter_error{S_OK};
std::atomic<protocol::AgentDiagnosticStage> g_diagnostic_stage{
    protocol::AgentDiagnosticStage::none};
SRWLOCK g_controller_lock = SRWLOCK_INIT;
IVisualTreeService* g_visual_tree_service = nullptr;
class VisualTreeWatcher;
VisualTreeWatcher* g_visual_tree_watcher = nullptr;
bool g_visual_tree_advised = false;

enum class InitialSubscriptionState : std::uint32_t {
    idle = 0,
    subscribing = 1,
    complete = 2,
    waiting_for_site = 3,
};

HANDLE g_initial_subscription_event = nullptr;
std::atomic<InitialSubscriptionState> g_initial_subscription_state{
    InitialSubscriptionState::idle};
std::atomic<HRESULT> g_initial_subscription_result{E_UNEXPECTED};
SRWLOCK g_initialization_lock = SRWLOCK_INIT;
bool g_initialization_attempted = false;
HRESULT g_initialization_result = E_UNEXPECTED;

void DebugLog(const wchar_t* message) noexcept {
    ::OutputDebugStringW(L"[Metaplasia Shell XAML] ");
    ::OutputDebugStringW(message);
    ::OutputDebugStringW(L"\n");
}

[[nodiscard]] ShellXamlSettings LoadDesiredSettings() noexcept {
    return ShellXamlSettings{
        g_desired_taskbar_opacity_milli.load(std::memory_order_acquire),
        g_desired_taskbar_hide_notification_center.load(
            std::memory_order_acquire),
        g_desired_taskbar_hide_control_center.load(
            std::memory_order_acquire),
        g_desired_taskbar_hide_show_desktop.load(
            std::memory_order_acquire),
        g_desired_taskbar_capsule_enabled.load(std::memory_order_acquire),
        g_desired_taskbar_background_color_enabled.load(
            std::memory_order_acquire),
        g_desired_taskbar_background_color.load(std::memory_order_acquire),
        g_desired_opacity_milli.load(std::memory_order_acquire),
        g_desired_hide_recommended.load(std::memory_order_acquire),
        g_desired_start_menu_background_color_enabled.load(
            std::memory_order_acquire),
        g_desired_start_menu_background_color.load(
            std::memory_order_acquire)};
}

class XamlElementAccessor final : public ShellXamlElementAccessor {
public:
    explicit XamlElementAccessor(IXamlDiagnostics* diagnostics) noexcept
        : diagnostics_(diagnostics) {}

    [[nodiscard]] HRESULT ReadOpacity(
        const std::uint64_t handle,
        double& opacity) noexcept override {
        ComPtr<UiElement> element;
        const HRESULT result = ResolveElement(handle, element);
        return FAILED(result) ? result : element->get_Opacity(&opacity);
    }

    [[nodiscard]] HRESULT WriteOpacity(
        const std::uint64_t handle,
        const double opacity) noexcept override {
        ComPtr<UiElement> element;
        const HRESULT result = ResolveElement(handle, element);
        return FAILED(result) ? result : element->put_Opacity(opacity);
    }

    [[nodiscard]] HRESULT ReadVisibility(
        const std::uint64_t handle,
        bool& visible) noexcept override {
        ComPtr<UiElement> element;
        const HRESULT result = ResolveElement(handle, element);
        if (FAILED(result)) {
            return result;
        }
        XamlVisibility visibility{};
        const HRESULT read_result = element->get_Visibility(&visibility);
        if (FAILED(read_result)) {
            return read_result;
        }
        if (visibility != ABI::Windows::UI::Xaml::Visibility_Visible &&
            visibility != ABI::Windows::UI::Xaml::Visibility_Collapsed) {
            return E_UNEXPECTED;
        }
        visible = visibility ==
                  ABI::Windows::UI::Xaml::Visibility_Visible;
        return S_OK;
    }

    [[nodiscard]] HRESULT WriteVisibility(
        const std::uint64_t handle,
        const bool visible) noexcept override {
        ComPtr<UiElement> element;
        const HRESULT result = ResolveElement(handle, element);
        return FAILED(result)
            ? result
            : element->put_Visibility(
                  visible
                      ? ABI::Windows::UI::Xaml::Visibility_Visible
                      : ABI::Windows::UI::Xaml::Visibility_Collapsed);
    }

    [[nodiscard]] HRESULT ReadActualWidth(
        const std::uint64_t handle,
        double& width) noexcept override {
        ComPtr<FrameworkElement> element;
        const HRESULT result = ResolveFrameworkElement(handle, element);
        return FAILED(result) ? result : element->get_ActualWidth(&width);
    }

    [[nodiscard]] HRESULT ReadMargin(
        const std::uint64_t handle,
        ShellThickness& margin) noexcept override {
        ComPtr<FrameworkElement> element;
        const HRESULT result = ResolveFrameworkElement(handle, element);
        if (FAILED(result)) {
            return result;
        }
        XamlThickness value{};
        const HRESULT read_result = element->get_Margin(&value);
        if (SUCCEEDED(read_result)) {
            margin = {value.Left, value.Top, value.Right, value.Bottom};
        }
        return read_result;
    }

    [[nodiscard]] HRESULT WriteMargin(
        const std::uint64_t handle,
        const ShellThickness& margin) noexcept override {
        ComPtr<FrameworkElement> element;
        const HRESULT result = ResolveFrameworkElement(handle, element);
        if (FAILED(result)) {
            return result;
        }
        return element->put_Margin(XamlThickness{
            margin.left,
            margin.top,
            margin.right,
            margin.bottom});
    }

    [[nodiscard]] HRESULT ReadCornerRadius(
        const std::uint64_t handle,
        ShellCornerRadius& radius) noexcept override {
        ComPtr<IInspectable> inspectable;
        HRESULT result = ResolveInspectable(handle, inspectable);
        if (FAILED(result)) {
            return result;
        }
        ComPtr<XamlControl7> control;
        result = inspectable.As(&control);
        if (FAILED(result)) {
            return result;
        }
        XamlCornerRadius value{};
        result = control->get_CornerRadius(&value);
        if (SUCCEEDED(result)) {
            radius = {
                value.TopLeft,
                value.TopRight,
                value.BottomRight,
                value.BottomLeft};
        }
        return result;
    }

    [[nodiscard]] HRESULT WriteCornerRadius(
        const std::uint64_t handle,
        const ShellCornerRadius& radius) noexcept override {
        ComPtr<IInspectable> inspectable;
        HRESULT result = ResolveInspectable(handle, inspectable);
        if (FAILED(result)) {
            return result;
        }
        ComPtr<XamlControl7> control;
        result = inspectable.As(&control);
        if (FAILED(result)) {
            return result;
        }
        return control->put_CornerRadius(XamlCornerRadius{
            radius.top_left,
            radius.top_right,
            radius.bottom_right,
            radius.bottom_left});
    }

    [[nodiscard]] HRESULT CaptureBrush(
        const std::uint64_t handle,
        const ShellBrushProperty property,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        ComPtr<XamlBrush> brush;
        const HRESULT result = ReadBrush(handle, property, brush);
        if (FAILED(result)) {
            return result;
        }
        snapshot = reinterpret_cast<std::uint64_t>(brush.Detach());
        return S_OK;
    }

    [[nodiscard]] HRESULT WriteBrushColor(
        const std::uint64_t handle,
        const ShellBrushProperty property,
        const std::uint32_t argb) noexcept override {
        ComPtr<XamlBrush> brush;
        const HRESULT result = CreateSolidColorBrush(argb, brush);
        return FAILED(result) ? result : WriteBrush(handle, property, brush.Get());
    }

    [[nodiscard]] HRESULT RestoreBrush(
        const std::uint64_t handle,
        const ShellBrushProperty property,
        const std::uint64_t snapshot) noexcept override {
        auto* brush = reinterpret_cast<XamlBrush*>(snapshot);
        return WriteBrush(handle, property, brush);
    }

    void ReleaseBrushSnapshot(const std::uint64_t snapshot) noexcept override {
        if (snapshot != 0) {
            reinterpret_cast<XamlBrush*>(snapshot)->Release();
        }
    }

    [[nodiscard]] HRESULT CaptureTaskbarCapsuleOutline(
        const std::uint64_t handle,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        ComPtr<IInspectable> inspectable;
        HRESULT result = ResolveInspectable(handle, inspectable);
        if (FAILED(result)) {
            return result;
        }
        ComPtr<UiElement> ui_element;
        ComPtr<FrameworkElement> framework_element;
        ComPtr<XamlShape> shape;
        ComPtr<XamlRectangle> rectangle;
        if (FAILED(result = inspectable.As(&ui_element)) ||
            FAILED(result = inspectable.As(&framework_element)) ||
            FAILED(result = inspectable.As(&shape)) ||
            FAILED(result = inspectable.As(&rectangle))) {
            return result;
        }

        auto* captured =
            new (std::nothrow) TaskbarCapsuleOutlineSnapshot();
        if (captured == nullptr) {
            return E_OUTOFMEMORY;
        }
        if (FAILED(result = framework_element->get_Width(&captured->width)) ||
            FAILED(result = framework_element->get_Height(&captured->height)) ||
            FAILED(result = framework_element->get_HorizontalAlignment(
                       &captured->horizontal_alignment)) ||
            FAILED(result = framework_element->get_VerticalAlignment(
                       &captured->vertical_alignment)) ||
            FAILED(result = framework_element->get_Margin(&captured->margin)) ||
            FAILED(result = ui_element->get_Visibility(
                       &captured->visibility)) ||
            FAILED(result = ui_element->get_IsHitTestVisible(
                       &captured->is_hit_test_visible)) ||
            FAILED(result = ui_element->get_UseLayoutRounding(
                       &captured->use_layout_rounding)) ||
            FAILED(result = shape->get_StrokeThickness(
                       &captured->stroke_thickness)) ||
            FAILED(result = rectangle->get_RadiusX(&captured->radius_x)) ||
            FAILED(result = rectangle->get_RadiusY(&captured->radius_y)) ||
            FAILED(result = shape->get_Fill(
                       captured->fill.GetAddressOf())) ||
            FAILED(result = shape->get_Stroke(
                       captured->stroke.GetAddressOf()))) {
            delete captured;
            return result;
        }
        snapshot = reinterpret_cast<std::uint64_t>(captured);
        return S_OK;
    }

    [[nodiscard]] HRESULT WriteTaskbarCapsuleOutline(
        const std::uint64_t handle,
        const std::uint32_t argb,
        const double thickness,
        const double corner_radius,
        const double inset) noexcept override {
        if (!std::isfinite(thickness) || thickness <= 0.0 ||
            !std::isfinite(corner_radius) || corner_radius < 0.0 ||
            !std::isfinite(inset) || inset < 0.0) {
            return E_INVALIDARG;
        }

        ComPtr<IInspectable> inspectable;
        HRESULT result = ResolveInspectable(handle, inspectable);
        if (FAILED(result)) {
            return result;
        }
        ComPtr<UiElement> ui_element;
        ComPtr<FrameworkElement> framework_element;
        ComPtr<XamlShape> shape;
        ComPtr<XamlRectangle> rectangle;
        if (FAILED(result = inspectable.As(&ui_element)) ||
            FAILED(result = inspectable.As(&framework_element)) ||
            FAILED(result = inspectable.As(&shape)) ||
            FAILED(result = inspectable.As(&rectangle))) {
            return result;
        }

        ComPtr<XamlBrush> stroke;
        result = CreateSolidColorBrush(argb, stroke);
        if (FAILED(result)) {
            return result;
        }
        const double automatic_size =
            (std::numeric_limits<double>::quiet_NaN)();
        if (FAILED(result = ui_element->put_Visibility(
                       ABI::Windows::UI::Xaml::Visibility_Visible)) ||
            FAILED(result = ui_element->put_IsHitTestVisible(false)) ||
            FAILED(result = ui_element->put_UseLayoutRounding(false)) ||
            FAILED(result = framework_element->put_Width(automatic_size)) ||
            FAILED(result = framework_element->put_Height(automatic_size)) ||
            FAILED(result = framework_element->put_HorizontalAlignment(
                       ABI::Windows::UI::Xaml::HorizontalAlignment_Stretch)) ||
            FAILED(result = framework_element->put_VerticalAlignment(
                       ABI::Windows::UI::Xaml::VerticalAlignment_Stretch)) ||
            FAILED(result = framework_element->put_Margin(
                       XamlThickness{inset, inset, inset, inset})) ||
            FAILED(result = shape->put_Fill(nullptr)) ||
            FAILED(result = shape->put_Stroke(stroke.Get())) ||
            FAILED(result = shape->put_StrokeThickness(thickness)) ||
            FAILED(result = rectangle->put_RadiusX(corner_radius)) ||
            FAILED(result = rectangle->put_RadiusY(corner_radius))) {
            return result;
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT RestoreTaskbarCapsuleOutline(
        const std::uint64_t handle,
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        const auto* captured =
            reinterpret_cast<const TaskbarCapsuleOutlineSnapshot*>(snapshot);
        ComPtr<IInspectable> inspectable;
        HRESULT result = ResolveInspectable(handle, inspectable);
        if (FAILED(result)) {
            return result;
        }
        ComPtr<UiElement> ui_element;
        ComPtr<FrameworkElement> framework_element;
        ComPtr<XamlShape> shape;
        ComPtr<XamlRectangle> rectangle;
        if (FAILED(result = inspectable.As(&ui_element)) ||
            FAILED(result = inspectable.As(&framework_element)) ||
            FAILED(result = inspectable.As(&shape)) ||
            FAILED(result = inspectable.As(&rectangle))) {
            return result;
        }

        HRESULT first_failure = S_OK;
        const auto preserve_first_failure =
            [&first_failure](const HRESULT candidate) noexcept {
                if (FAILED(candidate) && SUCCEEDED(first_failure)) {
                    first_failure = candidate;
                }
            };
        preserve_first_failure(shape->put_Fill(captured->fill.Get()));
        preserve_first_failure(shape->put_Stroke(captured->stroke.Get()));
        preserve_first_failure(
            shape->put_StrokeThickness(captured->stroke_thickness));
        preserve_first_failure(rectangle->put_RadiusX(captured->radius_x));
        preserve_first_failure(rectangle->put_RadiusY(captured->radius_y));
        preserve_first_failure(
            framework_element->put_Margin(captured->margin));
        preserve_first_failure(framework_element->put_HorizontalAlignment(
            captured->horizontal_alignment));
        preserve_first_failure(framework_element->put_VerticalAlignment(
            captured->vertical_alignment));
        preserve_first_failure(
            framework_element->put_Width(captured->width));
        preserve_first_failure(
            framework_element->put_Height(captured->height));
        preserve_first_failure(ui_element->put_IsHitTestVisible(
            captured->is_hit_test_visible));
        preserve_first_failure(ui_element->put_UseLayoutRounding(
            captured->use_layout_rounding));
        preserve_first_failure(
            ui_element->put_Visibility(captured->visibility));
        return first_failure;
    }

    void ReleaseTaskbarCapsuleOutlineSnapshot(
        const std::uint64_t snapshot) noexcept override {
        delete reinterpret_cast<TaskbarCapsuleOutlineSnapshot*>(snapshot);
    }

    [[nodiscard]] HRESULT GetDispatcher(
        ComPtr<CoreDispatcher>& dispatcher) noexcept {
        ComPtr<IInspectable> inspectable;
        const HRESULT result =
            diagnostics_->GetDispatcher(inspectable.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        if (inspectable == nullptr) {
            return E_NOINTERFACE;
        }
        return inspectable.As(&dispatcher);
    }

private:
    [[nodiscard]] static HRESULT CreateSolidColorBrush(
        const std::uint32_t argb,
        ComPtr<XamlBrush>& brush) noexcept {
        brush.Reset();
        Microsoft::WRL::Wrappers::HStringReference class_name(
            L"Windows.UI.Xaml.Media.SolidColorBrush");
        ComPtr<XamlSolidColorBrushFactory> factory;
        HRESULT result = ::RoGetActivationFactory(
            class_name.Get(),
            __uuidof(XamlSolidColorBrushFactory),
            reinterpret_cast<void**>(factory.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        const ABI::Windows::UI::Color color{
            static_cast<BYTE>((argb >> 24U) & 0xFFU),
            static_cast<BYTE>((argb >> 16U) & 0xFFU),
            static_cast<BYTE>((argb >> 8U) & 0xFFU),
            static_cast<BYTE>(argb & 0xFFU)};
        ComPtr<XamlSolidColorBrush> solid_brush;
        result = factory->CreateInstanceWithColor(
            color,
            solid_brush.GetAddressOf());
        return FAILED(result) ? result : solid_brush.As(&brush);
    }

    [[nodiscard]] HRESULT ResolveInspectable(
        const std::uint64_t handle,
        ComPtr<IInspectable>& inspectable) noexcept {
        const HRESULT result = diagnostics_->GetIInspectableFromHandle(
            static_cast<InstanceHandle>(handle),
            inspectable.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        return inspectable == nullptr ? E_NOTFOUND : S_OK;
    }

    [[nodiscard]] HRESULT ReadBrush(
        const std::uint64_t handle,
        const ShellBrushProperty property,
        ComPtr<XamlBrush>& brush) noexcept {
        ComPtr<IInspectable> inspectable;
        HRESULT result = ResolveInspectable(handle, inspectable);
        if (FAILED(result)) {
            return result;
        }
        if (property == ShellBrushProperty::background) {
            ComPtr<XamlBorder> border;
            result = inspectable.As(&border);
            return FAILED(result)
                ? result
                : border->get_Background(brush.GetAddressOf());
        }
        if (property == ShellBrushProperty::fill) {
            ComPtr<XamlShape> shape;
            result = inspectable.As(&shape);
            return FAILED(result)
                ? result
                : shape->get_Fill(brush.GetAddressOf());
        }
        return E_INVALIDARG;
    }

    [[nodiscard]] HRESULT WriteBrush(
        const std::uint64_t handle,
        const ShellBrushProperty property,
        XamlBrush* brush) noexcept {
        ComPtr<IInspectable> inspectable;
        HRESULT result = ResolveInspectable(handle, inspectable);
        if (FAILED(result)) {
            return result;
        }
        if (property == ShellBrushProperty::background) {
            ComPtr<XamlBorder> border;
            result = inspectable.As(&border);
            return FAILED(result) ? result : border->put_Background(brush);
        }
        if (property == ShellBrushProperty::fill) {
            ComPtr<XamlShape> shape;
            result = inspectable.As(&shape);
            return FAILED(result) ? result : shape->put_Fill(brush);
        }
        return E_INVALIDARG;
    }

    [[nodiscard]] HRESULT ResolveElement(
        const std::uint64_t handle,
        ComPtr<UiElement>& element) noexcept {
        ComPtr<IInspectable> inspectable;
        const HRESULT result = diagnostics_->GetIInspectableFromHandle(
            static_cast<InstanceHandle>(handle),
            inspectable.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        if (inspectable == nullptr) {
            return E_NOTFOUND;
        }
        return inspectable.As(&element);
    }

    [[nodiscard]] HRESULT ResolveFrameworkElement(
        const std::uint64_t handle,
        ComPtr<FrameworkElement>& element) noexcept {
        ComPtr<IInspectable> inspectable;
        const HRESULT result = ResolveInspectable(handle, inspectable);
        return FAILED(result) ? result : inspectable.As(&element);
    }

    ComPtr<IXamlDiagnostics> diagnostics_;
};

class VisualTreeWatcher final
    : public RuntimeClass<
          RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ChainInterfaces<
              IVisualTreeServiceCallback2,
              IVisualTreeServiceCallback>> {
public:
    VisualTreeWatcher(
        IXamlDiagnostics* diagnostics,
        const protocol::AgentTarget target) noexcept
        : target_(target), accessor_(diagnostics), style_(accessor_, target) {}

    [[nodiscard]] protocol::AgentTarget target() const noexcept {
        return target_;
    }

    [[nodiscard]] bool Configure(
        const bool enabled,
        const ShellXamlSettings& settings) noexcept {
        return style_.Configure(
            enabled,
            settings.taskbar_opacity_milli,
            settings.taskbar_hide_notification_center,
            settings.taskbar_hide_control_center,
            settings.taskbar_hide_show_desktop,
            settings.taskbar_capsule_enabled,
            settings.taskbar_background_color_enabled,
            settings.taskbar_background_color,
            settings.start_menu_opacity_milli,
            settings.start_menu_hide_recommended,
            settings.start_menu_background_color_enabled,
            settings.start_menu_background_color);
    }

    [[nodiscard]] HRESULT ApplyDesiredToTrackedElements() noexcept {
        return style_.ApplyDesiredToTrackedElements();
    }

    [[nodiscard]] bool HasTrackedElements() const noexcept {
        return style_.tracked_count() != 0;
    }

    [[nodiscard]] std::size_t TrackedElementCount() const noexcept {
        return style_.tracked_count();
    }

    [[nodiscard]] HRESULT GetDispatcher(
        ComPtr<CoreDispatcher>& dispatcher) noexcept {
        return accessor_.GetDispatcher(dispatcher);
    }

    void CopyDiagnostics(
        protocol::XamlDiagnosticsSnapshot& snapshot) noexcept {
        protocol::XamlDiagnosticsSnapshot copy;
        copy.target = target_;
        ::AcquireSRWLockShared(&diagnostics_lock_);
        copy.type_count = diagnostic_type_count_;
        copy.dropped_type_count = dropped_diagnostic_type_count_;
        copy.element_count = diagnostic_element_count_;
        copy.dropped_element_count = dropped_diagnostic_element_count_;
        copy.tracked_element_count = static_cast<std::uint32_t>(
            style_.tracked_count());
        std::copy_n(
            diagnostic_types_.begin(),
            diagnostic_type_count_,
            std::begin(copy.types));
        std::copy_n(
            diagnostic_elements_.begin(),
            diagnostic_element_count_,
            std::begin(copy.elements));
        ::ReleaseSRWLockShared(&diagnostics_lock_);
        snapshot = copy;
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        const ParentChildRelation relation,
        const VisualElement element,
        const VisualMutationType mutation_type) noexcept override {
        if (mutation_type == Remove) {
            style_.OnElementRemoved(
                static_cast<std::uint64_t>(element.Handle));
            return S_OK;
        }
        if (mutation_type != Add || element.Type == nullptr) {
            return S_OK;
        }

        const std::size_t length = ::SysStringLen(element.Type);
        if (length == 0) {
            return S_OK;
        }
        const std::wstring_view type_name(element.Type, length);
        RecordDiagnosticElement(relation, element, type_name);
        if (length > protocol::kMaximumXamlDiagnosticTypeNameLength) {
            return S_OK;
        }
        const std::size_t name_length = element.Name == nullptr
            ? 0U
            : ::SysStringLen(element.Name);
        const std::wstring_view element_name =
            element.Name != nullptr &&
                name_length <=
                    protocol::kMaximumXamlDiagnosticElementNameLength
            ? std::wstring_view(element.Name, name_length)
            : std::wstring_view{};
        const HRESULT result = style_.OnElementAdded(
            static_cast<std::uint64_t>(element.Handle),
            type_name,
            element_name,
            static_cast<std::uint64_t>(relation.Parent));
        if (FAILED(result)) {
            DebugLog(L"A supported shell element could not be styled");
        }
        // A styling failure must never break XAML Diagnostics enumeration.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(
        InstanceHandle,
        VisualElementState,
        LPCWSTR) noexcept override {
        return S_OK;
    }

private:
    static void SaturatingIncrement(std::uint32_t& counter) noexcept {
        if (counter != (std::numeric_limits<std::uint32_t>::max)()) {
            ++counter;
        }
    }

    std::uint16_t FindOrAddDiagnosticTypeLocked(
        const std::wstring_view type_name) noexcept {
        if (type_name.size() > protocol::kMaximumXamlDiagnosticTypeNameLength) {
            SaturatingIncrement(dropped_diagnostic_type_count_);
            return (std::numeric_limits<std::uint16_t>::max)();
        }
        for (std::uint32_t index = 0; index < diagnostic_type_count_; ++index) {
            auto& entry = diagnostic_types_[index];
            if (std::wstring_view(entry.type_name) == type_name) {
                SaturatingIncrement(entry.observation_count);
                return static_cast<std::uint16_t>(index);
            }
        }
        if (diagnostic_type_count_ >= diagnostic_types_.size()) {
            SaturatingIncrement(dropped_diagnostic_type_count_);
            return (std::numeric_limits<std::uint16_t>::max)();
        }
        const auto index = static_cast<std::uint16_t>(diagnostic_type_count_++);
        auto& entry = diagnostic_types_[index];
        std::copy(type_name.begin(), type_name.end(), entry.type_name);
        entry.type_name[type_name.size()] = L'\0';
        entry.observation_count = 1;
        return index;
    }

    void RecordDiagnosticElement(
        const ParentChildRelation relation,
        const VisualElement element,
        const std::wstring_view type_name) noexcept {
        ::AcquireSRWLockExclusive(&diagnostics_lock_);
        const std::uint16_t type_index =
            FindOrAddDiagnosticTypeLocked(type_name);
        if (type_index == (std::numeric_limits<std::uint16_t>::max)()) {
            SaturatingIncrement(dropped_diagnostic_element_count_);
            ::ReleaseSRWLockExclusive(&diagnostics_lock_);
            return;
        }

        const std::size_t name_length =
            element.Name == nullptr ? 0U : ::SysStringLen(element.Name);
        const bool framework_type =
            type_name.starts_with(L"Windows.UI.Xaml.") ||
            type_name.starts_with(L"Windows.UI.Composition.");
        if (name_length == 0 && framework_type) {
            ::ReleaseSRWLockExclusive(&diagnostics_lock_);
            return;
        }
        for (std::uint32_t index = 0;
             index < diagnostic_element_count_;
             ++index) {
            if (diagnostic_elements_[index].handle ==
                static_cast<std::uint64_t>(element.Handle)) {
                ::ReleaseSRWLockExclusive(&diagnostics_lock_);
                return;
            }
        }
        if (diagnostic_element_count_ >= diagnostic_elements_.size()) {
            SaturatingIncrement(dropped_diagnostic_element_count_);
            ::ReleaseSRWLockExclusive(&diagnostics_lock_);
            return;
        }

        auto& entry = diagnostic_elements_[diagnostic_element_count_++];
        entry.handle = static_cast<std::uint64_t>(element.Handle);
        entry.parent_handle = static_cast<std::uint64_t>(relation.Parent);
        entry.child_index = relation.ChildIndex;
        entry.child_count = element.NumChildren;
        entry.type_index = type_index;
        const std::size_t copied_name_length = (std::min)(
            name_length,
            protocol::kMaximumXamlDiagnosticElementNameLength);
        if (copied_name_length != 0) {
            std::copy_n(
                element.Name,
                copied_name_length,
                entry.name);
            entry.name[copied_name_length] = L'\0';
        }
        ::ReleaseSRWLockExclusive(&diagnostics_lock_);
    }

    const protocol::AgentTarget target_;
    XamlElementAccessor accessor_;
    ShellXamlStyle style_;
    SRWLOCK diagnostics_lock_{};
    std::array<
        protocol::XamlTypeDiagnostic,
        protocol::kMaximumXamlDiagnosticTypes>
        diagnostic_types_{};
    std::uint32_t diagnostic_type_count_{0};
    std::uint32_t dropped_diagnostic_type_count_{0};
    std::array<
        protocol::XamlElementDiagnostic,
        protocol::kMaximumXamlDiagnosticElements>
        diagnostic_elements_{};
    std::uint32_t diagnostic_element_count_{0};
    std::uint32_t dropped_diagnostic_element_count_{0};
};

class StyleUpdateHandler final
    : public RuntimeClass<
          RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          DispatchedHandler,
          FtmBase> {
public:
    StyleUpdateHandler(
        VisualTreeWatcher* watcher,
        const HANDLE completion_event) noexcept
        : watcher_(watcher), completion_event_(completion_event) {}

    ~StyleUpdateHandler() override {
        if (completion_event_ != nullptr) {
            ::CloseHandle(completion_event_);
        }
    }

    HRESULT STDMETHODCALLTYPE Invoke() noexcept override {
        result_.store(
            watcher_->ApplyDesiredToTrackedElements(),
            std::memory_order_release);
        if (completion_event_ != nullptr) {
            ::SetEvent(completion_event_);
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT result() const noexcept {
        return result_.load(std::memory_order_acquire);
    }

private:
    ComPtr<VisualTreeWatcher> watcher_;
    HANDLE completion_event_{nullptr};
    std::atomic<HRESULT> result_{E_PENDING};
};

[[nodiscard]] HRESULT DispatchStyleUpdate(
    VisualTreeWatcher* watcher,
    const bool wait_for_completion) noexcept {
    if (watcher == nullptr) {
        return E_POINTER;
    }

    ComPtr<CoreDispatcher> dispatcher;
    HRESULT result = watcher->GetDispatcher(dispatcher);
    if (FAILED(result)) {
        return result;
    }

    boolean has_thread_access = false;
    result = dispatcher->get_HasThreadAccess(&has_thread_access);
    if (FAILED(result)) {
        return result;
    }
    if (has_thread_access) {
        return watcher->ApplyDesiredToTrackedElements();
    }

    HANDLE completion_event = nullptr;
    HANDLE wait_handle = nullptr;
    if (wait_for_completion) {
        completion_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (completion_event == nullptr) {
            return HRESULT_FROM_WIN32(::GetLastError());
        }
        if (!::DuplicateHandle(
                ::GetCurrentProcess(),
                completion_event,
                ::GetCurrentProcess(),
                &wait_handle,
                SYNCHRONIZE,
                FALSE,
                0)) {
            const HRESULT duplicate_result =
                HRESULT_FROM_WIN32(::GetLastError());
            ::CloseHandle(completion_event);
            return duplicate_result;
        }
    }

    auto handler = Make<StyleUpdateHandler>(watcher, completion_event);
    if (handler == nullptr) {
        if (wait_handle != nullptr) {
            ::CloseHandle(wait_handle);
        }
        if (completion_event != nullptr) {
            ::CloseHandle(completion_event);
        }
        return E_OUTOFMEMORY;
    }

    ComPtr<AsyncAction> action;
    result = dispatcher->RunAsync(
        ABI::Windows::UI::Core::CoreDispatcherPriority_Normal,
        handler.Get(),
        action.GetAddressOf());
    if (FAILED(result) || !wait_for_completion) {
        if (wait_handle != nullptr) {
            ::CloseHandle(wait_handle);
        }
        return result;
    }

    const DWORD wait_result = ::WaitForSingleObject(
        wait_handle,
        kDispatcherTimeoutMilliseconds);
    ::CloseHandle(wait_handle);
    if (wait_result == WAIT_OBJECT_0) {
        return handler->result();
    }
    if (wait_result == WAIT_TIMEOUT) {
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    return HRESULT_FROM_WIN32(::GetLastError());
}

[[nodiscard]] bool IsControllerAttached() noexcept {
    ::AcquireSRWLockShared(&g_controller_lock);
    const bool attached = g_visual_tree_service != nullptr &&
                          g_visual_tree_watcher != nullptr;
    ::ReleaseSRWLockShared(&g_controller_lock);
    return attached;
}

[[nodiscard]] bool IsControllerOperational() noexcept {
    ::AcquireSRWLockShared(&g_controller_lock);
    const bool operational = g_visual_tree_service != nullptr &&
                             g_visual_tree_watcher != nullptr &&
                             g_visual_tree_advised;
    ::ReleaseSRWLockShared(&g_controller_lock);
    return operational;
}

[[nodiscard]] HRESULT AttachController(
    IXamlDiagnostics* diagnostics,
    IVisualTreeService* visual_tree_service) noexcept {
    if (diagnostics == nullptr || visual_tree_service == nullptr) {
        return E_POINTER;
    }

    g_diagnostic_stage.store(
        protocol::AgentDiagnosticStage::attach_controller,
        std::memory_order_release);

    auto watcher = Make<VisualTreeWatcher>(
        diagnostics,
        g_desired_target.load(std::memory_order_acquire));
    if (watcher == nullptr) {
        return E_OUTOFMEMORY;
    }
    if (!watcher->Configure(
            g_desired_enabled.load(std::memory_order_acquire),
            LoadDesiredSettings())) {
        return E_INVALIDARG;
    }
    ComPtr<IVisualTreeServiceCallback> callback;
    const HRESULT callback_result = watcher.As(&callback);
    if (FAILED(callback_result)) {
        return callback_result;
    }

    ::AcquireSRWLockExclusive(&g_controller_lock);
    if (g_visual_tree_service != nullptr || g_visual_tree_watcher != nullptr) {
        ::ReleaseSRWLockExclusive(&g_controller_lock);
        return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    }
    visual_tree_service->AddRef();
    g_visual_tree_service = visual_tree_service;
    g_visual_tree_watcher = watcher.Detach();
    g_visual_tree_advised = false;
    ::ReleaseSRWLockExclusive(&g_controller_lock);
    g_diagnostic_stage.store(
        protocol::AgentDiagnosticStage::controller_attached,
        std::memory_order_release);
    DebugLog(L"XAML diagnostics controller prepared");
    return S_OK;
}

struct VisualTreeSubscriptionContext final {
    IVisualTreeService* service{nullptr};
    VisualTreeWatcher* watcher{nullptr};
    bool subscribe{false};
};

DWORD WINAPI VisualTreeSubscriptionWorker(void* raw_context) noexcept {
    auto* context =
        static_cast<VisualTreeSubscriptionContext*>(raw_context);
    if (context == nullptr || context->service == nullptr ||
        context->watcher == nullptr) {
        if (context != nullptr) {
            if (context->watcher != nullptr) {
                context->watcher->Release();
            }
            if (context->service != nullptr) {
                context->service->Release();
            }
        }
        delete context;
        return static_cast<DWORD>(E_POINTER);
    }

    const HRESULT result = context->subscribe
                               ? context->service->AdviseVisualTreeChange(
                                     context->watcher)
                               : context->service->UnadviseVisualTreeChange(
                                     context->watcher);
    if (SUCCEEDED(result)) {
        ::AcquireSRWLockExclusive(&g_controller_lock);
        if (g_visual_tree_service == context->service &&
            g_visual_tree_watcher == context->watcher) {
            g_visual_tree_advised = context->subscribe;
        }
        ::ReleaseSRWLockExclusive(&g_controller_lock);
    }
    context->watcher->Release();
    context->service->Release();
    delete context;
    return static_cast<DWORD>(result);
}

[[nodiscard]] HRESULT UpdateVisualTreeSubscription(
    IVisualTreeService* service,
    VisualTreeWatcher* watcher,
    const bool subscribe) noexcept {
    if (service == nullptr || watcher == nullptr) {
        return E_POINTER;
    }

    auto* context = new (std::nothrow) VisualTreeSubscriptionContext{
        service,
        watcher,
        subscribe};
    if (context == nullptr) {
        return E_OUTOFMEMORY;
    }
    service->AddRef();
    watcher->AddRef();

    const HANDLE thread = ::CreateThread(
        nullptr,
        0,
        &VisualTreeSubscriptionWorker,
        context,
        0,
        nullptr);
    if (thread == nullptr) {
        const HRESULT result = HRESULT_FROM_WIN32(::GetLastError());
        watcher->Release();
        service->Release();
        delete context;
        return result;
    }

    const DWORD wait_result = ::WaitForSingleObject(
        thread,
        kVisualTreeSubscriptionTimeoutMilliseconds);
    if (wait_result != WAIT_OBJECT_0) {
        const HRESULT result = wait_result == WAIT_TIMEOUT
                                   ? HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                                   : HRESULT_FROM_WIN32(::GetLastError());
        ::CloseHandle(thread);
        // The worker owns its context and COM references after CreateThread.
        // Never terminate it; it will release everything when the XAML call
        // eventually returns.
        return result;
    }

    DWORD exit_code = 0;
    const BOOL read_exit_code = ::GetExitCodeThread(thread, &exit_code);
    const DWORD exit_error = read_exit_code ? ERROR_SUCCESS : ::GetLastError();
    ::CloseHandle(thread);
    return read_exit_code ? static_cast<HRESULT>(exit_code)
                          : HRESULT_FROM_WIN32(exit_error);
}

struct InitialVisualTreeSubscriptionContext final {
    IVisualTreeService* service{nullptr};
    VisualTreeWatcher* watcher{nullptr};
    HANDLE completion_event{nullptr};
};

DWORD WINAPI InitialVisualTreeSubscriptionWorker(
    void* raw_context) noexcept {
    auto* context = static_cast<
        InitialVisualTreeSubscriptionContext*>(raw_context);
    if (context == nullptr || context->service == nullptr ||
        context->watcher == nullptr ||
        context->completion_event == nullptr) {
        if (context != nullptr) {
            if (context->watcher != nullptr) {
                context->watcher->Release();
            }
            if (context->service != nullptr) {
                context->service->Release();
            }
        }
        delete context;
        return static_cast<DWORD>(E_POINTER);
    }

    const HRESULT result = context->service->AdviseVisualTreeChange(
        context->watcher);
    if (SUCCEEDED(result)) {
        ::AcquireSRWLockExclusive(&g_controller_lock);
        if (g_visual_tree_service == context->service &&
            g_visual_tree_watcher == context->watcher) {
            g_visual_tree_advised = true;
        }
        ::ReleaseSRWLockExclusive(&g_controller_lock);
    }

    g_initial_subscription_result.store(result, std::memory_order_release);
    g_initial_subscription_state.store(
        InitialSubscriptionState::complete,
        std::memory_order_release);
    ::SetEvent(context->completion_event);
    context->watcher->Release();
    context->service->Release();
    delete context;
    return static_cast<DWORD>(result);
}

[[nodiscard]] HRESULT ResetInitialSubscription(
    const InitialSubscriptionState state,
    HANDLE& completion_event) noexcept {
    DWORD error = ERROR_SUCCESS;
    ::AcquireSRWLockExclusive(&g_controller_lock);
    if (g_initial_subscription_event == nullptr) {
        g_initial_subscription_event =
            ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (g_initial_subscription_event == nullptr) {
            error = ::GetLastError();
        }
    }
    completion_event = g_initial_subscription_event;
    if (completion_event != nullptr && !::ResetEvent(completion_event)) {
        error = ::GetLastError();
    }
    if (error == ERROR_SUCCESS) {
        g_initial_subscription_result.store(
            E_PENDING,
            std::memory_order_release);
        g_initial_subscription_state.store(state, std::memory_order_release);
    }
    ::ReleaseSRWLockExclusive(&g_controller_lock);
    return error == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(error);
}

[[nodiscard]] HRESULT StartInitialVisualTreeSubscription(
    IVisualTreeService* service,
    VisualTreeWatcher* watcher) noexcept {
    if (service == nullptr || watcher == nullptr) {
        return E_POINTER;
    }

    HANDLE completion_event = nullptr;
    const HRESULT reset_result = ResetInitialSubscription(
        InitialSubscriptionState::subscribing,
        completion_event);
    if (FAILED(reset_result)) {
        return reset_result;
    }

    auto* context = new (std::nothrow)
        InitialVisualTreeSubscriptionContext{
            service,
            watcher,
            completion_event};
    if (context == nullptr) {
        g_initial_subscription_state.store(
            InitialSubscriptionState::idle,
            std::memory_order_release);
        return E_OUTOFMEMORY;
    }
    service->AddRef();
    watcher->AddRef();

    const HANDLE thread = ::CreateThread(
        nullptr,
        0,
        &InitialVisualTreeSubscriptionWorker,
        context,
        0,
        nullptr);
    if (thread == nullptr) {
        const HRESULT result = HRESULT_FROM_WIN32(::GetLastError());
        watcher->Release();
        service->Release();
        delete context;
        g_initial_subscription_result.store(result, std::memory_order_release);
        g_initial_subscription_state.store(
            InitialSubscriptionState::complete,
            std::memory_order_release);
        ::SetEvent(completion_event);
        return result;
    }
    ::CloseHandle(thread);
    return S_OK;
}

[[nodiscard]] HRESULT PrepareInitialVisualTreeSubscription() noexcept {
    HANDLE completion_event = nullptr;
    return ResetInitialSubscription(
        InitialSubscriptionState::waiting_for_site,
        completion_event);
}

[[nodiscard]] HRESULT ConsumeInitialVisualTreeSubscription() noexcept {
    InitialSubscriptionState state =
        g_initial_subscription_state.load(std::memory_order_acquire);
    if (state == InitialSubscriptionState::idle) {
        return S_FALSE;
    }
    if (state == InitialSubscriptionState::subscribing ||
        state == InitialSubscriptionState::waiting_for_site) {
        const HANDLE completion_event = g_initial_subscription_event;
        if (completion_event == nullptr) {
            return E_UNEXPECTED;
        }
        const DWORD wait_result = ::WaitForSingleObject(
            completion_event,
            kVisualTreeSubscriptionTimeoutMilliseconds);
        if (wait_result == WAIT_TIMEOUT) {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        if (wait_result != WAIT_OBJECT_0) {
            return HRESULT_FROM_WIN32(::GetLastError());
        }
        state =
            g_initial_subscription_state.load(std::memory_order_acquire);
        if (state != InitialSubscriptionState::complete) {
            return E_UNEXPECTED;
        }
    }

    const HRESULT result =
        g_initial_subscription_result.load(std::memory_order_acquire);
    g_initial_subscription_state.store(
        InitialSubscriptionState::idle,
        std::memory_order_release);
    return result;
}

[[nodiscard]] HRESULT ConfigureAttachedController(
    const protocol::AgentTarget target,
    const bool enabled,
    const ShellXamlSettings& settings,
    const bool wait_for_restore) noexcept {
    ::AcquireSRWLockShared(&g_controller_lock);
    IVisualTreeService* service = g_visual_tree_service;
    VisualTreeWatcher* watcher = g_visual_tree_watcher;
    bool advised = g_visual_tree_advised;
    if (service != nullptr) {
        service->AddRef();
    }
    if (watcher != nullptr) {
        watcher->AddRef();
    }
    ::ReleaseSRWLockShared(&g_controller_lock);

    if (service == nullptr || watcher == nullptr) {
        if (watcher != nullptr) {
            watcher->Release();
        }
        if (service != nullptr) {
            service->Release();
        }
        return S_FALSE;
    }
    if (watcher->target() != target) {
        watcher->Release();
        service->Release();
        return E_INVALIDARG;
    }

    HRESULT result = ConsumeInitialVisualTreeSubscription();
    if (result == S_FALSE) {
        result = S_OK;
    } else if (SUCCEEDED(result)) {
        ::AcquireSRWLockShared(&g_controller_lock);
        advised = g_visual_tree_advised;
        ::ReleaseSRWLockShared(&g_controller_lock);
    }
    if (SUCCEEDED(result)) {
        result = watcher->Configure(enabled, settings)
                     ? S_OK
                     : E_INVALIDARG;
    }
    if (SUCCEEDED(result) && enabled) {
        if (advised) {
            if (watcher->HasTrackedElements()) {
                result = DispatchStyleUpdate(watcher, false);
            }
        } else {
            g_diagnostic_stage.store(
                protocol::AgentDiagnosticStage::advise_visual_tree,
                std::memory_order_release);
            result = UpdateVisualTreeSubscription(service, watcher, true);
            if (SUCCEEDED(result)) {
                ::AcquireSRWLockExclusive(&g_controller_lock);
                if (g_visual_tree_service == service &&
                    g_visual_tree_watcher == watcher) {
                    g_visual_tree_advised = true;
                }
                ::ReleaseSRWLockExclusive(&g_controller_lock);
                g_diagnostic_stage.store(
                    protocol::AgentDiagnosticStage::controller_attached,
                    std::memory_order_release);
            }
        }
    } else if (SUCCEEDED(result) && !enabled) {
        if (watcher->HasTrackedElements()) {
            result = DispatchStyleUpdate(watcher, wait_for_restore);
        }
        if (advised) {
            const HRESULT unadvise_result =
                UpdateVisualTreeSubscription(service, watcher, false);
            if (SUCCEEDED(unadvise_result)) {
                ::AcquireSRWLockExclusive(&g_controller_lock);
                if (g_visual_tree_service == service &&
                    g_visual_tree_watcher == watcher) {
                    g_visual_tree_advised = false;
                }
                ::ReleaseSRWLockExclusive(&g_controller_lock);
            } else if (SUCCEEDED(result)) {
                result = unadvise_result;
            }
        }
    }

    watcher->Release();
    service->Release();
    return result;
}

[[nodiscard]] HRESULT DetachController() noexcept {
    const HRESULT configure_result = ConfigureAttachedController(
        g_desired_target.load(std::memory_order_acquire),
        false,
        LoadDesiredSettings(),
        true);

    ::AcquireSRWLockExclusive(&g_controller_lock);
    IVisualTreeService* service = g_visual_tree_service;
    VisualTreeWatcher* watcher = g_visual_tree_watcher;
    g_visual_tree_service = nullptr;
    g_visual_tree_watcher = nullptr;
    g_visual_tree_advised = false;
    ::ReleaseSRWLockExclusive(&g_controller_lock);

    if (watcher != nullptr) {
        watcher->Release();
    }
    if (service != nullptr) {
        service->Release();
    }
    return configure_result == S_FALSE ? S_OK : configure_result;
}

class TapSite final
    : public RuntimeClass<
          RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IObjectWithSite> {
public:
    ~TapSite() override {
        // XAML Diagnostics is allowed to release the activation object after
        // SetSite succeeds. The process-wide controller owns its own service
        // and watcher references, so ordinary COM destruction must not be
        // mistaken for an explicit SetSite(nullptr) detach request.
        static_cast<void>(ClearSite(false));
    }

    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override {
        if (site == nullptr) {
            return ClearSite(true);
        }

        g_diagnostic_stage.store(
            protocol::AgentDiagnosticStage::set_tap_site,
            std::memory_order_release);
        ComPtr<IXamlDiagnostics> diagnostics;
        g_diagnostic_stage.store(
            protocol::AgentDiagnosticStage::query_xaml_diagnostics,
            std::memory_order_release);
        HRESULT result = site->QueryInterface(
            __uuidof(IXamlDiagnostics),
            reinterpret_cast<void**>(diagnostics.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        ComPtr<IVisualTreeService3> visual_tree_service;
        g_diagnostic_stage.store(
            protocol::AgentDiagnosticStage::query_visual_tree_service,
            std::memory_order_release);
        result = site->QueryInterface(
            __uuidof(IVisualTreeService3),
            reinterpret_cast<void**>(visual_tree_service.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        result = ClearSite(true);
        if (FAILED(result)) {
            return result;
        }
        result = AttachController(diagnostics.Get(), visual_tree_service.Get());
        if (FAILED(result)) {
            return result;
        }

        site->AddRef();
        ::AcquireSRWLockExclusive(&site_lock_);
        site_ = site;
        attached_ = true;
        ::ReleaseSRWLockExclusive(&site_lock_);

        if (g_desired_enabled.load(std::memory_order_acquire)) {
            g_diagnostic_stage.store(
                protocol::AgentDiagnosticStage::advise_visual_tree,
                std::memory_order_release);
            result = StartInitialVisualTreeSubscription(
                visual_tree_service.Get(),
                g_visual_tree_watcher);
            if (FAILED(result)) {
                static_cast<void>(ClearSite(true));
                return result;
            }
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetSite(
        REFIID interface_id,
        void** object) noexcept override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        g_diagnostic_stage.store(
            protocol::AgentDiagnosticStage::get_tap_site,
            std::memory_order_release);

        ::AcquireSRWLockShared(&site_lock_);
        IUnknown* site = site_;
        if (site != nullptr) {
            site->AddRef();
        }
        ::ReleaseSRWLockShared(&site_lock_);
        if (site == nullptr) {
            return MK_E_NOSTORAGE;
        }
        const HRESULT result = site->QueryInterface(interface_id, object);
        site->Release();
        return result;
    }

private:
    [[nodiscard]] HRESULT ClearSite(
        const bool detach_controller) noexcept {
        ::AcquireSRWLockExclusive(&site_lock_);
        IUnknown* site = site_;
        const bool attached = attached_;
        site_ = nullptr;
        attached_ = false;
        ::ReleaseSRWLockExclusive(&site_lock_);

        if (attached) {
            g_diagnostic_stage.store(
                detach_controller
                    ? protocol::AgentDiagnosticStage::tap_site_detached
                    : protocol::AgentDiagnosticStage::tap_site_released,
                std::memory_order_release);
        }

        const HRESULT result = attached && detach_controller
                                   ? DetachController()
                                   : S_OK;
        if (site != nullptr) {
            site->Release();
        }
        return result;
    }

    SRWLOCK site_lock_ = SRWLOCK_INIT;
    IUnknown* site_{nullptr};
    bool attached_{false};
};

class TapClassFactory final
    : public RuntimeClass<
          RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IClassFactory> {
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(
        IUnknown* outer,
        REFIID interface_id,
        void** object) noexcept override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (outer != nullptr) {
            return CLASS_E_NOAGGREGATION;
        }
        g_diagnostic_stage.store(
            protocol::AgentDiagnosticStage::create_tap_instance,
            std::memory_order_release);
        auto tap = Make<TapSite>();
        return tap == nullptr ? E_OUTOFMEMORY
                              : tap.CopyTo(interface_id, object);
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override {
        g_diagnostic_stage.store(
            protocol::AgentDiagnosticStage::lock_class_factory,
            std::memory_order_release);
        return S_OK;
    }
};

[[nodiscard]] HRESULT InitializeDiagnosticsAdapter() noexcept {
    ::AcquireSRWLockExclusive(&g_initialization_lock);
    if (g_initialization_attempted) {
        if (g_initialization_result ==
                HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) &&
            ::GetModuleHandleW(L"Windows.UI.Xaml.dll") != nullptr) {
            // The host can discover StartMenuExperienceHost before its XAML
            // runtime finishes loading. Retrying is safe because the previous
            // attempt never called InitializeXamlDiagnosticsEx.
            g_initialization_attempted = false;
        } else {
            const HRESULT result = g_initialization_result;
            ::ReleaseSRWLockExclusive(&g_initialization_lock);
            return result;
        }
    }
    g_initialization_attempted = true;

    HRESULT result = E_UNEXPECTED;
    const HMODULE module = g_agent_module.load(std::memory_order_acquire);
    const HMODULE windows_xaml = ::GetModuleHandleW(L"Windows.UI.Xaml.dll");
    if (module == nullptr || windows_xaml == nullptr) {
        result = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    } else {
        const auto initialize = reinterpret_cast<
            decltype(&::InitializeXamlDiagnosticsEx)>(
            ::GetProcAddress(windows_xaml, "InitializeXamlDiagnosticsEx"));
        if (initialize == nullptr) {
            result = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        } else {
            g_diagnostic_stage.store(
                protocol::AgentDiagnosticStage::initialize_xaml_diagnostics,
                std::memory_order_release);
            std::array<wchar_t, 32768> module_path{};
            const DWORD path_length = ::GetModuleFileNameW(
                module,
                module_path.data(),
                static_cast<DWORD>(module_path.size()));
            if (path_length == 0 || path_length >= module_path.size()) {
                result = HRESULT_FROM_WIN32(
                    path_length == 0 ? ::GetLastError()
                                     : ERROR_INSUFFICIENT_BUFFER);
            } else {
                const DWORD process_id = ::GetCurrentProcessId();
                result = PrepareInitialVisualTreeSubscription();
                if (SUCCEEDED(result)) {
                    result = initialize(
                        kVisualDiagnosticsEndpoint,
                        process_id,
                        nullptr,
                        module_path.data(),
                        kStartMenuTapClsid,
                        nullptr);
                }
            }
        }
    }

    g_initialization_result = result;
    ::ReleaseSRWLockExclusive(&g_initialization_lock);
    if (FAILED(result)) {
        DebugLog(L"XAML Diagnostics initialization failed closed");
    } else {
        DebugLog(L"XAML Diagnostics initialized");
    }
    return result;
}

}  // namespace

void SetAgentModule(const HMODULE module) noexcept {
    g_agent_module.store(module, std::memory_order_release);
}

protocol::AgentResult ConfigureShellXaml(
    const protocol::AgentTarget target,
    const bool enabled,
    const ShellXamlSettings& settings) noexcept {
    if ((target != protocol::AgentTarget::explorer_shell &&
         target != protocol::AgentTarget::start_menu) ||
        settings.taskbar_opacity_milli < kMinimumTaskbarOpacityMilli ||
         settings.taskbar_opacity_milli > kMaximumTaskbarOpacityMilli ||
         settings.start_menu_opacity_milli < kMinimumStartMenuOpacityMilli ||
         settings.start_menu_opacity_milli > kMaximumStartMenuOpacityMilli ||
         (settings.taskbar_background_color & 0xFF000000U) != 0xFF000000U ||
         (settings.start_menu_background_color & 0xFF000000U) != 0xFF000000U) {
        g_last_adapter_error.store(
            static_cast<std::uint32_t>(E_INVALIDARG),
            std::memory_order_release);
        return protocol::AgentResult::invalid_configuration;
    }

    g_desired_target.store(target, std::memory_order_release);
    g_desired_taskbar_opacity_milli.store(
        settings.taskbar_opacity_milli,
        std::memory_order_release);
    g_desired_taskbar_hide_notification_center.store(
        settings.taskbar_hide_notification_center,
        std::memory_order_release);
    g_desired_taskbar_hide_control_center.store(
        settings.taskbar_hide_control_center,
        std::memory_order_release);
    g_desired_taskbar_hide_show_desktop.store(
        settings.taskbar_hide_show_desktop,
        std::memory_order_release);
    g_desired_taskbar_capsule_enabled.store(
        settings.taskbar_capsule_enabled,
        std::memory_order_release);
    g_desired_taskbar_background_color_enabled.store(
        settings.taskbar_background_color_enabled,
        std::memory_order_release);
    g_desired_taskbar_background_color.store(
        settings.taskbar_background_color,
        std::memory_order_release);
    g_desired_opacity_milli.store(
        settings.start_menu_opacity_milli,
        std::memory_order_release);
    g_desired_hide_recommended.store(
        settings.start_menu_hide_recommended,
        std::memory_order_release);
    g_desired_start_menu_background_color_enabled.store(
        settings.start_menu_background_color_enabled,
        std::memory_order_release);
    g_desired_start_menu_background_color.store(
        settings.start_menu_background_color,
        std::memory_order_release);
    g_desired_enabled.store(enabled, std::memory_order_release);

    HRESULT result = ConfigureAttachedController(
        target,
        enabled,
        settings,
        !enabled);
    if (result == S_FALSE && enabled) {
        const HRESULT initialization_result = InitializeDiagnosticsAdapter();
        const HRESULT subscription_result =
            ConsumeInitialVisualTreeSubscription();
        if (subscription_result != S_FALSE) {
            result = subscription_result;
            if (SUCCEEDED(result) && !IsControllerOperational()) {
                result = E_NOINTERFACE;
            }
        } else if (IsControllerAttached()) {
            result = ConfigureAttachedController(
                target,
                true,
                settings,
                false);
        } else {
            result = initialization_result;
        }
    } else if (result == S_FALSE) {
        result = S_OK;
    }

    g_last_adapter_error.store(
        static_cast<std::uint32_t>(result),
        std::memory_order_release);
    if (SUCCEEDED(result)) {
        return protocol::AgentResult::success;
    }
    return enabled ? protocol::AgentResult::adapter_unavailable
                   : protocol::AgentResult::hook_failed;
}

protocol::AgentResult StopShellXaml() noexcept {
    return ConfigureShellXaml(
        g_desired_target.load(std::memory_order_acquire),
        false,
        LoadDesiredSettings());
}

std::uint32_t StartMenuXamlLastError() noexcept {
    return g_last_adapter_error.load(std::memory_order_acquire);
}

protocol::AgentDiagnosticStage StartMenuXamlDiagnosticStage() noexcept {
    return g_diagnostic_stage.load(std::memory_order_acquire);
}

std::uint32_t StartMenuXamlControllerState() noexcept {
    constexpr std::uint32_t service_present = 1U << 0U;
    constexpr std::uint32_t watcher_present = 1U << 1U;
    constexpr std::uint32_t advised = 1U << 2U;
    constexpr std::uint32_t tracked_count_shift = 8U;

    ::AcquireSRWLockShared(&g_controller_lock);
    std::uint32_t state = 0;
    if (g_visual_tree_service != nullptr) {
        state |= service_present;
    }
    if (g_visual_tree_watcher != nullptr) {
        state |= watcher_present;
        state |= static_cast<std::uint32_t>(
                     g_visual_tree_watcher->TrackedElementCount())
                 << tracked_count_shift;
    }
    if (g_visual_tree_advised) {
        state |= advised;
    }
    ::ReleaseSRWLockShared(&g_controller_lock);
    return state;
}

protocol::AgentResult CopyShellXamlDiagnostics(
    protocol::XamlDiagnosticsSnapshot* snapshot) noexcept {
    if (snapshot == nullptr ||
        snapshot->magic != protocol::kXamlDiagnosticsMagic ||
        snapshot->version != protocol::kXamlDiagnosticsVersion ||
        snapshot->size != sizeof(*snapshot) || snapshot->reserved != 0 ||
        (snapshot->target != protocol::AgentTarget::explorer_shell &&
         snapshot->target != protocol::AgentTarget::start_menu)) {
        return protocol::AgentResult::invalid_configuration;
    }

    ::AcquireSRWLockShared(&g_controller_lock);
    VisualTreeWatcher* watcher = g_visual_tree_watcher;
    if (watcher != nullptr) {
        watcher->AddRef();
    }
    ::ReleaseSRWLockShared(&g_controller_lock);
    if (watcher == nullptr) {
        return protocol::AgentResult::adapter_unavailable;
    }
    if (watcher->target() != snapshot->target) {
        watcher->Release();
        return protocol::AgentResult::incompatible_process;
    }
    watcher->CopyDiagnostics(*snapshot);
    watcher->Release();
    return protocol::AgentResult::success;
}

HRESULT GetStartMenuTapClassObject(
    REFCLSID class_id,
    REFIID interface_id,
    void** object) noexcept {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (class_id != kStartMenuTapClsid) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    g_diagnostic_stage.store(
        protocol::AgentDiagnosticStage::request_class_factory,
        std::memory_order_release);
    auto factory = Make<TapClassFactory>();
    return factory == nullptr ? E_OUTOFMEMORY
                              : factory.CopyTo(interface_id, object);
}

}  // namespace metaplasia::agent
