#pragma once

#include "metaplasia/protocol/agent_abi.hpp"

#include <Windows.h>
#include <Unknwn.h>

#include <cstdint>

namespace metaplasia::agent {

// {37C21DBA-462E-4BD0-8F75-84761104835A}
inline constexpr CLSID kExplorerWinUiTapClsid{
    0x37c21dba,
    0x462e,
    0x4bd0,
    {0x8f, 0x75, 0x84, 0x76, 0x11, 0x04, 0x83, 0x5a}};

[[nodiscard]] protocol::AgentResult ConfigureExplorerWinUiColor(
    bool enabled,
    std::uint32_t argb) noexcept;

// Applies the latest color to elements owned by the calling Explorer UI
// thread. The DWM hook calls this after Explorer processes activation/theme
// changes, preventing WinUI from silently restoring its stock brushes.
void ApplyExplorerWinUiColorForCurrentThread() noexcept;

[[nodiscard]] protocol::AgentResult StopExplorerWinUiColor() noexcept;

[[nodiscard]] HRESULT GetExplorerWinUiTapClassObject(
    REFCLSID class_id,
    REFIID interface_id,
    void** object) noexcept;

}  // namespace metaplasia::agent
