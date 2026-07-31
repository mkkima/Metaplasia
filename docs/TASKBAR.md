# Taskbar adapter

The Taskbar adapter combines one narrowly filtered native hook with a
reversible Windows.UI.Xaml property engine inside the shell `explorer.exe`.
The host selects the process that owns `Shell_TrayWnd`; it never applies
Taskbar features to auxiliary Explorer processes.

## Supported customizations

- UTF-8/UTF-16 validated clock prefix, up to 15 UTF-16 code units;
- optional responsive floating capsule with a 1420-DIP maximum width,
  12-DIP minimum outer margins, 2-DIP vertical spacing, 12-DIP corners, and a
  1-DIP non-interactive outline;
- independent opaque solid background color (`#RRGGBB` in the UI/CLI);
- Taskbar opacity from 10% to 100%;
- optional hiding of Notification Center;
- optional hiding of Control Center;
- optional hiding of the Show Desktop area.

The clock hook intercepts `GetTimeFormatEx` but changes a result only when the
return address belongs to `Taskbar.dll`, `Taskbar.View.dll`, or
`twinui.pcshell.dll`. Buffer sizing, null termination, overflow checks, and the
unmodified pass-through path follow the Win32 contract.

The XAML rules use identities observed on the certified Windows build:

- opacity: `Taskbar.TaskbarFrame` / `TaskbarFrame`;
- opacity: `SystemTray.SystemTrayFrame` with an empty element name;
- capsule layout root: the common parent of the exact
  `Taskbar.TaskbarFrame` / `TaskbarFrame` element and its
  `SystemTray.SystemTrayFrame` sibling;
- capsule background geometry:
  `Taskbar.TaskbarBackground` / `BackgroundControl`;
- fill: `Windows.UI.Xaml.Shapes.Rectangle` / `BackgroundFill`;
- outline: `Windows.UI.Xaml.Shapes.Rectangle` / `BackgroundStroke`;
- visibility: `SystemTray.OmniButton` / `NotificationCenterButton`;
- visibility: `SystemTray.OmniButton` / `ControlCenterButton`;
- visibility: `SystemTray.Stack` / `ShowDesktopStack`.

Every matched element is resolved through the standard XAML ABI. The engine
reads and retains its exact original opacity, visibility, brush, size,
alignment, hit-test behavior, margin, or corner radius before the first write.
Capsule margins are calculated from the actual shared-layout width, so a
1920-DIP Taskbar gets 250-DIP visual boundaries while narrow displays retain
at least 12 DIPs. The common parent owns the right layout boundary that keeps
System Tray inside the capsule. The background receives matching left and
right margins so both rounded edges are rendered before the parent's clipping
boundary. The native one-edge stroke is expanded into a transparent,
non-interactive rounded rectangle with a `#4A4A4A` outline. Its 1-DIP stroke
path is inset by 0.5 DIPs and uses an 11.5-DIP radius, keeping the outer stroke
edge aligned with the capsule's 12-DIP boundary without clipping the curve.
Layout rounding is disabled only for the outline so the half-DIP compensation
is not rounded to a full-pixel inset. This
preserves Windows' native task-list centering and avoids fighting the
system-owned `SystemTrayFrame.Margin` value. Without an explicit custom
Taskbar color, the capsule uses opaque `#151519`; an enabled custom color takes
precedence.

Reconfiguration runs on the XAML dispatcher. Turning off the capsule or the
Taskbar target restores the exact captured geometry and brush. Full disable
restores all owned values, releases ownership, removes the visual-tree
subscription, and then removes the clock hook.

## Diagnostics and failure behavior

`metaplasia-cli xaml-types taskbar` returns a bounded local snapshot of observed
types and named/custom elements. The command never injects an agent. It works
only when the exact Metaplasia agent is already mapped in the selected shell
process.

Taskbar activation is transactional. If either native-hook installation or
the XAML adapter fails, the agent returns every component to the last committed
configuration. Unknown types, unexpected names, unsupported Windows builds,
missing XAML interfaces, and ownership-table overflow fail closed.
