#include "metaplasia/agent/explorer_smooth_scroll.hpp"
#include "metaplasia/agent/smooth_scroll.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <cwchar>
#include <wrl/client.h>

namespace metaplasia::agent {
namespace {
constexpr std::size_t kMaximumTargets = 32;
constexpr wchar_t kIdentity[] = L"Metaplasia.Explorer.SmoothScroll.Identity";
constexpr DWORD kFrameMilliseconds = 16;
struct Request final {
    HWND window{};
    ULONG_PTR identity{};
    bool ready{};
    bool posted{};
    bool cancel{};
    double delta{};
    double view_percent{};
    double viewport_pixels{};
    double row_pixels{};
    int native_delta{};
    LPARAM point{};
};
struct Target final {
    HWND window{};
    ULONG_PTR identity{};
    Microsoft::WRL::ComPtr<IUIAutomationScrollPattern> pattern;
    SmoothScrollAnimation animation;
    bool pending{};
    bool applied{};
    int native_delta{};
    LPARAM point{};
    double row_pixels{};
    double viewport_pixels{};
    double view_percent{};
};
std::atomic<bool> g_enabled{false};
std::atomic<DWORD> g_worker{0};
std::atomic<ULONG_PTR> g_next_identity{1};
SRWLOCK g_requests_lock = SRWLOCK_INIT;
std::array<Request, kMaximumTargets> g_requests{};
// Worker-thread-only state. Never hold the request lock across a COM call.
thread_local std::array<Target, kMaximumTargets> g_targets{};

ULONG_PTR Identity(HWND window) noexcept {
    return reinterpret_cast<ULONG_PTR>(::GetPropW(window, kIdentity));
}
bool OwnWindow(HWND window) noexcept {
    DWORD process{};
    if (!::IsWindow(window) || !::GetWindowThreadProcessId(window, &process) ||
        process != ::GetCurrentProcessId()) { return false; }
    std::array<wchar_t, 64> name{};
    if (!::GetClassNameW(window, name.data(), static_cast<int>(name.size())) ||
        _wcsicmp(name.data(), L"DirectUIHWND") != 0) { return false; }
    const HWND root = ::GetAncestor(window, GA_ROOT);
    if (!::GetClassNameW(root, name.data(), static_cast<int>(name.size()))) { return false; }
    return _wcsicmp(name.data(), L"CabinetWClass") == 0 ||
           _wcsicmp(name.data(), L"ExploreWClass") == 0;
}
Request* FindRequest(HWND window, ULONG_PTR identity) noexcept {
    for (auto& request : g_requests) {
        if (request.window == window && request.identity == identity) { return &request; }
    }
    return nullptr;
}
bool Current(const Target& target) noexcept {
    return g_enabled.load(std::memory_order_acquire) && OwnWindow(target.window) &&
           target.identity != 0 && Identity(target.window) == target.identity;
}
void Unready(const Target& target) noexcept {
    ::AcquireSRWLockExclusive(&g_requests_lock);
    if (auto* request = FindRequest(target.window, target.identity)) {
        request->ready = false;
        request->delta = 0;
    }
    ::ReleaseSRWLockExclusive(&g_requests_lock);
}
void Failed(Target& target) noexcept {
    Unready(target);
    // Only replay when no animated frame was applied, preventing double scroll.
    if (!target.applied && target.native_delta != 0 && Current(target)) {
        ::PostMessageW(target.window, kSmoothScrollNativeWheelMessage,
            MAKEWPARAM(0, static_cast<WORD>(static_cast<short>(target.native_delta))), target.point);
    }
    target.pending = false;
    target.animation.Cancel();
    target.pattern.Reset();
}
} // namespace

bool ExplorerSmoothScrollEnabled() noexcept { return g_enabled.load(std::memory_order_acquire); }
void SetExplorerSmoothScrollEnabled(bool enabled) noexcept {
    g_enabled.store(enabled, std::memory_order_release);
    if (!enabled) {
        ::AcquireSRWLockExclusive(&g_requests_lock);
        for (auto& request : g_requests) { request.ready = false; request.cancel = true; request.delta = 0; }
        ::ReleaseSRWLockExclusive(&g_requests_lock);
        const DWORD worker = g_worker.load(std::memory_order_acquire);
        if (worker) { ::PostThreadMessageW(worker, kSmoothScrollWorkerMessage, 0, 0); }
    }
}
void AttachExplorerSmoothScrollWindow(HWND window) noexcept {
    if (!ExplorerSmoothScrollEnabled() || !OwnWindow(window) ||
        ::GetWindowThreadProcessId(window, nullptr) != ::GetCurrentThreadId() || Identity(window)) { return; }
    const ULONG_PTR identity = g_next_identity.fetch_add(1, std::memory_order_relaxed);
    if (identity != 0) { ::SetPropW(window, kIdentity, reinterpret_cast<HANDLE>(identity)); }
}
void CancelExplorerSmoothScroll(HWND window, bool destroy) noexcept {
    const ULONG_PTR identity = Identity(window);
    if (!identity) { return; }
    ::AcquireSRWLockExclusive(&g_requests_lock);
    if (auto* request = FindRequest(window, identity)) {
        request->ready = false;
        request->cancel = true;
        request->delta = 0;
        request->native_delta = 0;
    }
    ::ReleaseSRWLockExclusive(&g_requests_lock);
    if (destroy) { ::RemovePropW(window, kIdentity); }
    const DWORD worker = g_worker.load(std::memory_order_acquire);
    if (worker) { ::PostThreadMessageW(worker, kSmoothScrollWorkerMessage, reinterpret_cast<WPARAM>(window), 0); }
}
bool HandleExplorerSmoothWheel(HWND window, WPARAM wheel, LPARAM point) noexcept {
    if (!ExplorerSmoothScrollEnabled()) { return false; }
    const int delta = static_cast<short>(HIWORD(wheel));
    BOOL animations = FALSE;
    UINT lines{};
    if ((LOWORD(wheel) & (MK_CONTROL | MK_SHIFT | MK_LBUTTON | MK_RBUTTON |
                         MK_MBUTTON | MK_XBUTTON1 | MK_XBUTTON2)) ||
        delta == 0 || delta % WHEEL_DELTA != 0 ||
        !::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) || !animations ||
        !::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0) || !lines) {
        CancelExplorerSmoothScroll(window);
        return false;
    }
    const auto identity = Identity(window);
    const DWORD worker = g_worker.load(std::memory_order_acquire);
    bool accepted = false;
    ::AcquireSRWLockExclusive(&g_requests_lock);
    if (auto* request = FindRequest(window, identity); request && request->ready && worker) {
        const auto change = SmoothWheelPercent(delta, lines, request->view_percent,
            request->viewport_pixels, request->row_pixels);
        if (change && (request->posted || ::PostThreadMessageW(worker, kSmoothScrollWorkerMessage,
                reinterpret_cast<WPARAM>(window), 0))) {
            request->posted = true;
            request->delta = std::clamp(request->delta + *change, -100.0, 100.0);
            request->native_delta = std::clamp(request->native_delta + delta, -32000, 32000);
            request->point = point;
            accepted = true;
        }
    }
    ::ReleaseSRWLockExclusive(&g_requests_lock);
    return accepted;
}
void StartExplorerSmoothScrollWorker() noexcept { g_worker.store(::GetCurrentThreadId(), std::memory_order_release); }
void StopExplorerSmoothScrollWorker() noexcept {
    for (auto& target : g_targets) { target = {}; }
    ::AcquireSRWLockExclusive(&g_requests_lock);
    g_requests = {};
    ::ReleaseSRWLockExclusive(&g_requests_lock);
    g_worker.store(0, std::memory_order_release);
}
void RegisterExplorerSmoothScrollTarget(HWND window, IUIAutomation* automation, IUIAutomationElement* element,
    IUIAutomationScrollPattern* pattern, double view_percent, const RECT& bounds) noexcept {
    const auto identity = Identity(window);
    if (!ExplorerSmoothScrollEnabled() || !identity || !pattern || !element || !OwnWindow(window) ||
        !std::isfinite(view_percent) || view_percent <= 0 || view_percent >= 100 ||
        bounds.bottom <= bounds.top) { return; }
    BSTR class_name{};
    const HRESULT name_result = element->get_CurrentClassName(&class_name);
    const bool file_view = SUCCEEDED(name_result) && class_name && wcscmp(class_name, L"UIItemsView") == 0;
    ::SysFreeString(class_name);
    if (!file_view) { CancelExplorerSmoothScroll(window); return; }
    Target* slot = nullptr;
    for (auto& target : g_targets) {
        if (target.window && !Current(target)) { target = {}; }
        if (target.window == window && target.identity == identity) { slot = &target; break; }
    }
    if (!slot) { for (auto& target : g_targets) { if (!target.window) { slot = &target; break; } } }
    if (!slot) { return; }
    slot->window = window;
    slot->identity = identity;
    slot->pattern = pattern;
    const double viewport = static_cast<double>(bounds.bottom - bounds.top);
    if (slot->row_pixels <= 0 || slot->viewport_pixels != viewport || slot->view_percent != view_percent) {
        // Read only one item's geometry when the view metrics change. Do not
        // enumerate items, inspect file names, or repeat this on every frame.
        double row = 24.0 * static_cast<double>((std::max)(96U, ::GetDpiForWindow(window))) / 96.0;
        Microsoft::WRL::ComPtr<IUIAutomationCondition> condition;
        Microsoft::WRL::ComPtr<IUIAutomationElement> item;
        VARIANT kind{}; kind.vt = VT_I4; kind.lVal = UIA_ListItemControlTypeId;
        RECT item_bounds{};
        if (automation && SUCCEEDED(automation->CreatePropertyCondition(UIA_ControlTypePropertyId,
                kind, condition.GetAddressOf())) &&
            SUCCEEDED(element->FindFirst(TreeScope_Descendants, condition.Get(), item.GetAddressOf())) &&
            item && SUCCEEDED(item->get_CurrentBoundingRectangle(&item_bounds)) &&
            item_bounds.bottom > item_bounds.top && item_bounds.bottom - item_bounds.top <= viewport) {
            row = static_cast<double>(item_bounds.bottom - item_bounds.top);
        }
        slot->row_pixels = row;
        slot->viewport_pixels = viewport;
        slot->view_percent = view_percent;
    }
    ::AcquireSRWLockExclusive(&g_requests_lock);
    auto* request = FindRequest(window, identity);
    if (!request) {
        for (auto& candidate : g_requests) {
            if (!candidate.window || !::IsWindow(candidate.window) || Identity(candidate.window) != candidate.identity) {
                candidate = {}; request = &candidate; break;
            }
        }
    }
    if (request) {
        request->window = window; request->identity = identity;
        request->ready = ExplorerSmoothScrollEnabled();
        request->view_percent = view_percent;
        request->viewport_pixels = viewport;
        request->row_pixels = slot->row_pixels;
    }
    ::ReleaseSRWLockExclusive(&g_requests_lock);
}
void ProcessExplorerSmoothScrollRequest(HWND window) noexcept {
    for (auto& target : g_targets) {
        if (!target.window) { continue; }
        if (!Current(target)) { Unready(target); target = {}; continue; }
        if (target.window != window) { continue; }
        Request pending{};
        ::AcquireSRWLockExclusive(&g_requests_lock);
        if (auto* request = FindRequest(window, target.identity)) {
            pending = *request;
            request->delta = 0; request->native_delta = 0; request->posted = false; request->cancel = false;
        }
        ::ReleaseSRWLockExclusive(&g_requests_lock);
        if (pending.cancel) { target.animation.Cancel(); target.pending = false; }
        if (pending.delta == 0 || !pending.ready || !target.pattern) { continue; }
        double actual{};
        target.native_delta = pending.native_delta; target.point = pending.point;
        if (!target.pending) { target.applied = false; }
        if (FAILED(target.pattern->get_CurrentVerticalScrollPercent(&actual)) ||
            !std::isfinite(actual) || actual < 0 || actual > 100) {
            Failed(target);
            continue;
        }
        target.pending = target.animation.Retarget(actual, pending.delta, ::GetTickCount64());
    }
}
void AdvanceExplorerSmoothScroll() noexcept {
    const auto now = ::GetTickCount64();
    for (auto& target : g_targets) {
        if (!target.pending) { continue; }
        bool cancelled = false;
        ::AcquireSRWLockShared(&g_requests_lock);
        if (const auto* request = FindRequest(target.window, target.identity)) { cancelled = request->cancel; }
        else { cancelled = true; }
        ::ReleaseSRWLockShared(&g_requests_lock);
        if (!Current(target) || cancelled) { target.animation.Cancel(); target.pending = false; continue; }
        const bool active = target.animation.Active(now);
        const double position = active ? target.animation.Position(now) : target.animation.Target();
        if (!target.pattern || FAILED(target.pattern->SetScrollPercent(UIA_ScrollPatternNoScroll, position))) {
            Failed(target); continue;
        }
        target.applied = true;
        if (!active) { target.pending = false; target.animation.Cancel(); }
    }
}
DWORD ExplorerSmoothScrollWaitMilliseconds() noexcept {
    for (const auto& target : g_targets) { if (target.pending) { return kFrameMilliseconds; } }
    return INFINITE;
}
} // namespace metaplasia::agent
