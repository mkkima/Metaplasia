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
bounded `VisualDiagConnection1` through `VisualDiagConnection4` endpoints.
Its TAP object obtains
`IXamlDiagnostics` and `IVisualTreeService3` through `IObjectWithSite` and
subscribes an `IVisualTreeServiceCallback2`. The callback explicitly exposes
both the derived and base callback IIDs required by the COM contract.

`InitializeXamlDiagnosticsEx` can return before its `SetSite` activation has
completed. The adapter therefore creates a readiness event before calling the
initializer. `SetSite` prepares the controller and starts
`AdviseVisualTreeChange` on a dedicated worker, then returns immediately to
avoid re-entering the XAML initializer. The configuration call waits at most
five seconds for `service + watcher + advised`; a timeout never terminates the
worker or releases objects it can still access. The host also gives a newly
observed Start process five seconds to finish its own XAML startup before the
first injection. If the diagnostics API accepts a connection but never creates
the TAP site, the next reconciliation uses the next endpoint. Four failed
endpoints are terminal for that process, preventing an unbounded retry loop.

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
missing. Callback failures return success to XAML Diagnostics so an optional
style cannot disrupt visual-tree enumeration, but the failure stage and native
HRESULT remain in the bounded agent health snapshot and are copied into the
persistent host log when diagnostics polls it.
The shared tracker is bounded to 32 shell elements and refuses overflow before
writing an element. Adapter failures include the native HRESULT, lifecycle
stage, and a compact state word whose low bits report service, watcher, and
subscription presence. Bits 4-7 report the bounded initialization-attempt
count; tracked-element count begins at the next byte. The persistent host log
also decodes these fields into readable `attempts`, `service`, `watcher`, and
`subscription` values.

Normal destruction of the temporary TAP activation object releases only its
site reference. It does not masquerade as `SetSite(nullptr)` and cannot tear
down the process-owned controller. An explicit detach still restores tracked
values and removes the visual-tree subscription.

The adapter does not scan memory, patch XAML vtables, guess property indexes,
or use private symbols. It also does not attempt to treat a
`Microsoft.UI.Xaml` object as a `Windows.UI.Xaml` object. WinUI 3 support will be
a separate adapter with its own compatibility evidence.

## Three-panel layout

The optional **Three-panel Start** control keeps Windows' native Pinned,
Recommended, and footer controls, while the left panel presents the operating
system's `shell:AppsFolder` inventory in a scrollable alphabetical list. Each
left-panel row activates the corresponding shell item; launch work is deferred
until after the XAML click event returns so closing Start cannot invalidate an
in-flight event dispatch. The layout is enabled only together with the Start
target. It hides the native search toggle, expands the verified Start frame to
1246 by 624 device-independent pixels, and creates three rounded panels from
the native acrylic brush.

The adapter accepts two exact scene contracts: the blended frame selects the
named `MainMenu` container, while the sizing frame selects `RootContent`.
Relation checks distinguish the main Start acrylic surfaces from same-named
companion surfaces before applying the layout. The sizing contract additionally
accepts its observed `TopLevelSuggestionsContainer`; the blended contract uses
`TopLevelSuggestionsRoot`. Width, height, margin, alignment, grid position,
visibility, opacity, original parent, and child index are captured before
mutation. Disable restores those values in dependency order; a failed
restoration keeps its snapshot so a later configuration pass can retry.

The blended frame-envelope contract requires the exact visual ancestry
`Border -> ScrollContentPresenter -> RootScrollViewer`; the sizing contract
requires its exact `Canvas` root. `GetRuntimeClassName` can project the internal
root scroll viewer through its public `Windows.UI.Xaml.Controls.ScrollViewer`
base on revision 26200.8037, and that single type/index pair is explicitly
allowed. `FrameworkElement.Parent` is not used because template boundaries can
make the logical tree differ from the visual tree. A concrete parent must still
be an approved `FullWindowMediaRoot` or `Window`. On builds where
`VisualTreeHelper.GetParent` ends at the verified root surface with
`ERROR_NOT_FOUND`, the surface is treated as Window-rooted and is validated but
not resized. This matches XAML Diagnostics, where `FullWindowMediaRoot` and
`PopupRoot` may be sibling root surfaces rather than ancestors of Start.

The expanded clipping envelope, the three acrylic surfaces, and their injected
content are committed as one scene. Until the current `MainMenu` is confirmed
as a descendant of the current frame and every required surface is available,
tracked native elements keep their original geometry. A transient attachment
failure rolls back the complete scene and retries on a later visual-tree event;
the adapter never leaves a wide inner frame inside the stock narrow popup.
Replacement frames are ancestry-checked so late removal of an older frame
cannot tear down or demote the health state of the current scene. Before
publishing `active`, the adapter runs `UpdateLayout` on the live main surface
and verifies the actual frame size, all three panel sizes and offsets, panel
visibility, and non-zero visible label geometry. A failed geometry check rolls
back the complete scene and reports `apply-element-layout` rather than exposing
a false healthy state.

Deterministic contract or property failures are not retried for every unrelated
visual-tree callback. One failure is retained in diagnostics until the user
reconfigures the target or a required scene dependency changes. Other repeated
scene failures are capped at three consecutive attempts per unchanged
dependency set.

Creating or reparenting scene content itself produces visual-tree callbacks.
The complete scene transaction is therefore guarded against re-entry, including
frame, panel, Recommended, All apps, layout, and rollback operations. Generated
child callbacks may still be observed, but they cannot begin a nested scene
commit against partially constructed snapshots.

In Three-panel mode, **Hide All apps content** changes only the visibility of
the injected list. The left panel and the full three-panel frame stay attached,
so the setting can be changed live without restarting Start or changing the
layout width. Windows' legacy `NoStartMenuMorePrograms` policy is reserved for
the native Start layout because that policy constrains the top-level popup to
the native width and would clip the injected center and right panels. When a
Metaplasia-owned policy from an earlier build is detected while enabling or
editing Three-panel mode, the control center removes it once through the
elevated policy helper and persists the equivalent injected setting.

**Hide Recommended** still controls the native right-panel content. The
current geometry is intended for a 1920 by 1080 desktop at 100% scaling;
smaller effective work areas require a future responsive layout rather than
unsafe clipping.

## Empty All apps section

For the native Start layout, the control center uses the operating system's
documented "Remove All Programs list" policy. Metaplasia writes the
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
element removal, and the exported TAP COM factory. The separate Windows 11
Hyper-V E2E harness injects into a disposable live Start menu, requires the
native live-layout verification, and additionally checks unique UI Automation
markers when the hidden RDP provider exposes the Start island; see
[Windows 11 end-to-end validation](WINDOWS_E2E.md).

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
