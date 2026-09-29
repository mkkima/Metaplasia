# Windows 11 Hyper-V end-to-end validation

Metaplasia reuses the disposable Windows 11 VM and clean checkpoint provisioned
by the ISeeYou lab, but uses a separate adapter and separate state namespaces.
The adapter reads ISeeYou's VM identity and DPAPI-protected credential; it never
writes ISeeYou packages, results, guest files, or `vm-state.json`.

## Isolation contract

- Source VM: `ISeeYou-Lab` with the unique `ISeeYou-Clean-Baseline` checkpoint.
- The default source is `C:\workspace\ISeeYou\out\lab\vm-state.json`;
  VM storage remains on the existing SSD under `C:\ProgramData\ISeeYouLab`.
  The adapter never clones, imports, or creates a VM. It never performs a broad
  Windows Update search or changes the host operating system.
- Host packages and results: `Metaplasia\out\e2e` only.
- Guest package, install, and results: `C:\MetaplasiaLab` only.
- Scheduled tasks: `Metaplasia-E2E-<random-id>` only.
- The VM must be Off and disconnected from every virtual switch before a run.
- Each run creates a uniquely named internal Hyper-V switch and one isolated
  host-to-guest `/30` subnet. It creates no NAT, gateway, DNS, or Internet
  sharing and removes the adapter and switch during cleanup.
- VM ID, Generation 2 configuration path, complete VHD chain, source credential
  path, and unique checkpoint are checked against ISeeYou's read-only
  `out\lab\vm-state.json` before the baseline is restored.
- Before any package copy or installation, the full Windows build and UBR must
  match the host. A mismatch writes `windows-version-preflight.json`, skips
  injection, and restores the baseline. Cleanup never reconnects to a VM that
  failed ownership validation or runs uninstall before staging a package.
- An optional `-WindowsUpdatePath` can prepare the existing VM from the pinned
  Microsoft KB5129195 x64 MSU (SHA-256 checked on host and guest). It runs DISM
  inside the disconnected guest only, waits for reboot, removes servicing
  staging, and saves `Metaplasia-Windows-26200.9457` in that same VM. This is
  marked `windows-serviced-only`, never a shell compatibility or E2E pass.
  PowerShell Direct runs in session 0 without Explorer: loaded shell fingerprints
  are collected only after interactive logon, before starting Metaplasia.
  The original ISeeYou checkpoint is never replaced. Subsequent runs select
  the prepared checkpoint by its recorded ID and intended host target; final rollback
  still restores ISeeYou's original checkpoint. Metadata stays in `out\e2e`.
  This saves repeated Windows installations without creating another VM.
- Before injection, the interactive guest also compares the host fingerprint:
  display version, installation type, build/UBR, BuildLabEx, architecture, and
  each Start module's path, PE/PDB key, SHA-256, and length.
  This workload enables only Start, so lazy-loaded Explorer-only modules are
  recorded but do not gate Start testing. The comparison tool defaults to all
  adapters; the Start workload explicitly selects `-Target start-menu`.
  Fingerprint schema 2 also includes the loaded Start UI payloads
  (`StartDocked.dll`, `StartMenu.dll`, `Windows.UI.Xaml.Controls.dll` when loaded).
  A matching launcher EXE alone does not prove the Start UI matches. Older
  fingerprints can identify a servicing checkpoint's intended target, but
  cannot pass workload verification.
  Product name and edition ID are evidence only: Pro and Pro for Workstations
  can contain the same shell binaries. A different revision cannot certify the
  host build even if the layout appears correct.
  Localized file-version strings are also evidence only: captured Russian and
  English resource strings differed for byte-identical images. Image hashes and
  PE/PDB keys remain mandatory; a genuine binary difference is never ignored.
- If the named checkpoint is missing and there are no other checkpoints, the
  adapter boots the still-disconnected identity-checked VM, proves that both
  projects' paths/processes/services and all temporary security/network state
  are absent, powers it off, and recreates only the original checkpoint name.
  Ambiguous checkpoints or any residue fail closed. Recovery evidence stays in
  Metaplasia's result directory and `vm-state.json` remains untouched.
- The baseline is restored after both successful and failed runs unless the
  explicit diagnostic `-KeepFailedVM` switch is used.

The two projects do not share packages, result directories, guest directories,
services, tasks, registry ownership, or update settings.

## One-command run

Run this from a normal PowerShell 7 session:

```powershell
.\tools\e2e\Start-MetaplasiaEndToEndLab.ps1
```

When explicitly authorized to show the test VM, add `-VisibleVM`. This opens
only the isolated RDP viewport in a labeled window; no screenshots are taken.
Hidden, non-activating transport remains the default. Both modes keep the same
limited guest workload, exact-image gate, and automatic cleanup/rollback.

The non-elevated stage parses the harness, builds a fresh Debug portable set,
runs native and Rust tests, checks Rust formatting and lints, and validates all
frontend JavaScript. It then requests UAC once. The single elevated stage owns
the remaining Hyper-V lifecycle using Windows PowerShell 5.1 and completes
unattended. A missing source VM/state is an error, never a provisioning request.
Do not run ISeeYou and Metaplasia labs concurrently; the adapter refuses a
running VM and restores the clean checkpoint before and after its own run.

