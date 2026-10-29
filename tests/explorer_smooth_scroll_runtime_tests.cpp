#include "metaplasia/agent/explorer_smooth_scroll.hpp"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <wrl.h>

namespace {
using namespace metaplasia::agent;
using Microsoft::WRL::ComPtr;
constexpr UINT kWheel = WM_APP + 1;
constexpr UINT kCancel = WM_APP + 2;
std::atomic<double> g_position{25.0};
std::atomic<unsigned> g_frames{0};
std::atomic<unsigned> g_replays{0};
std::atomic<bool> g_fail{false};
std::atomic<bool> g_supported_view{true};
std::atomic<HWND> g_window{};

void Require(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAILED: " << message << '\n'; std::exit(EXIT_FAILURE); }
}

// An owned hidden provider exercises real UI Automation marshalling without
// opening Explorer, sending desktop input, or changing any Windows preference.
class ScrollProvider final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IRawElementProviderSimple, IScrollProvider, Microsoft::WRL::FtmBase> {
public:
    explicit ScrollProvider(HWND window) : window_(window) {}
    IFACEMETHODIMP get_ProviderOptions(ProviderOptions* output) override {
        *output = ProviderOptions_ServerSideProvider; return S_OK;
    }
    IFACEMETHODIMP GetPatternProvider(PATTERNID id, IUnknown** output) override {
        *output = nullptr;
        return id == UIA_ScrollPatternId ? QueryInterface(__uuidof(IScrollProvider),
            reinterpret_cast<void**>(output)) : S_OK;
    }
    IFACEMETHODIMP GetPropertyValue(PROPERTYID id, VARIANT* output) override {
        ::VariantInit(output);
        if (id == UIA_ClassNamePropertyId) {
            output->vt = VT_BSTR;
            output->bstrVal = ::SysAllocString(g_supported_view.load() ? L"UIItemsView" : L"OtherView");
            return output->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        if (id == UIA_ControlTypePropertyId) { output->vt = VT_I4; output->lVal = UIA_ListControlTypeId; }
        return S_OK;
    }
    IFACEMETHODIMP get_HostRawElementProvider(IRawElementProviderSimple** output) override {
        return ::UiaHostProviderFromHwnd(window_, output);
    }
    IFACEMETHODIMP Scroll(ScrollAmount, ScrollAmount) override { return E_NOTIMPL; }
    IFACEMETHODIMP SetScrollPercent(double horizontal, double vertical) override {
        if (g_fail.load()) { return E_FAIL; }
        if (horizontal != UIA_ScrollPatternNoScroll || vertical < 0 || vertical > 100) { return E_INVALIDARG; }
        g_position.store(vertical); ++g_frames; return S_OK;
    }
    IFACEMETHODIMP get_HorizontalScrollPercent(double* output) override { *output = -1; return S_OK; }
    IFACEMETHODIMP get_VerticalScrollPercent(double* output) override { *output = g_position.load(); return S_OK; }
    IFACEMETHODIMP get_HorizontalViewSize(double* output) override { *output = 100; return S_OK; }
    IFACEMETHODIMP get_VerticalViewSize(double* output) override { *output = 20; return S_OK; }
    IFACEMETHODIMP get_HorizontallyScrollable(BOOL* output) override { *output = FALSE; return S_OK; }
    IFACEMETHODIMP get_VerticallyScrollable(BOOL* output) override { *output = TRUE; return S_OK; }
private:
    HWND window_;
};

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_GETOBJECT && static_cast<LONG>(lparam) == UiaRootObjectId) {
        auto provider = Microsoft::WRL::Make<ScrollProvider>(window);
        return ::UiaReturnRawElementProvider(window, wparam, lparam, provider.Get());
    }
    if (message == kWheel) { return HandleExplorerSmoothWheel(window, wparam, lparam); }
    if (message == kCancel) { CancelExplorerSmoothScroll(window); return 0; }
    if (message == kSmoothScrollNativeWheelMessage) { ++g_replays; return 0; }
    if (message == WM_NCDESTROY) { CancelExplorerSmoothScroll(window, true); }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

void Pump(unsigned milliseconds) {
    const auto until = ::GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (!message.hwnd && message.message == kSmoothScrollWorkerMessage) {
                ProcessExplorerSmoothScrollRequest(reinterpret_cast<HWND>(message.wParam));
            } else { ::DispatchMessageW(&message); }
        }
        AdvanceExplorerSmoothScroll();
        ::Sleep(8);
    } while (::GetTickCount64() < until);
}
} // namespace

