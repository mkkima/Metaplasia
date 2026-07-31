#include "metaplasia/agent/explorer_winui_adapter.hpp"

#include "metaplasia/agent/explorer_window_color.hpp"

#include <commctrl.h>
#include <ocidl.h>
#include <xamlom.h>
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.UI.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>

namespace metaplasia::agent {
namespace {

namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;

constexpr DWORD kSubscriptionTimeoutMilliseconds = 5000;
constexpr DWORD kWindowMessageTimeoutMilliseconds = 2000;
constexpr std::size_t kMaximumTrackedElements = 256;
constexpr UINT kApplyColorMessage = WM_APP + 0x4A1;
constexpr UINT kDetachSubclassMessage = WM_APP + 0x4A2;
constexpr UINT_PTR kExplorerWinUiSubclassId = 0x4D575549U;  // "MWUI"

std::atomic<bool> g_desired_enabled{false};
std::atomic<std::uint32_t> g_desired_color{
    protocol::kDefaultShellBackgroundColor};
std::atomic<HRESULT> g_last_error{S_OK};

SRWLOCK g_controller_lock = SRWLOCK_INIT;
IVisualTreeService3* g_visual_tree_service = nullptr;
class VisualTreeWatcher;
VisualTreeWatcher* g_visual_tree_watcher = nullptr;
bool g_visual_tree_advised = false;

SRWLOCK g_initialization_lock = SRWLOCK_INIT;
bool g_initialization_attempted = false;
HRESULT g_initialization_result = E_UNEXPECTED;
HANDLE g_subscription_event = nullptr;
std::atomic<HRESULT> g_subscription_result{E_PENDING};

void DebugLog(const wchar_t* message) noexcept {
    ::OutputDebugStringW(L"[Metaplasia Explorer WinUI] ");
    ::OutputDebugStringW(message);
    ::OutputDebugStringW(L"\n");
}

class ExclusiveSrwLock final {
public:
    explicit ExclusiveSrwLock(SRWLOCK& lock) noexcept : lock_(lock) {
        ::AcquireSRWLockExclusive(&lock_);
    }
    ~ExclusiveSrwLock() {
        ::ReleaseSRWLockExclusive(&lock_);
    }
    ExclusiveSrwLock(const ExclusiveSrwLock&) = delete;
    ExclusiveSrwLock& operator=(const ExclusiveSrwLock&) = delete;

private:
    SRWLOCK& lock_;
};

[[nodiscard]] bool IsExplorerWindow(const HWND window) noexcept {
    if (window == nullptr || !::IsWindow(window)) {
        return false;
    }
    DWORD process_id = 0;
    ::GetWindowThreadProcessId(window, &process_id);
    if (process_id != ::GetCurrentProcessId()) {
        return false;
    }
    std::array<wchar_t, 64> class_name{};
    const int length = ::GetClassNameW(
        window,
        class_name.data(),
        static_cast<int>(class_name.size()));
    return length > 0 &&
           static_cast<std::size_t>(length) < class_name.size() &&
           (_wcsicmp(class_name.data(), L"CabinetWClass") == 0 ||
            _wcsicmp(class_name.data(), L"ExploreWClass") == 0);
}

enum class BackgroundProperty : std::uint8_t {
    panel = 1,
    control = 2,
};

[[nodiscard]] BackgroundProperty IdentifyBackgroundProperty(
    const std::wstring_view type_name,
    const std::wstring_view element_name) noexcept {
    // TabContainerGrid and FileExplorerTabControlGrid extend through DWM's
    // caption-button bounds. Giving either root an opaque brush covers the
    // native glyph layer; the window-level caption color handles that region.
    constexpr std::array<std::wstring_view, 5> panel_names{
        L"NavigationBarControlGrid",
        L"CommandBarControlRootGrid",
        L"DetailsViewControlRootGrid",
        L"HomeViewRootGrid",
        L"GalleryRootGrid",
    };
    if (std::ranges::find(panel_names, element_name) != panel_names.end() &&
        (type_name.ends_with(L".Grid") || type_name == L"Grid")) {
        return BackgroundProperty::panel;
    }
    if (element_name == L"FileExplorerCommandBar" &&
        type_name.ends_with(L".CommandBar")) {
        return BackgroundProperty::control;
    }
    return static_cast<BackgroundProperty>(0);
}

[[nodiscard]] muxm::Brush ReadBackground(
    const mux::FrameworkElement& element,
    const BackgroundProperty property) {
    if (property == BackgroundProperty::panel) {
        return element.as<muxc::Panel>().Background();
    }
    return element.as<muxc::Control>().Background();
}

void WriteBackground(
    const mux::FrameworkElement& element,
    const BackgroundProperty property,
    const muxm::Brush& brush) {
    if (property == BackgroundProperty::panel) {
        element.as<muxc::Panel>().Background(brush);
    } else {
        element.as<muxc::Control>().Background(brush);
    }
}

[[nodiscard]] muxm::Brush MakeColorBrush(const std::uint32_t argb) {
    return muxm::SolidColorBrush(winrt::Windows::UI::Color{
        static_cast<BYTE>((argb >> 24U) & 0xFFU),
        static_cast<BYTE>((argb >> 16U) & 0xFFU),
        static_cast<BYTE>((argb >> 8U) & 0xFFU),
        static_cast<BYTE>(argb & 0xFFU)});
}

LRESULT CALLBACK ExplorerWinUiSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR,
    const DWORD_PTR) noexcept;

BOOL CALLBACK AttachSubclassToThreadWindow(
    const HWND window,
    LPARAM) noexcept {
    if (IsExplorerWindow(window)) {
        static_cast<void>(::SetWindowSubclass(
            window,
            &ExplorerWinUiSubclassProc,
            kExplorerWinUiSubclassId,
            0));
    }
    return TRUE;
}

void EnsureCurrentThreadWindowSubclass() noexcept {
    static_cast<void>(::EnumThreadWindows(
        ::GetCurrentThreadId(),
        &AttachSubclassToThreadWindow,
        0));
}

class VisualTreeWatcher
    : public winrt::implements<
          VisualTreeWatcher,
          IVisualTreeServiceCallback2,
          winrt::non_agile> {
public:
    explicit VisualTreeWatcher(IUnknown* site) {
        winrt::com_ptr<IUnknown> site_pointer;
        site_pointer.copy_from(site);
        diagnostics_ = site_pointer.as<IXamlDiagnostics>();
    }

    [[nodiscard]] HRESULT ApplyForCurrentThread() noexcept {
        const DWORD thread_id = ::GetCurrentThreadId();
        const bool enabled = g_desired_enabled.load(std::memory_order_acquire);
        const std::uint32_t color =
            g_desired_color.load(std::memory_order_acquire);
        try {
            muxm::Brush desired{nullptr};
            if (enabled) {
                desired = MakeColorBrush(color);
            }

            HRESULT first_failure = S_OK;
            ::AcquireSRWLockExclusive(&elements_lock_);
            for (auto& tracked : elements_) {
                if (tracked.handle == 0 || tracked.thread_id != thread_id) {
                    continue;
                }
                try {
                    auto element = tracked.element.get();
                    if (!element) {
                        tracked = {};
                        continue;
                    }
                    WriteBackground(
                        element,
                        tracked.property,
                        enabled ? desired : tracked.original_background);
                    tracked.owned_color = enabled ? color : 0;
                } catch (...) {
                    if (SUCCEEDED(first_failure)) {
                        first_failure = winrt::to_hresult();
                    }
                }
            }
            ::ReleaseSRWLockExclusive(&elements_lock_);
            return first_failure;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    [[nodiscard]] std::size_t tracked_count() const noexcept {
        ::AcquireSRWLockShared(&elements_lock_);
        const auto count = static_cast<std::size_t>(std::count_if(
            elements_.begin(),
            elements_.end(),
            [](const TrackedElement& element) {
                return element.handle != 0;
            }));
        ::ReleaseSRWLockShared(&elements_lock_);
        return count;
    }

private:
    struct TrackedElement final {
        InstanceHandle handle{0};
        DWORD thread_id{0};
        BackgroundProperty property{static_cast<BackgroundProperty>(0)};
        winrt::weak_ref<mux::FrameworkElement> element{};
        muxm::Brush original_background{nullptr};
        std::uint32_t owned_color{0};
    };

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation,
        const VisualElement visual_element,
        const VisualMutationType mutation_type) noexcept override {
        if (mutation_type == Remove) {
            ::AcquireSRWLockExclusive(&elements_lock_);
            const auto found = std::ranges::find(
                elements_,
                visual_element.Handle,
                &TrackedElement::handle);
            if (found != elements_.end()) {
                *found = {};
            }
            ::ReleaseSRWLockExclusive(&elements_lock_);
            return S_OK;
        }
        if (mutation_type != Add || visual_element.Handle == 0 ||
            visual_element.Type == nullptr) {
            return S_OK;
        }

        try {
            winrt::com_ptr<::IInspectable> inspectable;
            winrt::check_hresult(diagnostics_->GetIInspectableFromHandle(
                visual_element.Handle,
                reinterpret_cast<::IInspectable**>(inspectable.put())));
            auto element = inspectable.try_as<mux::FrameworkElement>();
            if (!element) {
                return S_OK;
            }

            const std::wstring_view type_name(
                visual_element.Type,
                ::SysStringLen(visual_element.Type));
            const auto name = element.Name();
            const BackgroundProperty property = IdentifyBackgroundProperty(
                type_name,
                std::wstring_view(name.c_str(), name.size()));
            if (property != BackgroundProperty::panel &&
                property != BackgroundProperty::control) {
                return S_OK;
            }

            {
                ExclusiveSrwLock lock(elements_lock_);
                auto found = std::ranges::find(
                    elements_,
                    visual_element.Handle,
                    &TrackedElement::handle);
                if (found == elements_.end()) {
                    found = std::ranges::find(
                        elements_,
                        static_cast<InstanceHandle>(0),
                        &TrackedElement::handle);
                }
                if (found == elements_.end()) {
                    return S_OK;
                }

                if (found->handle == 0) {
                    found->handle = visual_element.Handle;
                    found->thread_id = ::GetCurrentThreadId();
                    found->property = property;
                    found->element = winrt::make_weak(element);
                    found->original_background =
                        ReadBackground(element, property);
                }
                if (g_desired_enabled.load(std::memory_order_acquire)) {
                    const std::uint32_t color =
                        g_desired_color.load(std::memory_order_acquire);
                    WriteBackground(element, property, MakeColorBrush(color));
                    found->owned_color = color;
                }
            }
            EnsureCurrentThreadWindowSubclass();
        } catch (...) {
            g_last_error.store(winrt::to_hresult(), std::memory_order_release);
        }
        // Never abort the runtime's visual-tree enumeration because one
        // optional Explorer element failed to accept a brush.
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(
        const InstanceHandle handle,
        VisualElementState,
        LPCWSTR) noexcept override {
        if (handle == 0 ||
            !g_desired_enabled.load(std::memory_order_acquire)) {
            return S_OK;
        }

        // Navigation changes visual states after the new tree has been
        // announced. Reassert the brush in that same UI-thread callback so a
        // stock Explorer background is never left visible until a later DWM
        // or reconciliation event.
        static thread_local bool applying_state = false;
        if (applying_state) {
            return S_OK;
        }
        applying_state = true;
        try {
            ExclusiveSrwLock lock(elements_lock_);
            const auto found = std::ranges::find(
                elements_, handle, &TrackedElement::handle);
            if (found != elements_.end() &&
                found->thread_id == ::GetCurrentThreadId()) {
                auto element = found->element.get();
                if (element) {
                    const std::uint32_t color =
                        g_desired_color.load(std::memory_order_acquire);
                    WriteBackground(
                        element, found->property, MakeColorBrush(color));
                    found->owned_color = color;
                } else {
                    *found = {};
                }
            }
        } catch (...) {
            g_last_error.store(winrt::to_hresult(), std::memory_order_release);
        }
        applying_state = false;
        return S_OK;
    }

    winrt::com_ptr<IXamlDiagnostics> diagnostics_;
    mutable SRWLOCK elements_lock_ = SRWLOCK_INIT;
    std::array<TrackedElement, kMaximumTrackedElements> elements_{};
};

void ApplyAttachedWatcherForCurrentThread() noexcept {
    VisualTreeWatcher* watcher = nullptr;
    ::AcquireSRWLockShared(&g_controller_lock);
    watcher = g_visual_tree_watcher;
    if (watcher != nullptr) {
        watcher->AddRef();
    }
    ::ReleaseSRWLockShared(&g_controller_lock);
    if (watcher == nullptr) {
        return;
    }
    const HRESULT result = watcher->ApplyForCurrentThread();
    watcher->Release();
    if (FAILED(result)) {
        g_last_error.store(result, std::memory_order_release);
    }
}

LRESULT CALLBACK ExplorerWinUiSubclassProc(
    const HWND window,
    const UINT message,
    const WPARAM wparam,
    const LPARAM lparam,
    const UINT_PTR,
    const DWORD_PTR) noexcept {
    if (message == kApplyColorMessage) {
        ApplyAttachedWatcherForCurrentThread();
        RefreshExplorerNativeControlsForWindow(window);
        PrimeExplorerTransitionOverlayForWindow(window);
        return 0;
    }
    if (message == kDetachSubclassMessage) {
        ApplyAttachedWatcherForCurrentThread();
        DetachExplorerNativeControlsForWindow(window);
        static_cast<void>(::RemoveWindowSubclass(
            window,
            &ExplorerWinUiSubclassProc,
            kExplorerWinUiSubclassId));
        return 0;
    }
    if (message == WM_NCDESTROY) {
        static_cast<void>(::RemoveWindowSubclass(
            window,
            &ExplorerWinUiSubclassProc,
            kExplorerWinUiSubclassId));
        return ::DefSubclassProc(window, message, wparam, lparam);
    }
    if (message == WM_NCACTIVATE &&
        g_desired_enabled.load(std::memory_order_acquire)) {
        // Explorer's inactive extended title bar paints its system caption
        // glyphs black. Keep only the non-client visual state active so the
        // native minimize/maximize/close hit targets remain visible and fully
        // owned by DWM; this does not move keyboard focus or activate the app.
        const LRESULT result =
            ::DefSubclassProc(window, message, TRUE, lparam);
        ApplyAttachedWatcherForCurrentThread();
        RefreshExplorerNativeControlsForWindow(window);
        return result;
    }
    if (message == WM_ACTIVATE || message == WM_NCACTIVATE ||
        message == WM_THEMECHANGED ||
        message == WM_DWMCOLORIZATIONCOLORCHANGED ||
        message == WM_DWMCOMPOSITIONCHANGED || message == WM_SETTINGCHANGE) {
        const LRESULT result =
            ::DefSubclassProc(window, message, wparam, lparam);
        ApplyAttachedWatcherForCurrentThread();
        RefreshExplorerNativeControlsForWindow(window);
        return result;
    }
    return ::DefSubclassProc(window, message, wparam, lparam);
}

struct BroadcastContext final {
    UINT message;
    HRESULT result{S_OK};
};

BOOL CALLBACK BroadcastToExplorerWindow(
    const HWND window,
    const LPARAM raw_context) noexcept {
    auto* context = reinterpret_cast<BroadcastContext*>(raw_context);
    if (context == nullptr || !IsExplorerWindow(window)) {
        return TRUE;
    }
    DWORD_PTR ignored = 0;
    if (::SendMessageTimeoutW(
            window,
            context->message,
            0,
            0,
            SMTO_ABORTIFHUNG | SMTO_BLOCK,
            kWindowMessageTimeoutMilliseconds,
            &ignored) == 0 &&
        SUCCEEDED(context->result)) {
        context->result = HRESULT_FROM_WIN32(::GetLastError());
    }
    return TRUE;
}

[[nodiscard]] HRESULT Broadcast(const UINT message) noexcept {
    BroadcastContext context{message};
    static_cast<void>(::EnumWindows(
        &BroadcastToExplorerWindow,
        reinterpret_cast<LPARAM>(&context)));
    return context.result;
}

struct SubscriptionContext final {
    IVisualTreeService3* service;
    VisualTreeWatcher* watcher;
    bool subscribe;
    HANDLE completion_event;
    bool close_completion_event;
    std::atomic<HRESULT>* result;
};

DWORD WINAPI SubscriptionWorker(void* raw_context) noexcept {
    auto* context = static_cast<SubscriptionContext*>(raw_context);
    if (context == nullptr) {
        return static_cast<DWORD>(E_POINTER);
    }
    const HRESULT result = context->subscribe
        ? context->service->AdviseVisualTreeChange(context->watcher)
        : context->service->UnadviseVisualTreeChange(context->watcher);
    if (context->result != nullptr) {
        context->result->store(result, std::memory_order_release);
    }
    if (SUCCEEDED(result)) {
        ::AcquireSRWLockExclusive(&g_controller_lock);
        if (g_visual_tree_service == context->service &&
            g_visual_tree_watcher == context->watcher) {
            g_visual_tree_advised = context->subscribe;
        }
        ::ReleaseSRWLockExclusive(&g_controller_lock);
    }
    if (context->completion_event != nullptr) {
        ::SetEvent(context->completion_event);
        if (context->close_completion_event) {
            ::CloseHandle(context->completion_event);
        }
    }
    context->watcher->Release();
    context->service->Release();
    delete context;
    return static_cast<DWORD>(result);
}

[[nodiscard]] HRESULT StartSubscription(
    IVisualTreeService3* service,
    VisualTreeWatcher* watcher,
    const bool subscribe,
    HANDLE completion_event,
    const bool close_completion_event,
    std::atomic<HRESULT>* result) noexcept {
    auto* context = new (std::nothrow) SubscriptionContext{
        service,
        watcher,
        subscribe,
        completion_event,
        close_completion_event,
        result};
    if (context == nullptr) {
        return E_OUTOFMEMORY;
    }
    service->AddRef();
    watcher->AddRef();
    const HANDLE thread = ::CreateThread(
        nullptr, 0, &SubscriptionWorker, context, 0, nullptr);
    if (thread == nullptr) {
        const HRESULT error = HRESULT_FROM_WIN32(::GetLastError());
        watcher->Release();
        service->Release();
        delete context;
        return error;
    }
    ::CloseHandle(thread);
    return S_OK;
}

class ExplorerTapSite
    : public winrt::implements<
          ExplorerTapSite,
          IObjectWithSite,
          winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override {
        if (site == nullptr) {
            site_ = nullptr;
            return S_OK;
        }
        try {
            winrt::com_ptr<IUnknown> site_pointer;
            site_pointer.copy_from(site);
            auto service = site_pointer.as<IVisualTreeService3>();
            auto watcher = winrt::make_self<VisualTreeWatcher>(site);

            ::AcquireSRWLockExclusive(&g_controller_lock);
            if (g_visual_tree_service != nullptr ||
                g_visual_tree_watcher != nullptr) {
                ::ReleaseSRWLockExclusive(&g_controller_lock);
                return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
            }
            service->AddRef();
            watcher->AddRef();
            g_visual_tree_service = service.get();
            g_visual_tree_watcher = watcher.get();
            g_visual_tree_advised = false;
            ::ReleaseSRWLockExclusive(&g_controller_lock);

            site_.copy_from(site);
            const HRESULT result = StartSubscription(
                service.get(),
                watcher.get(),
                true,
                g_subscription_event,
                false,
                &g_subscription_result);
            if (FAILED(result)) {
                ::AcquireSRWLockExclusive(&g_controller_lock);
                g_visual_tree_service->Release();
                g_visual_tree_watcher->Release();
                g_visual_tree_service = nullptr;
                g_visual_tree_watcher = nullptr;
                ::ReleaseSRWLockExclusive(&g_controller_lock);
            }
            return result;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE GetSite(
        REFIID interface_id,
        void** object) noexcept override {
        if (object == nullptr) {
            return E_POINTER;
        }
        *object = nullptr;
        if (!site_) {
            return MK_E_NOSTORAGE;
        }
        return site_.as(interface_id, object);
    }

private:
    winrt::com_ptr<IUnknown> site_;
};

template <typename T>
class SimpleClassFactory
    : public winrt::implements<
          SimpleClassFactory<T>,
          IClassFactory,
          winrt::non_agile> {
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
        try {
            return winrt::make<T>().as(interface_id, object);
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override {
        return S_OK;
    }
};

[[nodiscard]] HMODULE CurrentModule() noexcept {
    HMODULE module = nullptr;
    static_cast<void>(::GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&CurrentModule),
        &module));
    return module;
}

[[nodiscard]] HRESULT InitializeAdapter() noexcept {
    ::AcquireSRWLockExclusive(&g_initialization_lock);
    if (g_initialization_attempted) {
        if (g_initialization_result ==
                HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND) &&
            ::GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll") !=
                nullptr) {
            g_initialization_attempted = false;
        } else {
            const HRESULT cached = g_initialization_result;
            ::ReleaseSRWLockExclusive(&g_initialization_lock);
            return cached;
        }
    }
    g_initialization_attempted = true;

    HRESULT result = E_UNEXPECTED;
    const HMODULE framework_udk =
        ::GetModuleHandleW(L"Microsoft.Internal.FrameworkUdk.dll");
    const HMODULE module = CurrentModule();
    if (framework_udk == nullptr || module == nullptr) {
        result = HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    } else {
        const auto initialize = reinterpret_cast<
            decltype(&::InitializeXamlDiagnosticsEx)>(::GetProcAddress(
            framework_udk,
            "InitializeXamlDiagnosticsEx"));
        if (initialize == nullptr) {
            result = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        } else {
            std::array<wchar_t, 32768> module_path{};
            const DWORD length = ::GetModuleFileNameW(
                module,
                module_path.data(),
                static_cast<DWORD>(module_path.size()));
            if (length == 0 || length >= module_path.size()) {
                result = HRESULT_FROM_WIN32(
                    length == 0 ? ::GetLastError()
                                : ERROR_INSUFFICIENT_BUFFER);
            } else {
                if (g_subscription_event == nullptr) {
                    g_subscription_event =
                        ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
                } else {
                    ::ResetEvent(g_subscription_event);
                }
                if (g_subscription_event == nullptr) {
                    result = HRESULT_FROM_WIN32(::GetLastError());
                } else {
                    g_subscription_result.store(
                        E_PENDING, std::memory_order_release);
                    constexpr HRESULT endpoint_not_found =
                        HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
                    for (unsigned index = 1; index <= 128; ++index) {
                        std::array<wchar_t, 64> endpoint{};
                        _snwprintf_s(
                            endpoint.data(),
                            endpoint.size(),
                            _TRUNCATE,
                            L"WinUIVisualDiagConnection%u",
                            index);
                        result = initialize(
                            endpoint.data(),
                            ::GetCurrentProcessId(),
                            L"",
                            module_path.data(),
                            kExplorerWinUiTapClsid,
                            nullptr);
                        if (result != endpoint_not_found) {
                            break;
                        }
                    }
                    if (SUCCEEDED(result)) {
                        const DWORD wait_result = ::WaitForSingleObject(
                            g_subscription_event,
                            kSubscriptionTimeoutMilliseconds);
                        if (wait_result == WAIT_OBJECT_0) {
                            result = g_subscription_result.load(
                                std::memory_order_acquire);
                        } else if (wait_result == WAIT_TIMEOUT) {
                            result = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                        } else {
                            result = HRESULT_FROM_WIN32(::GetLastError());
                        }
                    }
                }
            }
        }
    }

    g_initialization_result = result;
    ::ReleaseSRWLockExclusive(&g_initialization_lock);
    DebugLog(SUCCEEDED(result) ? L"adapter initialized"
                               : L"adapter initialization failed");
    return result;
}

}  // namespace

protocol::AgentResult ConfigureExplorerWinUiColor(
    const bool enabled,
    const std::uint32_t argb) noexcept {
    if ((argb & 0xFF000000U) != 0xFF000000U) {
        return protocol::AgentResult::invalid_configuration;
    }
    g_desired_color.store(argb, std::memory_order_release);
    g_desired_enabled.store(enabled, std::memory_order_release);

    HRESULT result = S_OK;
    if (enabled) {
        ::AcquireSRWLockShared(&g_controller_lock);
        const bool attached = g_visual_tree_service != nullptr &&
                              g_visual_tree_watcher != nullptr &&
                              g_visual_tree_advised;
        ::ReleaseSRWLockShared(&g_controller_lock);
        if (!attached) {
            result = InitializeAdapter();
        }
    }
    if (SUCCEEDED(result)) {
        result = Broadcast(kApplyColorMessage);
    }
    g_last_error.store(result, std::memory_order_release);
    return SUCCEEDED(result)
        ? protocol::AgentResult::success
        : (enabled ? protocol::AgentResult::adapter_unavailable
                   : protocol::AgentResult::hook_failed);
}

void ApplyExplorerWinUiColorForCurrentThread() noexcept {
    ApplyAttachedWatcherForCurrentThread();
}

protocol::AgentResult StopExplorerWinUiColor() noexcept {
    g_desired_enabled.store(false, std::memory_order_release);
    HRESULT result = Broadcast(kDetachSubclassMessage);

