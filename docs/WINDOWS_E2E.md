# Windows 11 Hyper-V end-to-end validation

Metaplasia reuses the disposable Windows 11 VM and clean checkpoint provisioned
by the ISeeYou lab, but uses a separate adapter and separate state namespaces.
The adapter reads ISeeYou's VM identity and DPAPI-protected credential; it never
writes ISeeYou packages, results, guest files, or `vm-state.json`.

## Isolation contract

- Source VM: `ISeeYou-Lab` with the unique `ISeeYou-Clean-Baseline` checkpoint.
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

The non-elevated stage parses the harness, builds a fresh Debug portable set,
runs native and Rust tests, checks Rust formatting and lints, and validates all
frontend JavaScript. It then requests UAC once. The single elevated stage owns
the remaining Hyper-V lifecycle and completes unattended.

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
   preserves the exact previous RDP/service state, and opens a hidden local RDP
   client with every device, drive, clipboard, printer, and port redirect off;
5. completes the baseline's one-time first-user OOBE in a first session, then
   starts an on-demand scheduled task in a second interactive session. The task
   is bound to the expected SID and must prove it has a limited, non-admin token;
6. enables only Start menu customization and confirms Taskbar remains disabled;
7. opens the real Windows 11 Start menu and requires native XAML diagnostics to
   be active only after `UpdateLayout` verifies the actual frame, panel, offset,
   visibility, and label geometry. Unique UI Automation markers are checked as
   a second signal when the hidden RDP session publishes the Start island;
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
- the final target snapshot and any matching crash events; and
- best-effort failure artifacts copied before cleanup.

A valid pass requires `success`, `cleanupVerified`, and `baselineRestored` to all
be `true`. A successful controller response without the native live-layout
verification is not an E2E pass. UI Automation is additional evidence, but its
absence alone is not a failure on hidden RDP sessions that do not publish the
Start XAML island to the desktop provider.
