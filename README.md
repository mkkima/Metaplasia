# Metaplasia

Metaplasia is a native Windows 11 shell-customization engine for the taskbar,
File Explorer, and Start menu. Its control center uses Tauri v2 with a bundled
AMOLED web frontend; shell modifications live in a small, independently
controlled C++ agent DLL.

The repository currently contains the first end-to-end engine slice:

- asynchronous Tauri control center with live target state and direct local
  named-pipe commands;
- actionable Diagnostics with Taskbar/Start XAML selection, native error
  decoding, and bounded rotating local host logs;
- per-user host, process discovery, persistence, and reinjection monitoring;
- authenticated, versioned named-pipe protocol shared by the UI and CLI;
- same-user, same-session, same-architecture targeted injector;
- mapped-image PE export resolution that never mixes a rebuilt DLL's RVA with
  an older agent already loaded in a shell process;
- pre-injection loaded-module policy that rejects known Windhawk and
  ExplorerPatcher conflicts while preserving the ability to disable;
- minimal agent DLL with explicit start/configure/stop exports;
- a reversible Taskbar clock-prefix hook plus exact XAML rules for a responsive
  floating capsule, solid background color, opacity, Notification Center,
  Control Center, and Show Desktop;
- a reversible ownership-aware File Explorer title adapter plus a composite
  DWM, WinUI 3, and DirectUI full-window color adapter, dynamically applied only
  to `explorer.exe` processes that own folder windows;
- a shared fail-closed Windows.UI.Xaml Diagnostics/TAP adapter that observes
  Taskbar and Start trees and exactly restores every owned property;
- PE/PDB compatibility fingerprints, executable-section validation, serialized
  DbgHelp resolution, and SHA-256 code fingerprints;
- per-adapter fail-closed compatibility catalog that checks the exact Windows
  build revision and PE/PDB identity of modules mapped in each shell process;
- production Authenticode trust gate requiring the host, watchdog, agent, and
  optional compatibility pack to share one exact leaf signer certificate;
- signed, resource-only external compatibility packs that can add exact
  profiles without executing pack code or overriding compiled approvals;
- integrated crash-loop protection that disables a target after three observed
  shell-process exits in 60 seconds;
- independently supervised per-session watchdog that enters persistent safe
  mode and deactivates already loaded agents after an ungraceful host exit;
- protocol, component-trust, named-pipe transaction, settings-store, symbol,
  compatibility-pack, injector-policy, hook,
  watchdog-protocol, COM-factory, and shell XAML property-engine tests.

The current Start adapter supports an independent solid surface color,
configurable root opacity, and optional removal of the Recommended section. The
current Taskbar adapter supports its own solid background color, configurable
opacity, and optional removal of three exact system-tray elements. Both use
observed, exact type/name identities and retain the original property values for
live reconfiguration and disable. File Explorer has a separately persisted
full-window color: an opaque compositor surface covers the DirectUI navigation
tree and folder canvas while owned WinUI 3 brushes cover the tab, navigation,
and command surfaces. Directory changes use a persisted reveal mode (`none`,
`fade`, `slide`, or `scale`) while the opaque guard prevents the stock gray
composition frame from becoming visible. If XAML Diagnostics or an approved identity is
unavailable, an adapter fails closed instead of guessing. See the
[Taskbar adapter](docs/TASKBAR.md) and [Start menu adapter](docs/START_MENU.md).

No global injection, kernel driver, system-file replacement, or registry-based
shell patching is used.

## Components

- `metaplasia.exe` — graphical control center.
- `metaplasia-host.exe` — one orchestration process per interactive session.
- `metaplasia-watchdog.exe` — minimal crash recovery process supervised by the
  host and scoped to the same user/session.
- `metaplasia-cli.exe` — diagnostics and explicit configuration commands.
- `metaplasia-agent.dll` — code loaded only into a selected shell process.
See [Architecture](docs/ARCHITECTURE.md), [Security](docs/SECURITY.md), and
[Diagnostics](docs/DIAGNOSTICS.md) before adding new hooks.

## Requirements

- Windows 11 x64;
- Visual Studio with the MSVC C++ workload and a current Windows SDK;
- CMake 3.24 or newer;
- Ninja;
- Rust/Cargo for the Tauri control center;
- Microsoft Edge WebView2 Runtime (included with supported Windows 11 builds).

## Configure, build, and test

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

Release configuration:

```powershell
cmake --preset windows-msvc-release
cmake --build --preset windows-msvc-release
ctest --preset windows-msvc-release
```

To build the native engine without the Tauri control center and its Rust build
dependencies:

```powershell
cmake --preset windows-msvc-debug -DMETAPLASIA_BUILD_UI=OFF
cmake --build --preset windows-msvc-debug
```