To prepare the same VM for the pinned revision, download the x64 MSU from
[Microsoft KB5129195](https://support.microsoft.com/help/5129195), then pass its
path with `-WindowsUpdatePath`. This is guest-only servicing, not a host update.
The original 26200.8037 source already contains the KB5043080 prerequisite.
Servicing progress and DISM logs remain in the run's result directory. The MSU
cache can be removed after the serviced checkpoint is committed; later runs do
not need it. The checkpoint only saves OS servicing work: loaded shell binaries
and Start geometry must still pass the independent gates on every test run.

Debug components are intentional: the project permits its tightly scoped
unsigned development trust policy only in Debug builds. The VM remains
network-isolated and the package never leaves the test boundary.

## Guest lifecycle and checks

The adapter performs the following operations:

1. verifies (or safely reconstitutes) the clean checkpoint, restores it, and
   starts the isolated VM;
2. hashes every Metaplasia package file before and after PowerShell Direct copy;
3. installs only the five portable components under `C:\MetaplasiaLab\install`
   and grants the test user access only to that portable directory and its
   private result directory;
4. temporarily enables RDP only on the isolated guest address and host address,
   preserves the exact previous RDP/service state, and opens a local RDP
   client with every device, drive, clipboard, printer, and port redirect off;
   by default its off-screen host window uses `NOACTIVATE` and cannot take
   desktop focus. Explicit `-VisibleVM` shows only the labeled guest viewport;
5. completes the baseline's one-time first-user OOBE in a first session, then
   starts an on-demand scheduled task in a second interactive session. The task
   is bound to the expected SID and must prove it has a limited, non-admin token.
   The guest UI Automation client runs in an MTA; the separate RDP form stays STA;
6. enables only Start menu customization and confirms Taskbar remains disabled;
7. opens the real Windows 11 Start menu and requires native XAML diagnostics to
   be active only after `UpdateLayout` verifies the actual frame, panel, offset,
   visibility, and label geometry. Both UI Automation markers must also be
   observed inside a visible, uncloaked Start window as an independent signal;
8. repeatedly opens and closes Start, toggles All apps content, verifies live
   reconfiguration, disables and checks exact visual removal, terminates the
   Start host, and verifies reinjection against the replacement process;
9. fails on Application Error or Windows Error Reporting crash events for
   Metaplasia or `StartMenuExperienceHost.exe` during the workload;
10. disables the target, stops only binaries under the isolated install root,
   removes Metaplasia settings/startup/policy ownership, removes the temporary
   task, restores the saved RDP state, removes its firewall rule, and reboots;
11. verifies no process, task, startup entry, settings, policy ownership,
    RDP override, isolated firewall rule, or test-signing state remains; and
12. restores the original checkpoint and removes the temporary switch regardless
    of the test result.

Metaplasia contains no kernel driver, so enabling Driver Verifier for one of its
DLL or EXE files would be invalid. The dedicated adapter explicitly asserts that
the package contains no `.sys` file, records Driver Verifier settings and active
state during install and clean-state verification, and fails if either state
references Metaplasia. It does not reuse, enable, reset, or otherwise alter the
ISeeYou driver's Verifier configuration.

## Evidence

Every run produces a unique host result directory with:

- `host-status.json` as the top-level success/rollback result;
- `baseline-recovery-clean-state.json` when a missing checkpoint was safely
  reconstituted;
- host and elevated transcripts;
- guest install, uninstall, and post-reboot clean-state JSON;
- bounded Start XAML summaries and the complete active diagnostic snapshot;
- bounded `host.log` and `watchdog.log` copies on every run;
- live window/UI Automation geometry samples;
- guest-only input/thread desktop, foreground/shell HWND, and bounded window
  metadata (`guest-desktop-evidence.json`), including failures before injection;
- the final target snapshot and any matching crash events; and
- best-effort failure artifacts copied before cleanup.

A valid pass requires `success`, `cleanupVerified`, and `baselineRestored` to all
be `true`. A successful controller response without the native live-layout
verification is not an E2E pass. Missing UI Automation geometry makes the run
inconclusive and unsuccessful, including hidden RDP sessions that do not publish
the Start island. An internal `Active` status alone cannot certify a visual fix.

The workload verifies it is inside a Hyper-V guest before sending input. It
requires the active RDP session on `WinSta0` and the Default input desktop,
finds the real Start button through the taskbar's native child HWND providers,
and clicks the center of its rendered UI Automation bounds using a checked
mouse-input batch. The point
must lie inside the unobstructed guest taskbar; guessed coordinates and
host-desktop input are forbidden. The test requires an uncloaked Start window,
rendered UIA content, and a native hit on that content belonging to Start.
Foreground ownership alone is insufficient: the Search process can retain it
while Start is displayed. Both final panel markers must additionally pass the
native hit test, rejecting overlays from other windows. A preloaded full-monitor
CoreWindow alone does not prove an open menu. Opening and closing toggle only
the identified guest Start button. Missing visibility always fails the run,
even when injection and internal layout checks succeeded. If the first toggle
closes a first-logon Start/Search pair, the test retries once only after DWM
confirms that every Start window is cloaked. An open or ambiguous surface is
never blindly toggled again.

Opening the unmodified menu before fingerprinting is necessary only when a
required UI payload is not loaded yet. Already loaded payloads still pass the
same complete hash gate before injection. After runtime startup, failures also
save the actual target snapshot, XAML state, UIA geometry, and bounded shell
activation/crash events. Failure evidence never counts as an E2E pass.

The 26200.9457 Start compatibility entry is validated for the Debug/development
runtime. Run `20260929-170508-832` passed with five Start images matching the
target host byte-for-byte. Both labels were visible and unobstructed (x=114 and
x=980), including after six reconfiguration cycles and a Start process restart.
Disable, uninstall, post-reboot cleanliness, original-baseline rollback and
isolated-network removal passed. Taskbar remained disabled and uninjected.
This is geometry/input validation at 1366x768, not a screenshot/pixel comparison.
Release/Authenticode runtime certification remains separate; that profile stays
excluded from Release builds.
