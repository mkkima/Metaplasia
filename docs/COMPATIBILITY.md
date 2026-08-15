# Windows compatibility catalog

Metaplasia treats every cumulative Windows update as a potentially incompatible
shell implementation. A marketing version, build family, exported symbol name,
or matching file path is not enough to approve native hooks.

## Decision inputs

Each adapter has an independent compiled profile containing:

- exact `major.minor.build.UBR` Windows version;
- exact expected path of every required module relative to `%SystemRoot%`;
- PE machine, COFF timestamp, image size, checksum, PDB filename, PDB GUID, and
  PDB age encoded in the module compatibility key;
- an adapter-specific profile identifier.

The compiled catalog contains separate exact profiles for Windows 11 25H2 x64
revisions `10.0.26200.8875` and `10.0.26200.9168`. The latter was captured from
the mapped shell processes and cross-checked against the protected on-disk
images on the affected machine. Neither entry implies support for any other
revision in the 26200 family, and broad compatibility still requires a clean
multi-build VM matrix.

## Signed external profiles

An optional sibling `metaplasia-compatibility-pack.dll` can add profiles for
new exact Windows revisions without rebuilding the engine. It is loaded as a
resource-only PE image, never as executable code, and must have a trusted
embedded Authenticode signature from the same exact leaf signer certificate as
the running component set. A present but invalid pack blocks host startup.

The versioned binary resource parser caps the complete pack, profile count,
record sizes, module count, and every string. Paths are relative, traversal-
free, restricted to a narrow ASCII character set, and must end in the declared
module name. Duplicate or ambiguous adapter/version entries fail closed. An
external profile cannot override a compiled profile. See
[Component trust and pack format](TRUST.md).

## Mapped-image inspection

Runtime approval reads the PE headers and RSDS record from the images actually
mapped in Explorer or `StartMenuExperienceHost.exe`. It does not calculate a
decision from files at the same paths on disk. This distinction matters during
servicing: an update can replace a DLL while an older copy remains mapped in a
long-running shell process.

All remote offsets are bounded by the module snapshot's `SizeOfImage`. The
header image size must match that snapshot, debug directory counts and CodeView
record sizes are capped, and malformed or unreadable data fails closed.

## Adapter isolation

Taskbar, File Explorer, and Start menu are evaluated separately. For example,
an unknown Taskbar module blocks only the taskbar feature bit; a separately
approved File Explorer title adapter in the same agent may remain active. The
host reports the blocked target as `incompatible` and never loads an agent when
all requested bits for that process are rejected.

Compatibility decisions are cached for 30 seconds per adapter and target PID.
Refreshes re-read the true Windows revision and mapped identities. If a
previously active adapter becomes incompatible, the host sends an empty or
filtered configuration using the configure-only path.

## Diagnostic report

The report does not require a running host:

```powershell
.\build\debug\bin\metaplasia-cli.exe compatibility-report
```

It prints the true Windows version, selected shell PID, decision, profile ID,
mapped module paths, and exact observed keys. Unknown Windows revisions remain
blocked, but the report now still inspects the bounded adapter-specific module
set so the output contains the evidence required to diagnose and add a new
exact profile. `module-info` remains useful for offline PE inspection, but its
result alone must never be copied into the catalog as approval.

## Adding a profile

Compiled profiles are source-controlled code. Verified external packs may add
new exact profiles. Runtime learning, wildcards, build ranges, and a
user-facing “ignore compatibility” switch are intentionally absent.

Before adding a profile:

1. Start from a clean disposable VM snapshot with no other shell injector.
2. Capture `compatibility-report` and verify every module path is a protected
   Windows location.
3. Test each adapter independently: enable, update, disable, host restart,
   target restart, forced-host watchdog recovery, sign-out, and sign-in.
4. Verify exact restoration and that no shell process hangs or crash-loops.
5. Run Debug and Release test suites.
6. Add a new exact compiled profile or same-publisher signed external pack;
   never edit an old profile to broaden its scope.

A multi-build certification matrix is still required before declaring broad
production support.
