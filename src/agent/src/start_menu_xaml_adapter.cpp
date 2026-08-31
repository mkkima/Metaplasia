#include "metaplasia/agent/start_menu_xaml_adapter.hpp"

#include "metaplasia/agent/start_menu_style.hpp"
#include "metaplasia/agent/xaml_diagnostics_initialization.hpp"

#include <Windows.h>
#include <roapi.h>
#include <shellapi.h>
#include <ShlGuid.h>
#include <shobjidl_core.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif
#include <windows.ui.core.h>
#include <windows.storage.streams.h>
#include <windows.ui.xaml.h>
#include <windows.ui.xaml.controls.h>
#include <windows.ui.xaml.controls.primitives.h>
#include <windows.ui.xaml.media.h>
#include <windows.ui.xaml.media.imaging.h>
#include <windows.ui.xaml.shapes.h>
#include <wincodec.h>
#include <robuffer.h>
#include <xamlom.h>
#include <wrl.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
using DependencyObject = ABI::Windows::UI::Xaml::IDependencyObject;
using XamlVisibility = ABI::Windows::UI::Xaml::Visibility;
using XamlThickness = ABI::Windows::UI::Xaml::Thickness;
using XamlCornerRadius = ABI::Windows::UI::Xaml::CornerRadius;
using XamlHorizontalAlignment =
    ABI::Windows::UI::Xaml::HorizontalAlignment;
using XamlVerticalAlignment =
    ABI::Windows::UI::Xaml::VerticalAlignment;
using XamlBorder = ABI::Windows::UI::Xaml::Controls::IBorder;
using XamlControl7 = ABI::Windows::UI::Xaml::Controls::IControl7;
using XamlPanel = ABI::Windows::UI::Xaml::Controls::IPanel;
using XamlTextBlock = ABI::Windows::UI::Xaml::Controls::ITextBlock;
using XamlImage = ABI::Windows::UI::Xaml::Controls::IImage;
using XamlContentPresenter =
    ABI::Windows::UI::Xaml::Controls::IContentPresenter;
using XamlContentControl =
    ABI::Windows::UI::Xaml::Controls::IContentControl;
using XamlCanvasStatics =
    ABI::Windows::UI::Xaml::Controls::ICanvasStatics;
using XamlControl = ABI::Windows::UI::Xaml::Controls::IControl;
using XamlScrollViewer =
    ABI::Windows::UI::Xaml::Controls::IScrollViewer;
using XamlStackPanel = ABI::Windows::UI::Xaml::Controls::IStackPanel;
using XamlButtonBase =
    ABI::Windows::UI::Xaml::Controls::Primitives::IButtonBase;
using XamlGridStatics = ABI::Windows::UI::Xaml::Controls::IGridStatics;
using XamlUiElementVector =
    __FIVector_1_Windows__CUI__CXaml__CUIElement;
using XamlBrush = ABI::Windows::UI::Xaml::Media::IBrush;
using XamlVisualTreeHelperStatics =
    ABI::Windows::UI::Xaml::Media::IVisualTreeHelperStatics;
using XamlImageSource = ABI::Windows::UI::Xaml::Media::IImageSource;
using XamlWriteableBitmap =
    ABI::Windows::UI::Xaml::Media::Imaging::IWriteableBitmap;
using XamlWriteableBitmapFactory =
    ABI::Windows::UI::Xaml::Media::Imaging::IWriteableBitmapFactory;
using WinRtBuffer = ABI::Windows::Storage::Streams::IBuffer;
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
constexpr LONG kStartMenuAppIconSize = 22;
constexpr std::size_t kStartMenuAppIconByteCount =
    static_cast<std::size_t>(kStartMenuAppIconSize) *
    static_cast<std::size_t>(kStartMenuAppIconSize) * 4U;

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

struct ElementLayoutSnapshot final {
    double width{0.0};
    double height{0.0};
    XamlHorizontalAlignment horizontal_alignment{};
    XamlVerticalAlignment vertical_alignment{};
    XamlThickness margin{};
    XamlVisibility visibility{};
    INT32 grid_row{0};
    INT32 grid_column{0};
    INT32 grid_row_span{1};
    INT32 grid_column_span{1};
};

struct StartMenuFrameEnvelopeSnapshot final {
    // FullWindowMediaRoot is the verified outer boundary. Windows owns the
    // root surfaces above it, so only the three descendants participate in
    // the snapshot and in reversible layout mutations.
    std::array<ComPtr<FrameworkElement>, 3> elements;
    std::array<ElementLayoutSnapshot, 3> layouts;
    bool restored{false};
};

struct StartMenuThreePanelSurfaceSnapshot final {
    ComPtr<XamlUiElementVector> children;
    ComPtr<XamlBorder> acrylic_border;
    ComPtr<UiElement> acrylic_border_element;
    ComPtr<UiElement> acrylic_overlay_element;
    std::array<ComPtr<UiElement>, 3> panel_elements;
    std::array<ComPtr<XamlPanel>, 3> panel_hosts;
    std::array<ComPtr<XamlShape>, 3> panel_outlines;
    std::array<ComPtr<UiElement>, 2> panel_labels;
    double acrylic_border_opacity{1.0};
    double acrylic_overlay_opacity{1.0};
    bool restored{false};
};

struct StartMenuRecommendedSnapshot final {
    ComPtr<XamlContentPresenter> original_content_parent;
    ComPtr<IInspectable> original_content;
    ComPtr<XamlUiElementVector> original_children;
    ComPtr<XamlUiElementVector> destination_children;
    ComPtr<UiElement> recommended;
    UINT32 original_index{0};
    bool original_is_content{false};
    bool restored{false};
};

struct StartMenuAppEntry final {
    std::wstring display_name;
    std::wstring parsing_name;
    ComPtr<IShellItem> shell_item;
};

class StartMenuAppLaunchHandler final
    : public RuntimeClass<
          RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ABI::Windows::UI::Xaml::IRoutedEventHandler> {
public:
    [[nodiscard]] HRESULT Initialize(
        const std::wstring_view parsing_name) noexcept {
        if (parsing_name.empty() ||
            parsing_name.size() > (std::numeric_limits<UINT32>::max)()) {
            return E_INVALIDARG;
        }
        return parsing_name_.Set(
            parsing_name.data(),
            static_cast<UINT32>(parsing_name.size()));
    }

    HRESULT STDMETHODCALLTYPE Invoke(
        IInspectable*,
        ABI::Windows::UI::Xaml::IRoutedEventArgs*) noexcept override {
        auto* request = new (std::nothrow) LaunchRequest();
        if (request == nullptr ||
            FAILED(request->parsing_name.Set(parsing_name_.Get()))) {
            delete request;
            return S_OK;
        }
        // Launching synchronously can close Start re-entrantly while XAML is
        // still dispatching this Click event. Submit the shell activation only
        // after the handler has returned to keep the visual tree lifetime
        // deterministic. Activation failures are deliberately not propagated
        // into XAML; a failing routed event HRESULT terminates the Start host.
        if (!::TrySubmitThreadpoolCallback(
                LaunchOnThreadPool,
                request,
                nullptr)) {
            delete request;
        }
        return S_OK;
    }

private:
    struct LaunchRequest final {
        Microsoft::WRL::Wrappers::HString parsing_name;
    };

    static void CALLBACK LaunchOnThreadPool(
        PTP_CALLBACK_INSTANCE,
        void* context) noexcept {
        auto* request = static_cast<LaunchRequest*>(context);
        if (request == nullptr) {
            return;
        }
        UINT32 parsing_name_length = 0;
        const wchar_t* parsing_name = ::WindowsGetStringRawBuffer(
            request->parsing_name.Get(),
            &parsing_name_length);
        if (parsing_name != nullptr && parsing_name_length != 0) {
            const HRESULT apartment_result =
                ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            SHELLEXECUTEINFOW activation{};
            activation.cbSize = sizeof(activation);
            activation.fMask = SEE_MASK_FLAG_NO_UI;
            activation.lpVerb = L"open";
            activation.lpFile = parsing_name;
            activation.nShow = SW_SHOWNORMAL;
            static_cast<void>(::ShellExecuteExW(&activation));
            if (apartment_result == S_OK || apartment_result == S_FALSE) {
                ::CoUninitialize();
            }
        }
        delete request;
    }

    Microsoft::WRL::Wrappers::HString parsing_name_;
};

struct StartMenuAppButtonSubscription final {
    ComPtr<XamlButtonBase> button;
    ComPtr<ABI::Windows::UI::Xaml::IRoutedEventHandler> handler;
    EventRegistrationToken token{};
};

