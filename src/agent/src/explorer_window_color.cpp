#include "metaplasia/agent/explorer_window_color.hpp"

#include "metaplasia/agent/explorer_winui_adapter.hpp"

#include <MinHook.h>
#include <UIAutomation.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <intrin.h>
#include <uxtheme.h>
#include <vsstyle.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace metaplasia::agent {
namespace {

using DwmSetWindowAttributeFunction = HRESULT(WINAPI*)(
    HWND, DWORD, LPCVOID, DWORD);
using DwmExtendFrameIntoClientAreaFunction = HRESULT(WINAPI*)(
    HWND, const MARGINS*);
using FillRectFunction = int(WINAPI*)(HDC, const RECT*, HBRUSH);
using DrawThemeBackgroundFunction = HRESULT(WINAPI*)(
    HTHEME, HDC, int, int, const RECT*, const RECT*);
using DrawThemeBackgroundExFunction = HRESULT(WINAPI*)(
    HTHEME, HDC, int, int, const RECT*, const DTBGOPTS*);

struct AccentPolicy final {
    int state;
    int flags;
    std::uint32_t color;
    int animation_id;
};

struct WindowCompositionAttributeData final {
    int attribute;
    void* data;
    ULONG data_size;
};

using SetWindowCompositionAttributeFunction = BOOL(WINAPI*)(
    HWND, WindowCompositionAttributeData*);

constexpr DWORD kDwmCaptionColor = DWMWA_CAPTION_COLOR;
constexpr DWORD kDwmTextColor = DWMWA_TEXT_COLOR;
constexpr COLORREF kDwmDefaultColor = DWMWA_COLOR_DEFAULT;
constexpr int kWindowCompositionAttributeAccentPolicy = 19;
constexpr int kAccentDisabled = 0;
constexpr int kAccentEnableGradient = 1;
constexpr std::size_t kMaximumTrackedWindows = 256;
constexpr std::size_t kMaximumTrackedTreeViews = 512;
constexpr std::size_t kMaximumExplorerUiThreads = 64;
constexpr std::size_t kMaximumDirectUiScrollbarMetrics = 128;
constexpr DWORD kTreeViewMessageTimeoutMilliseconds = 1000;
constexpr DWORD kDirectUiEventMonitorStartMilliseconds = 2000;
constexpr DWORD kDirectUiEventMonitorStopMilliseconds = 5000;
constexpr UINT_PTR kExplorerTreeViewSubclassId = 0x4D545256U;  // "MTRV"
constexpr UINT_PTR kExplorerScrollbarOwnerSubclassId = 0x4D534F57U;  // "MSOW"
constexpr UINT_PTR kTreeViewScrollbarPaintTimerId = 0x4D545350U;  // "MTSP"
constexpr UINT kTreeViewScrollbarFrameMilliseconds = 16;
constexpr UINT_PTR kExplorerDirectUiSubclassId = 0x4D445549U;  // "MDUI"
constexpr UINT_PTR kDirectUiTransitionTimerId = 0x4D445554U;  // "MDUT"
constexpr UINT kDirectUiTransitionStartMessage = WM_APP + 0x4D5U;
constexpr UINT kDirectUiScrollbarRefreshThreadMessage = WM_APP + 0x4D6U;
constexpr UINT_PTR kDirectUiScrollbarPaintTimerId = 0x4D445350U;  // "MDSP"
constexpr DWORD kDirectUiTransitionHoldMilliseconds = 80;
constexpr DWORD kDirectUiTransitionFadeMilliseconds = 150;
constexpr UINT kDirectUiTransitionFrameMilliseconds = 16;
constexpr int kDirectUiTransitionSlidePixels = 48;
constexpr int kDirectUiTransitionScaleInsetPixels = 16;
constexpr wchar_t kDirectUiOverlayClass[] =
    L"Metaplasia.Explorer.DirectUI.TransitionOverlay";
constexpr wchar_t kDirectUiOverlayProperty[] =
    L"Metaplasia.Explorer.DirectUI.TransitionOverlay.Window";
constexpr wchar_t kTreeViewScrollbarOverlayClass[] =
    L"Metaplasia.Explorer.TreeView.ScrollbarOverlay";
constexpr wchar_t kTreeViewScrollbarOverlayProperty[] =
    L"Metaplasia.Explorer.TreeView.ScrollbarOverlay.Window";
constexpr wchar_t kDirectUiScrollbarOverlayClass[] =
    L"Metaplasia.Explorer.DirectUI.ScrollbarOverlay";
constexpr wchar_t kDirectUiScrollbarOverlayProperty[] =
    L"Metaplasia.Explorer.DirectUI.ScrollbarOverlay.Window";
constexpr wchar_t kDirectUiScrollbarRefreshPendingProperty[] =
    L"Metaplasia.Explorer.DirectUI.ScrollbarRefresh.Pending";

struct TrackedWindow final {
    HWND window{nullptr};
};

struct TrackedTreeView final {
    HWND window{nullptr};
    DWORD thread_id{0};
    COLORREF original_background{CLR_NONE};
    COLORREF original_text{CLR_NONE};
};

struct ExplorerUiThreadHook final {
    DWORD thread_id{0};
    HHOOK call_wnd_proc_hook{nullptr};
    HHOOK cbt_hook{nullptr};
};

struct DirectUiScrollbarMetrics final {
    HWND window{nullptr};
    std::uint16_t scroll_percent{0};
    std::uint16_t view_percent{0};
};

struct DirectUiTransitionState final {
    DWORD started_at{0};
    RECT bounds{};
    std::uint32_t animation{protocol::kDefaultExplorerTransition};
};

SRWLOCK g_windows_lock = SRWLOCK_INIT;
std::array<TrackedWindow, kMaximumTrackedWindows> g_windows{};
std::size_t g_window_count = 0;

SRWLOCK g_tree_views_lock = SRWLOCK_INIT;
std::array<TrackedTreeView, kMaximumTrackedTreeViews> g_tree_views{};
std::size_t g_tree_view_count = 0;

SRWLOCK g_hooks_lock = SRWLOCK_INIT;
DwmSetWindowAttributeFunction g_original_dwm_set_window_attribute = nullptr;
DwmExtendFrameIntoClientAreaFunction g_original_dwm_extend_frame = nullptr;
FillRectFunction g_original_fill_rect = nullptr;
DrawThemeBackgroundFunction g_original_draw_theme_background = nullptr;
DrawThemeBackgroundExFunction g_original_draw_theme_background_ex = nullptr;
SetWindowCompositionAttributeFunction g_set_window_composition_attribute =
    nullptr;
std::uintptr_t g_dui70_begin = 0;
std::uintptr_t g_dui70_end = 0;
std::atomic<HWINEVENTHOOK> g_direct_ui_event_hook{nullptr};
std::atomic<HWINEVENTHOOK> g_direct_ui_scroll_event_hook{nullptr};
std::atomic<DWORD> g_direct_ui_event_thread_id{0};
std::atomic<bool> g_direct_ui_scroll_metrics_ready{false};
HANDLE g_direct_ui_event_thread = nullptr;
HANDLE g_direct_ui_event_stop = nullptr;
HANDLE g_direct_ui_event_ready = nullptr;
bool g_hooks_installed = false;

SRWLOCK g_explorer_ui_thread_hooks_lock = SRWLOCK_INIT;
std::array<ExplorerUiThreadHook, kMaximumExplorerUiThreads>
    g_explorer_ui_thread_hooks{};

SRWLOCK g_direct_ui_scrollbar_metrics_lock = SRWLOCK_INIT;
std::array<DirectUiScrollbarMetrics, kMaximumDirectUiScrollbarMetrics>
    g_direct_ui_scrollbar_metrics{};
std::size_t g_direct_ui_scrollbar_metrics_count = 0;

std::atomic<bool> g_enabled{false};
std::atomic<bool> g_custom_scrollbar_enabled{true};
std::atomic<std::uint32_t> g_color{
    protocol::kDefaultShellBackgroundColor};
std::atomic<std::uint32_t> g_transition_animation{
    protocol::kDefaultExplorerTransition};

void EnsureTreeViewSubclass(HWND window) noexcept;
void DestroyTreeViewScrollbarOverlay(HWND tree_view) noexcept;
void UpdateTreeViewScrollbarOverlay(HWND tree_view) noexcept;
void RequestDirectUiScrollbarMetrics(HWND direct_ui_window) noexcept;
void QueueDirectUiScrollbarMetrics(
    HWND direct_ui_window,
    std::uint16_t scroll_percent,
    std::uint16_t view_percent) noexcept;
void ApplyPendingDirectUiScrollbarMetrics(HWND explorer_window) noexcept;
void ForgetDirectUiScrollbarMetrics(HWND direct_ui_window) noexcept;
void DestroyDirectUiScrollbarOverlay(HWND direct_ui_window) noexcept;
void UpdateDirectUiScrollbarOverlay(
    HWND direct_ui_window,
    bool visible,
    std::uint16_t scroll_percent,
    std::uint16_t view_percent) noexcept;
void DrawScrollbarThumb(
    HDC device_context,
    const RECT& bounds,
    int state,
    COLORREF background,
    COLORREF foreground) noexcept;
LRESULT CALLBACK ExplorerUiThreadCbtProc(
    int code, WPARAM wparam, LPARAM lparam) noexcept;
LRESULT CALLBACK ExplorerScrollbarOwnerSubclassProc(
    HWND window,
    UINT message,
    WPARAM wparam,
    LPARAM lparam,
    UINT_PTR subclass_id,
    DWORD_PTR reference_data) noexcept;

[[nodiscard]] bool ClassNameEquals(
    const HWND window,
    const wchar_t* expected) noexcept {
    std::array<wchar_t, 64> class_name{};
    const int length = ::GetClassNameW(
        window,
        class_name.data(),
        static_cast<int>(class_name.size()));
    return length > 0 &&
           static_cast<std::size_t>(length) < class_name.size() &&
           _wcsicmp(class_name.data(), expected) == 0;
}

[[nodiscard]] bool IsCurrentProcessExplorerWindow(const HWND window) noexcept {
    if (window == nullptr || !::IsWindow(window)) {
        return false;
    }
    DWORD process_id = 0;
    ::GetWindowThreadProcessId(window, &process_id);
    return process_id == ::GetCurrentProcessId() &&
           (ClassNameEquals(window, L"CabinetWClass") ||
            ClassNameEquals(window, L"ExploreWClass"));
}

[[nodiscard]] COLORREF ToColorRef(const std::uint32_t argb) noexcept {
    return RGB(
        static_cast<BYTE>((argb >> 16U) & 0xFFU),
        static_cast<BYTE>((argb >> 8U) & 0xFFU),
        static_cast<BYTE>(argb & 0xFFU));
}

[[nodiscard]] constexpr std::uint32_t ToAccentColor(
    const std::uint32_t argb) noexcept {
    // ACCENT_POLICY uses AABBGGRR while the protocol uses AARRGGBB.
    return (argb & 0xFF000000U) |
           ((argb & 0x000000FFU) << 16U) |
           (argb & 0x0000FF00U) |
           ((argb & 0x00FF0000U) >> 16U);
}

static_assert(ToAccentColor(0xFF0B6B3AU) == 0xFF3A6B0BU);

[[nodiscard]] COLORREF ContrastingTextColor(
    const std::uint32_t argb) noexcept {
    const std::uint32_t red = (argb >> 16U) & 0xFFU;
    const std::uint32_t green = (argb >> 8U) & 0xFFU;
    const std::uint32_t blue = argb & 0xFFU;
    const std::uint32_t luminance =
        (299U * red + 587U * green + 114U * blue) / 1000U;
    return luminance >= 150U ? RGB(0, 0, 0) : RGB(255, 255, 255);
}

[[nodiscard]] HRESULT WriteColor(
    const HWND window,
    const DWORD attribute,
    const COLORREF color) noexcept {
    const auto original = g_original_dwm_set_window_attribute;
    return original != nullptr
        ? original(window, attribute, &color, sizeof(color))
        : ::DwmSetWindowAttribute(window, attribute, &color, sizeof(color));
}

[[nodiscard]] HRESULT WriteAccent(
    const HWND window,
    const int state,
    const std::uint32_t color) noexcept {
    const auto set_attribute = g_set_window_composition_attribute;
    if (set_attribute == nullptr) {
        return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    }
    AccentPolicy policy{state, 0, color, 0};
    WindowCompositionAttributeData data{
        kWindowCompositionAttributeAccentPolicy,
        &policy,
        sizeof(policy)};
    if (set_attribute(window, &data)) {
        return S_OK;
    }
    const DWORD error = ::GetLastError();
    return HRESULT_FROM_WIN32(
        error == ERROR_SUCCESS ? ERROR_GEN_FAILURE : error);
}

[[nodiscard]] HRESULT ApplyWindowSurface(
    const HWND window,
    const std::uint32_t argb) noexcept {
    const COLORREF color = ToColorRef(argb);
    const COLORREF text = ContrastingTextColor(argb);
    HRESULT result = WriteColor(window, kDwmCaptionColor, color);
    if (SUCCEEDED(result)) {
        result = WriteColor(window, kDwmTextColor, text);
    }
    if (SUCCEEDED(result)) {
        result = WriteAccent(
            window, kAccentEnableGradient, ToAccentColor(argb));
    }
    // Deliberately preserve Explorer's native frame margins and immersive
    // caption mode. Overriding either one suppresses DWM's system caption
    // glyphs on this Explorer build.
    return result;
}

[[nodiscard]] HRESULT RestoreWindowSurface(
    const TrackedWindow& tracked) noexcept {
    const HWND window = tracked.window;
    if (!IsCurrentProcessExplorerWindow(window)) {
        return S_OK;
    }
    HRESULT result = WriteAccent(window, kAccentDisabled, 0);
    const HRESULT caption = WriteColor(
        window, kDwmCaptionColor, kDwmDefaultColor);
    if (SUCCEEDED(result) && FAILED(caption)) {
        result = caption;
    }
    const HRESULT text = WriteColor(
        window, kDwmTextColor, kDwmDefaultColor);
    if (SUCCEEDED(result) && FAILED(text)) {
        result = text;
    }
    return result;
}

[[nodiscard]] bool TrackWindow(const HWND window) noexcept {
    ::AcquireSRWLockExclusive(&g_windows_lock);
    auto found = std::find_if(
        g_windows.begin(),
        g_windows.begin() + g_window_count,
        [window](const TrackedWindow& entry) {
            return entry.window == window;
        });
    if (found == g_windows.begin() + g_window_count) {
        if (g_window_count >= g_windows.size()) {
            ::ReleaseSRWLockExclusive(&g_windows_lock);
            return false;
        }
        found = g_windows.begin() + g_window_count++;
        found->window = window;
    }
    ::ReleaseSRWLockExclusive(&g_windows_lock);
    return true;
}

[[nodiscard]] HRESULT SendTreeViewMessage(
    const HWND window,
    const UINT message,
    const COLORREF color,
    COLORREF* const response = nullptr) noexcept {
    DWORD_PTR raw_response = 0;
    ::SetLastError(ERROR_SUCCESS);
    if (::SendMessageTimeoutW(
            window,
            message,
            0,
            static_cast<LPARAM>(color),
            SMTO_ABORTIFHUNG | SMTO_BLOCK,
            kTreeViewMessageTimeoutMilliseconds,
            &raw_response) == 0) {
        const DWORD error = ::GetLastError();
        return HRESULT_FROM_WIN32(
            error == ERROR_SUCCESS ? ERROR_TIMEOUT : error);
    }
    if (response != nullptr) {
        *response = static_cast<COLORREF>(raw_response);
    }
    return S_OK;
}

[[nodiscard]] bool ReadTrackedTreeView(
    const HWND window,
    const DWORD thread_id,
    TrackedTreeView* const tracked) noexcept {
    bool found = false;
    ::AcquireSRWLockShared(&g_tree_views_lock);
    const auto iterator = std::find_if(
        g_tree_views.begin(),
        g_tree_views.begin() + g_tree_view_count,
        [window, thread_id](const TrackedTreeView& entry) {
            return entry.window == window && entry.thread_id == thread_id;
        });
    if (iterator != g_tree_views.begin() + g_tree_view_count) {
        if (tracked != nullptr) {
            *tracked = *iterator;
        }
        found = true;
    }
    ::ReleaseSRWLockShared(&g_tree_views_lock);
    return found;
}

[[nodiscard]] HRESULT TrackTreeView(
    const HWND window,
    TrackedTreeView* const tracked) noexcept {
    DWORD process_id = 0;
    const DWORD thread_id =
        ::GetWindowThreadProcessId(window, &process_id);
    if (thread_id == 0 || process_id != ::GetCurrentProcessId() ||
        !ClassNameEquals(window, WC_TREEVIEWW)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }

    TrackedTreeView captured{};
    if (ReadTrackedTreeView(window, thread_id, &captured)) {
        if (tracked != nullptr) {
            *tracked = captured;
        }
        return S_OK;
    }

    captured.window = window;
    captured.thread_id = thread_id;
    HRESULT result = SendTreeViewMessage(
        window, TVM_GETBKCOLOR, 0, &captured.original_background);
    if (SUCCEEDED(result)) {
        result = SendTreeViewMessage(
            window, TVM_GETTEXTCOLOR, 0, &captured.original_text);
    }
    if (FAILED(result)) {
        return result;
    }

    ::AcquireSRWLockExclusive(&g_tree_views_lock);
    auto iterator = std::find_if(
        g_tree_views.begin(),
        g_tree_views.begin() + g_tree_view_count,
        [window, thread_id](const TrackedTreeView& entry) {
            return entry.window == window && entry.thread_id == thread_id;
        });
    if (iterator == g_tree_views.begin() + g_tree_view_count) {
        if (g_tree_view_count >= g_tree_views.size()) {
            ::ReleaseSRWLockExclusive(&g_tree_views_lock);
            return HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
        }
        iterator = g_tree_views.begin() + g_tree_view_count++;
        *iterator = captured;
    }
    captured = *iterator;
    ::ReleaseSRWLockExclusive(&g_tree_views_lock);

    if (tracked != nullptr) {
        *tracked = captured;
    }
    return S_OK;
}

[[nodiscard]] HRESULT ApplyTreeViewColor(
    const HWND window,
    const std::uint32_t argb) noexcept {
    HRESULT result = TrackTreeView(window, nullptr);
    if (FAILED(result)) {
        return result;
    }
    result = SendTreeViewMessage(window, TVM_SETBKCOLOR, ToColorRef(argb));
    if (SUCCEEDED(result)) {
        result = SendTreeViewMessage(
            window, TVM_SETTEXTCOLOR, ContrastingTextColor(argb));
    }
    if (SUCCEEDED(result)) {
        static_cast<void>(::RedrawWindow(
            window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
    }
    EnsureTreeViewSubclass(window);
    return result;
}

struct TreeViewEnumerationContext final {
    std::uint32_t argb;
    HRESULT result{S_OK};
};

BOOL CALLBACK ApplyEnumeratedTreeView(
    const HWND window,
    const LPARAM raw_context) noexcept {
    auto* const context =
        reinterpret_cast<TreeViewEnumerationContext*>(raw_context);
    if (context == nullptr) {
        return FALSE;
    }
    if (!ClassNameEquals(window, WC_TREEVIEWW)) {
        return TRUE;
    }
    const HRESULT result = ApplyTreeViewColor(window, context->argb);
    if (FAILED(result) && SUCCEEDED(context->result)) {
        context->result = result;
    }
    return TRUE;
}

[[nodiscard]] HRESULT ApplyExplorerTreeViewColors(
    const HWND explorer_window,
    const std::uint32_t argb) noexcept {
    if (!IsCurrentProcessExplorerWindow(explorer_window)) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_WINDOW_HANDLE);
    }
    TreeViewEnumerationContext context{argb};
    static_cast<void>(::EnumChildWindows(
        explorer_window,
        &ApplyEnumeratedTreeView,
        reinterpret_cast<LPARAM>(&context)));
    return context.result;
}

[[nodiscard]] HRESULT RestoreTreeView(
    const TrackedTreeView& tracked) noexcept {
    DWORD process_id = 0;
    const DWORD thread_id =
        ::GetWindowThreadProcessId(tracked.window, &process_id);
    if (thread_id == 0 || process_id != ::GetCurrentProcessId() ||
        thread_id != tracked.thread_id ||
        !ClassNameEquals(tracked.window, WC_TREEVIEWW)) {
        return S_OK;
    }
    DestroyTreeViewScrollbarOverlay(tracked.window);
    HRESULT result = SendTreeViewMessage(
        tracked.window, TVM_SETBKCOLOR, tracked.original_background);
    if (SUCCEEDED(result)) {
        result = SendTreeViewMessage(
            tracked.window, TVM_SETTEXTCOLOR, tracked.original_text);
    }
    if (SUCCEEDED(result)) {
        static_cast<void>(::RedrawWindow(
            tracked.window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
    }
    return result;
}

[[nodiscard]] HRESULT RestoreAllTreeViews() noexcept {
    std::array<TrackedTreeView, kMaximumTrackedTreeViews> tree_views{};
    std::size_t count = 0;
    ::AcquireSRWLockExclusive(&g_tree_views_lock);
    count = g_tree_view_count;
    std::copy_n(g_tree_views.begin(), count, tree_views.begin());
    ::ReleaseSRWLockExclusive(&g_tree_views_lock);

    HRESULT result = S_OK;
    std::array<TrackedTreeView, kMaximumTrackedTreeViews> failed{};
    std::size_t failed_count = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const HRESULT restored = RestoreTreeView(tree_views[index]);
        if (FAILED(restored)) {
            if (SUCCEEDED(result)) {
                result = restored;
            }
            failed[failed_count++] = tree_views[index];
        }
    }

    ::AcquireSRWLockExclusive(&g_tree_views_lock);
    g_tree_views = failed;
    g_tree_view_count = failed_count;
    ::ReleaseSRWLockExclusive(&g_tree_views_lock);
    return result;
}

[[nodiscard]] int FillRectWithColor(
    const FillRectFunction original,
    const HDC device_context,
    const RECT* const rectangle,
    const HBRUSH fallback_brush,
    const COLORREF color) noexcept {
    const HBRUSH brush =
        static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
    if (brush == nullptr) {
        return original(device_context, rectangle, fallback_brush);
    }
    const COLORREF previous = ::SetDCBrushColor(device_context, color);
    if (previous == CLR_INVALID) {
        return original(device_context, rectangle, fallback_brush);
    }
    const int result = original(device_context, rectangle, brush);
    static_cast<void>(::SetDCBrushColor(device_context, previous));
    return result;
}

[[nodiscard]] int FillRectWithOpaqueColor(
    const FillRectFunction original,
    const HDC device_context,
    const RECT* const rectangle,
    const HBRUSH fallback_brush,
    const std::uint32_t argb) noexcept {
    const int width = rectangle->right - rectangle->left;
    const int height = rectangle->bottom - rectangle->top;
    if (width <= 0 || height <= 0) {
        return original(device_context, rectangle, fallback_brush);
    }

    // FillRect updates RGB but not the alpha channel of Explorer's 32-bit
    // composition DIB. Its transparent #191919 pixels are then added over the
    // requested accent color. StretchDIBits copies one premultiplied opaque
    // pixel, preserving alpha as well as RGB and eliminating that transient.
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(information.bmiHeader);
    information.bmiHeader.biWidth = 1;
    information.bmiHeader.biHeight = -1;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    const std::uint32_t pixel = argb | 0xFF000000U;
    const int saved = ::SaveDC(device_context);
    static_cast<void>(::SetStretchBltMode(device_context, COLORONCOLOR));
    const int copied = ::StretchDIBits(
        device_context,
        rectangle->left,
        rectangle->top,
        width,
        height,
        0,
        0,
        1,
        1,
        &pixel,
        &information,
        DIB_RGB_COLORS,
        SRCCOPY);
    if (saved != 0) {
        static_cast<void>(::RestoreDC(device_context, saved));
    }
    if (copied != GDI_ERROR) {
        return 1;
    }
    return FillRectWithColor(
        original,
        device_context,
        rectangle,
        fallback_brush,
        ToColorRef(argb));
}

void PaintDirectUiClient(
    const HWND window,
    const HDC device_context) noexcept {
    if (g_original_fill_rect == nullptr) {
        return;
    }
    RECT bounds{};
    if (!::GetClientRect(window, &bounds)) {
        return;
    }
    static_cast<void>(FillRectWithOpaqueColor(
        g_original_fill_rect,
        device_context,
        &bounds,
        static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)),
        g_color.load(std::memory_order_acquire)));
}