    IVisualTreeService3* service = nullptr;
    VisualTreeWatcher* watcher = nullptr;
    bool advised = false;
    ::AcquireSRWLockShared(&g_controller_lock);
    service = g_visual_tree_service;
    watcher = g_visual_tree_watcher;
    advised = g_visual_tree_advised;
    if (service != nullptr) {
        service->AddRef();
    }
    if (watcher != nullptr) {
        watcher->AddRef();
    }
    ::ReleaseSRWLockShared(&g_controller_lock);

    HANDLE event = nullptr;
    HANDLE worker_event = nullptr;
    std::atomic<HRESULT> unadvise_result{E_PENDING};
    if (advised && service != nullptr && watcher != nullptr) {
        event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (event == nullptr) {
            if (SUCCEEDED(result)) {
                result = HRESULT_FROM_WIN32(::GetLastError());
            }
        } else {
            HRESULT started = S_OK;
            if (!::DuplicateHandle(
                    ::GetCurrentProcess(),
                    event,
                    ::GetCurrentProcess(),
                    &worker_event,
                    EVENT_MODIFY_STATE,
                    FALSE,
                    0)) {
                started = HRESULT_FROM_WIN32(::GetLastError());
            } else {
                started = StartSubscription(
                    service,
                    watcher,
                    false,
                    worker_event,
                    true,
                    &unadvise_result);
                if (FAILED(started)) {
                    ::CloseHandle(worker_event);
                    worker_event = nullptr;
                }
            }
            if (FAILED(started)) {
                if (SUCCEEDED(result)) {
                    result = started;
                }
            } else {
                const DWORD wait = ::WaitForSingleObject(
                    event, kSubscriptionTimeoutMilliseconds);
                const HRESULT stopped = wait == WAIT_OBJECT_0
                    ? unadvise_result.load(std::memory_order_acquire)
                    : HRESULT_FROM_WIN32(
                          wait == WAIT_TIMEOUT ? ERROR_TIMEOUT
                                               : ::GetLastError());
                if (FAILED(stopped) && SUCCEEDED(result)) {
                    result = stopped;
                }
            }
            ::CloseHandle(event);
        }
    }
    if (watcher != nullptr) {
        watcher->Release();
    }
    if (service != nullptr) {
        service->Release();
    }

    if (SUCCEEDED(result)) {
        ::AcquireSRWLockExclusive(&g_controller_lock);
        if (g_visual_tree_watcher != nullptr) {
            g_visual_tree_watcher->Release();
            g_visual_tree_watcher = nullptr;
        }
        if (g_visual_tree_service != nullptr) {
            g_visual_tree_service->Release();
            g_visual_tree_service = nullptr;
        }
        g_visual_tree_advised = false;
        ::ReleaseSRWLockExclusive(&g_controller_lock);
    }
    g_last_error.store(result, std::memory_order_release);
    return SUCCEEDED(result) ? protocol::AgentResult::success
                             : protocol::AgentResult::hook_failed;
}

HRESULT GetExplorerWinUiTapClassObject(
    REFCLSID class_id,
    REFIID interface_id,
    void** object) noexcept {
    if (object == nullptr) {
        return E_POINTER;
    }
    *object = nullptr;
    if (class_id != kExplorerWinUiTapClsid) {
        return CLASS_E_CLASSNOTAVAILABLE;
    }
    try {
        return winrt::make<SimpleClassFactory<ExplorerTapSite>>().as(
            interface_id,
            object);
    } catch (...) {
        return winrt::to_hresult();
    }
}

}  // namespace metaplasia::agent
