#pragma once

#include "metaplasia/protocol/agent_abi.hpp"

#include <Windows.h>

#include <cstdint>

namespace metaplasia::agent {

[[nodiscard]] protocol::AgentResult ConfigureExplorerWindowColor(
    bool enabled,
    std::uint32_t argb,
    std::uint32_t transition_animation,
    bool custom_scrollbar_enabled) noexcept;

// Called from the existing Explorer window-title hook whenever Explorer
// creates or updates a folder window. This keeps newly opened windows in sync
// without installing another broad process hook.
void ObserveExplorerWindowForColor(HWND window) noexcept;

// Reapplies colors to native Explorer child controls which Windows can
// recreate after a theme or composition change. The WinUI adapter invokes
// this on the owning window thread together with its XAML brush refresh.
void RefreshExplorerNativeControlsForWindow(HWND window) noexcept;

// Prepares the already-painted transition guard on the Explorer UI thread.
// This removes the only first-navigation allocation from the critical path.
void PrimeExplorerTransitionOverlayForWindow(HWND window) noexcept;

// Removes native-control subclasses before the agent can be unloaded.
void DetachExplorerNativeControlsForWindow(HWND window) noexcept;

// Stops all Explorer color adapters and returns owned DWM/DirectUI/WinUI
// properties to Windows. This is separate from Configure(false) so agent
// shutdown can also release the WinUI diagnostics subscription.
[[nodiscard]] protocol::AgentResult StopExplorerWindowColor() noexcept;

}  // namespace metaplasia::agent