[[nodiscard]] constexpr std::uint32_t DirectUiTransitionProgress(
    const DWORD elapsed_milliseconds) noexcept {
    if (elapsed_milliseconds <= kDirectUiTransitionHoldMilliseconds) {
        return 0;
    }
    const DWORD fade_elapsed =
        elapsed_milliseconds - kDirectUiTransitionHoldMilliseconds;
    if (fade_elapsed >= kDirectUiTransitionFadeMilliseconds) {
        return 65'535U;
    }

    // Smoothstep easing: p²(3 - 2p). Fixed-point arithmetic keeps the timer
    // callback deterministic and avoids floating-point state in Explorer.
    constexpr std::uint64_t scale = 65'535U;
    const std::uint64_t progress =
        static_cast<std::uint64_t>(fade_elapsed) * scale /
        kDirectUiTransitionFadeMilliseconds;
    return static_cast<std::uint32_t>(
        progress * progress * (3U * scale - 2U * progress) /
        (scale * scale));
}

[[nodiscard]] constexpr BYTE DirectUiTransitionOpacity(
    const DWORD elapsed_milliseconds) noexcept {
    constexpr std::uint64_t scale = 65'535U;
    const std::uint64_t eased =
        DirectUiTransitionProgress(elapsed_milliseconds);
    return static_cast<BYTE>(
        (255U * (scale - eased) + scale / 2U) / scale);
}

static_assert(DirectUiTransitionOpacity(0) == 255);
static_assert(DirectUiTransitionOpacity(
                  kDirectUiTransitionHoldMilliseconds) == 255);
static_assert(DirectUiTransitionOpacity(
                  kDirectUiTransitionHoldMilliseconds +
                  kDirectUiTransitionFadeMilliseconds) == 0);
static_assert(DirectUiTransitionOpacity(
                  kDirectUiTransitionHoldMilliseconds +
                  kDirectUiTransitionFadeMilliseconds / 2U) >= 127);
static_assert(DirectUiTransitionOpacity(
                  kDirectUiTransitionHoldMilliseconds +
                  kDirectUiTransitionFadeMilliseconds / 2U) <= 128);

void FinishDirectUiTransition(
    const HWND window,
    const DirectUiTransitionState* const state) noexcept {
    static_cast<void>(::KillTimer(window, kDirectUiTransitionTimerId));
    if (state != nullptr) {
        static_cast<void>(::SetWindowPos(
            window,
            nullptr,
            state->bounds.left,
            state->bounds.top,
            state->bounds.right - state->bounds.left,
            state->bounds.bottom - state->bounds.top,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING));
    }
    static_cast<void>(::ShowWindow(window, SW_HIDE));
    static_cast<void>(::SetLayeredWindowAttributes(
        window, 0, 255, LWA_ALPHA));
}

void ApplyDirectUiTransitionGeometry(
    const HWND window,
    const DirectUiTransitionState& state,
    const std::uint32_t progress) noexcept {
    constexpr std::uint32_t scale = 65'535U;
    RECT bounds = state.bounds;
    if (state.animation == protocol::kExplorerTransitionSlideFade) {
        const int offset = static_cast<int>(
            (static_cast<std::uint64_t>(kDirectUiTransitionSlidePixels) *
             progress + scale / 2U) /
            scale);
        bounds.left -= offset;
        bounds.right -= offset;
    } else if (state.animation == protocol::kExplorerTransitionScaleFade) {
        const int inset = static_cast<int>(
            (static_cast<std::uint64_t>(kDirectUiTransitionScaleInsetPixels) *
             progress + scale / 2U) /
            scale);
        bounds.left += inset;
        bounds.top += inset;
        bounds.right -= inset;
        bounds.bottom -= inset;
    }
    const int width = std::max(1L, bounds.right - bounds.left);
    const int height = std::max(1L, bounds.bottom - bounds.top);
    static_cast<void>(::SetWindowPos(
        window,
        nullptr,
        bounds.left,
        bounds.top,
        width,
        height,
        SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOSENDCHANGING));
}

