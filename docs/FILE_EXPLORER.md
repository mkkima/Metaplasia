# File Explorer adapter

File Explorer windows do not necessarily live in the process that owns the
Taskbar. Metaplasia enumerates top-level `CabinetWClass` and `ExploreWClass`
windows, maps them to same-session `explorer.exe` processes, and applies the
adapter only to those process IDs. Service-style `/factory ... -Embedding`
Explorer processes without folder windows are not injected. Opening or closing
a folder window is reconciled automatically.

The optional Explorer-only color is applied to the complete folder window. The
adapter coordinates DWM caption attributes, the compositor accent, narrowly
scoped DirectUI canvas painting, the native `SysTreeView32` navigation tree,
the DirectUI `UIViewHeader` used by Details view, and owned WinUI 3 background
brushes for navigation, command, Home, Gallery, and Details surfaces. The
header adapter replaces only the themed background and cell surfaces; native
text, sort arrows, hover states, separators, and column resizing remain
intact. It preserves Explorer's native frame margins so DWM keeps ownership
of the minimize, maximize, and close glyphs. Activation, theme, and
composition changes reapply the owned colors without a polling timer.

Directory navigation is covered by a preallocated, non-activating DirectUI
guard owned by the Explorer window. It stays opaque for the first composition
frame and then reveals the completed directory with one of four persisted
modes: no visible animation, fade, slide plus fade, or scale plus fade. The
timer runs on Explorer's UI thread at a 16 ms cadence, uses integer smoothstep
easing, never captures user content, and restores its full bounds and opacity
before it is hidden for reuse.

The navigation tree's original background and text colors are captured before
the first write. A persisted `Custom scrollbars` switch controls both its
non-client vertical scrollbar and the DirectUI file-area scrollbar. Native hit
testing remains intact while same-thread, hit-transparent owned popups cover
the system rendering. The TreeView popup uses native scroll metrics; the file
area uses the supported UI Automation Scroll pattern exposed by `UIItemsView`
to mirror its scroll percentage and viewport size without polling Explorer's
UI thread. Both draw the configured track color and a contrasting rounded
capsule that widens on hover without changing the native hit targets. Turning
the switch off destroys both popups immediately and repaints the native
surfaces; subclasses are also removed before agent teardown. Disabling the
Explorer color restores the TreeView colors and captured WinUI brushes,
disables the window accent, and passes `DWMWA_COLOR_DEFAULT` back to DWM for
the caption and caption text.

The agent intercepts `SetWindowTextW` only for top-level folder-window classes
owned by its current process. Prefixes are validated UTF-8, converted to
bounded UTF-16, and limited to 31 UTF-16 code units. Title construction uses a
fixed 4096-code-unit buffer and refuses overflow.

Ownership is explicit. A window enters the bounded ownership table only after
Metaplasia successfully writes a transformed title. A title already carrying
the configured prefix is not claimed. If an external caller later supplies an
already-prefixed title, Metaplasia releases ownership so disable cannot strip
text it no longer owns. Prefix changes restore owned titles first and then
apply the replacement. Disable restores only still-owned titles.

The host maintains independent configuration and exact compatibility state for
every folder-window process. The UI/CLI target reports active only after all
currently open folder-window processes have accepted the configuration. With
no folder windows open, the adapter remains armed without injecting an idle
Explorer process.