struct StartMenuAllAppsSnapshot final {
    ComPtr<XamlUiElementVector> destination_children;
    ComPtr<UiElement> scroll_viewer_element;
    std::vector<StartMenuAppButtonSubscription> subscriptions;
    bool restored{false};
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
std::atomic<bool> g_desired_start_menu_three_panel_layout_enabled{false};
std::atomic<bool> g_desired_start_menu_hide_all_apps{false};
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
XamlDiagnosticsInitialization g_diagnostics_initialization;
std::atomic<std::uint32_t> g_diagnostics_initialization_attempts{0};

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
            std::memory_order_acquire),
        g_desired_start_menu_three_panel_layout_enabled.load(
            std::memory_order_acquire),
        g_desired_start_menu_hide_all_apps.load(
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

    [[nodiscard]] HRESULT CaptureElementLayout(
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
        ComPtr<XamlGridStatics> grid;
        if (FAILED(result = inspectable.As(&ui_element)) ||
            FAILED(result = inspectable.As(&framework_element)) ||
            FAILED(result = GetGridStatics(grid))) {
            return result;
        }
        auto* captured = new (std::nothrow) ElementLayoutSnapshot();
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
            FAILED(result = grid->GetRow(
                       framework_element.Get(),
                       &captured->grid_row)) ||
            FAILED(result = grid->GetColumn(
                       framework_element.Get(),
                       &captured->grid_column)) ||
            FAILED(result = grid->GetRowSpan(
                       framework_element.Get(),
                       &captured->grid_row_span)) ||
            FAILED(result = grid->GetColumnSpan(
                       framework_element.Get(),
                       &captured->grid_column_span))) {
            delete captured;
            return result;
        }
        snapshot = reinterpret_cast<std::uint64_t>(captured);
        return S_OK;
    }

    [[nodiscard]] HRESULT WriteElementLayout(
        const std::uint64_t handle,
        const ShellElementLayout& layout) noexcept override {
        const auto valid_size = [](const double value) noexcept {
            return value == -1.0 || (std::isfinite(value) && value >= 0.0);
        };
        if (!valid_size(layout.width) || !valid_size(layout.height) ||
            !std::isfinite(layout.margin.left) ||
            !std::isfinite(layout.margin.top) ||
            !std::isfinite(layout.margin.right) ||
            !std::isfinite(layout.margin.bottom) ||
            (layout.reset_grid_position &&
             (layout.grid_row < 0 || layout.grid_column < 0 ||
              layout.grid_row_span < 1 || layout.grid_column_span < 1))) {
            return E_INVALIDARG;
        }
        ComPtr<IInspectable> inspectable;
        HRESULT result = ResolveInspectable(handle, inspectable);
        if (FAILED(result)) {
            return result;
        }
        ComPtr<UiElement> ui_element;
        ComPtr<FrameworkElement> framework_element;
        if (FAILED(result = inspectable.As(&ui_element)) ||
            FAILED(result = inspectable.As(&framework_element))) {
            return result;
        }
        const auto horizontal = ToXamlHorizontalAlignment(
            layout.horizontal_alignment);
        const auto vertical = ToXamlVerticalAlignment(
            layout.vertical_alignment);
        if (!horizontal.has_value() || !vertical.has_value()) {
            return E_INVALIDARG;
        }
        const double automatic_size =
            (std::numeric_limits<double>::quiet_NaN)();
        if (layout.write_size &&
            (FAILED(result = framework_element->put_Width(
                        layout.width == -1.0 ? automatic_size : layout.width)) ||
             FAILED(result = framework_element->put_Height(
                        layout.height == -1.0
                            ? automatic_size
                            : layout.height)))) {
            return result;
        }
        if (layout.write_alignment &&
            (FAILED(result = framework_element->put_HorizontalAlignment(
                        *horizontal)) ||
             FAILED(result = framework_element->put_VerticalAlignment(
                        *vertical)))) {
            return result;
        }
        if (layout.write_margin &&
            FAILED(result = framework_element->put_Margin(XamlThickness{
                       layout.margin.left,
                       layout.margin.top,
                       layout.margin.right,
                       layout.margin.bottom}))) {
            return result;
        }
        if (layout.write_visibility &&
            FAILED(result = ui_element->put_Visibility(
                       layout.visible
                           ? ABI::Windows::UI::Xaml::Visibility_Visible
                           : ABI::Windows::UI::Xaml::Visibility_Collapsed))) {
            return result;
        }
        if (!layout.reset_grid_position) {
            return S_OK;
        }
        ComPtr<XamlGridStatics> grid;
        if (FAILED(result = GetGridStatics(grid)) ||
            FAILED(result = grid->SetRow(
                       framework_element.Get(),
                       layout.grid_row)) ||
            FAILED(result = grid->SetColumn(
                       framework_element.Get(),
                       layout.grid_column)) ||
            FAILED(result = grid->SetRowSpan(
                       framework_element.Get(),
                       layout.grid_row_span)) ||
            FAILED(result = grid->SetColumnSpan(
                       framework_element.Get(),
                       layout.grid_column_span))) {
            return result;
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT RestoreElementLayout(
        const std::uint64_t handle,
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        const auto* captured =
            reinterpret_cast<const ElementLayoutSnapshot*>(snapshot);
        ComPtr<IInspectable> inspectable;
        HRESULT result = ResolveInspectable(handle, inspectable);
        if (FAILED(result)) {
            return result;
        }
        ComPtr<UiElement> ui_element;
        ComPtr<FrameworkElement> framework_element;
        ComPtr<XamlGridStatics> grid;
        if (FAILED(result = inspectable.As(&ui_element)) ||
            FAILED(result = inspectable.As(&framework_element)) ||
            FAILED(result = GetGridStatics(grid))) {
            return result;
        }
        HRESULT first_failure = S_OK;
        const auto preserve_first_failure =
            [&first_failure](const HRESULT candidate) noexcept {
                if (FAILED(candidate) && SUCCEEDED(first_failure)) {
                    first_failure = candidate;
                }
            };
        preserve_first_failure(framework_element->put_Width(captured->width));
        preserve_first_failure(framework_element->put_Height(captured->height));
        preserve_first_failure(framework_element->put_HorizontalAlignment(
            captured->horizontal_alignment));
        preserve_first_failure(framework_element->put_VerticalAlignment(
            captured->vertical_alignment));
        preserve_first_failure(
            framework_element->put_Margin(captured->margin));
        preserve_first_failure(
            ui_element->put_Visibility(captured->visibility));
        preserve_first_failure(
            grid->SetRow(framework_element.Get(), captured->grid_row));
        preserve_first_failure(
            grid->SetColumn(framework_element.Get(), captured->grid_column));
        preserve_first_failure(grid->SetRowSpan(
            framework_element.Get(),
            captured->grid_row_span));
        preserve_first_failure(grid->SetColumnSpan(
            framework_element.Get(),
            captured->grid_column_span));
        return first_failure;
    }

    void ReleaseElementLayoutSnapshot(
        const std::uint64_t snapshot) noexcept override {
        delete reinterpret_cast<ElementLayoutSnapshot*>(snapshot);
    }

    [[nodiscard]] HRESULT IsDescendantOf(
        const std::uint64_t descendant_handle,
        const std::uint64_t ancestor_handle,
        bool& is_descendant) noexcept override {
        is_descendant = false;
        if (descendant_handle == 0 || ancestor_handle == 0) {
            return E_INVALIDARG;
        }
        ComPtr<IInspectable> descendant_inspectable;
        ComPtr<IInspectable> ancestor_inspectable;
        HRESULT result = ResolveInspectable(
            descendant_handle,
            descendant_inspectable);
        if (FAILED(result) ||
            FAILED(result = ResolveInspectable(
                       ancestor_handle,
                       ancestor_inspectable))) {
            return result;
        }
        ComPtr<IUnknown> ancestor_identity;
        ComPtr<DependencyObject> current;
        ComPtr<XamlVisualTreeHelperStatics> visual_tree;
        if (FAILED(result = ancestor_inspectable.As(&ancestor_identity)) ||
            FAILED(result = descendant_inspectable.As(&current)) ||
            FAILED(result = GetVisualTreeHelperStatics(visual_tree))) {
            return result;
        }
        constexpr std::size_t kMaximumAncestorDepth = 64;
        for (std::size_t depth = 0;
             depth < kMaximumAncestorDepth && current != nullptr;
             ++depth) {
            ComPtr<IUnknown> current_identity;
            if (FAILED(result = current.As(&current_identity))) {
                return result;
            }
            if (current_identity.Get() == ancestor_identity.Get()) {
                is_descendant = true;
                return S_OK;
            }
            ComPtr<DependencyObject> parent;
            if (FAILED(result = visual_tree->GetParent(
                           current.Get(),
                           parent.GetAddressOf()))) {
                return result;
            }
            if (parent == nullptr) {
                current.Reset();
            } else {
                current = std::move(parent);
            }
        }
        return current == nullptr ? S_OK : E_UNEXPECTED;
    }

    [[nodiscard]] HRESULT CreateStartMenuFrameEnvelope(
        const std::uint64_t frame_handle,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        ComPtr<IInspectable> frame_inspectable;
        HRESULT result = ResolveInspectable(frame_handle, frame_inspectable);
        ComPtr<DependencyObject> current;
        ComPtr<XamlVisualTreeHelperStatics> visual_tree;
        if (FAILED(result) ||
            FAILED(result = frame_inspectable.As(&current)) ||
            FAILED(result = GetVisualTreeHelperStatics(visual_tree))) {
            return result;
        }

        constexpr std::array<std::wstring_view, 4> expected_types{
            L"Windows.UI.Xaml.Controls.Border",
            L"Windows.UI.Xaml.Controls.ScrollContentPresenter",
            L"Windows.UI.Xaml.Internal.RootScrollViewer",
            L"Windows.UI.Xaml.FullWindowMediaRoot"};
        std::array<ComPtr<FrameworkElement>, 3> elements;
        for (std::size_t index = 0; index < expected_types.size(); ++index) {
            ComPtr<DependencyObject> parent;
            result = visual_tree->GetParent(
                current.Get(),
                parent.GetAddressOf());
            if (FAILED(result) || parent == nullptr) {
                return FAILED(result) ? result : E_NOTFOUND;
            }
            std::wstring_view runtime_name;
            Microsoft::WRL::Wrappers::HString runtime_name_storage;
            result = ReadRuntimeClassName(
                parent.Get(),
                runtime_name_storage,
                runtime_name);
            if (FAILED(result)) {
                return result;
            }
            if (runtime_name != expected_types[index]) {
                return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            }
            ComPtr<FrameworkElement> parent_element;
            if (FAILED(result = parent.As(&parent_element))) {
                return result;
            }
            if (index < elements.size()) {
                elements[index] = parent_element;
            }
            current = std::move(parent);
        }

        auto* captured =
            new (std::nothrow) StartMenuFrameEnvelopeSnapshot();
        if (captured == nullptr) {
            return E_OUTOFMEMORY;
        }
        captured->elements = std::move(elements);
        for (std::size_t index = 0; index < captured->elements.size(); ++index) {
            result = CaptureFrameworkElementLayout(
                captured->elements[index].Get(),
                captured->layouts[index]);
            if (FAILED(result)) {
                delete captured;
                return result;
            }
        }

        const ShellElementLayout layout =
            StartMenuLayoutFor(StartMenuLayoutRule::frame);
        for (std::size_t remaining = captured->elements.size();
             remaining > 0;
             --remaining) {
            result = WriteFrameworkElementLayout(
                captured->elements[remaining - 1].Get(),
                layout);
            if (FAILED(result)) {
                static_cast<void>(CleanupStartMenuFrameEnvelope(captured));
                delete captured;
                return result;
            }
        }
        snapshot = reinterpret_cast<std::uint64_t>(captured);
        return S_OK;
    }

    [[nodiscard]] HRESULT RestoreStartMenuFrameEnvelope(
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        return CleanupStartMenuFrameEnvelope(reinterpret_cast<
            StartMenuFrameEnvelopeSnapshot*>(snapshot));
    }

    void ReleaseStartMenuFrameEnvelopeSnapshot(
        const std::uint64_t snapshot) noexcept override {
        auto* captured = reinterpret_cast<
            StartMenuFrameEnvelopeSnapshot*>(snapshot);
        if (captured != nullptr) {
            static_cast<void>(CleanupStartMenuFrameEnvelope(captured));
            delete captured;
        }
    }

    [[nodiscard]] HRESULT CreateStartMenuThreePanelSurface(
        const std::uint64_t main_menu_handle,
        const std::uint64_t acrylic_border_handle,
        const std::uint64_t acrylic_overlay_handle,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        ComPtr<IInspectable> main_menu_inspectable;
        ComPtr<IInspectable> acrylic_border_inspectable;
        ComPtr<IInspectable> acrylic_overlay_inspectable;
        HRESULT result = ResolveInspectable(
            main_menu_handle,
            main_menu_inspectable);
        if (FAILED(result) ||
            FAILED(result = ResolveInspectable(
                       acrylic_border_handle,
                       acrylic_border_inspectable)) ||
            FAILED(result = ResolveInspectable(
                       acrylic_overlay_handle,
                       acrylic_overlay_inspectable))) {
            return result;
        }
        auto* captured =
            new (std::nothrow) StartMenuThreePanelSurfaceSnapshot();
        if (captured == nullptr) {
            return E_OUTOFMEMORY;
        }
        ComPtr<XamlPanel> main_menu;
        ComPtr<XamlBrush> panel_background;
        if (FAILED(result = main_menu_inspectable.As(&main_menu)) ||
            FAILED(result = acrylic_border_inspectable.As(
                       &captured->acrylic_border)) ||
            FAILED(result = acrylic_border_inspectable.As(
                       &captured->acrylic_border_element)) ||
            FAILED(result = acrylic_overlay_inspectable.As(
                       &captured->acrylic_overlay_element)) ||
            FAILED(result = main_menu->get_Children(
                       captured->children.GetAddressOf())) ||
            FAILED(result = captured->acrylic_border->get_Background(
                       panel_background.GetAddressOf())) ||
            FAILED(result = captured->acrylic_border_element->get_Opacity(
                       &captured->acrylic_border_opacity)) ||
            FAILED(result = captured->acrylic_overlay_element->get_Opacity(
                       &captured->acrylic_overlay_opacity))) {
            delete captured;
            return result;
        }

        ComPtr<XamlBrush> outline;
        result = CreateSolidColorBrush(0x4DFFFFFFU, outline);
        ComPtr<XamlBrush> label_foreground;
        if (FAILED(result) ||
            FAILED(result = CreateSolidColorBrush(
                       0xFFFFFFFFU,
                       label_foreground))) {
            delete captured;
            return result;
        }
        constexpr std::array<double, 3> widths{320.0, 530.0, 320.0};
        // Keep the central surface fixed and move both side surfaces inward.
        // This yields equal 8-DIP gaps and equal 30-DIP outer margins.
        constexpr std::array<double, 3> left_offsets{30.0, 358.0, 896.0};
        constexpr double panel_corner_radius = 24.0;
        // Rounded rectangles need a transparent render gutter. Without it,
        // WinUI clips the antialiased pixels at the Grid's top boundary and
        // visibly shaves the upper arcs. Keep the original side and bottom
        // bounds, but move the rendered top edge inside the host.
        constexpr double horizontal_render_inset = 2.0;
        constexpr double top_render_inset = 4.0;
        constexpr double horizontal_host_expansion =
            horizontal_render_inset * 2.0;
        constexpr double outline_thickness = 1.0;
        constexpr double outline_half = outline_thickness / 2.0;
        constexpr double shape_horizontal_inset =
            horizontal_render_inset + outline_half;
        constexpr double shape_top_inset =
            top_render_inset + outline_half;
        for (std::size_t index = 0;
             index < captured->panel_hosts.size();
             ++index) {
            ComPtr<IInspectable> panel_inspectable;
            Microsoft::WRL::Wrappers::HStringReference class_name(
                RuntimeClass_Windows_UI_Xaml_Controls_Grid);
            result = ::RoActivateInstance(
                class_name.Get(),
                panel_inspectable.GetAddressOf());
            ComPtr<FrameworkElement> framework_element;
            ComPtr<UiElement> ui_element;
            if (FAILED(result) ||
                FAILED(result = panel_inspectable.As(
                           &captured->panel_hosts[index])) ||
                FAILED(result = panel_inspectable.As(
                           &captured->panel_elements[index])) ||
                FAILED(result = panel_inspectable.As(&framework_element)) ||
                FAILED(result = panel_inspectable.As(&ui_element)) ||
                FAILED(result = ui_element->put_UseLayoutRounding(true)) ||
                FAILED(result = framework_element->put_Width(
                           widths[index] + horizontal_host_expansion)) ||
                FAILED(result = framework_element->put_Height(
                           kStartMenuThreePanelPanelHeight)) ||
                FAILED(result = framework_element->put_HorizontalAlignment(
                           ABI::Windows::UI::Xaml::
                               HorizontalAlignment_Left)) ||
                FAILED(result = framework_element->put_VerticalAlignment(
                           ABI::Windows::UI::Xaml::
                               VerticalAlignment_Top)) ||
                FAILED(result = framework_element->put_Margin(XamlThickness{
                           left_offsets[index] - horizontal_render_inset,
                           kStartMenuThreePanelTopInset,
                           0.0,
                           0.0})) ||
                FAILED(result = captured->panel_elements[index]
                                    ->put_IsHitTestVisible(true))) {
                static_cast<void>(CleanupStartMenuThreePanelSurface(captured));
                delete captured;
                return result;
            }
            if (index == 0) {
                // MainContent is a later sibling and spans the expanded
                // frame. Without an explicit z-order it intercepts pointer
                // input over the otherwise visible All apps panel.
                Microsoft::WRL::Wrappers::HStringReference canvas_class(
                    RuntimeClass_Windows_UI_Xaml_Controls_Canvas);
                ComPtr<XamlCanvasStatics> canvas;
                result = ::RoGetActivationFactory(
                    canvas_class.Get(),
                    __uuidof(XamlCanvasStatics),
                    reinterpret_cast<void**>(canvas.GetAddressOf()));
                if (FAILED(result) ||
                    FAILED(result = canvas->SetZIndex(
                               captured->panel_elements[index].Get(),
                               100))) {
                    static_cast<void>(
                        CleanupStartMenuThreePanelSurface(captured));
                    delete captured;
                    return result;
                }
            }

            ComPtr<XamlUiElementVector> panel_children;
            ComPtr<IInspectable> outline_inspectable;
            Microsoft::WRL::Wrappers::HStringReference rectangle_class(
                RuntimeClass_Windows_UI_Xaml_Shapes_Rectangle);
            result = captured->panel_hosts[index]->get_Children(
                panel_children.GetAddressOf());
            if (SUCCEEDED(result)) {
                result = ::RoActivateInstance(
                    rectangle_class.Get(),
                    outline_inspectable.GetAddressOf());
            }
            ComPtr<XamlRectangle> outline_rectangle;
            ComPtr<FrameworkElement> outline_element;
            ComPtr<UiElement> outline_ui_element;
            if (FAILED(result) ||
                FAILED(result = outline_inspectable.As(
                           &captured->panel_outlines[index])) ||
                FAILED(result = outline_inspectable.As(
                           &outline_rectangle)) ||
                FAILED(result = outline_inspectable.As(&outline_element)) ||
                FAILED(result = outline_inspectable.As(&outline_ui_element)) ||
                FAILED(result = captured->panel_outlines[index]->put_Fill(
                           panel_background.Get())) ||
                FAILED(result = captured->panel_outlines[index]->put_Stroke(
                           outline.Get())) ||
                FAILED(result = captured->panel_outlines[index]
                                    ->put_StrokeThickness(
                                        outline_thickness)) ||
                FAILED(result = outline_rectangle->put_RadiusX(
                           panel_corner_radius - outline_half)) ||
                FAILED(result = outline_rectangle->put_RadiusY(
                           panel_corner_radius - outline_half)) ||
                FAILED(result = outline_ui_element->put_UseLayoutRounding(
                           false)) ||
                FAILED(result = outline_ui_element->put_IsHitTestVisible(
                           false)) ||
                FAILED(result = outline_element->put_Width(
                           widths[index] - outline_thickness)) ||
                FAILED(result = outline_element->put_Height(
                           kStartMenuThreePanelPanelHeight -
                               top_render_inset -
                               outline_thickness)) ||
                FAILED(result = outline_element->put_HorizontalAlignment(
                           ABI::Windows::UI::Xaml::
                               HorizontalAlignment_Left)) ||
                FAILED(result = outline_element->put_VerticalAlignment(
                           ABI::Windows::UI::Xaml::
                               VerticalAlignment_Top)) ||
                FAILED(result = outline_element->put_Margin(XamlThickness{
                           shape_horizontal_inset,
                           shape_top_inset,
                           0.0,
                           0.0})) ||
                FAILED(result = panel_children->Append(
                           outline_ui_element.Get()))) {
                static_cast<void>(CleanupStartMenuThreePanelSurface(captured));
                delete captured;
                return result;
            }
            if (index == 0 || index == 2) {
                const std::size_t label_index = index == 0 ? 0U : 1U;
                constexpr std::array<std::wstring_view, 2> labels{
                    L"Все приложения",
                    L"Рекомендуемые"};
                ComPtr<IInspectable> label_inspectable;
                Microsoft::WRL::Wrappers::HStringReference label_class(
                    RuntimeClass_Windows_UI_Xaml_Controls_TextBlock);
                result = ::RoActivateInstance(
                    label_class.Get(),
                    label_inspectable.GetAddressOf());
                ComPtr<XamlTextBlock> label;
                ComPtr<FrameworkElement> label_element;
                Microsoft::WRL::Wrappers::HStringReference label_text(
                    labels[label_index].data());
                if (FAILED(result) ||
                    FAILED(result = label_inspectable.As(&label)) ||
                    FAILED(result = label_inspectable.As(&label_element)) ||
                    FAILED(result = label_inspectable.As(
                               &captured->panel_labels[label_index])) ||
                    FAILED(result = captured->panel_labels[label_index]
                                        ->put_IsHitTestVisible(false)) ||
                    FAILED(result = label->put_Text(label_text.Get())) ||
                    FAILED(result = label->put_FontSize(15.0)) ||
                    FAILED(result = label->put_Foreground(
                               label_foreground.Get())) ||
                    FAILED(result = label_element->put_HorizontalAlignment(
                               ABI::Windows::UI::Xaml::
                                   HorizontalAlignment_Left)) ||
                    FAILED(result = label_element->put_VerticalAlignment(
                               ABI::Windows::UI::Xaml::
                                   VerticalAlignment_Top)) ||
                    FAILED(result = label_element->put_Margin(XamlThickness{
                               24.0 + horizontal_render_inset,
                               24.0 + top_render_inset,
                               0.0,
                               0.0})) ||
                    FAILED(result = panel_children->Append(
                               captured->panel_labels[label_index].Get()))) {
                    static_cast<void>(
                        CleanupStartMenuThreePanelSurface(captured));
                    delete captured;
                    return result;
                }
            }
            UINT32 size = 0;
            if (FAILED(result = captured->children->get_Size(&size))) {
                static_cast<void>(CleanupStartMenuThreePanelSurface(captured));
                delete captured;
                return result;
            }
            const UINT32 insertion_index =
                (std::min)(size, static_cast<UINT32>(index + 1U));
            if (FAILED(result = captured->children->InsertAt(
                           insertion_index,
                           captured->panel_elements[index].Get()))) {
                static_cast<void>(CleanupStartMenuThreePanelSurface(captured));
                delete captured;
                return result;
            }
        }
        if (FAILED(result = captured->acrylic_border_element->put_Opacity(0.0)) ||
            FAILED(result = captured->acrylic_overlay_element->put_Opacity(0.0))) {
            static_cast<void>(CleanupStartMenuThreePanelSurface(captured));
            delete captured;
            return result;
        }
        snapshot = reinterpret_cast<std::uint64_t>(captured);
        return S_OK;
    }

    [[nodiscard]] HRESULT UpdateStartMenuThreePanelSurface(
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        auto* captured = reinterpret_cast<
            StartMenuThreePanelSurfaceSnapshot*>(snapshot);
        if (captured->restored) {
            return E_UNEXPECTED;
        }
        ComPtr<XamlBrush> background;
        HRESULT result = captured->acrylic_border->get_Background(
            background.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        for (const auto& panel_surface : captured->panel_outlines) {
            if (FAILED(result = panel_surface->put_Fill(background.Get()))) {
                return result;
            }
        }
        return S_OK;
    }

    [[nodiscard]] HRESULT RestoreStartMenuThreePanelSurface(
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        return CleanupStartMenuThreePanelSurface(reinterpret_cast<
            StartMenuThreePanelSurfaceSnapshot*>(snapshot));
    }

    void ReleaseStartMenuThreePanelSurfaceSnapshot(
        const std::uint64_t snapshot) noexcept override {
        auto* captured = reinterpret_cast<
            StartMenuThreePanelSurfaceSnapshot*>(snapshot);
        if (captured != nullptr) {
            static_cast<void>(CleanupStartMenuThreePanelSurface(captured));
            delete captured;
        }
    }

    [[nodiscard]] HRESULT AttachStartMenuRecommended(
        const std::uint64_t recommended_handle,
        const std::uint64_t original_parent_handle,
        const std::uint64_t destination_panel_handle,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        if (recommended_handle == 0 || original_parent_handle == 0 ||
            destination_panel_handle == 0 ||
            recommended_handle == original_parent_handle ||
            recommended_handle == destination_panel_handle ||
            original_parent_handle == destination_panel_handle) {
            return E_INVALIDARG;
        }
        ComPtr<IInspectable> recommended_inspectable;
        ComPtr<IInspectable> parent_inspectable;
        ComPtr<IInspectable> destination_inspectable;
        HRESULT result = ResolveInspectable(
            recommended_handle,
            recommended_inspectable);
        if (FAILED(result) ||
            FAILED(result = ResolveInspectable(
                       original_parent_handle,
                       parent_inspectable)) ||
            FAILED(result = ResolveInspectable(
                       destination_panel_handle,
                       destination_inspectable))) {
            return result;
        }
        auto* captured =
            new (std::nothrow) StartMenuRecommendedSnapshot();
        if (captured == nullptr) {
            return E_OUTOFMEMORY;
        }
        ComPtr<XamlPanel> destination;
        if (FAILED(result = recommended_inspectable.As(
                       &captured->recommended)) ||
            FAILED(result = destination_inspectable.As(&destination)) ||
            FAILED(result = destination->get_Children(
                       captured->destination_children.GetAddressOf()))) {
            delete captured;
            return result;
        }
        boolean already_in_destination = false;
        UINT32 destination_index = 0;
        if (FAILED(result = captured->destination_children->IndexOf(
                       captured->recommended.Get(),
                       &destination_index,
                       &already_in_destination)) ||
            already_in_destination) {
            delete captured;
            return FAILED(result) ? result : E_UNEXPECTED;
        }

        result = parent_inspectable.As(&captured->original_content_parent);
        if (SUCCEEDED(result)) {
            captured->original_is_content = true;
            if (FAILED(result = captured->original_content_parent->get_Content(
                           captured->original_content.GetAddressOf()))) {
                delete captured;
                return result;
            }
            ComPtr<IUnknown> content_identity;
            ComPtr<IUnknown> recommended_identity;
            if (captured->original_content == nullptr ||
                FAILED(result = captured->original_content.As(
                           &content_identity)) ||
                FAILED(result = recommended_inspectable.As(
                           &recommended_identity)) ||
                content_identity.Get() != recommended_identity.Get()) {
                delete captured;
                return E_NOINTERFACE;
            }
            result = captured->original_content_parent->put_Content(nullptr);
        } else {
            ComPtr<XamlPanel> original_panel;
            if (FAILED(result = parent_inspectable.As(&original_panel)) ||
                FAILED(result = original_panel->get_Children(
                           captured->original_children.GetAddressOf()))) {
                delete captured;
                return result;
            }
            boolean found = false;
            if (FAILED(result = captured->original_children->IndexOf(
                           captured->recommended.Get(),
                           &captured->original_index,
                           &found)) ||
                !found) {
                delete captured;
                return FAILED(result) ? result : E_NOINTERFACE;
            }
            result = captured->original_children->RemoveAt(
                captured->original_index);
        }
        if (FAILED(result)) {
            delete captured;
            return result;
        }

        result = captured->destination_children->Append(
            captured->recommended.Get());
        if (FAILED(result)) {
            if (captured->original_is_content) {
                static_cast<void>(
                    captured->original_content_parent->put_Content(
                        captured->original_content.Get()));
            } else {
                static_cast<void>(captured->original_children->InsertAt(
                    captured->original_index,
                    captured->recommended.Get()));
            }
            delete captured;
            return result;
        }
        snapshot = reinterpret_cast<std::uint64_t>(captured);
        return S_OK;
    }

    [[nodiscard]] HRESULT RestoreStartMenuRecommended(
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        return CleanupStartMenuRecommended(reinterpret_cast<
            StartMenuRecommendedSnapshot*>(snapshot));
    }

    void ReleaseStartMenuRecommendedSnapshot(
        const std::uint64_t snapshot) noexcept override {
        auto* captured = reinterpret_cast<
            StartMenuRecommendedSnapshot*>(snapshot);
        if (captured != nullptr) {
            static_cast<void>(CleanupStartMenuRecommended(captured));
            delete captured;
        }
    }

    [[nodiscard]] HRESULT CreateStartMenuAllAppsPanel(
        const std::uint64_t panel_surface_snapshot,
        const bool visible,
        std::uint64_t& snapshot) noexcept override {
        snapshot = 0;
        if (panel_surface_snapshot == 0) {
            return E_INVALIDARG;
        }
        auto* surface = reinterpret_cast<
            StartMenuThreePanelSurfaceSnapshot*>(panel_surface_snapshot);
        if (surface->restored || surface->panel_hosts[0] == nullptr) {
            return E_UNEXPECTED;
        }
        std::vector<StartMenuAppEntry> apps;
        HRESULT result = EnumerateStartMenuApps(apps);
        if (FAILED(result)) {
            return result;
        }
        auto* captured = new (std::nothrow) StartMenuAllAppsSnapshot();
        if (captured == nullptr) {
            return E_OUTOFMEMORY;
        }
        if (FAILED(result = surface->panel_hosts[0]->get_Children(
                       captured->destination_children.GetAddressOf()))) {
            delete captured;
            return result;
        }
        try {
            captured->subscriptions.reserve(apps.size());
        } catch (const std::bad_alloc&) {
            delete captured;
            return E_OUTOFMEMORY;
        }

        ComPtr<XamlBrush> foreground;
        ComPtr<XamlBrush> transparent;
        result = CreateSolidColorBrush(0xFFFFFFFFU, foreground);
        if (SUCCEEDED(result)) {
            result = CreateSolidColorBrush(0x00FFFFFFU, transparent);
        }
        if (FAILED(result)) {
            delete captured;
            return result;
        }
        ComPtr<IWICImagingFactory> icon_imaging_factory;
        static_cast<void>(::CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            __uuidof(IWICImagingFactory),
            reinterpret_cast<void**>(
                icon_imaging_factory.GetAddressOf())));

        ComPtr<IInspectable> scroll_inspectable;
        Microsoft::WRL::Wrappers::HStringReference scroll_class(
            RuntimeClass_Windows_UI_Xaml_Controls_ScrollViewer);
        result = ::RoActivateInstance(
            scroll_class.Get(),
            scroll_inspectable.GetAddressOf());
        ComPtr<XamlScrollViewer> scroll_viewer;
        ComPtr<XamlContentControl> scroll_content;
        ComPtr<FrameworkElement> scroll_element;
        if (FAILED(result) ||
            FAILED(result = scroll_inspectable.As(&scroll_viewer)) ||
            FAILED(result = scroll_inspectable.As(&scroll_content)) ||
            FAILED(result = scroll_inspectable.As(&scroll_element)) ||
            FAILED(result = scroll_inspectable.As(
                       &captured->scroll_viewer_element)) ||
            FAILED(result = scroll_viewer->put_HorizontalScrollBarVisibility(
                       ABI::Windows::UI::Xaml::Controls::
                           ScrollBarVisibility_Disabled)) ||
            FAILED(result = scroll_viewer->put_VerticalScrollBarVisibility(
                       ABI::Windows::UI::Xaml::Controls::
                           ScrollBarVisibility_Auto)) ||
            FAILED(result = scroll_viewer->put_HorizontalScrollMode(
                       ABI::Windows::UI::Xaml::Controls::ScrollMode_Disabled)) ||
            FAILED(result = scroll_viewer->put_VerticalScrollMode(
                       ABI::Windows::UI::Xaml::Controls::ScrollMode_Enabled)) ||
            FAILED(result = scroll_element->put_Width(276.0)) ||
            FAILED(result = scroll_element->put_Height(
                       kStartMenuThreePanelContentHeight)) ||
            FAILED(result = scroll_element->put_HorizontalAlignment(
                       ABI::Windows::UI::Xaml::HorizontalAlignment_Left)) ||
            FAILED(result = scroll_element->put_VerticalAlignment(
                       ABI::Windows::UI::Xaml::VerticalAlignment_Top)) ||
            FAILED(result = scroll_element->put_Margin(XamlThickness{
                       28.0,
                       kStartMenuThreePanelContentTop,
                       0.0,
                       0.0})) ||
            FAILED(result = captured->scroll_viewer_element->put_Visibility(
                       visible
                           ? ABI::Windows::UI::Xaml::Visibility_Visible
                           : ABI::Windows::UI::Xaml::Visibility_Collapsed)) ||
            FAILED(result = captured->scroll_viewer_element
                                ->put_IsHitTestVisible(true))) {
            delete captured;
            return result;
        }

        ComPtr<IInspectable> stack_inspectable;
        Microsoft::WRL::Wrappers::HStringReference stack_class(
            RuntimeClass_Windows_UI_Xaml_Controls_StackPanel);
        result = ::RoActivateInstance(
            stack_class.Get(),
            stack_inspectable.GetAddressOf());
        ComPtr<XamlStackPanel> stack_panel;
        ComPtr<XamlPanel> stack_children_owner;
        ComPtr<FrameworkElement> stack_element;
        ComPtr<XamlUiElementVector> stack_children;
        if (FAILED(result) ||
            FAILED(result = stack_inspectable.As(&stack_panel)) ||
            FAILED(result = stack_inspectable.As(&stack_children_owner)) ||
            FAILED(result = stack_inspectable.As(&stack_element)) ||
            FAILED(result = stack_children_owner->get_Children(
                       stack_children.GetAddressOf())) ||
            FAILED(result = stack_panel->put_Orientation(
                       ABI::Windows::UI::Xaml::Controls::
                           Orientation_Vertical)) ||
            FAILED(result = stack_element->put_Width(260.0)) ||
            FAILED(result = scroll_content->put_Content(
                       stack_inspectable.Get()))) {
            delete captured;
            return result;
        }

        wchar_t current_group = L'\0';
        for (const auto& app : apps) {
            const wchar_t group = StartMenuAppGroup(app.display_name);
            if (group != current_group) {
                current_group = group;
                const wchar_t group_text[2]{current_group, L'\0'};
                ComPtr<IInspectable> heading_inspectable;
                ComPtr<UiElement> heading_element;
                result = CreateStartMenuTextBlock(
                    std::wstring_view(group_text, 1),
                    12.0,
                    0.78,
                    XamlThickness{8.0, 12.0, 0.0, 4.0},
                    foreground.Get(),
                    heading_inspectable,
                    heading_element);
                if (FAILED(result) ||
                    FAILED(result = stack_children->Append(
                               heading_element.Get()))) {
                    static_cast<void>(CleanupStartMenuAllApps(captured));
                    delete captured;
                    return result;
                }
            }

            ComPtr<IInspectable> text_inspectable;
            ComPtr<UiElement> text_element;
            result = CreateStartMenuTextBlock(
                app.display_name,
                14.0,
                1.0,
                XamlThickness{},
                foreground.Get(),
                text_inspectable,
                text_element);
            ComPtr<XamlTextBlock> text_block;
            ComPtr<FrameworkElement> text_framework_element;
            if (SUCCEEDED(result)) {
                result = text_inspectable.As(&text_block);
            }
            if (SUCCEEDED(result)) {
                result = text_inspectable.As(&text_framework_element);
            }
            if (SUCCEEDED(result)) {
                result = text_block->put_TextTrimming(
                    ABI::Windows::UI::Xaml::TextTrimming_CharacterEllipsis);
            }
            if (SUCCEEDED(result)) {
                result = text_framework_element->put_Width(202.0);
            }
            if (SUCCEEDED(result)) {
                result = text_framework_element->put_VerticalAlignment(
                    ABI::Windows::UI::Xaml::VerticalAlignment_Center);
            }

            ComPtr<IInspectable> icon_inspectable;
            ComPtr<UiElement> icon_element;
            if (SUCCEEDED(result)) {
                result = CreateStartMenuAppIcon(
                    app,
                    icon_imaging_factory.Get(),
                    icon_inspectable,
                    icon_element);
                if (FAILED(result)) {
                    result = CreateStartMenuFallbackAppIcon(
                        app.display_name,
                        foreground.Get(),
                        icon_inspectable,
                        icon_element);
                }
            }

            ComPtr<IInspectable> row_inspectable;
            ComPtr<XamlStackPanel> row_stack_panel;
            ComPtr<XamlPanel> row_children_owner;
            ComPtr<XamlUiElementVector> row_children;
            ComPtr<FrameworkElement> row_element;
            if (SUCCEEDED(result)) {
                Microsoft::WRL::Wrappers::HStringReference row_class(
                    RuntimeClass_Windows_UI_Xaml_Controls_StackPanel);
                result = ::RoActivateInstance(
                    row_class.Get(),
                    row_inspectable.GetAddressOf());
            }
            if (SUCCEEDED(result)) {
                result = row_inspectable.As(&row_stack_panel);
            }
            if (SUCCEEDED(result)) {
                result = row_inspectable.As(&row_children_owner);
            }
            if (SUCCEEDED(result)) {
                result = row_inspectable.As(&row_element);
            }
            if (SUCCEEDED(result)) {
                result = row_children_owner->get_Children(
                    row_children.GetAddressOf());
            }
            if (SUCCEEDED(result)) {
                result = row_stack_panel->put_Orientation(
                    ABI::Windows::UI::Xaml::Controls::Orientation_Horizontal);
            }
            if (SUCCEEDED(result)) {
                result = row_element->put_Width(252.0);
            }
            if (SUCCEEDED(result)) {
                result = row_element->put_Height(38.0);
            }
            if (SUCCEEDED(result)) {
                result = row_children->Append(icon_element.Get());
            }
            if (SUCCEEDED(result)) {
                result = row_children->Append(text_element.Get());
            }
            ComPtr<IInspectable> button_inspectable;
            Microsoft::WRL::Wrappers::HStringReference button_class(
                RuntimeClass_Windows_UI_Xaml_Controls_Button);
            if (SUCCEEDED(result)) {
                result = ::RoActivateInstance(
                    button_class.Get(),
                    button_inspectable.GetAddressOf());
            }
            ComPtr<XamlContentControl> button_content;
            ComPtr<XamlControl> button_control;
            ComPtr<FrameworkElement> button_element;
            ComPtr<UiElement> button_ui_element;
            ComPtr<XamlButtonBase> button_base;
            if (FAILED(result) ||
                FAILED(result = button_inspectable.As(&button_content)) ||
                FAILED(result = button_inspectable.As(&button_control)) ||
                FAILED(result = button_inspectable.As(&button_element)) ||
                FAILED(result = button_inspectable.As(&button_ui_element)) ||
                FAILED(result = button_inspectable.As(&button_base)) ||
                FAILED(result = button_content->put_Content(
                           row_inspectable.Get())) ||
                FAILED(result = button_control->put_Background(
                           transparent.Get())) ||
                FAILED(result = button_control->put_Foreground(
                           foreground.Get())) ||
                FAILED(result = button_control->put_Padding(
                           XamlThickness{})) ||
                FAILED(result = button_control
                                    ->put_HorizontalContentAlignment(
                                        ABI::Windows::UI::Xaml::
                                            HorizontalAlignment_Left)) ||
                FAILED(result = button_element->put_Width(252.0)) ||
                FAILED(result = button_element->put_Height(38.0)) ||
                FAILED(result = button_element->put_HorizontalAlignment(
                           ABI::Windows::UI::Xaml::
                               HorizontalAlignment_Left)) ||
                FAILED(result = button_ui_element->put_IsHitTestVisible(
                           true))) {
                static_cast<void>(CleanupStartMenuAllApps(captured));
                delete captured;
                return result;
            }

            auto launch_handler = Make<StartMenuAppLaunchHandler>();
            ComPtr<ABI::Windows::UI::Xaml::IRoutedEventHandler>
                routed_handler;
            EventRegistrationToken token{};
            if (launch_handler == nullptr ||
                FAILED(result = launch_handler->Initialize(
                           app.parsing_name)) ||
                FAILED(result = launch_handler.As(&routed_handler)) ||
                FAILED(result = button_base->add_Click(
                           routed_handler.Get(),
                           &token))) {
                static_cast<void>(CleanupStartMenuAllApps(captured));
                delete captured;
                return launch_handler == nullptr ? E_OUTOFMEMORY : result;
            }
            captured->subscriptions.push_back(
                {button_base, routed_handler, token});
            result = stack_children->Append(button_ui_element.Get());
            if (FAILED(result)) {
                static_cast<void>(CleanupStartMenuAllApps(captured));
                delete captured;
                return result;
            }
        }

        if (apps.empty()) {
            ComPtr<IInspectable> empty_inspectable;
            ComPtr<UiElement> empty_element;
            result = CreateStartMenuTextBlock(
                L"Приложения не найдены",
                14.0,
                0.78,
                XamlThickness{8.0, 12.0, 0.0, 0.0},
                foreground.Get(),
                empty_inspectable,
                empty_element);
            if (FAILED(result) ||
                FAILED(result = stack_children->Append(
                           empty_element.Get()))) {
                static_cast<void>(CleanupStartMenuAllApps(captured));
                delete captured;
                return result;
            }
        }

        result = captured->destination_children->Append(
            captured->scroll_viewer_element.Get());
        if (FAILED(result)) {
            static_cast<void>(CleanupStartMenuAllApps(captured));
            delete captured;
            return result;
        }
        snapshot = reinterpret_cast<std::uint64_t>(captured);
        return S_OK;
    }

    [[nodiscard]] HRESULT UpdateStartMenuAllAppsVisibility(
        const std::uint64_t snapshot,
        const bool visible) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        const auto* captured = reinterpret_cast<const StartMenuAllAppsSnapshot*>(
            snapshot);
        if (captured->restored || captured->scroll_viewer_element == nullptr) {
            return E_UNEXPECTED;
        }
        return captured->scroll_viewer_element->put_Visibility(
            visible
                ? ABI::Windows::UI::Xaml::Visibility_Visible
                : ABI::Windows::UI::Xaml::Visibility_Collapsed);
    }

    [[nodiscard]] HRESULT RestoreStartMenuAllApps(
        const std::uint64_t snapshot) noexcept override {
        if (snapshot == 0) {
            return E_INVALIDARG;
        }
        return CleanupStartMenuAllApps(
            reinterpret_cast<StartMenuAllAppsSnapshot*>(snapshot));
    }

    void ReleaseStartMenuAllAppsSnapshot(
        const std::uint64_t snapshot) noexcept override {
        auto* captured =
            reinterpret_cast<StartMenuAllAppsSnapshot*>(snapshot);
        if (captured != nullptr) {
            static_cast<void>(CleanupStartMenuAllApps(captured));
            delete captured;
        }
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
    [[nodiscard]] static std::optional<XamlHorizontalAlignment>
    ToXamlHorizontalAlignment(
        const ShellHorizontalAlignment alignment) noexcept {
        switch (alignment) {
            case ShellHorizontalAlignment::left:
                return ABI::Windows::UI::Xaml::HorizontalAlignment_Left;
            case ShellHorizontalAlignment::center:
                return ABI::Windows::UI::Xaml::HorizontalAlignment_Center;
            case ShellHorizontalAlignment::right:
                return ABI::Windows::UI::Xaml::HorizontalAlignment_Right;
            case ShellHorizontalAlignment::stretch:
                return ABI::Windows::UI::Xaml::HorizontalAlignment_Stretch;
            default:
                return std::nullopt;
        }
    }

    [[nodiscard]] static std::optional<XamlVerticalAlignment>
    ToXamlVerticalAlignment(
        const ShellVerticalAlignment alignment) noexcept {
        switch (alignment) {
            case ShellVerticalAlignment::top:
                return ABI::Windows::UI::Xaml::VerticalAlignment_Top;
            case ShellVerticalAlignment::center:
                return ABI::Windows::UI::Xaml::VerticalAlignment_Center;
            case ShellVerticalAlignment::bottom:
                return ABI::Windows::UI::Xaml::VerticalAlignment_Bottom;
            case ShellVerticalAlignment::stretch:
                return ABI::Windows::UI::Xaml::VerticalAlignment_Stretch;
            default:
                return std::nullopt;
        }
    }

    [[nodiscard]] static HRESULT ReadRuntimeClassName(
        IInspectable* inspectable,
        Microsoft::WRL::Wrappers::HString& storage,
        std::wstring_view& name) noexcept {
        name = {};
        if (inspectable == nullptr) {
            return E_INVALIDARG;
        }
        const HRESULT result =
            inspectable->GetRuntimeClassName(storage.GetAddressOf());
        if (FAILED(result)) {
            return result;
        }
        UINT32 length = 0;
        const wchar_t* value =
            ::WindowsGetStringRawBuffer(storage.Get(), &length);
        if (value == nullptr && length != 0) {
            return E_UNEXPECTED;
        }
        name = std::wstring_view(value == nullptr ? L"" : value, length);
        return S_OK;
    }

    [[nodiscard]] static HRESULT CaptureFrameworkElementLayout(
        FrameworkElement* framework_element,
        ElementLayoutSnapshot& captured) noexcept {
        if (framework_element == nullptr) {
            return E_INVALIDARG;
        }
        ComPtr<UiElement> ui_element;
        ComPtr<XamlGridStatics> grid;
        HRESULT result = framework_element->QueryInterface(
            __uuidof(UiElement),
            reinterpret_cast<void**>(ui_element.GetAddressOf()));
        if (FAILED(result) || FAILED(result = GetGridStatics(grid))) {
            return result;
        }
        if (FAILED(result = framework_element->get_Width(&captured.width)) ||
            FAILED(result = framework_element->get_Height(&captured.height)) ||
            FAILED(result = framework_element->get_HorizontalAlignment(
                       &captured.horizontal_alignment)) ||
            FAILED(result = framework_element->get_VerticalAlignment(
                       &captured.vertical_alignment)) ||
            FAILED(result = framework_element->get_Margin(&captured.margin)) ||
            FAILED(result = ui_element->get_Visibility(
                       &captured.visibility)) ||
            FAILED(result = grid->GetRow(
                       framework_element,
                       &captured.grid_row)) ||
            FAILED(result = grid->GetColumn(
                       framework_element,
                       &captured.grid_column)) ||
            FAILED(result = grid->GetRowSpan(
                       framework_element,
                       &captured.grid_row_span)) ||
            FAILED(result = grid->GetColumnSpan(
                       framework_element,
                       &captured.grid_column_span))) {
            return result;
        }
        return S_OK;
    }

    [[nodiscard]] static HRESULT WriteFrameworkElementLayout(
        FrameworkElement* framework_element,
        const ShellElementLayout& layout) noexcept {
        if (framework_element == nullptr || !layout.write_size ||
            !layout.write_margin || !layout.write_alignment ||
            !std::isfinite(layout.width) || layout.width < 0.0 ||
            !std::isfinite(layout.height) || layout.height < 0.0 ||
            !std::isfinite(layout.margin.left) ||
            !std::isfinite(layout.margin.top) ||
            !std::isfinite(layout.margin.right) ||
            !std::isfinite(layout.margin.bottom)) {
            return E_INVALIDARG;
        }
        const auto horizontal =
            ToXamlHorizontalAlignment(layout.horizontal_alignment);
        const auto vertical =
            ToXamlVerticalAlignment(layout.vertical_alignment);
        if (!horizontal.has_value() || !vertical.has_value()) {
            return E_INVALIDARG;
        }
        HRESULT result = framework_element->put_Width(layout.width);
        if (FAILED(result) ||
            FAILED(result = framework_element->put_Height(layout.height)) ||
            FAILED(result = framework_element->put_HorizontalAlignment(
                       *horizontal)) ||
            FAILED(result = framework_element->put_VerticalAlignment(
                       *vertical)) ||
            FAILED(result = framework_element->put_Margin(XamlThickness{
                       layout.margin.left,
                       layout.margin.top,
                       layout.margin.right,
                       layout.margin.bottom}))) {
            return result;
        }
        return S_OK;
    }

    [[nodiscard]] static HRESULT RestoreFrameworkElementLayout(
        FrameworkElement* framework_element,
        const ElementLayoutSnapshot& captured) noexcept {
        if (framework_element == nullptr) {
            return E_INVALIDARG;
        }
        ComPtr<UiElement> ui_element;
        ComPtr<XamlGridStatics> grid;
        HRESULT result = framework_element->QueryInterface(
            __uuidof(UiElement),
            reinterpret_cast<void**>(ui_element.GetAddressOf()));
        if (FAILED(result) || FAILED(result = GetGridStatics(grid))) {
            return result;
        }
        HRESULT first_failure = S_OK;
        const auto preserve_first_failure =
            [&first_failure](const HRESULT candidate) noexcept {
                if (FAILED(candidate) && SUCCEEDED(first_failure)) {
                    first_failure = candidate;
                }
            };
        preserve_first_failure(framework_element->put_Width(captured.width));
        preserve_first_failure(framework_element->put_Height(captured.height));
        preserve_first_failure(framework_element->put_HorizontalAlignment(
            captured.horizontal_alignment));
        preserve_first_failure(framework_element->put_VerticalAlignment(
            captured.vertical_alignment));
        preserve_first_failure(
            framework_element->put_Margin(captured.margin));
        preserve_first_failure(
            ui_element->put_Visibility(captured.visibility));
        preserve_first_failure(
            grid->SetRow(framework_element, captured.grid_row));
        preserve_first_failure(
            grid->SetColumn(framework_element, captured.grid_column));
        preserve_first_failure(
            grid->SetRowSpan(framework_element, captured.grid_row_span));
        preserve_first_failure(
            grid->SetColumnSpan(framework_element, captured.grid_column_span));
        return first_failure;
    }

    [[nodiscard]] static HRESULT CleanupStartMenuFrameEnvelope(
        StartMenuFrameEnvelopeSnapshot* captured) noexcept {
        if (captured == nullptr) {
            return E_INVALIDARG;
        }
        if (captured->restored) {
            return S_OK;
        }
        HRESULT first_failure = S_OK;
        for (std::size_t index = 0; index < captured->elements.size(); ++index) {
            const HRESULT restore_result = RestoreFrameworkElementLayout(
                captured->elements[index].Get(),
                captured->layouts[index]);
            if (FAILED(restore_result) && SUCCEEDED(first_failure)) {
                first_failure = restore_result;
            }
        }
        captured->restored = SUCCEEDED(first_failure);
        return first_failure;
    }

    [[nodiscard]] static HRESULT GetGridStatics(
        ComPtr<XamlGridStatics>& grid) noexcept {
        grid.Reset();
        Microsoft::WRL::Wrappers::HStringReference class_name(
            RuntimeClass_Windows_UI_Xaml_Controls_Grid);
        return ::RoGetActivationFactory(
            class_name.Get(),
            __uuidof(XamlGridStatics),
            reinterpret_cast<void**>(grid.GetAddressOf()));
    }

    [[nodiscard]] static HRESULT GetVisualTreeHelperStatics(
        ComPtr<XamlVisualTreeHelperStatics>& visual_tree) noexcept {
        visual_tree.Reset();
        Microsoft::WRL::Wrappers::HStringReference class_name(
            RuntimeClass_Windows_UI_Xaml_Media_VisualTreeHelper);
        return ::RoGetActivationFactory(
            class_name.Get(),
            __uuidof(XamlVisualTreeHelperStatics),
            reinterpret_cast<void**>(visual_tree.GetAddressOf()));
    }

    [[nodiscard]] static HRESULT RemoveElement(
        XamlUiElementVector* children,
        UiElement* element,
        bool& removed) noexcept {
        removed = false;
        if (children == nullptr || element == nullptr) {
            return E_INVALIDARG;
        }
        UINT32 index = 0;
        boolean found = false;
        HRESULT result = children->IndexOf(element, &index, &found);
        if (FAILED(result) || !found) {
            return result;
        }
        result = children->RemoveAt(index);
        if (SUCCEEDED(result)) {
            removed = true;
        }
        return result;
    }

    [[nodiscard]] static HRESULT CleanupStartMenuThreePanelSurface(
        StartMenuThreePanelSurfaceSnapshot* captured) noexcept {
        if (captured == nullptr) {
            return E_INVALIDARG;
        }
        if (captured->restored) {
            return S_OK;
        }
        HRESULT first_failure = S_OK;
        const auto preserve_first_failure =
            [&first_failure](const HRESULT candidate) noexcept {
                if (FAILED(candidate) && SUCCEEDED(first_failure)) {
                    first_failure = candidate;
                }
            };
        if (captured->children != nullptr) {
            for (auto iterator = captured->panel_elements.rbegin();
                 iterator != captured->panel_elements.rend();
                 ++iterator) {
                if (*iterator == nullptr) {
                    continue;
                }
                bool removed = false;
                preserve_first_failure(RemoveElement(
                    captured->children.Get(),
                    iterator->Get(),
                    removed));
            }
        }
        if (captured->acrylic_border_element != nullptr) {
            preserve_first_failure(
                captured->acrylic_border_element->put_Opacity(
                    captured->acrylic_border_opacity));
        }
        if (captured->acrylic_overlay_element != nullptr) {
            preserve_first_failure(
                captured->acrylic_overlay_element->put_Opacity(
                    captured->acrylic_overlay_opacity));
        }
        captured->restored = SUCCEEDED(first_failure);
        return first_failure;
    }

    [[nodiscard]] static HRESULT CleanupStartMenuRecommended(
        StartMenuRecommendedSnapshot* captured) noexcept {
        if (captured == nullptr) {
            return E_INVALIDARG;
        }
        if (captured->restored) {
            return S_OK;
        }
        bool removed = false;
        HRESULT result = RemoveElement(
            captured->destination_children.Get(),
            captured->recommended.Get(),
            removed);
        if (FAILED(result)) {
            return result;
        }
        if (captured->original_is_content) {
            result = captured->original_content_parent->put_Content(
                captured->original_content.Get());
        } else {
            UINT32 size = 0;
            if (FAILED(result = captured->original_children->get_Size(&size))) {
                if (removed) {
                    static_cast<void>(captured->destination_children->Append(
                        captured->recommended.Get()));
                }
                return result;
            }
            if (captured->original_index > size) {
                result = E_BOUNDS;
            } else {
                result = captured->original_children->InsertAt(
                    captured->original_index,
                    captured->recommended.Get());
            }
        }
        if (FAILED(result)) {
            if (removed) {
                static_cast<void>(captured->destination_children->Append(
                    captured->recommended.Get()));
            }
            return result;
        }
        captured->restored = true;
        return S_OK;
    }

    [[nodiscard]] static HRESULT CleanupStartMenuAllApps(
        StartMenuAllAppsSnapshot* captured) noexcept {
        if (captured == nullptr) {
            return E_INVALIDARG;
        }
        if (captured->restored) {
            return S_OK;
        }
        for (auto iterator = captured->subscriptions.rbegin();
             iterator != captured->subscriptions.rend();
             ++iterator) {
            if (iterator->button != nullptr) {
                static_cast<void>(
                    iterator->button->remove_Click(iterator->token));
            }
        }
        captured->subscriptions.clear();
        bool removed = false;
        const HRESULT result = RemoveElement(
            captured->destination_children.Get(),
            captured->scroll_viewer_element.Get(),
            removed);
        if (FAILED(result)) {
            return result;
        }
        captured->restored = true;
        return S_OK;
    }

    [[nodiscard]] static HRESULT EnumerateStartMenuApps(
        std::vector<StartMenuAppEntry>& apps) noexcept {
        apps.clear();
        ComPtr<IShellItem> apps_folder;
        HRESULT result = ::SHCreateItemFromParsingName(
            L"shell:AppsFolder",
            nullptr,
            __uuidof(IShellItem),
            reinterpret_cast<void**>(apps_folder.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }
        ComPtr<IEnumShellItems> enumerator;
        result = apps_folder->BindToHandler(
            nullptr,
            BHID_EnumItems,
            __uuidof(IEnumShellItems),
            reinterpret_cast<void**>(enumerator.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        constexpr std::size_t kMaximumEnumeratedApps = 2048;
        for (;;) {
            ComPtr<IShellItem> item;
            ULONG fetched = 0;
            result = enumerator->Next(
                1,
                item.GetAddressOf(),
                &fetched);
            if (result == S_FALSE || fetched == 0) {
                break;
            }
            if (FAILED(result)) {
                return result;
            }
            PWSTR display_name = nullptr;
            PWSTR parsing_name = nullptr;
            const HRESULT display_result = item->GetDisplayName(
                SIGDN_NORMALDISPLAY,
                &display_name);
            const HRESULT parsing_result = item->GetDisplayName(
                SIGDN_DESKTOPABSOLUTEPARSING,
                &parsing_name);
            if (SUCCEEDED(display_result) && SUCCEEDED(parsing_result) &&
                display_name != nullptr && display_name[0] != L'\0' &&
                parsing_name != nullptr && parsing_name[0] != L'\0') {
                try {
                    apps.push_back({display_name, parsing_name, item});
                } catch (const std::bad_alloc&) {
                    ::CoTaskMemFree(display_name);
                    ::CoTaskMemFree(parsing_name);
                    return E_OUTOFMEMORY;
                }
            }
            ::CoTaskMemFree(display_name);
            ::CoTaskMemFree(parsing_name);
            if (apps.size() >= kMaximumEnumeratedApps) {
                break;
            }
        }

        const auto compare_ordinal_ignore_case = [](
                                                   const std::wstring& left,
                                                   const std::wstring& right) {
            const int result = ::CompareStringOrdinal(
                left.c_str(),
                static_cast<int>(left.size()),
                right.c_str(),
                static_cast<int>(right.size()),
                true);
            return result == CSTR_LESS_THAN;
        };
        std::sort(
            apps.begin(),
            apps.end(),
            [&compare_ordinal_ignore_case](
                const StartMenuAppEntry& left,
                const StartMenuAppEntry& right) {
                const int localized_result = ::CompareStringEx(
                    LOCALE_NAME_USER_DEFAULT,
                    NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                    left.display_name.c_str(),
                    static_cast<int>(left.display_name.size()),
                    right.display_name.c_str(),
                    static_cast<int>(right.display_name.size()),
                    nullptr,
                    nullptr,
                    0);
                if (localized_result != CSTR_EQUAL) {
                    return localized_result == CSTR_LESS_THAN;
                }
                return compare_ordinal_ignore_case(
                    left.parsing_name,
                    right.parsing_name);
            });
        apps.erase(
            std::unique(
                apps.begin(),
                apps.end(),
                [](const StartMenuAppEntry& left,
                   const StartMenuAppEntry& right) {
                    return ::CompareStringOrdinal(
                               left.parsing_name.c_str(),
                               static_cast<int>(left.parsing_name.size()),
                               right.parsing_name.c_str(),
                               static_cast<int>(right.parsing_name.size()),
                               true) == CSTR_EQUAL;
                }),
            apps.end());
        return S_OK;
    }

    [[nodiscard]] static wchar_t StartMenuAppGroup(
        const std::wstring& display_name) noexcept {
        for (const wchar_t character : display_name) {
            WORD character_type = 0;
            if (::GetStringTypeW(
                    CT_CTYPE1,
                    &character,
                    1,
                    &character_type) &&
                (character_type & C1_ALPHA) != 0) {
                wchar_t uppercase[2]{character, L'\0'};
                if (::LCMapStringEx(
                        LOCALE_NAME_USER_DEFAULT,
                        LCMAP_UPPERCASE,
                        &character,
                        1,
                        uppercase,
                        2,
                        nullptr,
                        nullptr,
                        0) > 0) {
                    return uppercase[0];
                }
                return character;
            }
            if ((character_type & C1_DIGIT) != 0) {
                return L'#';
            }
            if (!std::iswspace(character)) {
                return L'#';
            }
        }
        return L'#';
    }

    [[nodiscard]] static HRESULT ReadStartMenuAppIconPixels(
        IShellItem* shell_item,
        IWICImagingFactory* imaging_factory,
        std::array<std::uint8_t, kStartMenuAppIconByteCount>& pixels) noexcept {
        pixels.fill(0);
        if (shell_item == nullptr || imaging_factory == nullptr) {
            return E_INVALIDARG;
        }

        PIDLIST_ABSOLUTE item_id_list = nullptr;
        HRESULT result = ::SHGetIDListFromObject(
            shell_item,
            &item_id_list);
        if (FAILED(result)) {
            return result;
        }
        if (item_id_list == nullptr) {
            return E_FAIL;
        }
        const auto release_item_id_list = [&item_id_list]() noexcept {
            if (item_id_list != nullptr) {
                ::CoTaskMemFree(item_id_list);
                item_id_list = nullptr;
            }
        };

        SHFILEINFOW file_information{};
        const DWORD_PTR image_result = ::SHGetFileInfoW(
            reinterpret_cast<LPCWSTR>(item_id_list),
            0,
            &file_information,
            sizeof(file_information),
            SHGFI_PIDL | SHGFI_ICON | SHGFI_LARGEICON);
        release_item_id_list();
        if (image_result == 0 || file_information.hIcon == nullptr) {
            return E_FAIL;
        }
        HICON raw_icon = file_information.hIcon;
        const auto release_icon = [&raw_icon]() noexcept {
            if (raw_icon != nullptr) {
                static_cast<void>(::DestroyIcon(raw_icon));
                raw_icon = nullptr;
            }
        };

        ComPtr<IWICBitmap> source;
        result = imaging_factory->CreateBitmapFromHICON(
            raw_icon,
            source.GetAddressOf());
        if (FAILED(result)) {
            release_icon();
            return result;
        }
        UINT source_width = 0;
        UINT source_height = 0;
        result = source->GetSize(&source_width, &source_height);
        if (FAILED(result) || source_width == 0 || source_height == 0) {
            release_icon();
            return FAILED(result) ? result : E_FAIL;
        }
        constexpr UINT kMaximumShellIconDimension = 512;
        if (source_width > kMaximumShellIconDimension ||
            source_height > kMaximumShellIconDimension) {
            release_icon();
            return HRESULT_FROM_WIN32(ERROR_BAD_LENGTH);
        }

        const double scale = (std::min)(
            static_cast<double>(kStartMenuAppIconSize) /
                static_cast<double>(source_width),
            static_cast<double>(kStartMenuAppIconSize) /
                static_cast<double>(source_height));
        const UINT destination_width = (std::max)(
            1U,
            static_cast<UINT>(std::lround(
                static_cast<double>(source_width) * scale)));
        const UINT destination_height = (std::max)(
            1U,
            static_cast<UINT>(std::lround(
                static_cast<double>(source_height) * scale)));

        ComPtr<IWICBitmapSource> render_source;
        if (source_width == destination_width &&
            source_height == destination_height) {
            result = source.As(&render_source);
        } else {
            ComPtr<IWICBitmapScaler> scaler;
            result = imaging_factory->CreateBitmapScaler(
                scaler.GetAddressOf());
            if (SUCCEEDED(result)) {
                result = scaler->Initialize(
                    source.Get(),
                    destination_width,
                    destination_height,
                    WICBitmapInterpolationModeFant);
            }
            if (SUCCEEDED(result)) {
                result = scaler.As(&render_source);
            }
        }
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(result)) {
            result = imaging_factory->CreateFormatConverter(
                converter.GetAddressOf());
        }
        if (SUCCEEDED(result)) {
            result = converter->Initialize(
                render_source.Get(),
                // WriteableBitmap's BGRA8 pixel buffer is interpreted as
                // premultiplied alpha. Supplying straight-alpha BGRA makes
                // translucent antialiasing pixels appear over-bright and
                // visually thickens detailed icons.
                GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
        }
        if (FAILED(result)) {
            release_icon();
            return result;
        }

        const UINT destination_stride =
            static_cast<UINT>(kStartMenuAppIconSize) * 4U;
        const UINT left =
            (static_cast<UINT>(kStartMenuAppIconSize) -
             destination_width) /
            2U;
        const UINT top =
            (static_cast<UINT>(kStartMenuAppIconSize) -
             destination_height) /
            2U;
        const std::size_t destination_offset =
            static_cast<std::size_t>(top) * destination_stride +
            static_cast<std::size_t>(left) * 4U;
        result = converter->CopyPixels(
            nullptr,
            destination_stride,
            static_cast<UINT>(pixels.size() - destination_offset),
            pixels.data() + destination_offset);
        release_icon();
        if (FAILED(result)) {
            return result;
        }
        return S_OK;
    }

    [[nodiscard]] static HRESULT CreateStartMenuAppIcon(
        const StartMenuAppEntry& app,
        IWICImagingFactory* imaging_factory,
        ComPtr<IInspectable>& inspectable,
        ComPtr<UiElement>& ui_element) noexcept {
        inspectable.Reset();
        ui_element.Reset();
        std::array<std::uint8_t, kStartMenuAppIconByteCount> pixels{};
        HRESULT result = ReadStartMenuAppIconPixels(
            app.shell_item.Get(),
            imaging_factory,
            pixels);
        if (FAILED(result)) {
            return result;
        }

        Microsoft::WRL::Wrappers::HStringReference bitmap_class(
            RuntimeClass_Windows_UI_Xaml_Media_Imaging_WriteableBitmap);
        ComPtr<XamlWriteableBitmapFactory> bitmap_factory;
        result = ::RoGetActivationFactory(
            bitmap_class.Get(),
            __uuidof(XamlWriteableBitmapFactory),
            reinterpret_cast<void**>(bitmap_factory.GetAddressOf()));
        ComPtr<XamlWriteableBitmap> bitmap;
        if (SUCCEEDED(result)) {
            result = bitmap_factory->CreateInstanceWithDimensions(
                kStartMenuAppIconSize,
                kStartMenuAppIconSize,
                bitmap.GetAddressOf());
        }
        ComPtr<WinRtBuffer> pixel_buffer;
        if (SUCCEEDED(result)) {
            result = bitmap->get_PixelBuffer(pixel_buffer.GetAddressOf());
        }
        UINT32 capacity = 0;
        if (SUCCEEDED(result)) {
            result = pixel_buffer->get_Capacity(&capacity);
        }
        if (SUCCEEDED(result) && capacity < pixels.size()) {
            result = HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        }
        if (SUCCEEDED(result)) {
            result = pixel_buffer->put_Length(
                static_cast<UINT32>(pixels.size()));
        }
        ComPtr<Windows::Storage::Streams::IBufferByteAccess> byte_access;
        if (SUCCEEDED(result)) {
            result = pixel_buffer.As(&byte_access);
        }
        byte* destination = nullptr;
        if (SUCCEEDED(result)) {
            result = byte_access->Buffer(&destination);
        }
        if (SUCCEEDED(result) && destination == nullptr) {
            result = E_POINTER;
        }
        if (SUCCEEDED(result)) {
            std::copy(pixels.begin(), pixels.end(), destination);
            result = bitmap->Invalidate();
        }
        ComPtr<XamlImageSource> image_source;
        if (SUCCEEDED(result)) {
            result = bitmap.As(&image_source);
        }

        Microsoft::WRL::Wrappers::HStringReference image_class(
            RuntimeClass_Windows_UI_Xaml_Controls_Image);
        if (SUCCEEDED(result)) {
            result = ::RoActivateInstance(
                image_class.Get(),
                inspectable.GetAddressOf());
        }
        ComPtr<XamlImage> image;
        ComPtr<FrameworkElement> image_element;
        if (SUCCEEDED(result)) {
            result = inspectable.As(&image);
        }
        if (SUCCEEDED(result)) {
            result = inspectable.As(&image_element);
        }
        if (SUCCEEDED(result)) {
            result = inspectable.As(&ui_element);
        }
        if (SUCCEEDED(result)) {
            result = image->put_Source(image_source.Get());
        }
        if (SUCCEEDED(result)) {
            result = image->put_Stretch(
                ABI::Windows::UI::Xaml::Media::Stretch_Uniform);
        }
        if (SUCCEEDED(result)) {
            result = image_element->put_Width(
                static_cast<double>(kStartMenuAppIconSize));
        }
        if (SUCCEEDED(result)) {
            result = image_element->put_Height(
                static_cast<double>(kStartMenuAppIconSize));
        }
        if (SUCCEEDED(result)) {
            result = image_element->put_Margin(
                XamlThickness{8.0, 0.0, 10.0, 0.0});
        }
        if (SUCCEEDED(result)) {
            result = image_element->put_VerticalAlignment(
                ABI::Windows::UI::Xaml::VerticalAlignment_Center);
        }
        if (SUCCEEDED(result)) {
            result = ui_element->put_IsHitTestVisible(false);
        }
        if (FAILED(result)) {
            inspectable.Reset();
            ui_element.Reset();
        }
        return result;
    }

    [[nodiscard]] static HRESULT CreateStartMenuFallbackAppIcon(
        const std::wstring& display_name,
        XamlBrush* foreground,
        ComPtr<IInspectable>& inspectable,
        ComPtr<UiElement>& ui_element) noexcept {
        inspectable.Reset();
        ui_element.Reset();
        if (display_name.empty() || foreground == nullptr) {
            return E_INVALIDARG;
        }

        ComPtr<XamlBrush> background;
        HRESULT result = CreateSolidColorBrush(0x32FFFFFFU, background);
        Microsoft::WRL::Wrappers::HStringReference border_class(
            RuntimeClass_Windows_UI_Xaml_Controls_Border);
        if (SUCCEEDED(result)) {
            result = ::RoActivateInstance(
                border_class.Get(),
                inspectable.GetAddressOf());
        }
        ComPtr<XamlBorder> border;
        ComPtr<FrameworkElement> border_element;
        if (SUCCEEDED(result)) {
            result = inspectable.As(&border);
        }
        if (SUCCEEDED(result)) {
            result = inspectable.As(&border_element);
        }
        if (SUCCEEDED(result)) {
            result = inspectable.As(&ui_element);
        }

        const wchar_t initial_value[2]{
            StartMenuAppGroup(display_name),
            L'\0'};
        ComPtr<IInspectable> initial_inspectable;
        ComPtr<UiElement> initial_element;
        if (SUCCEEDED(result)) {
            result = CreateStartMenuTextBlock(
                std::wstring_view(initial_value, 1),
                11.0,
                1.0,
                XamlThickness{},
                foreground,
                initial_inspectable,
                initial_element);
        }
        ComPtr<FrameworkElement> initial_framework_element;
        if (SUCCEEDED(result)) {
            result = initial_inspectable.As(&initial_framework_element);
        }
        if (SUCCEEDED(result)) {
            result = initial_framework_element->put_HorizontalAlignment(
                ABI::Windows::UI::Xaml::HorizontalAlignment_Center);
        }
        if (SUCCEEDED(result)) {
            result = initial_framework_element->put_VerticalAlignment(
                ABI::Windows::UI::Xaml::VerticalAlignment_Center);
        }
        if (SUCCEEDED(result)) {
            result = border->put_Background(background.Get());
        }
        if (SUCCEEDED(result)) {
            result = border->put_CornerRadius(
                XamlCornerRadius{5.0, 5.0, 5.0, 5.0});
        }
        if (SUCCEEDED(result)) {
            result = border->put_Child(initial_element.Get());
        }
        if (SUCCEEDED(result)) {
            result = border_element->put_Width(
                static_cast<double>(kStartMenuAppIconSize));
        }
        if (SUCCEEDED(result)) {
            result = border_element->put_Height(
                static_cast<double>(kStartMenuAppIconSize));
        }
        if (SUCCEEDED(result)) {
            result = border_element->put_Margin(
                XamlThickness{8.0, 0.0, 10.0, 0.0});
        }
        if (SUCCEEDED(result)) {
            result = border_element->put_VerticalAlignment(
                ABI::Windows::UI::Xaml::VerticalAlignment_Center);
        }
        if (SUCCEEDED(result)) {
            result = ui_element->put_IsHitTestVisible(false);
        }
        if (FAILED(result)) {
            inspectable.Reset();
            ui_element.Reset();
        }
        return result;
    }

    [[nodiscard]] static HRESULT CreateStartMenuTextBlock(
        const std::wstring_view text,
        const double font_size,
        const double opacity,
        const XamlThickness margin,
        XamlBrush* foreground,
        ComPtr<IInspectable>& inspectable,
        ComPtr<UiElement>& ui_element) noexcept {
        inspectable.Reset();
        ui_element.Reset();
        if (text.empty() || foreground == nullptr ||
            !std::isfinite(font_size) || font_size <= 0.0 ||
            !std::isfinite(opacity) || opacity < 0.0 || opacity > 1.0) {
            return E_INVALIDARG;
        }
        Microsoft::WRL::Wrappers::HStringReference text_class(
            RuntimeClass_Windows_UI_Xaml_Controls_TextBlock);
        HRESULT result = ::RoActivateInstance(
            text_class.Get(),
            inspectable.GetAddressOf());
        ComPtr<XamlTextBlock> text_block;
        ComPtr<FrameworkElement> framework_element;
        Microsoft::WRL::Wrappers::HString value;
        if (FAILED(result = value.Set(
                       text.data(),
                       static_cast<UINT32>(text.size())))) {
            inspectable.Reset();
            ui_element.Reset();
            return result;
        }
        if (FAILED(result) ||
            FAILED(result = inspectable.As(&text_block)) ||
            FAILED(result = inspectable.As(&framework_element)) ||
            FAILED(result = inspectable.As(&ui_element)) ||
            FAILED(result = text_block->put_Text(value.Get())) ||
            FAILED(result = text_block->put_FontSize(font_size)) ||
            FAILED(result = text_block->put_Foreground(foreground)) ||
            FAILED(result = framework_element->put_Margin(margin)) ||
            FAILED(result = ui_element->put_Opacity(opacity)) ||
            FAILED(result = ui_element->put_IsHitTestVisible(false))) {
            inspectable.Reset();
            ui_element.Reset();
            return result;
        }
        return S_OK;
    }

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
            settings.start_menu_background_color,
            settings.start_menu_three_panel_layout_enabled,
            settings.start_menu_hide_all_apps);
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
        style_.CopyRuntimeDiagnostics(copy);
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
            const HRESULT timeout = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            InitialSubscriptionState expected =
                InitialSubscriptionState::waiting_for_site;
            if (state == InitialSubscriptionState::waiting_for_site &&
                g_initial_subscription_state.compare_exchange_strong(
                    expected,
                    InitialSubscriptionState::idle,
                    std::memory_order_acq_rel)) {
                g_initial_subscription_result.store(
                    timeout,
                    std::memory_order_release);
                ::AcquireSRWLockExclusive(&g_initialization_lock);
                const bool retry_allowed =
                    g_desired_target.load(std::memory_order_acquire) ==
                    protocol::AgentTarget::start_menu;
                const bool retry_available =
                    g_diagnostics_initialization.RecordSiteTimeout(
                        retry_allowed);
                ::ReleaseSRWLockExclusive(&g_initialization_lock);
                g_diagnostic_stage.store(
                    retry_available
                        ? protocol::AgentDiagnosticStage::
                              retry_xaml_diagnostics
                        : protocol::AgentDiagnosticStage::wait_for_tap_site,
                    std::memory_order_release);
            }
            return timeout;
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
    if (g_diagnostics_initialization.reuse_result()) {
        const HRESULT result = g_diagnostics_initialization.result();
        ::ReleaseSRWLockExclusive(&g_initialization_lock);
        return result;
    }

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
                const auto endpoint =
                    g_diagnostics_initialization.BeginAttempt();
                if (!endpoint.has_value()) {
                    result = g_diagnostics_initialization.result();
                } else {
                    g_diagnostics_initialization_attempts.store(
                        static_cast<std::uint32_t>(
                            g_diagnostics_initialization.attempt_count()),
                        std::memory_order_release);
                    if (g_diagnostics_initialization.attempt_count() > 1) {
                        g_diagnostic_stage.store(
                            protocol::AgentDiagnosticStage::
                                retry_xaml_diagnostics,
                            std::memory_order_release);
                    }
                    const DWORD process_id = ::GetCurrentProcessId();
                    result = PrepareInitialVisualTreeSubscription();
                    if (SUCCEEDED(result)) {
                        result = initialize(
                            endpoint->data(),
                            process_id,
                            nullptr,
                            module_path.data(),
                            kStartMenuTapClsid,
                            nullptr);
                        g_diagnostics_initialization.RecordApiResult(
                            result,
                            g_desired_target.load(
                                std::memory_order_acquire) ==
                                protocol::AgentTarget::start_menu);
                        if (SUCCEEDED(result) && !IsControllerAttached()) {
                            g_diagnostic_stage.store(
                                protocol::AgentDiagnosticStage::
                                    wait_for_tap_site,
                                std::memory_order_release);
                        }
                    } else {
                        g_diagnostics_initialization.RecordApiResult(
                            result,
                            g_desired_target.load(
                                std::memory_order_acquire) ==
                                protocol::AgentTarget::start_menu);
                    }
                }
            }
        }
    }

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
    g_desired_start_menu_three_panel_layout_enabled.store(
        settings.start_menu_three_panel_layout_enabled,
        std::memory_order_release);
    g_desired_start_menu_hide_all_apps.store(
        settings.start_menu_hide_all_apps,
        std::memory_order_release);
    g_desired_enabled.store(enabled, std::memory_order_release);

    HRESULT result = ConfigureAttachedController(
        target,
        enabled,
        settings,
        !enabled);
    if (result == S_FALSE && enabled) {
        const HRESULT initialization_result = InitializeDiagnosticsAdapter();
        if (FAILED(initialization_result)) {
            InitialSubscriptionState expected =
                InitialSubscriptionState::waiting_for_site;
            static_cast<void>(
                g_initial_subscription_state.compare_exchange_strong(
                    expected,
                    InitialSubscriptionState::idle,
                    std::memory_order_acq_rel));
            result = initialization_result;
        } else {
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

void PreserveShellXamlFailureDiagnostics(
    const std::uint32_t native_error,
    const protocol::AgentDiagnosticStage stage) noexcept {
    g_last_adapter_error.store(native_error, std::memory_order_release);
    g_diagnostic_stage.store(stage, std::memory_order_release);
}

std::uint32_t StartMenuXamlControllerState() noexcept {
    constexpr std::uint32_t service_present = 1U << 0U;
    constexpr std::uint32_t watcher_present = 1U << 1U;
    constexpr std::uint32_t advised = 1U << 2U;
    constexpr std::uint32_t initialization_attempt_shift = 4U;
    constexpr std::uint32_t initialization_attempt_mask = 0xFU;
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
    state |= (g_diagnostics_initialization_attempts.load(
                  std::memory_order_acquire) &
              initialization_attempt_mask)
             << initialization_attempt_shift;
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