LRESULT CALLBACK DirectUiOverlayWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        auto* const state = new (std::nothrow) DirectUiTransitionState;
        if (state == nullptr) {
            return FALSE;
        }
        static_cast<void>(::SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state)));
        const LRESULT result =
            ::DefWindowProcW(window, message, wparam, lparam);
        if (result == FALSE) {
            static_cast<void>(::SetWindowLongPtrW(
                window, GWLP_USERDATA, 0));
            delete state;
        }
        return result;
    }
    if (message == WM_ERASEBKGND && wparam != 0) {
        PaintDirectUiClient(window, reinterpret_cast<HDC>(wparam));
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC device_context = ::BeginPaint(window, &paint);
        if (device_context != nullptr) {
            PaintDirectUiClient(window, device_context);
            static_cast<void>(::EndPaint(window, &paint));
        }
        return 0;
    }
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    if (message == kDirectUiTransitionStartMessage) {
        auto* const state = reinterpret_cast<DirectUiTransitionState*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        const std::uint32_t animation = static_cast<std::uint32_t>(wparam);
        if (state == nullptr ||
            animation > protocol::kMaximumExplorerTransition ||
            !::GetWindowRect(window, &state->bounds)) {
            return FALSE;
        }
        state->started_at = ::GetTickCount();
        state->animation = animation;
        static_cast<void>(::KillTimer(window, kDirectUiTransitionTimerId));
        return ::SetTimer(
                   window,
                   kDirectUiTransitionTimerId,
                   kDirectUiTransitionFrameMilliseconds,
                   nullptr) != 0
            ? TRUE
            : FALSE;
    }
    if (message == WM_TIMER &&
        wparam == kDirectUiTransitionTimerId) {
        auto* const state = reinterpret_cast<DirectUiTransitionState*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state == nullptr) {
            FinishDirectUiTransition(window, nullptr);
            return 0;
        }
        const DWORD elapsed = ::GetTickCount() - state->started_at;
        const bool animation_finished =
            state->animation == protocol::kExplorerTransitionNone
            ? elapsed >= kDirectUiTransitionHoldMilliseconds
            : elapsed >= kDirectUiTransitionHoldMilliseconds +
                  kDirectUiTransitionFadeMilliseconds;
        if (animation_finished) {
            FinishDirectUiTransition(window, state);
            return 0;
        }
        const std::uint32_t progress =
            DirectUiTransitionProgress(elapsed);
        const BYTE opacity =
            state->animation == protocol::kExplorerTransitionNone
            ? 255
            : DirectUiTransitionOpacity(elapsed);
        ApplyDirectUiTransitionGeometry(window, *state, progress);
        if (opacity == 0 || !::SetLayeredWindowAttributes(
                window, 0, opacity, LWA_ALPHA)) {
            FinishDirectUiTransition(window, state);
        }
        return 0;
    }
    if (message == WM_NCDESTROY) {
        static_cast<void>(::KillTimer(
            window, kDirectUiTransitionTimerId));
        const HWND owner = ::GetWindow(window, GW_OWNER);
        if (owner != nullptr &&
            reinterpret_cast<HWND>(::GetPropW(
                owner, kDirectUiOverlayProperty)) == window) {
            static_cast<void>(::RemovePropW(
                owner, kDirectUiOverlayProperty));
        }
        auto* const state = reinterpret_cast<DirectUiTransitionState*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        static_cast<void>(::SetWindowLongPtrW(
            window, GWLP_USERDATA, 0));
        delete state;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

[[nodiscard]] HMODULE CurrentAgentModule() noexcept {
    HMODULE current_module = nullptr;
    static_cast<void>(::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&CurrentAgentModule),
        &current_module));
    return current_module;
}

BOOL CALLBACK RegisterDirectUiOverlayClass(
    PINIT_ONCE,
    PVOID,
    PVOID*) noexcept {
    const HMODULE current_module = CurrentAgentModule();
    if (current_module == nullptr) {
        return FALSE;
    }
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &DirectUiOverlayWindowProc;
    window_class.hInstance = current_module;
    window_class.lpszClassName = kDirectUiOverlayClass;
    return ::RegisterClassExW(&window_class) != 0 ||
           ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

[[nodiscard]] HWND ShowDirectUiTransitionOverlay(
    const HWND direct_ui_window,
    const bool create_if_missing,
    const bool restart_deadline) noexcept {
    if (direct_ui_window == nullptr ||
        !ClassNameEquals(direct_ui_window, L"DirectUIHWND")) {
        return nullptr;
    }
    static INIT_ONCE registration = INIT_ONCE_STATIC_INIT;
    if (!::InitOnceExecuteOnce(
            &registration, &RegisterDirectUiOverlayClass, nullptr, nullptr)) {
        return nullptr;
    }
    // Explorer renders the folder view through a redirected composition
    // surface. A WS_CHILD guard can therefore remain behind a newly created
    // surface regardless of its sibling z-order. Use a non-activating owned
    // popup: it stays above its owner without becoming globally topmost.
    const HWND parent = ::GetAncestor(direct_ui_window, GA_ROOT);
    RECT bounds{};
    if (!IsCurrentProcessExplorerWindow(parent) ||
        !::GetWindowRect(direct_ui_window, &bounds)) {
        return nullptr;
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return nullptr;
    }
    RECT parent_bounds{};
    if (!::GetClientRect(parent, &parent_bounds)) {
        return nullptr;
    }
    const auto overlay_area =
        static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height);
    const auto parent_area =
        static_cast<std::uint64_t>(parent_bounds.right) *
        static_cast<std::uint64_t>(parent_bounds.bottom);
    // Explorer contains several small DirectUIHWND controls in its command
    // and navigation bars. Only the large folder view may own the transition
    // guard; otherwise a toolbar event can shrink it mid-navigation.
    if (parent_area == 0 || overlay_area < parent_area / 3U) {
        return nullptr;
    }
    HWND overlay = reinterpret_cast<HWND>(::GetPropW(
        parent, kDirectUiOverlayProperty));
    if (overlay != nullptr &&
        (!::IsWindow(overlay) ||
         !ClassNameEquals(overlay, kDirectUiOverlayClass))) {
        static_cast<void>(::RemovePropW(
            parent, kDirectUiOverlayProperty));
        overlay = nullptr;
    }
    bool created = false;
    if (overlay == nullptr && create_if_missing) {
        overlay = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            kDirectUiOverlayClass,
            L"",
            WS_POPUP | WS_DISABLED,
            bounds.left,
            bounds.top,
            width,
            height,
            parent,
            nullptr,
            CurrentAgentModule(),
            nullptr);
        if (overlay != nullptr && !::SetPropW(
                parent, kDirectUiOverlayProperty, overlay)) {
            static_cast<void>(::DestroyWindow(overlay));
            overlay = nullptr;
        }
        created = overlay != nullptr;
    }
    if (overlay == nullptr) {
        return nullptr;
    }
    const bool was_hidden = !::IsWindowVisible(overlay);
    if (!created && !restart_deadline && was_hidden &&
        !create_if_missing) {
        return overlay;
    }
    const bool start_animation = created || restart_deadline || was_hidden;
    if (start_animation) {
        if (!::SetLayeredWindowAttributes(
                overlay, 0, 255, LWA_ALPHA)) {
            static_cast<void>(::PostMessageW(overlay, WM_CLOSE, 0, 0));
            return nullptr;
        }
    }
    const LONG_PTR owner_extended_style =
        ::GetWindowLongPtrW(parent, GWL_EXSTYLE);
    const HWND insert_after =
        (owner_extended_style & WS_EX_TOPMOST) != 0
        ? HWND_TOPMOST
        : HWND_NOTOPMOST;
    static_cast<void>(::SetWindowPos(
        overlay,
        insert_after,
        start_animation ? bounds.left : 0,
        start_animation ? bounds.top : 0,
        start_animation ? width : 0,
        start_animation ? height : 0,
        SWP_NOACTIVATE | SWP_SHOWWINDOW |
            (start_animation ? 0U : SWP_NOMOVE | SWP_NOSIZE)));
    static_cast<void>(::RedrawWindow(
        overlay,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
    if (start_animation) {
        DWORD_PTR started = FALSE;
        const LRESULT delivered = ::SendMessageTimeoutW(
            overlay,
            kDirectUiTransitionStartMessage,
            static_cast<WPARAM>(
                g_transition_animation.load(std::memory_order_acquire)),
            0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK,
            kTreeViewMessageTimeoutMilliseconds,
            &started);
        if (delivered == 0 || started == FALSE) {
            static_cast<void>(::PostMessageW(overlay, WM_CLOSE, 0, 0));
            return nullptr;
        }
    }
    return overlay;
}

void DestroyDirectUiTransitionOverlay(const HWND explorer_window) noexcept {
    const HWND overlay = reinterpret_cast<HWND>(::GetPropW(
        explorer_window, kDirectUiOverlayProperty));
    if (overlay == nullptr || !::IsWindow(overlay)) {
        static_cast<void>(::RemovePropW(
            explorer_window, kDirectUiOverlayProperty));
        return;
    }
    DWORD process_id = 0;
    const DWORD thread_id =
        ::GetWindowThreadProcessId(overlay, &process_id);
    if (process_id == ::GetCurrentProcessId() &&
        thread_id == ::GetCurrentThreadId()) {
        static_cast<void>(::DestroyWindow(overlay));
    } else {
        static_cast<void>(::PostMessageW(overlay, WM_CLOSE, 0, 0));
    }
}

void RequestDirectUiScrollbarMetrics(
    const HWND direct_ui_window) noexcept {
    if (!g_enabled.load(std::memory_order_acquire) ||
        !g_custom_scrollbar_enabled.load(std::memory_order_acquire) ||
        !::IsWindow(direct_ui_window) ||
        !ClassNameEquals(direct_ui_window, L"DirectUIHWND")) {
        return;
    }
    if (::GetPropW(
            direct_ui_window,
            kDirectUiScrollbarRefreshPendingProperty) != nullptr) {
        return;
    }
    if (!::SetPropW(
            direct_ui_window,
            kDirectUiScrollbarRefreshPendingProperty,
            reinterpret_cast<HANDLE>(1))) {
        return;
    }
    const DWORD thread_id =
        g_direct_ui_event_thread_id.load(std::memory_order_acquire);
    if (thread_id == 0 || !::PostThreadMessageW(
            thread_id,
            kDirectUiScrollbarRefreshThreadMessage,
            reinterpret_cast<WPARAM>(direct_ui_window),
            0)) {
        static_cast<void>(::RemovePropW(
            direct_ui_window,
            kDirectUiScrollbarRefreshPendingProperty));
    }
}

void QueueDirectUiScrollbarMetrics(
    const HWND direct_ui_window,
    const std::uint16_t scroll_percent,
    const std::uint16_t view_percent) noexcept {
    if (!::IsWindow(direct_ui_window)) {
        return;
    }
    bool queued = false;
    ::AcquireSRWLockExclusive(&g_direct_ui_scrollbar_metrics_lock);
    auto iterator = std::find_if(
        g_direct_ui_scrollbar_metrics.begin(),
        g_direct_ui_scrollbar_metrics.begin() +
            g_direct_ui_scrollbar_metrics_count,
        [direct_ui_window](const DirectUiScrollbarMetrics& entry) {
            return entry.window == direct_ui_window;
        });
    if (iterator == g_direct_ui_scrollbar_metrics.begin() +
            g_direct_ui_scrollbar_metrics_count &&
        g_direct_ui_scrollbar_metrics_count <
            g_direct_ui_scrollbar_metrics.size()) {
        iterator = g_direct_ui_scrollbar_metrics.begin() +
            g_direct_ui_scrollbar_metrics_count++;
    }
    if (iterator != g_direct_ui_scrollbar_metrics.begin() +
            g_direct_ui_scrollbar_metrics_count) {
        *iterator = DirectUiScrollbarMetrics{
            direct_ui_window, scroll_percent, view_percent};
        queued = true;
    }
    ::ReleaseSRWLockExclusive(&g_direct_ui_scrollbar_metrics_lock);
    if (!queued) {
        return;
    }
    const HWND root = ::GetAncestor(direct_ui_window, GA_ROOT);
    if (root != nullptr) {
        static_cast<void>(::PostMessageW(root, WM_NULL, 0, 0));
    }
}

void ApplyPendingDirectUiScrollbarMetrics(
    const HWND explorer_window) noexcept {
    std::array<DirectUiScrollbarMetrics, kMaximumDirectUiScrollbarMetrics>
        pending{};
    std::size_t pending_count = 0;
    const DWORD current_thread_id = ::GetCurrentThreadId();
    ::AcquireSRWLockExclusive(&g_direct_ui_scrollbar_metrics_lock);
    std::size_t retained_count = 0;
    for (std::size_t index = 0;
         index < g_direct_ui_scrollbar_metrics_count;
         ++index) {
        const DirectUiScrollbarMetrics entry =
            g_direct_ui_scrollbar_metrics[index];
        DWORD process_id = 0;
        const DWORD thread_id =
            ::GetWindowThreadProcessId(entry.window, &process_id);
        if (thread_id == 0 || process_id != ::GetCurrentProcessId()) {
            continue;
        }
        if (thread_id == current_thread_id &&
            ::GetAncestor(entry.window, GA_ROOT) == explorer_window) {
            pending[pending_count++] = entry;
            continue;
        }
        g_direct_ui_scrollbar_metrics[retained_count++] = entry;
    }
    std::fill(
        g_direct_ui_scrollbar_metrics.begin() + retained_count,
        g_direct_ui_scrollbar_metrics.begin() +
            g_direct_ui_scrollbar_metrics_count,
        DirectUiScrollbarMetrics{});
    g_direct_ui_scrollbar_metrics_count = retained_count;
    ::ReleaseSRWLockExclusive(&g_direct_ui_scrollbar_metrics_lock);

    for (std::size_t index = 0; index < pending_count; ++index) {
        UpdateDirectUiScrollbarOverlay(
            pending[index].window,
            pending[index].view_percent != 0,
            pending[index].scroll_percent,
            pending[index].view_percent);
    }
}

void ForgetDirectUiScrollbarMetrics(
    const HWND direct_ui_window) noexcept {
    ::AcquireSRWLockExclusive(&g_direct_ui_scrollbar_metrics_lock);
    const auto end = g_direct_ui_scrollbar_metrics.begin() +
        g_direct_ui_scrollbar_metrics_count;
    const auto iterator = std::find_if(
        g_direct_ui_scrollbar_metrics.begin(),
        end,
        [direct_ui_window](const DirectUiScrollbarMetrics& entry) {
            return entry.window == direct_ui_window;
        });
    if (iterator != end) {
        std::move(iterator + 1, end, iterator);
        --g_direct_ui_scrollbar_metrics_count;
        g_direct_ui_scrollbar_metrics[
            g_direct_ui_scrollbar_metrics_count] = {};
    }
    ::ReleaseSRWLockExclusive(&g_direct_ui_scrollbar_metrics_lock);
}

LRESULT CALLBACK ExplorerDirectUiSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR,
    DWORD_PTR) noexcept {
    if (message == WM_NCDESTROY) {
        static_cast<void>(::RemovePropW(
            window, kDirectUiScrollbarRefreshPendingProperty));
        DestroyDirectUiScrollbarOverlay(window);
        ForgetDirectUiScrollbarMetrics(window);
        static_cast<void>(::RemoveWindowSubclass(
            window,
            &ExplorerDirectUiSubclassProc,
            kExplorerDirectUiSubclassId));
        return ::DefSubclassProc(window, message, wparam, lparam);
    }
    if (message == WM_ERASEBKGND &&
        g_enabled.load(std::memory_order_acquire) && wparam != 0) {
        PaintDirectUiClient(window, reinterpret_cast<HDC>(wparam));
        return 1;
    }
    const LRESULT result =
        ::DefSubclassProc(window, message, wparam, lparam);
    if (g_enabled.load(std::memory_order_acquire) &&
        (message == WM_PAINT || message == WM_SHOWWINDOW ||
         message == WM_WINDOWPOSCHANGED)) {
        static_cast<void>(ShowDirectUiTransitionOverlay(
            window, false, false));
    }
    if (g_enabled.load(std::memory_order_acquire) &&
        g_custom_scrollbar_enabled.load(std::memory_order_acquire) &&
        (message == WM_PAINT || message == WM_SHOWWINDOW ||
         message == WM_WINDOWPOSCHANGED || message == WM_MOUSEWHEEL ||
         message == WM_VSCROLL || message == WM_KEYDOWN)) {
        RequestDirectUiScrollbarMetrics(window);
    }
    return result;
}