int main() {
    Require(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "initialize MTA client");
    MSG message{}; ::PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    SetExplorerSmoothScrollEnabled(true);
    StartExplorerSmoothScrollWorker();
    std::thread ui([] {
        Require(SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "initialize owned UI thread");
        WNDCLASSW type{}; type.lpfnWndProc = WindowProcedure;
        type.hInstance = ::GetModuleHandleW(nullptr); type.lpszClassName = L"CabinetWClass";
        Require(::RegisterClassW(&type) != 0, "register owned root class");
        type.lpszClassName = L"DirectUIHWND";
        Require(::RegisterClassW(&type) != 0, "register owned list class");
        const HWND root = ::CreateWindowW(L"CabinetWClass", L"", WS_OVERLAPPED, 0, 0, 640, 480,
            nullptr, nullptr, type.hInstance, nullptr);
        const HWND list = ::CreateWindowW(L"DirectUIHWND", L"", WS_CHILD, 0, 0, 640, 400,
            root, nullptr, type.hInstance, nullptr);
        Require(root && list, "create hidden owned windows");
        AttachExplorerSmoothScrollWindow(list); g_window.store(list);
        MSG next{};
        while (::GetMessageW(&next, nullptr, 0, 0) > 0) { ::DispatchMessageW(&next); }
        ::DestroyWindow(root); ::CoUninitialize();
    });
    const auto deadline = ::GetTickCount64() + 5000;
    while (!g_window.load() && ::GetTickCount64() < deadline) { ::Sleep(1); }
    const HWND window = g_window.load(); Require(window != nullptr, "owned UI thread ready");
    ComPtr<IUIAutomation> automation;
    Require(SUCCEEDED(::CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&automation))), "create UI Automation client");
    ComPtr<IUIAutomationElement> element;
    Require(SUCCEEDED(automation->ElementFromHandle(window, &element)), "marshal owned element");
    ComPtr<IUIAutomationScrollPattern> pattern;
    Require(SUCCEEDED(element->GetCurrentPatternAs(UIA_ScrollPatternId, IID_PPV_ARGS(&pattern))),
        "marshal owned scroll pattern");
    const RECT bounds{0, 0, 640, 400};
    auto ready = [&] { RegisterExplorerSmoothScrollTarget(window, automation.Get(), element.Get(),
        pattern.Get(), 20, bounds); };
    auto wheel = [&](short delta, WORD keys = 0) {
        return ::SendMessageW(window, kWheel, MAKEWPARAM(keys, static_cast<WORD>(delta)), 0) != 0;
    };
    ready();
    Require(ExplorerSmoothScrollWaitMilliseconds() == INFINITE, "idle worker has no timer");
    Require(!wheel(-30) && !wheel(-120, MK_CONTROL) && !wheel(-120, MK_SHIFT) &&
        !wheel(-120, MK_LBUTTON), "precision scrolling, zoom and dragging remain native");
    Pump(20); ready();
    g_supported_view.store(false); ready();
    Require(!wheel(-120), "a changed unsupported view invalidates the previous provider");
    Pump(20); g_supported_view.store(true); ready();
    BOOL animations{}; UINT lines{};
    Require(::SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) &&
        ::SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0), "read existing wheel preferences");
    if (animations && lines) {
        Require(wheel(-120), "accept supported wheel");
        Pump(60);
        Require(g_frames.load() > 1 && g_position.load() > 25, "apply intermediate UIA frames");
        g_fail.store(true); Pump(40);
        Require(g_replays.load() == 0, "partially applied animation is never replayed");
        g_fail.store(false); ready();
        Require(wheel(-120), "restart animation after provider recovers"); Pump(40);
        ::SendMessageW(window, kCancel, 0, 0); Pump(20);
        const double cancelled = g_position.load(); Pump(200);
        Require(g_position.load() == cancelled, "navigation cancellation stops animation");
        Require(ExplorerSmoothScrollWaitMilliseconds() == INFINITE, "cancelled animation is idle");
        ready(); g_fail.store(true);
        Require(wheel(-120), "queue wheel before simulated provider failure"); Pump(80);
        Require(g_replays.load() == 1, "first-frame failure replays native wheel exactly once");
        Require(!wheel(-120), "failed provider returns subsequent wheel to native processing");
        g_fail.store(false); ready();
        Require(wheel(-120), "provider may recover after metrics refresh"); Pump(220);
        Require(ExplorerSmoothScrollWaitMilliseconds() == INFINITE, "completed animation has no timer");
        ready(); Require(wheel(120), "accept reverse direction"); Pump(20);
        SetExplorerSmoothScrollEnabled(false); Pump(20);
        const double disabled = g_position.load(); Pump(200);
        Require(!wheel(-120) && g_position.load() == disabled, "disable stops writes and leaves native wheel");
    } else {
        Require(!wheel(-120), "disabled system animations or zero lines remain native");
        std::cout << "System preference disables animation; animated cases not exercised\n";
    }
    SetExplorerSmoothScrollEnabled(false);
    StopExplorerSmoothScrollWorker();
    pattern.Reset(); element.Reset(); automation.Reset();
    ::PostThreadMessageW(::GetWindowThreadProcessId(window, nullptr), WM_QUIT, 0, 0);
    ui.join(); ::CoUninitialize();
    std::cout << "Owned UI Automation scrolling integration passed\n";
}
