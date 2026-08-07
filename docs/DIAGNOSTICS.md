# Diagnostics and local logs

The Diagnostics page combines three independent sources so a generic `Error`
label can be traced without attaching a debugger:

- immutable target snapshots from the per-session host;
- bounded Taskbar or Start menu XAML observations copied from the already
  loaded agent;
- the host's persistent structured event log.

## Log location and retention

The host writes newline-delimited JSON to:

```text
%LOCALAPPDATA%\Metaplasia\logs\host.log
```

The current file is limited to 1 MiB and has three rotating backups named
`host.log.1` through `host.log.3`. Logging is fail-open: directory, rotation, or
write failures never block shell recovery or configuration rollback. The UI
reads at most the bounded four-file set, rejects malformed records, and returns
only the newest 500 valid entries.

Every record has schema version, UTC timestamp, severity, component, event,
target, process ID, and message. User-entered customization values are omitted;
configuration events record only that a setting changed. Logs remain local and
are never included in update requests or sent over the network.

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

The structured log records host lifecycle, watchdog failures, settings changes,
configuration success or failure, and crash-loop safe mode. Identical rapid
configuration failures are rate-limited in the log and repeated at most once
every 30 seconds while the host continues its normal recovery attempts.