void DetachExplorerDirectUiSubclass(const HWND window) noexcept {
    static_cast<void>(::RemovePropW(
        window, kDirectUiScrollbarRefreshPendingProperty));
    DestroyDirectUiScrollbarOverlay(window);
    ForgetDirectUiScrollbarMetrics(window);
    DWORD_PTR ref_data = 0;
    if (!::GetWindowSubclass(
            window,
            &ExplorerDirectUiSubclassProc,
            kExplorerDirectUiSubclassId,
            &ref_data)) {
        return;
    }
    static_cast<void>(::RemoveWindowSubclass(
        window,
        &ExplorerDirectUiSubclassProc,
        kExplorerDirectUiSubclassId));
}

void AttachExplorerDirectUiSubclass(const HWND window) noexcept {
    if (window == nullptr ||
        !ClassNameEquals(window, L"DirectUIHWND") ||
        !IsCurrentProcessExplorerWindow(::GetAncestor(window, GA_ROOT)) ||
        ::GetWindowThreadProcessId(window, nullptr) !=
            ::GetCurrentThreadId()) {
        return;
    }
    DWORD_PTR ref_data = 0;
    bool attached = ::GetWindowSubclass(
        window,
        &ExplorerDirectUiSubclassProc,
        kExplorerDirectUiSubclassId,
        &ref_data) != FALSE;
    if (!attached) {
        attached = ::SetWindowSubclass(
            window,
            &ExplorerDirectUiSubclassProc,
            kExplorerDirectUiSubclassId,
            0) != FALSE;
    }
    if (!attached) {
        return;
    }
    const HWND root = ::GetAncestor(window, GA_ROOT);
    if (root != nullptr) {
        static_cast<void>(::SetWindowSubclass(
            root,
            &ExplorerScrollbarOwnerSubclassProc,
            kExplorerScrollbarOwnerSubclassId,
            0));
    }
    if (g_custom_scrollbar_enabled.load(std::memory_order_acquire)) {
        RequestDirectUiScrollbarMetrics(window);
    } else {
        DestroyDirectUiScrollbarOverlay(window);
    }
}

BOOL CALLBACK AttachExistingDirectUiSubclass(
    const HWND window,
    const LPARAM) noexcept {
    if (!ClassNameEquals(window, L"DirectUIHWND")) {
        return TRUE;
    }
    if (!g_custom_scrollbar_enabled.load(std::memory_order_acquire)) {
        QueueDirectUiScrollbarMetrics(window, 0, 0);
    } else {
        AttachExplorerDirectUiSubclass(window);
        RequestDirectUiScrollbarMetrics(window);
    }
    return TRUE;
}

LRESULT CALLBACK ExplorerUiThreadCallWndProc(
    const int code,
    const WPARAM wparam,
    const LPARAM lparam) noexcept {
    if (code >= 0 && lparam != 0) {
        const auto* message = reinterpret_cast<const CWPSTRUCT*>(lparam);
        if (ClassNameEquals(message->hwnd, L"DirectUIHWND")) {
            if (message->message == WM_NCCREATE ||
                message->message == WM_CREATE ||
                message->message == WM_SHOWWINDOW ||
                message->message == WM_WINDOWPOSCHANGING ||
                message->message == WM_WINDOWPOSCHANGED ||
                message->message == WM_PAINT ||
                message->message == WM_DESTROY ||
                message->message == WM_NCDESTROY) {
                AttachExplorerDirectUiSubclass(message->hwnd);
                if (message->message == WM_SHOWWINDOW ||
                    message->message == WM_WINDOWPOSCHANGED ||
                    message->message == WM_PAINT) {
                    static_cast<void>(ShowDirectUiTransitionOverlay(
                        message->hwnd,
                        false,
                        false));
                }
            }
        }
    }
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

[[nodiscard]] bool EnsureExplorerUiThreadHook(
    const HWND explorer_window) noexcept {
    DWORD process_id = 0;
    const DWORD thread_id =
        ::GetWindowThreadProcessId(explorer_window, &process_id);
    if (thread_id == 0 || process_id != ::GetCurrentProcessId()) {
        return false;
    }
    ::AcquireSRWLockExclusive(&g_explorer_ui_thread_hooks_lock);
    const auto end = std::ranges::find(
        g_explorer_ui_thread_hooks,
        thread_id,
        &ExplorerUiThreadHook::thread_id);
    if (end != g_explorer_ui_thread_hooks.end()) {
        ::ReleaseSRWLockExclusive(&g_explorer_ui_thread_hooks_lock);
        return true;
    }
    const auto empty = std::ranges::find(
        g_explorer_ui_thread_hooks,
        static_cast<DWORD>(0),
        &ExplorerUiThreadHook::thread_id);
    if (empty == g_explorer_ui_thread_hooks.end()) {
        ::ReleaseSRWLockExclusive(&g_explorer_ui_thread_hooks_lock);
        return false;
    }
    const HHOOK call_wnd_proc_hook = ::SetWindowsHookExW(
        WH_CALLWNDPROC,
        &ExplorerUiThreadCallWndProc,
        CurrentAgentModule(),
        thread_id);
    const HHOOK cbt_hook = call_wnd_proc_hook != nullptr
        ? ::SetWindowsHookExW(
              WH_CBT,
              &ExplorerUiThreadCbtProc,
              CurrentAgentModule(),
              thread_id)
        : nullptr;
    if (call_wnd_proc_hook != nullptr && cbt_hook != nullptr) {
        *empty = ExplorerUiThreadHook{
            thread_id, call_wnd_proc_hook, cbt_hook};
    } else {
        if (cbt_hook != nullptr) {
            static_cast<void>(::UnhookWindowsHookEx(cbt_hook));
        }
        if (call_wnd_proc_hook != nullptr) {
            static_cast<void>(::UnhookWindowsHookEx(call_wnd_proc_hook));
        }
    }
    ::ReleaseSRWLockExclusive(&g_explorer_ui_thread_hooks_lock);
    return call_wnd_proc_hook != nullptr && cbt_hook != nullptr;
}

void RemoveExplorerUiThreadHooks() noexcept {
    std::array<ExplorerUiThreadHook, kMaximumExplorerUiThreads> hooks{};
    ::AcquireSRWLockExclusive(&g_explorer_ui_thread_hooks_lock);
    hooks = g_explorer_ui_thread_hooks;
    g_explorer_ui_thread_hooks = {};
    ::ReleaseSRWLockExclusive(&g_explorer_ui_thread_hooks_lock);
    for (const auto& entry : hooks) {
        if (entry.cbt_hook != nullptr) {
            static_cast<void>(::UnhookWindowsHookEx(entry.cbt_hook));
        }
        if (entry.call_wnd_proc_hook != nullptr) {
            static_cast<void>(::UnhookWindowsHookEx(
                entry.call_wnd_proc_hook));
        }
    }
}

struct DirectUiSearchContext final {
    HWND window{nullptr};
    std::uint64_t area{0};
};

BOOL CALLBACK FindLargestDirectUiWindow(
    const HWND window,
    const LPARAM raw_context) noexcept {
    if (!ClassNameEquals(window, L"DirectUIHWND")) {
        return TRUE;
    }
    RECT bounds{};
    if (!::GetWindowRect(window, &bounds)) {
        return TRUE;
    }
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) {
        return TRUE;
    }
    auto* context = reinterpret_cast<DirectUiSearchContext*>(raw_context);
    const auto area = static_cast<std::uint64_t>(width) *
        static_cast<std::uint64_t>(height);
    if (context != nullptr && area > context->area) {
        context->window = window;
        context->area = area;
    }
    return TRUE;
}

[[nodiscard]] HWND LargestDirectUiWindow(
    const HWND explorer_window) noexcept {
    DirectUiSearchContext context{};
    static_cast<void>(::EnumChildWindows(
        explorer_window,
        &FindLargestDirectUiWindow,
        reinterpret_cast<LPARAM>(&context)));
    return context.window;
}

LRESULT CALLBACK ExplorerUiThreadCbtProc(
    const int code,
    const WPARAM wparam,
    const LPARAM lparam) noexcept {
    if (code == HCBT_CREATEWND &&
        g_enabled.load(std::memory_order_acquire)) {
        const HWND window = reinterpret_cast<HWND>(wparam);
        if (ClassNameEquals(window, L"DirectUIHWND")) {
            const HWND root = ::GetAncestor(window, GA_ROOT);
            if (IsCurrentProcessExplorerWindow(root)) {
                const HWND reference = LargestDirectUiWindow(root);
                if (reference != nullptr) {
                    static_cast<void>(ShowDirectUiTransitionOverlay(
                        reference, true, true));
                }
            }
        }
    }
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

void CALLBACK ExplorerDirectUiWindowEventCallback(
    HWINEVENTHOOK,
    const DWORD event,
    const HWND window,
    const LONG object_id,
    const LONG child_id,
    DWORD,
    DWORD) noexcept {
    if (event == EVENT_OBJECT_VALUECHANGE) {
        if (ClassNameEquals(window, L"DirectUIHWND") &&
            IsCurrentProcessExplorerWindow(
                ::GetAncestor(window, GA_ROOT))) {
            RequestDirectUiScrollbarMetrics(window);
        }
        return;
    }
    if ((event != EVENT_OBJECT_CREATE && event != EVENT_OBJECT_SHOW) ||
        object_id != OBJID_WINDOW || child_id != CHILDID_SELF) {
        return;
    }
    if (!ClassNameEquals(window, L"DirectUIHWND")) {
        return;
    }
    const HWND root = ::GetAncestor(window, GA_ROOT);
    if (IsCurrentProcessExplorerWindow(root)) {
        static_cast<void>(EnsureExplorerUiThreadHook(root));
    }
    AttachExplorerDirectUiSubclass(window);
    // The replacement DirectUIHWND is created while the old view is still
    // visible but still has a 0x0 rectangle. Use the largest existing
    // DirectUI surface as the reference so the guard is visible before the
    // replacement receives its first stock-background composition frame.
    const HWND reference = LargestDirectUiWindow(root);
    static_cast<void>(ShowDirectUiTransitionOverlay(
        reference != nullptr ? reference : window,
        true,
        event == EVENT_OBJECT_CREATE));
    RequestDirectUiScrollbarMetrics(window);
}

void DeliverDirectUiScrollbarMetrics(
    const HWND direct_ui_window,
    const std::uint16_t scroll_percent,
    const std::uint16_t view_percent) noexcept {
    QueueDirectUiScrollbarMetrics(
        direct_ui_window, scroll_percent, view_percent);
}

void RefreshDirectUiScrollbarMetrics(
    IUIAutomation* const automation,
    IUIAutomationCondition* const scroll_condition,
    const HWND direct_ui_window) noexcept {
    static_cast<void>(::RemovePropW(
        direct_ui_window,
        kDirectUiScrollbarRefreshPendingProperty));
    if (automation == nullptr || scroll_condition == nullptr ||
        !g_enabled.load(std::memory_order_acquire) ||
        !g_custom_scrollbar_enabled.load(std::memory_order_acquire) ||
        !::IsWindow(direct_ui_window) ||
        !ClassNameEquals(direct_ui_window, L"DirectUIHWND") ||
        !IsCurrentProcessExplorerWindow(
            ::GetAncestor(direct_ui_window, GA_ROOT))) {
        return;
    }

    IUIAutomationElement* root = nullptr;
    const HRESULT root_result =
        automation->ElementFromHandle(direct_ui_window, &root);
    if (FAILED(root_result) || root == nullptr) {
        return;
    }
    IUIAutomationElement* scroll_element = nullptr;
    const HRESULT element_result = root->FindFirst(
        TreeScope_Subtree,
        scroll_condition,
        &scroll_element);
    root->Release();
    if (FAILED(element_result)) {
        return;
    }
    if (scroll_element == nullptr) {
        DeliverDirectUiScrollbarMetrics(direct_ui_window, 0, 0);
        return;
    }

    RECT scroll_bounds{};
    RECT direct_ui_bounds{};
    const HRESULT bounds_result =
        scroll_element->get_CurrentBoundingRectangle(&scroll_bounds);
    const auto nearly_equal = [](const LONG left, const LONG right) {
        return left >= right - 2 && left <= right + 2;
    };
    if (FAILED(bounds_result) ||
        !::GetWindowRect(direct_ui_window, &direct_ui_bounds)) {
        scroll_element->Release();
        return;
    }
    if (!nearly_equal(scroll_bounds.left, direct_ui_bounds.left) ||
        !nearly_equal(scroll_bounds.top, direct_ui_bounds.top) ||
        !nearly_equal(scroll_bounds.right, direct_ui_bounds.right) ||
        !nearly_equal(scroll_bounds.bottom, direct_ui_bounds.bottom)) {
        scroll_element->Release();
        DeliverDirectUiScrollbarMetrics(direct_ui_window, 0, 0);
        return;
    }

    IUIAutomationScrollPattern* scroll_pattern = nullptr;
    const HRESULT pattern_result = scroll_element->GetCurrentPatternAs(
        UIA_ScrollPatternId,
        IID_IUIAutomationScrollPattern,
        reinterpret_cast<void**>(&scroll_pattern));
    scroll_element->Release();
    if (FAILED(pattern_result) || scroll_pattern == nullptr) {
        return;
    }
    BOOL vertically_scrollable = FALSE;
    double scroll_percent = -1.0;
    double view_percent = 100.0;
    const bool metrics_valid =
        SUCCEEDED(scroll_pattern->get_CurrentVerticallyScrollable(
            &vertically_scrollable)) &&
        SUCCEEDED(scroll_pattern->get_CurrentVerticalScrollPercent(
            &scroll_percent)) &&
        SUCCEEDED(scroll_pattern->get_CurrentVerticalViewSize(
            &view_percent));
    scroll_pattern->Release();
    if (!metrics_valid) {
        return;
    }
    const bool visible = vertically_scrollable != FALSE &&
        scroll_percent >= 0.0 && scroll_percent <= 100.0 &&
        view_percent > 0.0 && view_percent < 100.0;
    if (!visible) {
        DeliverDirectUiScrollbarMetrics(direct_ui_window, 0, 0);
        return;
    }
    const auto scroll_units = static_cast<std::uint16_t>(
        std::clamp(scroll_percent * 100.0 + 0.5, 0.0, 10000.0));
    const auto view_units = static_cast<std::uint16_t>(
        std::clamp(view_percent * 100.0 + 0.5, 1.0, 9999.0));
    DeliverDirectUiScrollbarMetrics(
        direct_ui_window, scroll_units, view_units);
}

DWORD WINAPI DirectUiEventMonitorThread(void*) noexcept {
    g_direct_ui_event_thread_id.store(
        ::GetCurrentThreadId(), std::memory_order_release);
    MSG queue_message{};
    static_cast<void>(::PeekMessageW(
        &queue_message, nullptr, WM_USER, WM_USER, PM_NOREMOVE));

    const HRESULT com_result =
        ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_initialized = SUCCEEDED(com_result);
    IUIAutomation* automation = nullptr;
    IUIAutomationCondition* scroll_condition = nullptr;
    if (com_initialized) {
        static_cast<void>(::CoCreateInstance(
            CLSID_CUIAutomation8,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IUIAutomation,
            reinterpret_cast<void**>(&automation)));
    }
    if (automation != nullptr) {
        VARIANT scroll_available{};
        ::VariantInit(&scroll_available);
        scroll_available.vt = VT_BOOL;
        scroll_available.boolVal = VARIANT_TRUE;
        static_cast<void>(automation->CreatePropertyCondition(
            UIA_IsScrollPatternAvailablePropertyId,
            scroll_available,
            &scroll_condition));
        static_cast<void>(::VariantClear(&scroll_available));
    }

    const HWINEVENTHOOK hook = ::SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_SHOW,
        nullptr,
        &ExplorerDirectUiWindowEventCallback,
        ::GetCurrentProcessId(),
        0,
        WINEVENT_OUTOFCONTEXT);
    const HWINEVENTHOOK scroll_hook = ::SetWinEventHook(
        EVENT_OBJECT_VALUECHANGE,
        EVENT_OBJECT_VALUECHANGE,
        nullptr,
        &ExplorerDirectUiWindowEventCallback,
        ::GetCurrentProcessId(),
        0,
        WINEVENT_OUTOFCONTEXT);
    g_direct_ui_event_hook.store(hook, std::memory_order_release);
    g_direct_ui_scroll_event_hook.store(
        scroll_hook, std::memory_order_release);
    g_direct_ui_scroll_metrics_ready.store(
        automation != nullptr && scroll_condition != nullptr,
        std::memory_order_release);
    static_cast<void>(::SetEvent(g_direct_ui_event_ready));
    if (hook == nullptr || scroll_hook == nullptr || automation == nullptr ||
        scroll_condition == nullptr) {
        if (scroll_hook != nullptr) {
            static_cast<void>(::UnhookWinEvent(scroll_hook));
        }
        if (hook != nullptr) {
            static_cast<void>(::UnhookWinEvent(hook));
        }
        if (scroll_condition != nullptr) {
            scroll_condition->Release();
        }
        if (automation != nullptr) {
            automation->Release();
        }
        if (com_initialized) {
            ::CoUninitialize();
        }
        g_direct_ui_event_hook.store(nullptr, std::memory_order_release);
        g_direct_ui_scroll_event_hook.store(
            nullptr, std::memory_order_release);
        g_direct_ui_scroll_metrics_ready.store(
            false, std::memory_order_release);
        g_direct_ui_event_thread_id.store(0, std::memory_order_release);
        return 1;
    }

    bool stopping = false;
    while (!stopping) {
        const DWORD wait_result = ::MsgWaitForMultipleObjects(
            1,
            &g_direct_ui_event_stop,
            FALSE,
            INFINITE,
            QS_ALLINPUT);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (wait_result != WAIT_OBJECT_0 + 1U) {
            break;
        }
        MSG message{};
        while (::PeekMessageW(
            &message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                stopping = true;
                break;
            }
            if (message.hwnd == nullptr &&
                message.message == kDirectUiScrollbarRefreshThreadMessage) {
                RefreshDirectUiScrollbarMetrics(
                    automation,
                    scroll_condition,
                    reinterpret_cast<HWND>(message.wParam));
                continue;
            }
            static_cast<void>(::TranslateMessage(&message));
            static_cast<void>(::DispatchMessageW(&message));
        }
    }
    static_cast<void>(::UnhookWinEvent(scroll_hook));
    static_cast<void>(::UnhookWinEvent(hook));
    scroll_condition->Release();
    automation->Release();
    if (com_initialized) {
        ::CoUninitialize();
    }
    g_direct_ui_event_hook.store(nullptr, std::memory_order_release);
    g_direct_ui_scroll_event_hook.store(nullptr, std::memory_order_release);
    g_direct_ui_scroll_metrics_ready.store(
        false, std::memory_order_release);
    g_direct_ui_event_thread_id.store(0, std::memory_order_release);
    return 0;
}

