#pragma once
#include <Windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <UIAutomation.h>

namespace metaplasia::agent {
inline constexpr UINT kSmoothScrollWorkerMessage = WM_APP + 0x4D7U;
inline constexpr UINT kSmoothScrollNativeWheelMessage = WM_APP + 0x4D8U;
void SetExplorerSmoothScrollEnabled(bool enabled) noexcept;
[[nodiscard]] bool ExplorerSmoothScrollEnabled() noexcept;
void AttachExplorerSmoothScrollWindow(HWND window) noexcept;
void CancelExplorerSmoothScroll(HWND window, bool destroy = false) noexcept;
[[nodiscard]] bool HandleExplorerSmoothWheel(HWND window, WPARAM wheel, LPARAM point) noexcept;

// All COM references below are confined to the existing MTA event worker.
void StartExplorerSmoothScrollWorker() noexcept;
void StopExplorerSmoothScrollWorker() noexcept;
void RegisterExplorerSmoothScrollTarget(HWND window, IUIAutomation* automation, IUIAutomationElement* element,
    IUIAutomationScrollPattern* pattern, double view_percent, const RECT& bounds) noexcept;
void ProcessExplorerSmoothScrollRequest(HWND window) noexcept;
void AdvanceExplorerSmoothScroll() noexcept;
[[nodiscard]] DWORD ExplorerSmoothScrollWaitMilliseconds() noexcept;
} // namespace metaplasia::agent
