# Startup and tray lifecycle

Metaplasia can register its current portable executable for the current Windows
user under:

`HKCU\Software\Microsoft\Windows\CurrentVersion\Run\Metaplasia`

The registered command quotes the absolute executable path and adds the exact
`--background` argument. No administrator rights, scheduled task, service,
installer, copied executable, or machine-wide registry entry is used. Disabling
the setting removes only the `Metaplasia` value. If the portable directory is
moved, the UI keeps the switch enabled, reports that another portable path is
registered, and instructs the user to turn the switch off and on to replace it
with the current path.

## Background startup

Background mode creates the Tauri event loop and system tray icon, then starts
the native host. It deliberately does not create the main window or WebView2.
The host loads the existing atomic settings file from
`%LOCALAPPDATA%\Metaplasia\settings.conf`, so the same enabled targets and
customization values are reconciled after sign-in.

Opening Metaplasia from the tray or launching `metaplasia.exe` normally creates
the UI on demand. Closing the window destroys the WebView rather than keeping a
hidden browser instance alive; the tray and native host remain available.

## Single-instance behavior

One control-center process owns a per-session local mutex and an auto-reset
activation event. A second normal launch signals the existing process to open
or focus its window and exits. A duplicate `--background` launch exits without
opening the UI. Window creation is serialized, so tray activation and an EXE
launch cannot create competing windows.

The native host has its own existing per-session mutex. This independent guard
prevents a control-center race from ever creating two orchestration engines or
two injector owners.

The tray menu contains **Open Metaplasia** and **Exit Metaplasia**. Exiting the
control center removes the tray icon but intentionally does not disable saved
customizations or terminate the independently supervised native host. Launching
the EXE again reconnects to that host.