[[nodiscard]] bool StartDirectUiEventMonitor() noexcept {
    g_direct_ui_event_stop =
        ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_direct_ui_event_ready =
        ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_direct_ui_event_stop == nullptr ||
        g_direct_ui_event_ready == nullptr) {
        if (g_direct_ui_event_stop != nullptr) {
            static_cast<void>(::CloseHandle(g_direct_ui_event_stop));
        }
        if (g_direct_ui_event_ready != nullptr) {
            static_cast<void>(::CloseHandle(g_direct_ui_event_ready));
        }
        g_direct_ui_event_stop = nullptr;
        g_direct_ui_event_ready = nullptr;
        return false;
    }
    g_direct_ui_event_thread = ::CreateThread(
        nullptr, 0, &DirectUiEventMonitorThread, nullptr, 0, nullptr);
    if (g_direct_ui_event_thread == nullptr) {
        static_cast<void>(::CloseHandle(g_direct_ui_event_stop));
        static_cast<void>(::CloseHandle(g_direct_ui_event_ready));
        g_direct_ui_event_stop = nullptr;
        g_direct_ui_event_ready = nullptr;
        return false;
    }
    const DWORD wait_result = ::WaitForSingleObject(
        g_direct_ui_event_ready,
        kDirectUiEventMonitorStartMilliseconds);
    return wait_result == WAIT_OBJECT_0 &&
        g_direct_ui_event_hook.load(std::memory_order_acquire) != nullptr &&
        g_direct_ui_scroll_event_hook.load(
            std::memory_order_acquire) != nullptr &&
        g_direct_ui_scroll_metrics_ready.load(std::memory_order_acquire) &&
        g_direct_ui_event_thread_id.load(std::memory_order_acquire) != 0;
}

[[nodiscard]] bool StopDirectUiEventMonitor() noexcept {
    if (g_direct_ui_event_thread == nullptr) {
        return true;
    }
    static_cast<void>(::SetEvent(g_direct_ui_event_stop));
    const DWORD wait_result = ::WaitForSingleObject(
        g_direct_ui_event_thread,
        kDirectUiEventMonitorStopMilliseconds);
    if (wait_result != WAIT_OBJECT_0) {
        return false;
    }
    static_cast<void>(::CloseHandle(g_direct_ui_event_thread));
    static_cast<void>(::CloseHandle(g_direct_ui_event_stop));
    static_cast<void>(::CloseHandle(g_direct_ui_event_ready));
    g_direct_ui_event_thread = nullptr;
    g_direct_ui_event_stop = nullptr;
    g_direct_ui_event_ready = nullptr;
    return g_direct_ui_event_hook.load(std::memory_order_acquire) == nullptr &&
        g_direct_ui_scroll_event_hook.load(
            std::memory_order_acquire) == nullptr &&
        !g_direct_ui_scroll_metrics_ready.load(std::memory_order_acquire) &&
        g_direct_ui_event_thread_id.load(std::memory_order_acquire) == 0;
}

[[nodiscard]] COLORREF BlendColor(
    const COLORREF background,
    const COLORREF foreground,
    const unsigned foreground_weight) noexcept {
    const unsigned background_weight = 100U - foreground_weight;
    return RGB(
        (GetRValue(background) * background_weight +
         GetRValue(foreground) * foreground_weight) /
            100U,
        (GetGValue(background) * background_weight +
         GetGValue(foreground) * foreground_weight) /
            100U,
        (GetBValue(background) * background_weight +
         GetBValue(foreground) * foreground_weight) /
            100U);
}

void DrawScrollbarThumb(
    const HDC device_context,
    const RECT& bounds,
    const int state,
    const COLORREF background,
    const COLORREF foreground) noexcept {
    const int available_width = bounds.right - bounds.left;
    const int available_height = bounds.bottom - bounds.top;
    if (available_width <= 0 || available_height <= 0) {
        return;
    }
    const bool interactive = state == SCRBS_HOT || state == SCRBS_PRESSED;
    const int thickness = std::min(interactive ? 7 : 4, available_width);
    const int center = bounds.left + available_width / 2;
    const int vertical_inset = available_height >= 8 ? 2 : 0;
    RECT thumb{
        center - thickness / 2,
        bounds.top + vertical_inset,
        center - thickness / 2 + thickness,
        bounds.bottom - vertical_inset};
    if (thumb.bottom <= thumb.top) {
        return;
    }
    const COLORREF color = BlendColor(
        background, foreground, interactive ? 100U : 90U);
    const HRGN capsule = ::CreateRoundRectRgn(
        thumb.left,
        thumb.top,
        thumb.right + 1,
        thumb.bottom + 1,
        thickness,
        thickness);
    const HBRUSH brush = ::CreateSolidBrush(color);
    if (capsule != nullptr && brush != nullptr) {
        static_cast<void>(::FillRgn(device_context, capsule, brush));
    } else {
        static_cast<void>(FillRectWithColor(
            g_original_fill_rect,
            device_context,
            &thumb,
            static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH)),
            color));
    }
    if (brush != nullptr) {
        static_cast<void>(::DeleteObject(brush));
    }
    if (capsule != nullptr) {
        static_cast<void>(::DeleteObject(capsule));
    }
}

[[nodiscard]] bool IsCursorOverWindow(
    const HWND window) noexcept {
    RECT bounds{};
    POINT cursor{};
    return ::GetWindowRect(window, &bounds) &&
        ::GetCursorPos(&cursor) &&
        ::PtInRect(&bounds, cursor) != FALSE;
}

