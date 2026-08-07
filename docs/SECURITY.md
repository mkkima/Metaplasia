# Security model

Metaplasia intentionally writes executable code into Windows shell processes.
That capability carries the same effective trust as the current interactive
user and must be treated as privileged application behavior even though it
does not request elevation.

## Trust boundaries

The UI, CLI, host, injector library, and agent DLL are one trusted product.
The named pipe prevents access from other users and remote machines, but it is
not a boundary against malicious code already running as the same user. Such
code can control the user's Explorer directly and is outside this threat model.

Windows shell processes and their module layouts are untrusted inputs. Process
names, PIDs, sessions, ownership, architectures, protection levels, frame
lengths, enum values, and agent configuration fields are validated at the point
of use. A PID is never accepted as proof of process identity.

## Implemented controls

- Per-session pipe name and single host instance.
- Protected pipe DACL granting access only to SYSTEM and the current user.
- Remote clients rejected and connected client SID verified from its PID.
- Bounded framed messages with strict decoding and I/O timeouts.
- Production startup verifies embedded Authenticode on the host, watchdog, and
  agent, requires one exact leaf-certificate SHA-256 thumbprint, and rejects
  unsigned, invalid, and mixed component sets. Debug has an all-unsigned-only
  development policy that is absent from Release.
- The injector rechecks the pinned agent signer immediately before every new
  load; configure-only recovery remains available for an exact already mapped
  agent even if the installed file is damaged.
- Optional compatibility packs are resource-only images mapped without code
  execution, signed by the same exact component signer, strictly bounded and
  parsed, and forbidden from overriding compiled profiles.
- Absolute canonical agent path and regular-file/extension validation.
- Target image, user, session, architecture, and PPL validation before DLL load.
- Exact loaded-module conflict checks reject known Windhawk and ExplorerPatcher
  identities before enabling a feature; empty configurations remain allowed so
  Metaplasia can always restore its own changes.
- Fixed-size pointer-free agent ABI with magic, version, size, reserved-field,
  target, feature-mask, and string-termination checks.
- Agent entry points resolved from the target's mapped PE export directory,
  with bounded tables, exact names, validated ordinals, forwarded-export
  rejection, and executable-section enforcement. On-disk rebuilds cannot
  supply an RVA for an older loaded image.
- Bounded PE parsing, exact RSDS PDB identity, executable-section enforcement,
  and SHA-256 fingerprints of resolved code bytes.
- Per-adapter compatibility approval requires exact Windows build+UBR, expected
  protected-system path, and PE/PDB identity read from the mapped image rather
  than a replaceable on-disk file. Unknown and ambiguous identities fail closed.
- Minimal `DllMain`; no loader-lock initialization work.
- Start menu XAML changes require an exact visual-type allowlist, use a bounded
  ownership table, run through the XAML dispatcher, and restore the exact
  original opacity before unsubscribing.
- Compiler/linker hardening for project targets (`/sdl`, Control Flow Guard,
  ASLR, DEP, and CET compatibility where supported).
- Atomic settings replacement with write-through and fail-safe defaults.
- Bounded rotating LocalAppData diagnostics logs; malformed records are ignored,
  reads are byte/entry limited, user-entered setting values are not recorded,
  and logging failures cannot stop the host.
- Normal-shutdown hook removal and in-host crash-loop auto-disable.
- Ready/graceful watchdog handshake with random event identities, validated
  same-user/session host identity, and sibling install/data path enforcement.
- Cross-process recovery lease that blocks a replacement host until persistent
  safe mode and best-effort agent deactivation finish.
- Portable updates trust only a bounded Ed25519-signed manifest embedded-key
  channel, require an exact repository release URL, verify the full ZIP SHA-256
  twice, and accept only the exact five top-level runtime filenames. Extraction
  rejects traversal, directories, duplicates, links, and oversized content.
- The update helper runs outside the portable directory, waits for the UI,
  host, and watchdog to exit, restarts only the exact current-session shell,
  stages on the target volume, and preserves rollback files if restoration is
  incomplete. Installation requires explicit user confirmation.
- Configure-only recovery API that cannot call `LoadLibraryW`; watchdog
  recovery never injects a clean process or terminates/restarts the shell.
- No kernel driver, global hook, service, scheduled task, or elevation path.

Packaged shell processes need read/execute permission for the agent and
traversal permission for its directory. The host adds only the relevant
`ALL APPLICATION PACKAGES` and `ALL RESTRICTED APPLICATION PACKAGES` access
entries while preserving the existing ACL.

## Fail-safe rules

All new settings default to disabled. Unknown protocol values and agent feature
bits are rejected. Unknown settings keys are ignored for forward readability,
but an unknown settings format version is rejected. A malformed settings file
starts the host with every feature disabled.

Hook installation must be transactional: either the complete feature becomes
active or all changes made by that attempt are rolled back. Hook callbacks must
be `noexcept`, avoid allocation and blocking work on hot paths, validate output
buffers, and call the original function unless the caller is positively
identified.

## Known limitations and release blockers

- Taskbar, File Explorer, and Start are certified only for exact compatibility
  profiles; broad Windows-build coverage remains incomplete.
- The compiled compatibility catalog currently approves only one exact Windows
  11 25H2 revision. A clean multi-build VM matrix remains required for broad
  production support.
- The Start menu adapter currently supports only the `Windows.UI.Xaml` tree,
  the `VisualDiagConnection1` endpoint, and three exact root type identities. A
  WinUI 3-only or redesigned Start menu is unsupported and must fail closed.
- Start menu compatibility has not yet been certified across a clean Windows
  11 VM build matrix. Live testing on a shell already modified by another
  injector is explicitly unsupported.
- Watchdog deactivation is bounded and best effort. If a live shell process
  cannot accept an empty configuration, persistent settings still remain in
  safe mode; the next host retries deactivation, and restarting that shell
  process or signing out clears the mapped DLL.
- Development binaries are unsigned, so the current local Release output
  intentionally refuses to start. The tag-only release workflow and updater
  exist, but the repository signing secrets, protected tag rule, release
  environment approval, and a clean end-to-end published update test must be
  completed before calling the distribution production-ready. A portable,
  user-writable directory is not an integrity boundary against the same user;
  the signed channel protects transport and release provenance.
- Multiple products hooking the same shell function have undefined ownership
  and teardown order. Metaplasia must not be enabled alongside another shell
  injector until coexistence is explicitly tested.

## Development and testing policy

Native-hook development belongs in a disposable Windows VM with snapshots and
a separate recovery path. Test at minimum: normal enable/disable, host restart,
Explorer restart, Start host restart, sign-out/sign-in, malformed settings,
invalid protocol frames, injection timeout, access denial, unsupported build,
and three-crash fail-safe behavior.

Do not test a new private-symbol hook merely because a symbol name resolves.
Validate module identity, PDB signature, executable section, expected prologue
or another semantic invariant, and the complete teardown path first.
