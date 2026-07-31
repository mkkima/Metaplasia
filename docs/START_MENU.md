# Start menu XAML adapter

The Start menu adapter covers discovery, targeted injection, XAML Diagnostics
attachment, visual-tree observation, reversible mutation, and teardown.

It supports a Start-only opaque solid color by replacing the `Background`
brush on the exact `Windows.UI.Xaml.Controls.Border` elements named
`AcrylicBorder` and `AcrylicOverlay`. The original COM brush is retained before
the first write and restored exactly when the color or target is disabled.

## Supported path

The adapter runs only in `StartMenuExperienceHost.exe`. It requires an already
loaded `Windows.UI.Xaml.dll` with the standard
`InitializeXamlDiagnosticsEx` export and uses the
`VisualDiagConnection1` endpoint. Its TAP object obtains
`IXamlDiagnostics` and `IVisualTreeService3` through `IObjectWithSite` and
subscribes an `IVisualTreeServiceCallback2`. The callback explicitly exposes
both the derived and base callback IIDs required by the COM contract.

`InitializeXamlDiagnosticsEx` can return before its `SetSite` activation has
completed. The adapter therefore creates a readiness event before calling the
initializer. `SetSite` prepares the controller and starts
`AdviseVisualTreeChange` on a dedicated worker, then returns immediately to
avoid re-entering the XAML initializer. The configuration call waits at most
five seconds for `service + watcher + advised`; a timeout never terminates the
worker or releases objects it can still access.

Only these exact visual type identities are currently approved:

- `StartDocked.StartSizingFrame`;
- `StartMenu.StartInnerFrame` (legacy compatibility);
- `StartMenu.StartBlendedFlexFrame` (current certified build).

The optional Recommended rule matches only
`Windows.UI.Xaml.Controls.Grid` / `MoreSuggestionsRoot` and changes its
standard `Visibility` property to `Collapsed`.

For an approved root, the adapter resolves its standard
`Windows.UI.Xaml.IUIElement` interface, reads the current opacity, and records
it in a fixed-capacity table before applying the configured value from 10% to
100%. Visibility and brush ownership record the exact original value in the
same way.
Reconfiguration and restoration are marshalled to the XAML dispatcher. Disable
restores recorded values rather than assuming Windows defaults.

## Failure behavior

The feature is rejected without modifying a visual when any required module,
export, COM interface, endpoint, dispatcher, or approved root contract is
missing. Callback failures are logged to the debugger but return success to
XAML Diagnostics so an optional style cannot disrupt visual-tree enumeration.
The shared tracker is bounded to 32 shell elements and refuses overflow before
writing an element. Adapter failures include the native HRESULT, lifecycle
stage, and a compact state word whose low bits report service, watcher, and
subscription presence; tracked-element count occupies the next byte.

Normal destruction of the temporary TAP activation object releases only its
site reference. It does not masquerade as `SetSite(nullptr)` and cannot tear
down the process-owned controller. An explicit detach still restores tracked
values and removes the visual-tree subscription.

The adapter does not scan memory, patch XAML vtables, guess property indexes,
or use private symbols. It also does not attempt to treat a
`Microsoft.UI.Xaml` object as a `Windows.UI.Xaml` object. WinUI 3 support will be
a separate adapter with its own compatibility evidence.

## Empty All apps section

The control center exposes the operating system's documented "Remove All
Programs list" policy separately from XAML injection. Metaplasia uses the
device-scope DWORD
`HKLM\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer\NoStartMenuMorePrograms=1`,
which maps to "Remove and disable setting" in `StartMenu.admx`. This removes
the All apps contents instead of replacing Category with Grid.

An ownership marker under `HKCU\Software\Metaplasia\PolicyOwnership` prevents
Metaplasia from overwriting or removing an unrelated machine policy. Enable
writes DWORD `1`; disable deletes only a device value owned by Metaplasia. The
obsolete `HideCategoryView` device policy and ownership marker written by
earlier Metaplasia builds are removed during migration.

Device policy changes require administrator approval. The application
relaunches the same application executable through the standard `runas`
consent flow. The elevated entry point accepts only the exact
`--apply-start-all-apps-policy 0|1` command, performs no UI startup, and exits
after the single verified registry write.

Microsoft documents that the app-list policy requires a shell restart. After
a successful write, Metaplasia terminates only the exact current-session Start
host and the `explorer.exe` instance owning `Shell_TrayWnd`, then starts the
system Explorer image again without a console. Other users' sessions and
auxiliary Explorer processes are not targeted.

The control is offered only on supported Pro, Enterprise, Education, and IoT
Enterprise editions at Windows 11 build 26100.7019 / 26200.7019 or newer. It
does not edit `start2.bin`, automate localized UI text, or inject another Start
hook.

## Verification policy

Automated tests exercise allowlist matching, range validation, duplicate
events, bounded tracking, write failure, reconfiguration, exact restoration,
element removal, and the exported TAP COM factory. They do not inject into the
live Start menu.

Before live enablement, test a clean disposable VM snapshot for each supported
Windows build. Record at least the OS build, `StartDocked.dll` identity, XAML
module identity, observed root type, normal enable/disable, Start host restart,
host restart, sign-out, and crash-loop recovery. Do not test alongside
Windhawk, ExplorerPatcher, StartAllBack, or another shell injector.

API references:

- [InitializeXamlDiagnosticsEx](https://learn.microsoft.com/en-us/windows/win32/api/xamlom/nf-xamlom-initializexamldiagnosticsex)
- [IXamlDiagnostics](https://learn.microsoft.com/en-us/windows/win32/api/xamlom/nn-xamlom-ixamldiagnostics)
- [IVisualTreeService3](https://learn.microsoft.com/en-us/windows/win32/api/xamlom/nn-xamlom-ivisualtreeservice3)
- [AdviseVisualTreeChange](https://learn.microsoft.com/en-us/windows/win32/api/xamlom/nf-xamlom-ivisualtreeservice-advisevisualtreechange)
- [Start policy settings: Hide app list](https://learn.microsoft.com/en-us/windows/configuration/start/policy-settings#hide-app-list)
- [Microsoft Start menu troubleshooting](https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/troubleshoot-start-menu-errors)