void PaintDirectUiScrollbarOverlay(
    const HWND overlay,
    const HDC device_context) noexcept {
    RECT bounds{};
    if (device_context == nullptr ||
        !::GetClientRect(overlay, &bounds)) {
        return;
    }
    const std::uint32_t argb = g_color.load(std::memory_order_acquire);
    const COLORREF background = ToColorRef(argb);
    const COLORREF foreground = ContrastingTextColor(argb);
    static_cast<void>(FillRectWithColor(
        g_original_fill_rect,
        device_context,
        &bounds,
        static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)),
        background));

    const auto packed_metrics = static_cast<std::uint32_t>(
        ::GetWindowLongPtrW(overlay, 0));
    const std::uint16_t scroll_percent = LOWORD(packed_metrics);
    const std::uint16_t view_percent = HIWORD(packed_metrics);
    const int height = bounds.bottom - bounds.top;
    if (height <= 0 || scroll_percent > 10000U ||
        view_percent == 0 || view_percent >= 10000U) {
        return;
    }
    const UINT dpi = std::max<UINT>(
        ::GetDpiForWindow(overlay), USER_DEFAULT_SCREEN_DPI);
    const int minimum_thumb_height =
        ::MulDiv(24, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    const int thumb_height = std::clamp(
        static_cast<int>(
            (static_cast<std::int64_t>(height) * view_percent + 5000) /
            10000),
        std::min(minimum_thumb_height, height),
        height);
    const int travel = height - thumb_height;
    const int thumb_top = bounds.top + static_cast<int>(
        (static_cast<std::int64_t>(travel) * scroll_percent + 5000) /
        10000);
    RECT thumb{
        bounds.left,
        thumb_top,
        bounds.right,
        thumb_top + thumb_height};
    const bool hovered = IsCursorOverWindow(overlay);
    const bool pressed = hovered &&
        (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    DrawScrollbarThumb(
        device_context,
        thumb,
        pressed ? SCRBS_PRESSED : (hovered ? SCRBS_HOT : SCRBS_NORMAL),
        background,
        foreground);
}

LRESULT CALLBACK DirectUiScrollbarOverlayWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        const auto* const create =
            reinterpret_cast<const CREATESTRUCTW*>(lparam);
        if (create == nullptr || create->lpCreateParams == nullptr) {
            return FALSE;
        }
        static_cast<void>(::SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams)));
    }
    if (message == WM_ERASEBKGND && wparam != 0) {
        PaintDirectUiScrollbarOverlay(
            window, reinterpret_cast<HDC>(wparam));
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC device_context = ::BeginPaint(window, &paint);
        if (device_context != nullptr) {
            PaintDirectUiScrollbarOverlay(window, device_context);
            static_cast<void>(::EndPaint(window, &paint));
        }
        return 0;
    }
    if (message == WM_NCHITTEST) {
        static_cast<void>(::SetTimer(
            window,
            kDirectUiScrollbarPaintTimerId,
            kTreeViewScrollbarFrameMilliseconds,
            nullptr));
        static_cast<void>(::RedrawWindow(
            window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        return HTTRANSPARENT;
    }
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (message == WM_TIMER &&
        wparam == kDirectUiScrollbarPaintTimerId) {
        static_cast<void>(::RedrawWindow(
            window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        if (!IsCursorOverWindow(window)) {
            static_cast<void>(::KillTimer(
                window, kDirectUiScrollbarPaintTimerId));
        }
        return 0;
    }
    if (message == WM_NCDESTROY) {
        static_cast<void>(::KillTimer(
            window, kDirectUiScrollbarPaintTimerId));
        const HWND direct_ui_window = reinterpret_cast<HWND>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (direct_ui_window != nullptr &&
            reinterpret_cast<HWND>(::GetPropW(
                direct_ui_window,
                kDirectUiScrollbarOverlayProperty)) == window) {
            static_cast<void>(::RemovePropW(
                direct_ui_window,
                kDirectUiScrollbarOverlayProperty));
        }
        static_cast<void>(::SetWindowLongPtrW(
            window, GWLP_USERDATA, 0));
        static_cast<void>(::SetWindowLongPtrW(window, 0, 0));
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

BOOL CALLBACK RegisterDirectUiScrollbarOverlayClass(
    PINIT_ONCE,
    PVOID,
    PVOID*) noexcept {
    const HMODULE current_module = CurrentAgentModule();
    if (current_module == nullptr) {
        return FALSE;
    }
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &DirectUiScrollbarOverlayWindowProc;
    window_class.cbWndExtra = sizeof(LONG_PTR);
    window_class.hInstance = current_module;
    window_class.lpszClassName = kDirectUiScrollbarOverlayClass;
    return ::RegisterClassExW(&window_class) != 0 ||
        ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void DestroyDirectUiScrollbarOverlay(
    const HWND direct_ui_window) noexcept {
    const HWND overlay = reinterpret_cast<HWND>(::GetPropW(
        direct_ui_window, kDirectUiScrollbarOverlayProperty));
    if (overlay == nullptr || !::IsWindow(overlay)) {
        static_cast<void>(::RemovePropW(
            direct_ui_window, kDirectUiScrollbarOverlayProperty));
        return;
    }
    DWORD process_id = 0;
    const DWORD thread_id =
        ::GetWindowThreadProcessId(overlay, &process_id);
    if (process_id == ::GetCurrentProcessId() &&
        thread_id == ::GetCurrentThreadId()) {
        static_cast<void>(::DestroyWindow(overlay));
    } else {
        static_cast<void>(::PostMessageW(overlay, WM_CLOSE, 0, 0));
    }
    if (::IsWindow(direct_ui_window)) {
        static_cast<void>(::RedrawWindow(
            direct_ui_window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
    }
}

void UpdateDirectUiScrollbarOverlay(
    const HWND direct_ui_window,
    const bool visible,
    const std::uint16_t scroll_percent,
    const std::uint16_t view_percent) noexcept {
    if (!visible || !g_enabled.load(std::memory_order_acquire) ||
        !g_custom_scrollbar_enabled.load(std::memory_order_acquire) ||
        !::IsWindow(direct_ui_window) ||
        !::IsWindowVisible(direct_ui_window) ||
        !ClassNameEquals(direct_ui_window, L"DirectUIHWND") ||
        scroll_percent > 10000U || view_percent == 0 ||
        view_percent >= 10000U) {
        DestroyDirectUiScrollbarOverlay(direct_ui_window);
        return;
    }
    const HWND root = ::GetAncestor(direct_ui_window, GA_ROOT);
    RECT bounds{};
    if (!IsCurrentProcessExplorerWindow(root) ||
        !::GetWindowRect(direct_ui_window, &bounds)) {
        DestroyDirectUiScrollbarOverlay(direct_ui_window);
        return;
    }
    static_cast<void>(::SetWindowSubclass(
        root,
        &ExplorerScrollbarOwnerSubclassProc,
        kExplorerScrollbarOwnerSubclassId,
        0));
    const int height = bounds.bottom - bounds.top;
    const UINT dpi = std::max<UINT>(
        ::GetDpiForWindow(direct_ui_window), USER_DEFAULT_SCREEN_DPI);
    const int width = std::clamp(
        ::GetSystemMetricsForDpi(SM_CXVSCROLL, dpi), 12, 24);
    if (height <= 0) {
        DestroyDirectUiScrollbarOverlay(direct_ui_window);
        return;
    }

    static INIT_ONCE registration = INIT_ONCE_STATIC_INIT;
    if (!::InitOnceExecuteOnce(
            &registration,
            &RegisterDirectUiScrollbarOverlayClass,
            nullptr,
            nullptr)) {
        return;
    }
    HWND overlay = reinterpret_cast<HWND>(::GetPropW(
        direct_ui_window, kDirectUiScrollbarOverlayProperty));
    if (overlay != nullptr &&
        (!::IsWindow(overlay) ||
         !ClassNameEquals(overlay, kDirectUiScrollbarOverlayClass) ||
         ::GetWindow(overlay, GW_OWNER) != root)) {
        DestroyDirectUiScrollbarOverlay(direct_ui_window);
        overlay = nullptr;
    }
    if (overlay == nullptr) {
        overlay = ::CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
            kDirectUiScrollbarOverlayClass,
            L"",
            WS_POPUP,
            bounds.right - width,
            bounds.top,
            width,
            height,
            root,
            nullptr,
            CurrentAgentModule(),
            direct_ui_window);
        if (overlay == nullptr) {
            return;
        }
        if (!::SetPropW(
                direct_ui_window,
                kDirectUiScrollbarOverlayProperty,
                overlay)) {
            static_cast<void>(::DestroyWindow(overlay));
            return;
        }
    }
    const std::uint32_t packed_metrics =
        MAKELONG(scroll_percent, view_percent);
    static_cast<void>(::SetWindowLongPtrW(
        overlay, 0, static_cast<LONG_PTR>(packed_metrics)));
    const LONG_PTR root_extended_style =
        ::GetWindowLongPtrW(root, GWL_EXSTYLE);
    const HWND insert_after =
        (root_extended_style & WS_EX_TOPMOST) != 0
        ? HWND_TOPMOST
        : HWND_NOTOPMOST;
    static_cast<void>(::SetWindowPos(
        overlay,
        insert_after,
        bounds.right - width,
        bounds.top,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW));
    static_cast<void>(::RedrawWindow(
        overlay,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
}

void ForgetTrackedTreeView(const HWND window) noexcept {
    ::AcquireSRWLockExclusive(&g_tree_views_lock);
    const auto iterator = std::find_if(
        g_tree_views.begin(),
        g_tree_views.begin() + g_tree_view_count,
        [window](const TrackedTreeView& entry) {
            return entry.window == window;
        });
    if (iterator != g_tree_views.begin() + g_tree_view_count) {
        std::move(
            iterator + 1,
            g_tree_views.begin() + g_tree_view_count,
            iterator);
        --g_tree_view_count;
        g_tree_views[g_tree_view_count] = {};
    }
    ::ReleaseSRWLockExclusive(&g_tree_views_lock);
}

[[nodiscard]] bool IsCursorOverTreeViewScrollbar(
    const HWND window) noexcept {
    SCROLLBARINFO information{};
    information.cbSize = sizeof(information);
    POINT cursor{};
    return ::GetScrollBarInfo(window, OBJID_VSCROLL, &information) &&
           (information.rgstate[0] &
            (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) == 0 &&
           ::GetCursorPos(&cursor) &&
           ::PtInRect(&information.rcScrollBar, cursor) != FALSE;
}

void PaintTreeViewScrollbarOverlay(
    const HWND overlay,
    const HDC device_context) noexcept {
    const HWND tree_view = reinterpret_cast<HWND>(
        ::GetWindowLongPtrW(overlay, GWLP_USERDATA));
    RECT bounds{};
    if (tree_view == nullptr || device_context == nullptr ||
        !::GetClientRect(overlay, &bounds)) {
        return;
    }
    const std::uint32_t argb = g_color.load(std::memory_order_acquire);
    const COLORREF background = ToColorRef(argb);
    const COLORREF foreground = ContrastingTextColor(argb);
    static_cast<void>(FillRectWithColor(
        g_original_fill_rect,
        device_context,
        &bounds,
        static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)),
        background));

    SCROLLBARINFO information{};
    information.cbSize = sizeof(information);
    if (!::GetScrollBarInfo(tree_view, OBJID_VSCROLL, &information) ||
        (information.rgstate[0] &
         (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) != 0) {
        return;
    }
    RECT thumb = bounds;
    thumb.top = std::clamp<LONG>(
        static_cast<LONG>(information.xyThumbTop),
        bounds.top,
        bounds.bottom);
    thumb.bottom = std::clamp<LONG>(
        static_cast<LONG>(information.xyThumbBottom),
        thumb.top,
        bounds.bottom);
    if (thumb.bottom > thumb.top) {
        const bool hovered = IsCursorOverTreeViewScrollbar(tree_view);
        const bool pressed = hovered &&
                             (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        DrawScrollbarThumb(
            device_context,
            thumb,
            pressed ? SCRBS_PRESSED
                    : (hovered ? SCRBS_HOT : SCRBS_NORMAL),
            background,
            foreground);
    }
}

LRESULT CALLBACK TreeViewScrollbarOverlayWindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        const auto* const create =
            reinterpret_cast<const CREATESTRUCTW*>(lparam);
        if (create == nullptr || create->lpCreateParams == nullptr) {
            return FALSE;
        }
        static_cast<void>(::SetWindowLongPtrW(
            window,
            GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams)));
    }
    if (message == WM_ERASEBKGND && wparam != 0) {
        PaintTreeViewScrollbarOverlay(
            window, reinterpret_cast<HDC>(wparam));
        return 1;
    }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        const HDC device_context = ::BeginPaint(window, &paint);
        if (device_context != nullptr) {
            PaintTreeViewScrollbarOverlay(window, device_context);
            static_cast<void>(::EndPaint(window, &paint));
        }
        return 0;
    }
    if (message == WM_NCHITTEST) {
        static_cast<void>(::SetTimer(
            window,
            kTreeViewScrollbarPaintTimerId,
            kTreeViewScrollbarFrameMilliseconds,
            nullptr));
        static_cast<void>(::RedrawWindow(
            window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        return HTTRANSPARENT;
    }
    if (message == WM_MOUSEACTIVATE) {
        return MA_NOACTIVATE;
    }
    if (message == WM_TIMER &&
        wparam == kTreeViewScrollbarPaintTimerId) {
        const HWND tree_view = reinterpret_cast<HWND>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        static_cast<void>(::RedrawWindow(
            window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
        if (!IsCursorOverTreeViewScrollbar(tree_view)) {
            static_cast<void>(::KillTimer(
                window, kTreeViewScrollbarPaintTimerId));
        }
        return 0;
    }
    if (message == WM_NCDESTROY) {
        static_cast<void>(::KillTimer(
            window, kTreeViewScrollbarPaintTimerId));
        const HWND tree_view = reinterpret_cast<HWND>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (tree_view != nullptr &&
            reinterpret_cast<HWND>(::GetPropW(
                tree_view, kTreeViewScrollbarOverlayProperty)) == window) {
            static_cast<void>(::RemovePropW(
                tree_view, kTreeViewScrollbarOverlayProperty));
        }
        static_cast<void>(::SetWindowLongPtrW(
            window, GWLP_USERDATA, 0));
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

BOOL CALLBACK RegisterTreeViewScrollbarOverlayClass(
    PINIT_ONCE,
    PVOID,
    PVOID*) noexcept {
    const HMODULE current_module = CurrentAgentModule();
    if (current_module == nullptr) {
        return FALSE;
    }
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &TreeViewScrollbarOverlayWindowProc;
    window_class.hInstance = current_module;
    window_class.lpszClassName = kTreeViewScrollbarOverlayClass;
    return ::RegisterClassExW(&window_class) != 0 ||
           ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

void DestroyTreeViewScrollbarOverlay(const HWND tree_view) noexcept {
    const HWND overlay = reinterpret_cast<HWND>(::GetPropW(
        tree_view, kTreeViewScrollbarOverlayProperty));
    if (overlay == nullptr || !::IsWindow(overlay)) {
        static_cast<void>(::RemovePropW(
            tree_view, kTreeViewScrollbarOverlayProperty));
        return;
    }
    DWORD process_id = 0;
    const DWORD thread_id =
        ::GetWindowThreadProcessId(overlay, &process_id);
    if (process_id == ::GetCurrentProcessId() &&
        thread_id == ::GetCurrentThreadId()) {
        static_cast<void>(::DestroyWindow(overlay));
    } else {
        static_cast<void>(::PostMessageW(overlay, WM_CLOSE, 0, 0));
    }
    if (::IsWindow(tree_view)) {
        static_cast<void>(::RedrawWindow(
            tree_view,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_UPDATENOW));
    }
}

BOOL CALLBACK UpdateExplorerScrollbarOverlayForChild(
    const HWND window,
    LPARAM) noexcept {
    if (ClassNameEquals(window, WC_TREEVIEWW)) {
        UpdateTreeViewScrollbarOverlay(window);
    } else if (ClassNameEquals(window, L"DirectUIHWND")) {
        const HWND overlay = reinterpret_cast<HWND>(::GetPropW(
            window, kDirectUiScrollbarOverlayProperty));
        if (overlay != nullptr && ::IsWindow(overlay)) {
            const auto packed_metrics = static_cast<std::uint32_t>(
                ::GetWindowLongPtrW(overlay, 0));
            UpdateDirectUiScrollbarOverlay(
                window,
                true,
                LOWORD(packed_metrics),
                HIWORD(packed_metrics));
        } else {
            RequestDirectUiScrollbarMetrics(window);
        }
    }
    return TRUE;
}

LRESULT CALLBACK ExplorerScrollbarOwnerSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR,
    const DWORD_PTR) noexcept {
    if (message == WM_NULL) {
        ApplyPendingDirectUiScrollbarMetrics(window);
    }
    if (message == WM_NCDESTROY) {
        static_cast<void>(::RemoveWindowSubclass(
            window,
            &ExplorerScrollbarOwnerSubclassProc,
            kExplorerScrollbarOwnerSubclassId));
        return ::DefSubclassProc(window, message, wparam, lparam);
    }
    const LRESULT result =
        ::DefSubclassProc(window, message, wparam, lparam);
    if (g_enabled.load(std::memory_order_acquire) &&
        g_custom_scrollbar_enabled.load(std::memory_order_acquire) &&
        (message == WM_MOVE || message == WM_SIZE ||
         message == WM_WINDOWPOSCHANGED || message == WM_SHOWWINDOW ||
         message == WM_DPICHANGED || message == WM_THEMECHANGED)) {
        static_cast<void>(::EnumChildWindows(
            window,
            &UpdateExplorerScrollbarOverlayForChild,
            0));
    }
    return result;
}

void UpdateTreeViewScrollbarOverlay(const HWND tree_view) noexcept {
    if (!::IsWindow(tree_view)) {
        DestroyTreeViewScrollbarOverlay(tree_view);
        return;
    }
    if (!g_enabled.load(std::memory_order_acquire) ||
        !g_custom_scrollbar_enabled.load(std::memory_order_acquire)) {
        const HWND root = ::GetAncestor(tree_view, GA_ROOT);
        if (root != nullptr) {
            static_cast<void>(::RemoveWindowSubclass(
                root,
                &ExplorerScrollbarOwnerSubclassProc,
                kExplorerScrollbarOwnerSubclassId));
        }
        DestroyTreeViewScrollbarOverlay(tree_view);
        return;
    }
    SCROLLBARINFO information{};
    information.cbSize = sizeof(information);
    const bool visible =
        ::IsWindowVisible(tree_view) &&
        ::GetScrollBarInfo(tree_view, OBJID_VSCROLL, &information) &&
        (information.rgstate[0] &
         (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) == 0 &&
        information.rcScrollBar.right > information.rcScrollBar.left &&
        information.rcScrollBar.bottom > information.rcScrollBar.top;
    HWND overlay = reinterpret_cast<HWND>(::GetPropW(
        tree_view, kTreeViewScrollbarOverlayProperty));
    if (overlay != nullptr &&
        (!::IsWindow(overlay) ||
         !ClassNameEquals(overlay, kTreeViewScrollbarOverlayClass))) {
        static_cast<void>(::RemovePropW(
            tree_view, kTreeViewScrollbarOverlayProperty));
        overlay = nullptr;
    }
    if (!visible) {
        if (overlay != nullptr) {
            static_cast<void>(::ShowWindow(overlay, SW_HIDE));
        }
        return;
    }

    const HWND root = ::GetAncestor(tree_view, GA_ROOT);
    if (!IsCurrentProcessExplorerWindow(root)) {
        return;
    }
    static_cast<void>(::SetWindowSubclass(
        root,
        &ExplorerScrollbarOwnerSubclassProc,
        kExplorerScrollbarOwnerSubclassId,
        0));
    if (overlay != nullptr && ::GetWindow(overlay, GW_OWNER) != root) {
        DestroyTreeViewScrollbarOverlay(tree_view);
        overlay = nullptr;
    }
    static INIT_ONCE registration = INIT_ONCE_STATIC_INIT;
    if (!::InitOnceExecuteOnce(
            &registration,
            &RegisterTreeViewScrollbarOverlayClass,
            nullptr,
            nullptr)) {
        return;
    }
    const int width =
        information.rcScrollBar.right - information.rcScrollBar.left;
    const int height =
        information.rcScrollBar.bottom - information.rcScrollBar.top;
    if (width <= 0 || height <= 0) {
        return;
    }
    if (overlay == nullptr) {
        overlay = ::CreateWindowExW(
            WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
            kTreeViewScrollbarOverlayClass,
            L"",
            WS_POPUP,
            information.rcScrollBar.left,
            information.rcScrollBar.top,
            width,
            height,
            root,
            nullptr,
            CurrentAgentModule(),
            tree_view);
        if (overlay == nullptr) {
            return;
        }
        if (!::SetPropW(
                tree_view, kTreeViewScrollbarOverlayProperty, overlay)) {
            static_cast<void>(::DestroyWindow(overlay));
            return;
        }
    }
    const LONG_PTR root_extended_style =
        ::GetWindowLongPtrW(root, GWL_EXSTYLE);
    const HWND insert_after =
        (root_extended_style & WS_EX_TOPMOST) != 0
        ? HWND_TOPMOST
        : HWND_NOTOPMOST;
    static_cast<void>(::SetWindowPos(
        overlay,
        insert_after,
        information.rcScrollBar.left,
        information.rcScrollBar.top,
        width,
        height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW));
    static_cast<void>(::RedrawWindow(
        overlay,
        nullptr,
        nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW));
}

void PaintTreeViewScrollbar(const HWND window) noexcept {
    UpdateTreeViewScrollbarOverlay(window);
}

[[nodiscard]] bool ShouldRepaintTreeViewScrollbar(
    const UINT message) noexcept {
    return message == WM_PAINT || message == WM_NCPAINT ||
           message == WM_NCACTIVATE || message == WM_THEMECHANGED ||
           message == WM_SETTINGCHANGE || message == WM_STYLECHANGED ||
           message == WM_SIZE || message == WM_WINDOWPOSCHANGED ||
           message == WM_VSCROLL || message == WM_MOUSEWHEEL ||
           message == WM_MOUSEMOVE ||
           message == WM_NCHITTEST || message == WM_SETCURSOR ||
           message == WM_NCMOUSEMOVE || message == WM_NCMOUSELEAVE ||
           message == WM_NCLBUTTONDOWN || message == WM_NCLBUTTONUP;
}

LRESULT CALLBACK ExplorerTreeViewSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR,
    const DWORD_PTR) noexcept {
    if (message == WM_NCDESTROY) {
        DestroyTreeViewScrollbarOverlay(window);
        static_cast<void>(::RemoveWindowSubclass(
            window,
            &ExplorerTreeViewSubclassProc,
            kExplorerTreeViewSubclassId));
        ForgetTrackedTreeView(window);
        return ::DefSubclassProc(window, message, wparam, lparam);
    }
    const LRESULT result =
        ::DefSubclassProc(window, message, wparam, lparam);
    if (ShouldRepaintTreeViewScrollbar(message)) {
        PaintTreeViewScrollbar(window);
    }
    return result;
}

void EnsureTreeViewSubclass(const HWND window) noexcept {
    DWORD process_id = 0;
    const DWORD thread_id =
        ::GetWindowThreadProcessId(window, &process_id);
    if (thread_id == 0 || process_id != ::GetCurrentProcessId() ||
        thread_id != ::GetCurrentThreadId()) {
        return;
    }
    if (::SetWindowSubclass(
            window,
            &ExplorerTreeViewSubclassProc,
            kExplorerTreeViewSubclassId,
            0)) {
        PaintTreeViewScrollbar(window);
    }
}

BOOL CALLBACK DetachEnumeratedTreeView(
    const HWND window,
    LPARAM) noexcept {
    if (::GetWindowThreadProcessId(window, nullptr) !=
        ::GetCurrentThreadId()) {
        return TRUE;
    }
    if (ClassNameEquals(window, WC_TREEVIEWW)) {
        DestroyTreeViewScrollbarOverlay(window);
        static_cast<void>(::RemoveWindowSubclass(
            window,
            &ExplorerTreeViewSubclassProc,
            kExplorerTreeViewSubclassId));
    } else if (ClassNameEquals(
                   window, kTreeViewScrollbarOverlayClass)) {
        static_cast<void>(::DestroyWindow(window));
    } else if (ClassNameEquals(window, L"DirectUIHWND")) {
        DetachExplorerDirectUiSubclass(window);
    } else if (ClassNameEquals(window, kDirectUiOverlayClass)) {
        static_cast<void>(::KillTimer(
            window, kDirectUiTransitionTimerId));
        static_cast<void>(::DestroyWindow(window));
    }
    return TRUE;
}

[[nodiscard]] bool IsDui70Caller(const void* address) noexcept {
    const auto value = reinterpret_cast<std::uintptr_t>(address);
    return g_dui70_begin != 0 && value >= g_dui70_begin &&
           value < g_dui70_end;
}

[[nodiscard]] bool IsExplorerDirectUiDeviceContext(
    const HDC device_context) noexcept {
    const HWND paint_window = ::WindowFromDC(device_context);
    if (paint_window == nullptr ||
        !ClassNameEquals(paint_window, L"DirectUIHWND")) {
        return false;
    }
    return IsCurrentProcessExplorerWindow(
        ::GetAncestor(paint_window, GA_ROOT));
}

[[nodiscard]] bool IsDarkNeutralBrush(const HBRUSH brush) noexcept {
    LOGBRUSH description{};
    if (brush == nullptr ||
        ::GetObjectW(brush, sizeof(description), &description) !=
            sizeof(description) ||
        description.lbStyle != BS_SOLID) {
        return false;
    }
    const BYTE red = GetRValue(description.lbColor);
    const BYTE green = GetGValue(description.lbColor);
    const BYTE blue = GetBValue(description.lbColor);
    return red == green && green == blue && red >= 16 && red <= 32;
}

[[nodiscard]] UINT ExplorerDpiForCurrentThread() noexcept {
    const DWORD thread_id = ::GetCurrentThreadId();
    UINT dpi = 0;
    ::AcquireSRWLockShared(&g_windows_lock);
    for (std::size_t index = 0; index < g_window_count; ++index) {
        DWORD process_id = 0;
        if (::GetWindowThreadProcessId(
                g_windows[index].window, &process_id) == thread_id &&
            process_id == ::GetCurrentProcessId()) {
            dpi = ::GetDpiForWindow(g_windows[index].window);
            break;
        }
    }
    ::ReleaseSRWLockShared(&g_windows_lock);
    return dpi;
}

[[nodiscard]] bool IsHeaderHotState(const int state) noexcept {
    return state == HIS_HOT || state == HIS_SORTEDHOT ||
           state == HIS_ICONHOT || state == HIS_ICONSORTEDHOT;
}

[[nodiscard]] bool IsHeaderPressedState(const int state) noexcept {
    return state == HIS_PRESSED || state == HIS_SORTEDPRESSED ||
           state == HIS_ICONPRESSED || state == HIS_ICONSORTEDPRESSED;
}

[[nodiscard]] bool PaintExplorerHeaderThemePart(
    const HTHEME theme,
    const HDC device_context,
    const int part,
    const int state,
    const RECT* const bounds,
    const RECT* const clip) noexcept {
    static thread_local HTHEME header_theme = nullptr;
    static thread_local LONG header_height = 0;

    if (!g_enabled.load(std::memory_order_acquire) || bounds == nullptr ||
        g_original_fill_rect == nullptr) {
        return false;
    }
    const UINT dpi = ExplorerDpiForCurrentThread();
    if (dpi == 0) {
        return false;
    }
    const LONG width = bounds->right - bounds->left;
    const LONG height = bounds->bottom - bounds->top;
    const LONG minimum_height = ::MulDiv(18, dpi, USER_DEFAULT_SCREEN_DPI);
    const LONG maximum_height = ::MulDiv(34, dpi, USER_DEFAULT_SCREEN_DPI);
    const LONG minimum_width = ::MulDiv(160, dpi, USER_DEFAULT_SCREEN_DPI);

    if (part == 0 && state == 1 && bounds->left == 0 &&
        bounds->top == 0 && width >= minimum_width &&
        height >= minimum_height && height <= maximum_height) {
        header_theme = theme;
        header_height = height;
    } else if (theme != header_theme || part != HP_HEADERITEM ||
               bounds->top != 0 || height != header_height || width <= 0) {
        return false;
    }

    const int saved = ::SaveDC(device_context);
    if (clip != nullptr) {
        static_cast<void>(::IntersectClipRect(
            device_context,
            clip->left,
            clip->top,
            clip->right,
            clip->bottom));
    }
    const std::uint32_t argb = g_color.load(std::memory_order_acquire);
    const COLORREF background = ToColorRef(argb);
    const COLORREF foreground = ContrastingTextColor(argb);
    COLORREF surface = background;
    if (part == HP_HEADERITEM) {
        if (IsHeaderPressedState(state)) {
            surface = BlendColor(background, foreground, 18U);
        } else if (IsHeaderHotState(state)) {
            surface = BlendColor(background, foreground, 10U);
        }
    }
    static_cast<void>(FillRectWithColor(
        g_original_fill_rect,
        device_context,
        bounds,
        static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)),
        surface));
    if (part == HP_HEADERITEM && bounds->right > bounds->left) {
        RECT separator{
            bounds->right - 1,
            bounds->top,
            bounds->right,
            bounds->bottom};
        static_cast<void>(FillRectWithColor(
            g_original_fill_rect,
            device_context,
            &separator,
            static_cast<HBRUSH>(::GetStockObject(GRAY_BRUSH)),
            BlendColor(background, foreground, 28U)));
    }
    if (saved != 0) {
        static_cast<void>(::RestoreDC(device_context, saved));
    }
    return true;
}

HRESULT WINAPI HookedDrawThemeBackground(
    const HTHEME theme,
    const HDC device_context,
    const int part,
    const int state,
    const RECT* const bounds,
    const RECT* const clip) noexcept {
    if (PaintExplorerHeaderThemePart(
            theme, device_context, part, state, bounds, clip)) {
        return S_OK;
    }
    const auto original = g_original_draw_theme_background;
    return original != nullptr
        ? original(theme, device_context, part, state, bounds, clip)
        : E_UNEXPECTED;
}

HRESULT WINAPI HookedDrawThemeBackgroundEx(
    const HTHEME theme,
    const HDC device_context,
    const int part,
    const int state,
    const RECT* const bounds,
    const DTBGOPTS* const options) noexcept {
    const RECT* clip = options != nullptr &&
            (options->dwFlags & DTBG_CLIPRECT) != 0
        ? &options->rcClip
        : nullptr;
    if (PaintExplorerHeaderThemePart(
            theme, device_context, part, state, bounds, clip)) {
        return S_OK;
    }
    const auto original = g_original_draw_theme_background_ex;
    return original != nullptr
        ? original(theme, device_context, part, state, bounds, options)
        : E_UNEXPECTED;
}

int WINAPI HookedFillRect(
    const HDC device_context,
    const RECT* rectangle,
    const HBRUSH brush) noexcept {
    const auto original = g_original_fill_rect;
    if (original == nullptr) {
        return 0;
    }
    if (g_enabled.load(std::memory_order_acquire) && rectangle != nullptr &&
        IsDarkNeutralBrush(brush) &&
        IsExplorerDirectUiDeviceContext(device_context)) {
        // The first paint of a newly-created folder view is issued outside
        // DUI70, before its composition surface inherits the window accent.
        // Paint the configured color directly into the window DC so that
        // first frame cannot expose Explorer's stock #191919 canvas.
        return FillRectWithOpaqueColor(
            original,
            device_context,
            rectangle,
            brush,
            g_color.load(std::memory_order_acquire));
    }
    if (g_enabled.load(std::memory_order_acquire) && rectangle != nullptr &&
        IsDui70Caller(_ReturnAddress()) && IsDarkNeutralBrush(brush)) {
        // DirectUI paints its dark canvas into a transparent composition
        // surface. Copy an opaque pixel so Explorer cannot add the stock
        // neutral RGB value over the configured window accent.
        return FillRectWithOpaqueColor(
            original,
            device_context,
            rectangle,
            brush,
            g_color.load(std::memory_order_acquire));
    }
    return original(device_context, rectangle, brush);
}

HRESULT WINAPI HookedDwmSetWindowAttribute(
    const HWND window,
    const DWORD attribute,
    const LPCVOID value,
    const DWORD value_size) noexcept {
    const auto original = g_original_dwm_set_window_attribute;
    if (original == nullptr) {
        return E_UNEXPECTED;
    }
    const HRESULT result = original(window, attribute, value, value_size);
    if (!g_enabled.load(std::memory_order_acquire) ||
        !IsCurrentProcessExplorerWindow(window)) {
        return result;
    }
    const std::uint32_t color = g_color.load(std::memory_order_acquire);
    if (TrackWindow(window)) {
        static_cast<void>(ApplyWindowSurface(window, color));
    }
    static_cast<void>(ApplyExplorerTreeViewColors(window, color));
    ApplyExplorerWinUiColorForCurrentThread();
    return result;
}

HRESULT WINAPI HookedDwmExtendFrameIntoClientArea(
    const HWND window,
    const MARGINS* margins) noexcept {
    const auto original = g_original_dwm_extend_frame;
    if (original == nullptr) {
        return E_UNEXPECTED;
    }
    const HRESULT result = original(window, margins);
    if (!g_enabled.load(std::memory_order_acquire) ||
        !IsCurrentProcessExplorerWindow(window)) {
        return result;
    }
    const std::uint32_t color = g_color.load(std::memory_order_acquire);
    if (TrackWindow(window)) {
        static_cast<void>(ApplyWindowSurface(window, color));
    }
    static_cast<void>(ApplyExplorerTreeViewColors(window, color));
    return result;
}

struct HookDefinition final {
    HMODULE module;
    const char* procedure;
    LPVOID detour;
    LPVOID* original;
    LPVOID target{nullptr};
    bool created{false};
    bool enabled{false};
};

[[nodiscard]] protocol::AgentResult InstallHooks() noexcept {
    ::AcquireSRWLockExclusive(&g_hooks_lock);
    if (g_hooks_installed) {
        ::ReleaseSRWLockExclusive(&g_hooks_lock);
        return protocol::AgentResult::success;
    }
    const HMODULE dwmapi = ::GetModuleHandleW(L"dwmapi.dll");
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    const HMODULE uxtheme = ::GetModuleHandleW(L"uxtheme.dll");
    const HMODULE dui70 = ::GetModuleHandleW(L"dui70.dll");
    g_set_window_composition_attribute = user32 != nullptr
        ? reinterpret_cast<SetWindowCompositionAttributeFunction>(
              ::GetProcAddress(user32, "SetWindowCompositionAttribute"))
        : nullptr;
    if (dwmapi == nullptr || user32 == nullptr || uxtheme == nullptr ||
        g_set_window_composition_attribute == nullptr) {
        g_set_window_composition_attribute = nullptr;
        ::ReleaseSRWLockExclusive(&g_hooks_lock);
        return protocol::AgentResult::adapter_unavailable;
    }

    if (dui70 != nullptr) {
        const auto* dos_header =
            reinterpret_cast<const IMAGE_DOS_HEADER*>(dui70);
        const auto* nt_headers = dos_header->e_magic == IMAGE_DOS_SIGNATURE
            ? reinterpret_cast<const IMAGE_NT_HEADERS*>(
                  reinterpret_cast<const std::byte*>(dui70) +
                  dos_header->e_lfanew)
            : nullptr;
        if (nt_headers == nullptr ||
            nt_headers->Signature != IMAGE_NT_SIGNATURE ||
            nt_headers->OptionalHeader.SizeOfImage == 0) {
            g_set_window_composition_attribute = nullptr;
            ::ReleaseSRWLockExclusive(&g_hooks_lock);
            return protocol::AgentResult::adapter_unavailable;
        }
        g_dui70_begin = reinterpret_cast<std::uintptr_t>(dui70);
        g_dui70_end =
            g_dui70_begin + nt_headers->OptionalHeader.SizeOfImage;
    }

    std::array<HookDefinition, 5> hooks{
        HookDefinition{
            dwmapi,
            "DwmSetWindowAttribute",
            reinterpret_cast<LPVOID>(&HookedDwmSetWindowAttribute),
            reinterpret_cast<LPVOID*>(
                &g_original_dwm_set_window_attribute)},
        HookDefinition{
            dwmapi,
            "DwmExtendFrameIntoClientArea",
            reinterpret_cast<LPVOID>(
                &HookedDwmExtendFrameIntoClientArea),
            reinterpret_cast<LPVOID*>(&g_original_dwm_extend_frame)},
        HookDefinition{
            user32,
            "FillRect",
            reinterpret_cast<LPVOID>(&HookedFillRect),
            reinterpret_cast<LPVOID*>(&g_original_fill_rect)},
        HookDefinition{
            uxtheme,
            "DrawThemeBackground",
            reinterpret_cast<LPVOID>(&HookedDrawThemeBackground),
            reinterpret_cast<LPVOID*>(&g_original_draw_theme_background)},
        HookDefinition{
            uxtheme,
            "DrawThemeBackgroundEx",
            reinterpret_cast<LPVOID>(&HookedDrawThemeBackgroundEx),
            reinterpret_cast<LPVOID*>(
                &g_original_draw_theme_background_ex)},
    };

    bool success = true;
    for (auto& hook : hooks) {
        if (hook.module == nullptr) {
            continue;
        }
        hook.target = reinterpret_cast<LPVOID>(
            ::GetProcAddress(hook.module, hook.procedure));
        if (hook.target == nullptr ||
            ::MH_CreateHook(hook.target, hook.detour, hook.original) != MH_OK) {
            success = false;
            break;
        }
        hook.created = true;
        if (::MH_EnableHook(hook.target) != MH_OK) {
            success = false;
            break;
        }
        hook.enabled = true;
    }
    if (success) {
        success = StartDirectUiEventMonitor();
    }
    if (!success) {
        static_cast<void>(StopDirectUiEventMonitor());
        for (auto iterator = hooks.rbegin(); iterator != hooks.rend();
             ++iterator) {
            if (iterator->enabled) {
                static_cast<void>(::MH_DisableHook(iterator->target));
            }
            if (iterator->created) {
                static_cast<void>(::MH_RemoveHook(iterator->target));
            }
        }
        g_original_dwm_set_window_attribute = nullptr;
        g_original_dwm_extend_frame = nullptr;
        g_original_fill_rect = nullptr;
        g_original_draw_theme_background = nullptr;
        g_original_draw_theme_background_ex = nullptr;
        g_set_window_composition_attribute = nullptr;
        g_dui70_begin = 0;
        g_dui70_end = 0;
        ::ReleaseSRWLockExclusive(&g_hooks_lock);
        return protocol::AgentResult::hook_failed;
    }
    g_hooks_installed = true;
    ::ReleaseSRWLockExclusive(&g_hooks_lock);
    return protocol::AgentResult::success;
}

[[nodiscard]] protocol::AgentResult RemoveHooks() noexcept {
    ::AcquireSRWLockExclusive(&g_hooks_lock);
    if (!g_hooks_installed) {
        ::ReleaseSRWLockExclusive(&g_hooks_lock);
        return protocol::AgentResult::success;
    }
    RemoveExplorerUiThreadHooks();
    const HMODULE dwmapi = ::GetModuleHandleW(L"dwmapi.dll");
    const HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    const HMODULE uxtheme = ::GetModuleHandleW(L"uxtheme.dll");
    const bool fill_rect_hooked = g_original_fill_rect != nullptr;
    struct Procedure final {
        HMODULE module;
        const char* name;
        bool installed;
    };
    const bool draw_theme_hooked =
        g_original_draw_theme_background != nullptr;
    const bool draw_theme_ex_hooked =
        g_original_draw_theme_background_ex != nullptr;
    const std::array<Procedure, 5> procedures{
        Procedure{uxtheme, "DrawThemeBackgroundEx", draw_theme_ex_hooked},
        Procedure{uxtheme, "DrawThemeBackground", draw_theme_hooked},
        Procedure{user32, "FillRect", fill_rect_hooked},
        Procedure{dwmapi, "DwmExtendFrameIntoClientArea", true},
        Procedure{dwmapi, "DwmSetWindowAttribute", true},
    };
    bool success =
        dwmapi != nullptr && user32 != nullptr && uxtheme != nullptr;
    if (!StopDirectUiEventMonitor()) {
        ::ReleaseSRWLockExclusive(&g_hooks_lock);
        return protocol::AgentResult::hook_failed;
    }
    for (const auto& procedure : procedures) {
        if (!procedure.installed) {
            continue;
        }
        const LPVOID target = procedure.module != nullptr
            ? reinterpret_cast<LPVOID>(
                  ::GetProcAddress(procedure.module, procedure.name))
            : nullptr;
        if (target == nullptr) {
            success = false;
            continue;
        }
        const MH_STATUS disabled = ::MH_DisableHook(target);
        if (disabled != MH_OK && disabled != MH_ERROR_DISABLED) {
            success = false;
            continue;
        }
        const MH_STATUS removed = ::MH_RemoveHook(target);
        if (removed != MH_OK && removed != MH_ERROR_NOT_CREATED) {
            success = false;
        }
    }
    if (success) {
        g_hooks_installed = false;
        g_original_dwm_set_window_attribute = nullptr;
        g_original_dwm_extend_frame = nullptr;
        g_original_fill_rect = nullptr;
        g_original_draw_theme_background = nullptr;
        g_original_draw_theme_background_ex = nullptr;
        g_set_window_composition_attribute = nullptr;
        g_dui70_begin = 0;
        g_dui70_end = 0;
    }
    ::ReleaseSRWLockExclusive(&g_hooks_lock);
    return success ? protocol::AgentResult::success
                   : protocol::AgentResult::hook_failed;
}

struct EnumerationContext final {
    std::uint32_t argb;
    HRESULT result{S_OK};
};

BOOL CALLBACK ApplyEnumeratedWindow(
    const HWND window,
    const LPARAM raw_context) noexcept {
    auto* context = reinterpret_cast<EnumerationContext*>(raw_context);
    if (context == nullptr) {
        return FALSE;
    }
    if (!IsCurrentProcessExplorerWindow(window)) {
        return TRUE;
    }
    if (!TrackWindow(window)) {
        context->result = HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
        return FALSE;
    }
    if (!EnsureExplorerUiThreadHook(window)) {
        context->result = HRESULT_FROM_WIN32(ERROR_HOOK_NOT_INSTALLED);
        return FALSE;
    }
    const HRESULT result = ApplyWindowSurface(window, context->argb);
    if (FAILED(result) && SUCCEEDED(context->result)) {
        context->result = result;
    }
    const HRESULT tree_view_result =
        ApplyExplorerTreeViewColors(window, context->argb);
    if (FAILED(tree_view_result) && SUCCEEDED(context->result)) {
        context->result = tree_view_result;
    }
    static_cast<void>(::EnumChildWindows(
        window, &AttachExistingDirectUiSubclass, 0));
    return TRUE;
}

[[nodiscard]] HRESULT RestoreAllWindows() noexcept {
    std::array<TrackedWindow, kMaximumTrackedWindows> windows{};
    std::size_t count = 0;
    ::AcquireSRWLockExclusive(&g_windows_lock);
    count = g_window_count;
    std::copy_n(g_windows.begin(), count, windows.begin());
    ::ReleaseSRWLockExclusive(&g_windows_lock);

    HRESULT result = S_OK;
    std::array<TrackedWindow, kMaximumTrackedWindows> failed_windows{};
    std::size_t failed_count = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const HRESULT restored = RestoreWindowSurface(windows[index]);
        if (FAILED(restored) && SUCCEEDED(result)) {
            result = restored;
        }
        if (FAILED(restored)) {
            failed_windows[failed_count++] = windows[index];
        }
    }
    ::AcquireSRWLockExclusive(&g_windows_lock);
    g_windows = failed_windows;
    g_window_count = failed_count;
    ::ReleaseSRWLockExclusive(&g_windows_lock);
    return result;
}

BOOL CALLBACK RedrawExplorerWindow(const HWND window, LPARAM) noexcept {
    if (IsCurrentProcessExplorerWindow(window)) {
        static_cast<void>(::RedrawWindow(
            window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME));
    }
    return TRUE;
}

void RedrawAllExplorerWindows() noexcept {
    static_cast<void>(::EnumWindows(&RedrawExplorerWindow, 0));
}

}  // namespace

protocol::AgentResult ConfigureExplorerWindowColor(
    const bool enabled,
    const std::uint32_t argb,
    const std::uint32_t transition_animation,
    const bool custom_scrollbar_enabled) noexcept {
    if ((argb & 0xFF000000U) != 0xFF000000U ||
        transition_animation > protocol::kMaximumExplorerTransition) {
        return protocol::AgentResult::invalid_configuration;
    }
    g_transition_animation.store(
        transition_animation, std::memory_order_release);
    g_custom_scrollbar_enabled.store(
        custom_scrollbar_enabled, std::memory_order_release);

    if (!enabled) {
        g_enabled.store(false, std::memory_order_release);
        const auto winui_result = ConfigureExplorerWinUiColor(false, argb);
        const HRESULT tree_views_restored = RestoreAllTreeViews();
        const HRESULT restored = RestoreAllWindows();
        RedrawAllExplorerWindows();
        const auto hooks_result =
            winui_result == protocol::AgentResult::success &&
                    SUCCEEDED(tree_views_restored) && SUCCEEDED(restored)
                ? RemoveHooks()
                : protocol::AgentResult::hook_failed;
        return winui_result == protocol::AgentResult::success &&
                       SUCCEEDED(tree_views_restored) &&
                       SUCCEEDED(restored) &&
                       hooks_result == protocol::AgentResult::success
            ? protocol::AgentResult::success
            : protocol::AgentResult::hook_failed;
    }

    const protocol::AgentResult hooks_result = InstallHooks();
    if (hooks_result != protocol::AgentResult::success) {
        return hooks_result;
    }
    g_color.store(argb, std::memory_order_release);
    g_enabled.store(true, std::memory_order_release);

    EnumerationContext context{argb};
    static_cast<void>(::EnumWindows(
        &ApplyEnumeratedWindow,
        reinterpret_cast<LPARAM>(&context)));
    if (FAILED(context.result)) {
        g_enabled.store(false, std::memory_order_release);
        static_cast<void>(RestoreAllTreeViews());
        static_cast<void>(RestoreAllWindows());
        static_cast<void>(RemoveHooks());
        return protocol::AgentResult::hook_failed;
    }

    const protocol::AgentResult winui_result =
        ConfigureExplorerWinUiColor(true, argb);
    if (winui_result != protocol::AgentResult::success) {
        g_enabled.store(false, std::memory_order_release);
        static_cast<void>(ConfigureExplorerWinUiColor(false, argb));
        static_cast<void>(RestoreAllTreeViews());
        static_cast<void>(RestoreAllWindows());
        static_cast<void>(RemoveHooks());
        return winui_result;
    }
    RedrawAllExplorerWindows();
    return protocol::AgentResult::success;
}

void ObserveExplorerWindowForColor(const HWND window) noexcept {
    if (!g_enabled.load(std::memory_order_acquire) ||
        !IsCurrentProcessExplorerWindow(window)) {
        return;
    }
    const std::uint32_t color = g_color.load(std::memory_order_acquire);
    if (TrackWindow(window)) {
        static_cast<void>(ApplyWindowSurface(window, color));
    }
    static_cast<void>(ApplyExplorerTreeViewColors(window, color));
    ApplyExplorerWinUiColorForCurrentThread();
}

void RefreshExplorerNativeControlsForWindow(const HWND window) noexcept {
    if (!g_enabled.load(std::memory_order_acquire) ||
        !IsCurrentProcessExplorerWindow(window)) {
        return;
    }
    static_cast<void>(ApplyExplorerTreeViewColors(
        window, g_color.load(std::memory_order_acquire)));
}

void PrimeExplorerTransitionOverlayForWindow(const HWND window) noexcept {
    if (!g_enabled.load(std::memory_order_acquire) ||
        !IsCurrentProcessExplorerWindow(window) ||
        ::GetWindowThreadProcessId(window, nullptr) !=
            ::GetCurrentThreadId()) {
        return;
    }
    const HWND reference = LargestDirectUiWindow(window);
    if (reference != nullptr) {
        static_cast<void>(ShowDirectUiTransitionOverlay(
            reference, true, true));
    }
}

void DetachExplorerNativeControlsForWindow(const HWND window) noexcept {
    if (!IsCurrentProcessExplorerWindow(window)) {
        return;
    }
    static_cast<void>(::RemoveWindowSubclass(
        window,
        &ExplorerScrollbarOwnerSubclassProc,
        kExplorerScrollbarOwnerSubclassId));
    DestroyDirectUiTransitionOverlay(window);
    static_cast<void>(::EnumChildWindows(
        window, &DetachEnumeratedTreeView, 0));
}

protocol::AgentResult StopExplorerWindowColor() noexcept {
    g_enabled.store(false, std::memory_order_release);
    g_custom_scrollbar_enabled.store(false, std::memory_order_release);
    const protocol::AgentResult winui_result = StopExplorerWinUiColor();
    const HRESULT tree_views_restored = RestoreAllTreeViews();
    const HRESULT restored = RestoreAllWindows();
    RedrawAllExplorerWindows();
    const protocol::AgentResult hooks_result =
        winui_result == protocol::AgentResult::success &&
                SUCCEEDED(tree_views_restored) && SUCCEEDED(restored)
            ? RemoveHooks()
            : protocol::AgentResult::hook_failed;
    return winui_result == protocol::AgentResult::success &&
                   SUCCEEDED(tree_views_restored) &&
                   SUCCEEDED(restored) &&
                   hooks_result == protocol::AgentResult::success
        ? protocol::AgentResult::success
        : protocol::AgentResult::hook_failed;
}

}  // namespace metaplasia::agent