All executables and the agent DLL are placed in `build/<configuration>/bin`.
Keep them together: the host resolves both the agent and watchdog relative to
its own executable and refuses to activate without a completed watchdog
readiness handshake. Native C++ and Rust/Tauri builds statically link their
Microsoft C/C++ runtime dependency, so the five-file application stays portable
and does not require a separately installed Visual C++ Redistributable.

## Run

Start the control center; it starts the per-session host automatically:

```powershell
.\build\debug\bin\metaplasia.exe
```

The CLI expects a running host:

```powershell
.\build\debug\bin\metaplasia-cli.exe snapshot
.\build\debug\bin\metaplasia-cli.exe settings
.\build\debug\bin\metaplasia-cli.exe set taskbar-color "#101820"
.\build\debug\bin\metaplasia-cli.exe set taskbar-color-enabled true
.\build\debug\bin\metaplasia-cli.exe set taskbar-capsule-enabled true
.\build\debug\bin\metaplasia-cli.exe set taskbar-opacity 90
.\build\debug\bin\metaplasia-cli.exe set taskbar-hide-notification-center true
.\build\debug\bin\metaplasia-cli.exe set explorer-title-prefix "Files · "
.\build\debug\bin\metaplasia-cli.exe set explorer-color "#101820"
.\build\debug\bin\metaplasia-cli.exe set explorer-color-enabled true
.\build\debug\bin\metaplasia-cli.exe set explorer-animation slide
.\build\debug\bin\metaplasia-cli.exe set start-menu-opacity 94
.\build\debug\bin\metaplasia-cli.exe set start-menu-color "#101820"
.\build\debug\bin\metaplasia-cli.exe set start-menu-color-enabled true
.\build\debug\bin\metaplasia-cli.exe set start-menu-hide-recommended true
.\build\debug\bin\metaplasia-cli.exe enable taskbar --confirm
.\build\debug\bin\metaplasia-cli.exe disable taskbar
.\build\debug\bin\metaplasia-cli.exe module-info C:\Windows\System32\ntdll.dll
.\build\debug\bin\metaplasia-cli.exe component-trust
.\build\debug\bin\metaplasia-cli.exe authenticode-info C:\Windows\System32\kernel32.dll
.\build\debug\bin\metaplasia-cli.exe compatibility-report
.\build\debug\bin\metaplasia-cli.exe resolve-symbol C:\Windows\System32\ntdll.dll RtlGetVersion
```

Valid target names are `taskbar`, `file-explorer`, and `start-menu`. The
`--confirm` flag is mandatory for CLI enable operations. Settings are stored
atomically in `%LOCALAPPDATA%\Metaplasia\settings.conf`; a missing or malformed
file starts in fail-safe mode with all features disabled.

Shell customization and control traffic remain entirely local over a
per-session named pipe. The only production network client is the portable
updater: when automatic updates are enabled or the user clicks **Check now**, it
uses outbound HTTPS solely for signed assets from this repository's GitHub
Releases. The **Updates** page exposes the current channel, bounded local
installation history, cache cleanup, manual controls, and same-channel signed
rollback. There is no telemetry, cloud service, network listener, local web
server, installer, or background update service. See [Portable updates](docs/UPDATES.md).
The control center loads static HTML, CSS, and JavaScript bundled inside
`metaplasia.exe`. CMake may fetch the pinned MinHook source and Cargo may
download dependencies fixed by `src-tauri/Cargo.lock` during a developer build.

Unsigned binaries are accepted only by the Debug configuration, and only when
the complete checked component set is unsigned. An unsigned Release host exits
before launching its watchdog or accepting commands. Production artifacts must
be signed as described in [Component trust](docs/TRUST.md).

## Safety notice

This is pre-release shell-injection software. Test native hooks in a disposable
Windows VM with a snapshot. Do not enable Metaplasia in a process already
modified by Windhawk, ExplorerPatcher, StartAllBack, or another injector: hook
ownership and teardown ordering would be undefined.

The host deactivates loaded agents during a normal shutdown and disables
features after an observed crash loop. If the host is forcibly terminated, the
watchdog atomically persists all-disabled settings and sends an empty
configuration only to exact Metaplasia agents already mapped in same-session
shell processes. It never injects a clean process, unloads a DLL, kills a shell
process, or restarts Explorer. See [Recovery](docs/RECOVERY.md). Multi-build
compatibility certification and a production signing/installation pipeline
remain release blockers. See [Compatibility](docs/COMPATIBILITY.md) and
[Component trust](docs/TRUST.md).

Symbol inspection and cache layout are documented in
[Symbols and compatibility](docs/SYMBOLS.md). Third-party revisions and
licensing notes are listed in
[THIRD_PARTY.md](THIRD_PARTY.md).
