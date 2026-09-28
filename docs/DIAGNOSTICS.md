# Diagnostics and local logs

The Diagnostics page combines three independent sources so a generic `Error`
label can be traced without attaching a debugger:

- immutable target snapshots from the per-session host;
- bounded Taskbar or Start menu XAML observations copied from the already
  loaded agent;
- the host and watchdog persistent structured event logs.

## Log location and retention

The host and watchdog write newline-delimited JSON to separate files:

```text
%LOCALAPPDATA%\Metaplasia\logs\host.log
%LOCALAPPDATA%\Metaplasia\logs\watchdog.log
```

Each current file is limited to 1 MiB and has three rotating backups. Separate
files avoid cross-process append and rotation races. Logging is fail-open:
directory, rotation, or write failures never block shell recovery or
configuration rollback. The UI reads only the bounded eight-file set, merges
records by their fixed UTC timestamp, rejects malformed records, and returns
only the newest 500 valid entries.

Every record has schema version, UTC timestamp, severity, component, event,
target, process ID, and message. User-entered customization values are omitted;
configuration events identify the exact setting and configuration generation
without recording its value. Compatibility transitions record the result,
adapter, exact Windows revision, selected profile, decision detail, and the
path plus compatibility key of every inspected mapped module. Identical
compatibility decisions are written only when the PID or decision changes, not
on every monitor pass. Logs remain local and are never included in update
requests or sent over the network.

## Native failure interpretation

Agent failures may contain these fields:

- `code`: stable `AgentResult`, such as `7` for `adapter_unavailable`;
- `native`: the original HRESULT returned by the XAML or hook adapter;
- `stage`: the last native initialization stage;
- `state`: a bounded controller bit field.

For the shared shell XAML controller, state bit `0x1` means that the visual-tree
service exists, `0x2` means that a watcher exists, and `0x4` means that the
watcher is subscribed. The upper bits contain the bounded tracked-element
count. For example, `state=0x3` at `stage=advise-visual-tree` means the service
and watcher were created but callback subscription was not established.

Rollback uses the same adapter API as normal configuration. The agent preserves
the original failure HRESULT and stage after rollback so diagnostics report the
cause of the rejected transition instead of the rollback's successful `S_OK`.

## XAML observations

The Taskbar and Start menu selectors issue separate read-only requests. They do
not inject an agent or change configuration. A snapshot is available only when
the exact Metaplasia agent is already loaded in the corresponding process.
Type, element, and dropped-observation counts are bounded by the native ABI.

Start menu snapshots also expose the actual asynchronous style state rather
than treating successful DLL configuration as successful styling. `waiting`
means the required visual-tree dependencies have not all appeared, `applying`
identifies an in-progress scene commit, `active` is published only after the
complete style or three-panel scene has committed, and `failed` includes the
exact apply stage and HRESULT. Dependency, attempt, success, and failure
counters are included in both the UI summary and `metaplasia-cli xaml-types`.
State transitions and new failures are persisted as `xaml-style` host events;
unchanged health polls are deduplicated.

Frame-envelope failures add `envelope-operation`, `envelope-index`, and
`envelope-native` to the structured host event. These identify whether the
failure occurred while resolving the frame, reading or validating a specific
parent, resolving the outer boundary, capturing reversible state, or writing
layout. This avoids reducing a visual-contract mismatch to an unactionable
generic HRESULT.

The Start scene retry policy is also reflected in these counters. Deterministic
contract and unsupported-property failures stop after one attempt until an
explicit reconfiguration or required dependency change. Other failures stop
after three consecutive attempts for the same scene. This keeps `attempts` and
`failures` actionable instead of allowing unrelated XAML traffic to create a
retry storm.

The structured logs record host lifecycle, watchdog startup milestones and
recovery failures, settings changes, configuration success or failure, and
crash-loop safe mode. Identical rapid
configuration failures are rate-limited in the log and repeated at most once
every 30 seconds while the host continues its normal recovery attempts.
