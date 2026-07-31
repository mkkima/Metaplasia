# Recovery behavior

Metaplasia has two independent recovery layers. The host detects repeated shell
process exits and disables the affected target after three exits in 60 seconds.
The watchdog covers the different failure case where the host itself exits
without completing normal teardown.

## Startup contract

One watchdog is paired with one host process. Startup is fail closed:

1. The host waits for the session recovery mutex left by any previous watchdog.
2. It creates manual-reset `ready` and `graceful` events whose names include the
   session, host PID, and a cryptographically random 128-bit token.
3. It launches only the sibling `metaplasia-watchdog.exe` by absolute path.
4. The watchdog validates its arguments, sibling host and agent paths, the
   canonical `%LOCALAPPDATA%\Metaplasia\settings.conf` path, host image, user,
   and session.
5. The watchdog opens a synchronization handle to the host, owns the session
   recovery mutex, and signals `ready`.
6. Only after readiness does the host load settings and start reconciliation.

Readiness has a five-second bound. A failed or exited watchdog prevents the host
from activating any customization. While running, the host also supervises the
watchdog and begins a normal safe shutdown if the watchdog disappears.

## Normal shutdown

The host stops reconciliation and sends empty configurations to the agents it
knows are loaded. These calls use the injector's configure-only operation, so a
shutdown race cannot load a new agent. The host then signals `graceful`; the
watchdog exits without changing settings and releases the recovery mutex.

## Unexpected host exit

If the host process signals before `graceful`, the watchdog performs these
bounded steps while retaining the recovery mutex:

1. Atomically replace `settings.conf` with a valid all-disabled snapshot.
2. Enumerate only `explorer.exe` and `StartMenuExperienceHost.exe` processes in
   the watchdog's interactive session.
3. Check for the exact canonical installed agent path in each process.
4. Send an empty, target-compatible ABI configuration through
   `ConfigureLoaded` with a three-second operation timeout.
5. Exit and release the recovery mutex. A replacement host can now start and
   will read safe-mode settings.

The settings write happens first. Thus, even if one shell is exiting, hung, or
temporarily inaccessible, persistent state does not re-enable the feature. The
next host sees an already mapped agent while desired features are empty and can
retry deactivation.

## Deliberate non-actions

The watchdog never:

- calls `LoadLibraryW` or injects into a process without the exact agent already
  mapped;
- unloads the agent DLL, because executing code may still own callbacks or
  remote-thread frames;
- terminates or restarts Explorer or the Start menu;
- crosses Windows user or session boundaries;
- acts as an elevated service or persistent background task.

An inert agent remains mapped until the shell process exits. If bounded
deactivation fails, restarting the affected shell process or signing out is the
final recovery path.
