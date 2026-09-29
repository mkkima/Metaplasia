[CmdletBinding()]
param(
    [string]$InstallRoot = 'C:\MetaplasiaLab\install',
    [Parameter(Mandatory)][string]$ResultRoot,
    [Parameter(Mandatory)][string]$ExpectedUserSid,
    [Parameter(Mandatory)][string]$ExpectedCompatibilityPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ((Get-Process -Id $PID).SessionId -eq 0) {
    throw 'The Metaplasia workload must run in the interactive desktop session.'
}
$computer = Get-CimInstance Win32_ComputerSystem
if ($computer.Manufacturer -ne 'Microsoft Corporation' -or $computer.Model -ne 'Virtual Machine') {
    throw 'Interactive workload input is allowed only inside the Hyper-V guest.'
}
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if ($identity.User.Value -ine $ExpectedUserSid -or
    $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'The Metaplasia workload must use the expected limited user token.'
}
if (-not [IO.Path]::GetFullPath($InstallRoot).Equals(
        'C:\MetaplasiaLab\install', [StringComparison]::OrdinalIgnoreCase) -or
    -not [IO.Path]::GetFullPath($ResultRoot).StartsWith(
        'C:\MetaplasiaLab\results\',
        [StringComparison]::OrdinalIgnoreCase) -or
    -not [IO.Path]::GetFullPath($ExpectedCompatibilityPath).StartsWith(
        'C:\MetaplasiaLab\results\',
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The workload paths are outside the Metaplasia guest boundary.'
}
New-Item -ItemType Directory -Path $ResultRoot -Force | Out-Null
$resultItem = Get-Item -LiteralPath $ResultRoot -Force
if (-not $resultItem.PSIsContainer -or
    ($resultItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'The workload result root is not a regular directory.'
}

$statusPath = Join-Path $ResultRoot 'workload-status.json'
$startedUtc = [DateTime]::UtcNow
[ordered]@{
    processId = $PID
    sessionId = (Get-Process -Id $PID).SessionId
    userSid = $identity.User.Value
    elevated = $false
    apartment = [Threading.Thread]::CurrentThread.GetApartmentState().ToString()
    startedUtc = $startedUtc.ToString('o')
} | ConvertTo-Json -Compress | Set-Content -LiteralPath (
    Join-Path $ResultRoot 'workload-started.json') -Encoding utf8
$phase = 'startup'
$failure = $null
$diagnosticSamples = [Collections.Generic.List[object]]::new()
$geometrySamples = [Collections.Generic.List[object]]::new()
$desktopSamples = [Collections.Generic.List[object]]::new()
$lastStartXamlText = $null
$runtimeStarted = $false
$allAppsLabelText = -join (@(
    0x0412, 0x0441, 0x0435, 0x0020, 0x043F, 0x0440, 0x0438,
    0x043B, 0x043E, 0x0436, 0x0435, 0x043D, 0x0438, 0x044F
) | ForEach-Object { [char]$_ })
$recommendedLabelText = -join (@(
    0x0420, 0x0435, 0x043A, 0x043E, 0x043C, 0x0435, 0x043D,
    0x0434, 0x0443, 0x0435, 0x043C, 0x044B, 0x0435
) | ForEach-Object { [char]$_ })
$allAppsAutomationId = 'MetaplasiaThreePanelAllApps'
$recommendedAutomationId = 'MetaplasiaThreePanelRecommended'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public sealed class MetaplasiaWindowInfo {
    public long Handle;
    public uint ProcessId;
    public bool Visible;
    public string ClassName;
    public string Title;
    public int Left;
    public int Top;
    public int Width;
    public int Height;
    public int Cloaked;
}

public sealed class MetaplasiaDesktopInfo {
    public string WindowStation;
    public int SessionState;
    public int SessionProtocol;
    public string InputDesktop;
    public string ThreadDesktop;
    public long ForegroundWindow;
    public uint ForegroundProcessId;
    public string ForegroundClass;
    public string ForegroundTitle;
    public long ShellWindow;
    public uint ShellProcessId;
    public int ScreenWidth;
    public int ScreenHeight;
}

public static class MetaplasiaDesktopProbe {
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)]
    private struct POINT { public int X, Y; }

    [DllImport("user32.dll", SetLastError = true)]
    private static extern bool EnumDesktopWindows(IntPtr desktop, EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback, IntPtr parameter);
    [DllImport("user32.dll")]
    private static extern IntPtr GetProcessWindowStation();
    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode)]
    private static extern bool WTSQuerySessionInformation(IntPtr server, int sessionId, int info, out IntPtr buffer, out int bytes);
    [DllImport("wtsapi32.dll")]
    private static extern void WTSFreeMemory(IntPtr buffer);
    [DllImport("user32.dll")]
    private static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll")]
    private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);
    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hwnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetClassName(IntPtr hwnd, StringBuilder value, int capacity);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int GetWindowText(IntPtr hwnd, StringBuilder value, int capacity);
    [DllImport("dwmapi.dll")]
    private static extern int DwmGetWindowAttribute(IntPtr hwnd, int attribute, out int value, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindow(string className, string title);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, string className, string title);
    [DllImport("user32.dll")]
    private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")]
    private static extern IntPtr WindowFromPoint(POINT point);
    [DllImport("user32.dll")]
    private static extern int GetSystemMetrics(int index);
    [DllImport("user32.dll", SetLastError = true)]
    private static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll")]
    private static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("kernel32.dll")]
    private static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")]
    private static extern IntPtr GetThreadDesktop(uint threadId);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern bool GetUserObjectInformation(IntPtr obj, int index, StringBuilder value, int length, out int needed);
    [StructLayout(LayoutKind.Sequential)]
    private struct KEYBDINPUT {
        public ushort VirtualKey, ScanCode;
        public uint Flags, Time;
        public UIntPtr ExtraInfo;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct MOUSEINPUT {
        public int X, Y;
        public uint Data, Flags, Time;
        public UIntPtr ExtraInfo;
    }
    [StructLayout(LayoutKind.Explicit)]
    private struct INPUTUNION {
        [FieldOffset(0)] public KEYBDINPUT Keyboard;
        [FieldOffset(0)] public MOUSEINPUT Mouse;
    }
    [StructLayout(LayoutKind.Sequential)]
    private struct INPUT { public uint Type; public INPUTUNION Data; }
    [DllImport("user32.dll", SetLastError = true)]
    private static extern uint SendInput(uint count, INPUT[] input, int size);

    private static string DesktopName(IntPtr desktop) {
        if (desktop == IntPtr.Zero) return "unavailable:" + Marshal.GetLastWin32Error();
        var name = new StringBuilder(256);
        int needed;
        return GetUserObjectInformation(desktop, 2, name, name.Capacity * 2, out needed)
            ? name.ToString() : "unavailable:" + Marshal.GetLastWin32Error();
    }

    public static MetaplasiaDesktopInfo DesktopState() {
        IntPtr input = OpenInputDesktop(0, false, 1);
        string inputName;
        try { inputName = DesktopName(input); }
        finally { if (input != IntPtr.Zero) CloseDesktop(input); }
        IntPtr shell = FindWindow("Shell_TrayWnd", null);
        uint shellPid;
        GetWindowThreadProcessId(shell, out shellPid);
        IntPtr foreground = GetForegroundWindow();
        uint foregroundPid;
        GetWindowThreadProcessId(foreground, out foregroundPid);
        var foregroundClass = new StringBuilder(256);
        var foregroundTitle = new StringBuilder(512);
        GetClassName(foreground, foregroundClass, foregroundClass.Capacity);
        GetWindowText(foreground, foregroundTitle, foregroundTitle.Capacity);
        return new MetaplasiaDesktopInfo {
            WindowStation = DesktopName(GetProcessWindowStation()),
            SessionState = SessionValue(8), SessionProtocol = SessionValue(16),
            InputDesktop = inputName,
            ThreadDesktop = DesktopName(GetThreadDesktop(GetCurrentThreadId())),
            ForegroundWindow = foreground.ToInt64(), ForegroundProcessId = foregroundPid,
            ForegroundClass = foregroundClass.ToString(), ForegroundTitle = foregroundTitle.ToString(),
            ShellWindow = shell.ToInt64(), ShellProcessId = shellPid,
            ScreenWidth = GetSystemMetrics(0), ScreenHeight = GetSystemMetrics(1)
        };
    }

    private static int SessionValue(int information) {
        IntPtr buffer;
        int bytes;
        if (!WTSQuerySessionInformation(IntPtr.Zero,
            System.Diagnostics.Process.GetCurrentProcess().SessionId, information, out buffer, out bytes)) return -1;
        try {
            if (information == 16) return bytes >= 2 ? Marshal.ReadInt16(buffer) : -1;
            return bytes >= 4 ? Marshal.ReadInt32(buffer) : -1;
        } finally { WTSFreeMemory(buffer); }
    }

    public static long[] ChildWindows(long parent, uint expectedPid) {
        var result = new List<long>();
        EnumWindowsProc callback = delegate(IntPtr window, IntPtr ignored) {
            uint pid;
            GetWindowThreadProcessId(window, out pid);
            if (pid == expectedPid && IsWindowVisible(window)) result.Add(window.ToInt64());
            return result.Count < 32;
        };
        EnumChildWindows(new IntPtr(parent), callback, IntPtr.Zero);
        return result.ToArray();
    }

    public static void ClickStart(uint expectedExplorerPid, int x, int y) {
        IntPtr shell = FindWindow("Shell_TrayWnd", null);
        uint shellPid;
        GetWindowThreadProcessId(shell, out shellPid);
        if (shell == IntPtr.Zero || shellPid != expectedExplorerPid)
            throw new InvalidOperationException("The interactive Explorer taskbar is not ready.");
        RECT bounds;
        int width = GetSystemMetrics(0), height = GetSystemMetrics(1);
        uint hitPid;
        GetWindowThreadProcessId(WindowFromPoint(new POINT { X = x, Y = y }), out hitPid);
        if (!GetWindowRect(shell, out bounds) || width <= 1 || height <= 1 ||
            x < 0 || x >= width || y < 0 || y >= height ||
            x < bounds.Left || x >= bounds.Right || y < bounds.Top || y >= bounds.Bottom ||
            hitPid != expectedExplorerPid)
            throw new InvalidOperationException("The UI Automation Start point is outside the unobstructed guest taskbar.");
        // Use the point published by the real Start button, not a guessed
        // screen coordinate. The preceding guest/session guards are mandatory.
        var input = new INPUT[3];
        input[0].Data.Mouse.X = (int)Math.Round(x * 65535.0 / (width - 1));
        input[0].Data.Mouse.Y = (int)Math.Round(y * 65535.0 / (height - 1));
        input[0].Data.Mouse.Flags = 0x8001; // absolute move
        input[1].Data.Mouse.Flags = 2; // left down
        input[2].Data.Mouse.Flags = 4; // left up
        if (SendInput(3, input, Marshal.SizeOf(typeof(INPUT))) != 3)
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Guest Start click was not delivered.");
    }

    public static uint ProcessAtPoint(int x, int y) {
        uint pid;
        GetWindowThreadProcessId(WindowFromPoint(new POINT { X = x, Y = y }), out pid);
        return pid;
    }

    public static void CloseStart(uint expectedStartPid) {
        uint foregroundPid;
        GetWindowThreadProcessId(GetForegroundWindow(), out foregroundPid);
        if (foregroundPid != expectedStartPid)
            throw new InvalidOperationException("Refusing Escape input outside the Start foreground window.");
        var input = new INPUT[2];
        input[0].Type = input[1].Type = 1;
        input[0].Data.Keyboard.VirtualKey = input[1].Data.Keyboard.VirtualKey = 0x1B;
        input[1].Data.Keyboard.Flags = 2;
        if (SendInput(2, input, Marshal.SizeOf(typeof(INPUT))) != 2)
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Escape input was not delivered.");
    }

    public static MetaplasiaWindowInfo[] WindowsForProcess(uint expectedPid) {
        return EnumerateWindows(expectedPid, false);
    }

    public static MetaplasiaWindowInfo[] EnumerateWindows(uint expectedPid, bool includeHidden) {
        var result = new List<MetaplasiaWindowInfo>();
        var seen = new HashSet<long>();
        EnumWindowsProc callback = delegate(IntPtr hwnd, IntPtr ignored) {
            if (hwnd == IntPtr.Zero || !seen.Add(hwnd.ToInt64())) return true;
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            bool visible = IsWindowVisible(hwnd);
            if ((expectedPid != 0 && pid != expectedPid) || (!includeHidden && !visible)) return true;
            if (result.Count >= 256) return false;
            RECT rect;
            if (!GetWindowRect(hwnd, out rect)) return true;
            var className = new StringBuilder(256);
            var title = new StringBuilder(512);
            GetClassName(hwnd, className, className.Capacity);
            GetWindowText(hwnd, title, title.Capacity);
            int cloaked;
            if (DwmGetWindowAttribute(hwnd, 14, out cloaked, sizeof(int)) != 0) cloaked = -1;
            result.Add(new MetaplasiaWindowInfo {
                Handle = hwnd.ToInt64(),
                ProcessId = pid,
                Visible = visible,
                ClassName = className.ToString(),
                Title = title.ToString(),
                Left = rect.Left,
                Top = rect.Top,
                Width = rect.Right - rect.Left,
                Height = rect.Bottom - rect.Top,
                Cloaked = cloaked
            });
            return true;
        };
        // EnumWindows excludes non-desktop apps on Windows 8+. Start can be
        // absent from that list even when its CoreWindow owns the foreground.
        if (!EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), callback, IntPtr.Zero) && result.Count < 256)
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error(), "Guest desktop enumeration failed.");
        // Query CoreWindows explicitly as well: generic enumeration may omit
        // packaged shell windows. Never depend on localized window titles.
        IntPtr core = IntPtr.Zero;
        var coreSeen = new HashSet<long>();
        for (int index = 0; index < 64; ++index) {
            core = FindWindowEx(IntPtr.Zero, core, "Windows.UI.Core.CoreWindow", null);
            if (core == IntPtr.Zero || !coreSeen.Add(core.ToInt64())) break;
            if (!callback(core, IntPtr.Zero)) break;
        }
        callback(GetForegroundWindow(), IntPtr.Zero);
        return result.ToArray();
    }
}
'@
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

function Invoke-MetaplasiaCli {
    param(
        [Parameter(Mandatory)][string[]]$Arguments,
        [switch]$AllowFailure
    )

    $cli = Join-Path $InstallRoot 'metaplasia-cli.exe'
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = @(& $cli @Arguments 2>&1)
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    $text = ($lines | Out-String).Trim()
    if (-not $AllowFailure -and $exitCode -ne 0) {
        throw "metaplasia-cli $($Arguments -join ' ') failed with $exitCode`: $text"
    }
    return [ordered]@{ exitCode = $exitCode; text = $text }
}

function Wait-MetaplasiaHost {
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    do {
        $probe = Invoke-MetaplasiaCli -Arguments @('settings') -AllowFailure
        if ($probe.exitCode -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'The Metaplasia host did not expose its session pipe.'
}

function Wait-MetaplasiaStartState {
    param(
        [Parameter(Mandatory)][ValidateSet('active', 'failed')]
        [string]$Expected,
        [int]$Seconds = 45
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds)
    do {
        $probe = Invoke-MetaplasiaCli `
            -Arguments @('xaml-types', 'start-menu') -AllowFailure
        $script:lastStartXamlText = $probe.text
        $firstLine = @($probe.text -split "`r?`n")[0]
        $diagnosticSamples.Add([ordered]@{
            utc = [DateTime]::UtcNow.ToString('o')
            exitCode = $probe.exitCode
            summary = $firstLine
        })
        if ($probe.exitCode -eq 0) {
            if ($firstLine -match "style-state=$Expected(?:,|$)") {
                return $probe.text
            }
            if ($Expected -eq 'active' -and
                $firstLine -match 'style-state=failed(?:,|$)') {
                throw "Start styling failed: $firstLine"
            }
        }
        Start-Sleep -Milliseconds 750
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Start did not reach style-state=$Expected within $Seconds seconds."
}

function Get-MetaplasiaStartGeometry {
    $process = @(Get-Process StartMenuExperienceHost -ErrorAction Stop |
        Where-Object SessionId -eq (Get-Process -Id $PID).SessionId)
    if ($process.Count -ne 1) {
        throw 'Expected exactly one interactive StartMenuExperienceHost process.'
    }
    $allAppsCondition = New-Object `
        -TypeName System.Windows.Automation.PropertyCondition `
        -ArgumentList @(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty,
            $allAppsAutomationId)
    $recommendedCondition = New-Object `
        -TypeName System.Windows.Automation.PropertyCondition `
        -ArgumentList @(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty,
            $recommendedAutomationId)
    $condition = [System.Windows.Automation.OrCondition]::new(
        [System.Windows.Automation.Condition[]]@(
            $allAppsCondition,
            $recommendedCondition))
    $windows = @([MetaplasiaDesktopProbe]::WindowsForProcess([uint32]$process[0].Id))
    $elements = [Collections.Generic.List[System.Windows.Automation.AutomationElement]]::new()
    $automationRoots = [Collections.Generic.List[object]]::new()
    # Search only the real Start HWNDs. Searching the entire desktop is both
    # unbounded and prone to missing an island's UIA provider during activation.
    foreach ($window in @($windows | Where-Object { $_.Cloaked -eq 0 } | Select-Object -First 16)) {
        $rootEvidence = [ordered]@{ handle = $window.Handle; className = $window.ClassName; matches = 0; error = $null }
        try {
            $root = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$window.Handle)
            $matches = $root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $condition)
            $rootEvidence.matches = $matches.Count
            for ($index = 0; $index -lt [Math]::Min($matches.Count, 1024); ++$index) {
                $elements.Add($matches[$index])
            }
        } catch { $rootEvidence.error = $_.Exception.Message }
        $automationRoots.Add($rootEvidence)
    }
    $observed = [Collections.Generic.List[object]]::new()
    $allApps = $null
    $recommended = $null
    $limit = [Math]::Min($elements.Count, 1024)
    for ($index = 0; $index -lt $limit; ++$index) {
        try {
            $current = $elements[$index].Current
            $rectangle = $current.BoundingRectangle
            if ($rectangle.IsEmpty -or $current.IsOffscreen) {
                continue
            }
            $entry = [ordered]@{
                name = $current.Name
                automationId = $current.AutomationId
                processId = $current.ProcessId
                controlType = $current.ControlType.ProgrammaticName
                left = [Math]::Round($rectangle.Left, 2)
                top = [Math]::Round($rectangle.Top, 2)
                width = [Math]::Round($rectangle.Width, 2)
                height = [Math]::Round($rectangle.Height, 2)
                hitProcessId = [MetaplasiaDesktopProbe]::ProcessAtPoint(
                    [int]($rectangle.Left + $rectangle.Width / 2),
                    [int]($rectangle.Top + $rectangle.Height / 2))
            }
            $observed.Add($entry)
            if ($current.AutomationId -ceq $allAppsAutomationId -and
                $current.Name -ceq $allAppsLabelText) {
                $allApps = $entry
            }
            if ($current.AutomationId -ceq $recommendedAutomationId -and
                $current.Name -ceq $recommendedLabelText) {
                $recommended = $entry
            }
        } catch {
            # The Start tree can change while UI Automation is walking it.
        }
    }
    return [ordered]@{
        processId = $process[0].Id
        windows = $windows
        automationRoots = $automationRoots
        namedElements = $observed
        allAppsLabel = $allApps
        recommendedLabel = $recommended
    }
}

function Open-MetaplasiaStart {
    Set-MetaplasiaStartVisibility -Visible $true
}

function Close-MetaplasiaStart {
    Set-MetaplasiaStartVisibility -Visible $false
}

function Test-MetaplasiaStartVisible {
    foreach ($startProcess in @(Get-Process StartMenuExperienceHost -ErrorAction SilentlyContinue |
        Where-Object SessionId -eq (Get-Process -Id $PID).SessionId)) {
        # Search can own foreground while Start remains displayed. Conversely,
        # a preloaded CoreWindow alone does not prove an open menu. Require an
        # actual visible Start element and an unobstructed native hit on it.
        foreach ($window in @([MetaplasiaDesktopProbe]::WindowsForProcess([uint32]$startProcess.Id) |
            Where-Object { $_.Cloaked -eq 0 -and $_.Width -gt 200 -and $_.Height -gt 200 } | Select-Object -First 4)) {
            $root = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$window.Handle)
            foreach ($id in @($allAppsAutomationId, 'PinnedListHeaderGrid')) {
                $condition = [System.Windows.Automation.PropertyCondition]::new(
                    [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $id)
                $element = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
                if ($null -eq $element) { continue }
                $current = $element.Current
                $rect = $current.BoundingRectangle
                if (-not $current.IsOffscreen -and -not $rect.IsEmpty -and
                    $current.ProcessId -eq $startProcess.Id -and $rect.Width -gt 0 -and $rect.Height -gt 0 -and
                    [MetaplasiaDesktopProbe]::ProcessAtPoint(
                        [int]($rect.Left + $rect.Width / 2), [int]($rect.Top + $rect.Height / 2)) -eq [uint32]$startProcess.Id) {
                    return $true
                }
            }
        }
    }
    return $false
}

function Set-MetaplasiaStartVisibility {
    param(
        [Parameter(Mandatory)][bool]$Visible,
        [switch]$RetryingActivation
    )

    if ((Test-MetaplasiaStartVisible) -eq $Visible) { return }
    $desktop = [MetaplasiaDesktopProbe]::DesktopState()
    $desktopSamples.Add([ordered]@{ utc = [DateTime]::UtcNow.ToString('o'); phase = $phase; visibleRequested = $Visible; desktop = $desktop })
    if ($desktop.WindowStation -ine 'WinSta0' -or $desktop.SessionState -ne 0 -or $desktop.SessionProtocol -ne 2) {
        throw "The workload is not on the active RDP desktop: station=$($desktop.WindowStation), state=$($desktop.SessionState), protocol=$($desktop.SessionProtocol)."
    }
    if ($desktop.InputDesktop -ine 'Default' -or $desktop.ThreadDesktop -ine 'Default') {
        throw "The guest input desktop is not available: input=$($desktop.InputDesktop), thread=$($desktop.ThreadDesktop)."
    }
        $explorerProcesses = @(Get-Process explorer -ErrorAction SilentlyContinue |
            Where-Object SessionId -eq (Get-Process -Id $PID).SessionId)
        if ($explorerProcesses.Count -ne 1) { throw 'Expected one interactive Explorer before opening Start.' }
        # WinUI content islands can expose a separate HWND provider rather than
        # descendants of Shell_TrayWnd. Inspect only that tray's own children.
        $roots = @($desktop.ShellWindow) + @([MetaplasiaDesktopProbe]::ChildWindows(
            $desktop.ShellWindow, [uint32]$explorerProcesses[0].Id))
        $startButtons = @()
        $buttonEvidence = @()
        $seenButtons = [Collections.Generic.HashSet[string]]::new()
        foreach ($rootHandle in $roots) {
          $tray = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$rootHandle)
          $buttons = $tray.FindAll([System.Windows.Automation.TreeScope]::Descendants,
              [System.Windows.Automation.Condition]::TrueCondition)
          for ($index = 0; $index -lt [Math]::Min($buttons.Count, 128); ++$index) {
            if (-not $seenButtons.Add(($buttons[$index].GetRuntimeId() -join ','))) { continue }
            $current = $buttons[$index].Current
            $buttonEvidence += [ordered]@{ root = $rootHandle; name = $current.Name; id = $current.AutomationId; class = $current.ClassName; processId = $current.ProcessId; type = $current.ControlType.ProgrammaticName }
            if (($current.AutomationId -eq 'StartButton' -or $current.ClassName -eq 'StartButton') -and
                $current.ProcessId -eq $explorerProcesses[0].Id -and
                $current.IsEnabled -and -not $current.IsOffscreen) {
                $startButtons += $buttons[$index]
            }
          }
        }
        $desktopSamples.Add([ordered]@{ phase = $phase; taskbarButtons = $buttonEvidence; startButtonCount = $startButtons.Count })
        if ($startButtons.Count -ne 1) {
            throw 'Expected exactly one enabled, visible UI Automation Start button.'
        }
        # WinUI can omit GetClickablePoint even for a visible toggle. Its
        # actual bounds remain usable only after the native hit/owner check.
        $buttonBounds = $startButtons[0].Current.BoundingRectangle
        if ($buttonBounds.IsEmpty -or $buttonBounds.Width -le 0 -or $buttonBounds.Height -le 0) {
            throw 'The guest Start button has no rendered bounds.'
        }
        $clickPoint = [System.Windows.Point]::new(
            $buttonBounds.Left + $buttonBounds.Width / 2,
            $buttonBounds.Top + $buttonBounds.Height / 2)
        $desktopSamples.Add([ordered]@{ phase = $phase; input = 'guest-start-button-click'; x = $clickPoint.X; y = $clickPoint.Y })
        [MetaplasiaDesktopProbe]::ClickStart([uint32]$explorerProcesses[0].Id,
            [int][Math]::Round($clickPoint.X), [int][Math]::Round($clickPoint.Y))
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        Start-Sleep -Milliseconds 200
        if ((Test-MetaplasiaStartVisible) -eq $Visible) {
            # Allow the native open/close animation to settle before sampling.
            Start-Sleep -Milliseconds 700
            return
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($Visible -and -not $RetryingActivation) {
        $startWindows = @(Get-Process StartMenuExperienceHost -ErrorAction SilentlyContinue |
            Where-Object SessionId -eq (Get-Process -Id $PID).SessionId |
            ForEach-Object { [MetaplasiaDesktopProbe]::WindowsForProcess([uint32]$_.Id) })
        # First logon can leave Start underneath Search. The first toggle then
        # closes that pair. Retry once, only after DWM confirms every Start
        # surface is cloaked; never blindly toggle an already open menu.
        if ($startWindows.Count -gt 0 -and
            @($startWindows | Where-Object { $_.Cloaked -le 0 }).Count -eq 0) {
            $desktopSamples.Add([ordered]@{ phase = $phase; transition = 'reopen-confirmed-cloaked-start'; windows = $startWindows })
            Set-MetaplasiaStartVisibility -Visible $true -RetryingActivation
            return
        }
    }
    throw "The native Start window did not reach visible=$Visible within fifteen seconds; see guest-desktop-evidence.json."
}

function Assert-MetaplasiaThreePanelGeometry {
    param([Parameter(Mandatory)]$Geometry)

    # Active describes the agent's internal layout, not independently observed
    # screen visibility. Missing UIA evidence is inconclusive, never a pass.
    if ($null -eq $Geometry.allAppsLabel -and
        $null -eq $Geometry.recommendedLabel) {
        throw 'Independent Start geometry is unavailable; refusing to certify a visual fix from Active alone.'
    }
    if ($null -eq $Geometry.allAppsLabel -or
        $null -eq $Geometry.recommendedLabel) {
        throw 'UI Automation exposed only part of the injected three-panel markers.'
    }
    $horizontalSpan = [double]$Geometry.recommendedLabel.left -
        [double]$Geometry.allAppsLabel.left
    if ($horizontalSpan -lt 750.0) {
        throw "Three-panel label span is too small: $horizontalSpan pixels."
    }
    if ([double]$Geometry.allAppsLabel.width -le 0.0 -or
        [double]$Geometry.allAppsLabel.height -le 0.0 -or
        [double]$Geometry.recommendedLabel.width -le 0.0 -or
        [double]$Geometry.recommendedLabel.height -le 0.0) {
        throw 'Injected three-panel labels have no visible screen geometry.'
    }
    if ([int]$Geometry.allAppsLabel.processId -ne [int]$Geometry.processId -or
        [int]$Geometry.recommendedLabel.processId -ne [int]$Geometry.processId) {
        throw 'Injected three-panel labels are not owned by StartMenuExperienceHost.'
    }
    foreach ($label in @($Geometry.allAppsLabel, $Geometry.recommendedLabel)) {
        if ([uint32]$label.hitProcessId -ne [uint32]$Geometry.processId) {
            throw 'A three-panel marker is occluded by another native window.'
        }
        $containingWindows = @($Geometry.windows | Where-Object {
            $_.Cloaked -eq 0 -and $_.Width -gt 0 -and $_.Height -gt 0 -and
            $label.left -ge $_.Left -and $label.top -ge $_.Top -and
            ($label.left + $label.width) -le ($_.Left + $_.Width) -and
            ($label.top + $label.height) -le ($_.Top + $_.Height)
        })
        if ($containingWindows.Count -eq 0) {
            throw 'A three-panel marker is outside every visible Start window.'
        }
    }
}

try {
    $phase = 'wait-for-interactive-shell'
    $sessionId = (Get-Process -Id $PID).SessionId
    $initialExplorer = @(Get-Process explorer -ErrorAction SilentlyContinue |
        Where-Object SessionId -eq $sessionId)
    if ($initialExplorer.Count -eq 0) {
        Start-Process -FilePath (Join-Path $env:SystemRoot 'explorer.exe') |
            Out-Null
    }
    $shellDeadline = [DateTime]::UtcNow.AddSeconds(90)
    do {
        $explorer = @(Get-Process explorer -ErrorAction SilentlyContinue |
            Where-Object SessionId -eq $sessionId)
        $desktop = [MetaplasiaDesktopProbe]::DesktopState()
        if ($explorer.Count -eq 1 -and $desktop.ShellWindow -ne 0 -and
            $desktop.ShellProcessId -eq $explorer[0].Id -and $desktop.InputDesktop -ieq 'Default') { break }
        Start-Sleep -Seconds 1
    } while ([DateTime]::UtcNow -lt $shellDeadline)
    if ($explorer.Count -ne 1 -or $desktop.ShellWindow -eq 0 -or
        $desktop.ShellProcessId -ne $explorer[0].Id -or $desktop.InputDesktop -ine 'Default') {
        throw 'Explorer did not become ready in the workload desktop session.'
    }

    # Materialize the unmodified menu only if its UI payloads are still absent.
    # Loaded payloads always go through the same exact-image gate below.
    $phase = 'prime-native-start-before-fingerprint'
    $expectedFingerprint = Get-Content -LiteralPath $ExpectedCompatibilityPath -Raw | ConvertFrom-Json
    $expectedPayloadNames = @($expectedFingerprint.startMenuPayloads | ForEach-Object { [string]$_.name })
    if ($expectedFingerprint.schema -ne 2 -or $expectedPayloadNames.Count -eq 0) {
        throw 'The expected host fingerprint must include the Start UI payloads.'
    }
    $primedStart = $false
    try {
        $payloadDeadline = [DateTime]::UtcNow.AddSeconds(30)
        do {
            $startProcesses = @(Get-Process StartMenuExperienceHost -ErrorAction SilentlyContinue |
                Where-Object SessionId -eq $sessionId)
            $missingPayloads = $expectedPayloadNames
            if ($startProcesses.Count -eq 1) {
                $loadedNames = @($startProcesses[0].Modules | ForEach-Object ModuleName)
                $missingPayloads = @($expectedPayloadNames | Where-Object { $_ -notin $loadedNames })
            }
            if ($missingPayloads.Count -eq 0) { break }
            if (-not $primedStart) {
                Open-MetaplasiaStart
                $primedStart = $true
            }
            Start-Sleep -Milliseconds 500
        } while ([DateTime]::UtcNow -lt $payloadDeadline)
        if ($missingPayloads.Count -ne 0) {
            throw "The native Start UI did not load the target payloads: $($missingPayloads -join ', ')."
        }
    } finally { if ($primedStart) { Close-MetaplasiaStart } }

    $phase = 'verify-exact-target-fingerprint'
    $guestFingerprintPath = Join-Path $ResultRoot `
        'guest-compatibility-fingerprint.json'
    & (
        Join-Path $PSScriptRoot 'Get-MetaplasiaCompatibilityFingerprint.ps1') `
        -CliPath (Join-Path $InstallRoot 'metaplasia-cli.exe') `
        -OutputPath $guestFingerprintPath | Out-Null
    & (Join-Path $PSScriptRoot `
        'Compare-MetaplasiaCompatibilityFingerprint.ps1') `
        -ExpectedPath $ExpectedCompatibilityPath `
        -ActualPath $guestFingerprintPath -Target start-menu | Out-Null

    $phase = 'launch-background-runtime'
    $application = Join-Path $InstallRoot 'metaplasia.exe'
    Start-Process -FilePath $application -ArgumentList '--background' `
        -WindowStyle Hidden | Out-Null
    Wait-MetaplasiaHost
    $runtimeStarted = $true

    $phase = 'verify-isolated-defaults'
    $initialSnapshot = Invoke-MetaplasiaCli -Arguments @('snapshot')
    if ($initialSnapshot.text -notmatch '(?im)^taskbar: disabled, enabled=false,' -or
        $initialSnapshot.text -notmatch '(?im)^start-menu: disabled, enabled=false,') {
        throw "The clean guest did not start with shell targets disabled. $($initialSnapshot.text)"
    }

    $phase = 'configure-start-menu'
    foreach ($command in @(
        @('set', 'start-menu-opacity', '94'),
        @('set', 'start-menu-color', '#101820'),
        @('set', 'start-menu-color-enabled', 'true'),
        @('set', 'start-menu-hide-recommended', 'false'),
        @('set', 'start-menu-hide-all-apps', 'false'),
        @('set', 'start-menu-three-panel-layout-enabled', 'true'),
        @('enable', 'start-menu', '--confirm')
    )) {
        [void](Invoke-MetaplasiaCli -Arguments $command)
    }

    $phase = 'open-and-observe-start'
    Open-MetaplasiaStart
    $diagnostics = Wait-MetaplasiaStartState -Expected active
    $diagnostics | Set-Content -LiteralPath (
        Join-Path $ResultRoot 'start-xaml-active.txt') -Encoding utf8
    $geometry = Get-MetaplasiaStartGeometry
    $geometrySamples.Add($geometry)
    Assert-MetaplasiaThreePanelGeometry -Geometry $geometry

    $phase = 'live-reconfiguration-load'
    foreach ($iteration in 1..6) {
        [void](Invoke-MetaplasiaCli -Arguments @(
            'set', 'start-menu-hide-all-apps',
            $(if (($iteration % 2) -eq 0) { 'false' } else { 'true' })))
        [void](Wait-MetaplasiaStartState -Expected active -Seconds 15)
        Close-MetaplasiaStart
        Open-MetaplasiaStart
        [void](Wait-MetaplasiaStartState -Expected active -Seconds 15)
    }
    [void](Invoke-MetaplasiaCli -Arguments @(
        'set', 'start-menu-hide-all-apps', 'false'))
    $geometryAfterLoad = Get-MetaplasiaStartGeometry
    $geometrySamples.Add($geometryAfterLoad)
    Assert-MetaplasiaThreePanelGeometry -Geometry $geometryAfterLoad

    $phase = 'verify-reversible-disable'
    [void](Invoke-MetaplasiaCli -Arguments @('disable', 'start-menu'))
    Start-Sleep -Seconds 2
    $disabledSnapshot = Invoke-MetaplasiaCli -Arguments @('snapshot')
    if ($disabledSnapshot.text -notmatch
        '(?im)^start-menu: disabled, enabled=false,') {
        throw "Start target did not report disabled. $($disabledSnapshot.text)"
    }
    $disabledGeometry = Get-MetaplasiaStartGeometry
    if ($null -ne $disabledGeometry.allAppsLabel -or
        $null -ne $disabledGeometry.recommendedLabel) {
        throw 'Injected three-panel labels remain after Start target disable.'
    }

    $phase = 'verify-reinjection-after-start-restart'
    Close-MetaplasiaStart
    $startProcess = Get-Process StartMenuExperienceHost -ErrorAction Stop |
        Where-Object SessionId -eq (Get-Process -Id $PID).SessionId
    Stop-Process -Id $startProcess.Id -Force
    Start-Sleep -Seconds 3
    [void](Invoke-MetaplasiaCli -Arguments @('enable', 'start-menu', '--confirm'))
    Open-MetaplasiaStart
    [void](Wait-MetaplasiaStartState -Expected active)
    $geometryAfterRestart = Get-MetaplasiaStartGeometry
    $geometrySamples.Add($geometryAfterRestart)
    Assert-MetaplasiaThreePanelGeometry -Geometry $geometryAfterRestart

    $phase = 'collect-crash-evidence'
    $crashes = @(Get-WinEvent -FilterHashtable @{
            LogName = 'Application'
            StartTime = $startedUtc.ToLocalTime()
            Id = @(1000, 1001)
        } -ErrorAction SilentlyContinue | Where-Object {
            $_.Message -match '(?i)metaplasia|StartMenuExperienceHost'
        } | Select-Object TimeCreated, Id, ProviderName, Message)
    if ($crashes.Count -ne 0) {
        $crashes | ConvertTo-Json -Depth 4 |
            Set-Content -LiteralPath (Join-Path $ResultRoot 'crashes.json') `
                -Encoding utf8
        throw 'Application Error or WER recorded a Metaplasia/Start crash during the workload.'
    }

    $phase = 'final-disable'
    [void](Invoke-MetaplasiaCli -Arguments @('disable', 'start-menu'))
    Close-MetaplasiaStart
    $finalSnapshot = Invoke-MetaplasiaCli -Arguments @('snapshot')
    $finalSnapshot.text | Set-Content -LiteralPath (
        Join-Path $ResultRoot 'final-snapshot.txt') -Encoding utf8
} catch {
    $failure = $_
} finally {
    # Metadata only, inside the VM. Preserve the evidence even before injection
    # so transport/desktop failures are not misdiagnosed as styling failures.
    try {
        $desktopSamples.Add([ordered]@{
            utc = [DateTime]::UtcNow.ToString('o'); phase = $phase
            desktop = [MetaplasiaDesktopProbe]::DesktopState()
            windows = @([MetaplasiaDesktopProbe]::EnumerateWindows(0, $true))
            shellProcesses = @(Get-Process explorer,StartMenuExperienceHost,ShellExperienceHost -ErrorAction SilentlyContinue |
                Where-Object SessionId -eq (Get-Process -Id $PID).SessionId |
                Select-Object Id,ProcessName,SessionId)
        })
        $desktopSamples | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (
            Join-Path $ResultRoot 'guest-desktop-evidence.json') -Encoding utf8
    } catch {
        $_.Exception.Message | Set-Content -LiteralPath (
            Join-Path $ResultRoot 'guest-desktop-evidence-error.txt') -Encoding utf8
    }
    if ($null -ne $failure) {
        try {
            $events = @()
            foreach ($logName in @('Application', 'Microsoft-Windows-TWinUI/Operational',
                'Microsoft-Windows-AppXDeploymentServer/Operational')) {
                $events += @(Get-WinEvent -FilterHashtable @{ LogName = $logName;
                    StartTime = $startedUtc.ToLocalTime().AddMinutes(-3); Level = @(1, 2, 3) } `
                    -MaxEvents 24 -ErrorAction SilentlyContinue |
                    Select-Object TimeCreated, LogName, Id, ProviderName, Message)
            }
            ConvertTo-Json -InputObject @($events) -Depth 4 | Set-Content -LiteralPath (
                Join-Path $ResultRoot 'failure-shell-events.json') -Encoding utf8
        } catch {
            $_.Exception.Message | Set-Content -LiteralPath (
                Join-Path $ResultRoot 'failure-shell-events-error.txt') -Encoding utf8
        }
        if ($runtimeStarted) {
            try {
                $snapshot = Invoke-MetaplasiaCli -Arguments @('snapshot') -AllowFailure
                $snapshot.text | Set-Content -LiteralPath (
                    Join-Path $ResultRoot 'failure-target-snapshot.txt') -Encoding utf8
                $diagnostics = Invoke-MetaplasiaCli -Arguments @('xaml-types', 'start-menu') -AllowFailure
                $lastStartXamlText = $diagnostics.text
                Get-MetaplasiaStartGeometry | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (
                    Join-Path $ResultRoot 'failure-start-geometry.json') -Encoding utf8
            } catch {
                $_.Exception.Message | Set-Content -LiteralPath (
                    Join-Path $ResultRoot 'failure-runtime-diagnostics-error.txt') -Encoding utf8
            }
        }
        # Preserve the shell's own accessible content when activation disagrees
        # with foreground/cloaking. This reads only the guest Start HWNDs.
        try {
            $nativeElements = @()
            foreach ($startProcess in @(Get-Process StartMenuExperienceHost -ErrorAction SilentlyContinue |
                Where-Object SessionId -eq (Get-Process -Id $PID).SessionId)) {
                foreach ($window in @([MetaplasiaDesktopProbe]::WindowsForProcess([uint32]$startProcess.Id) | Select-Object -First 4)) {
                    $root = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$window.Handle)
                    $items = $root.FindAll([System.Windows.Automation.TreeScope]::Subtree,
                        [System.Windows.Automation.Condition]::TrueCondition)
                    for ($index = 0; $index -lt [Math]::Min($items.Count, 64); ++$index) {
                        $current = $items[$index].Current
                        $nativeElements += [ordered]@{ hwnd = $window.Handle; name = $current.Name; id = $current.AutomationId;
                            class = $current.ClassName; offscreen = $current.IsOffscreen; bounds = [string]$current.BoundingRectangle }
                    }
                }
            }
            ConvertTo-Json -InputObject @($nativeElements) -Depth 4 | Set-Content -LiteralPath (
                Join-Path $ResultRoot 'native-start-accessibility.json') -Encoding utf8
        } catch {
            $_.Exception.Message | Set-Content -LiteralPath (
                Join-Path $ResultRoot 'native-start-accessibility-error.txt') -Encoding utf8
        }
        try {
            & (Join-Path $PSScriptRoot 'Get-MetaplasiaCompatibilityFingerprint.ps1') `
                -CliPath (Join-Path $InstallRoot 'metaplasia-cli.exe') `
                -OutputPath (Join-Path $ResultRoot 'failure-compatibility-fingerprint.json') | Out-Null
        } catch {
            $_.Exception.Message | Set-Content -LiteralPath (
                Join-Path $ResultRoot 'failure-fingerprint-error.txt') -Encoding utf8
        }
        if (-not [string]::IsNullOrWhiteSpace($lastStartXamlText)) {
            $lastStartXamlText | Set-Content -LiteralPath (
                Join-Path $ResultRoot 'last-start-xaml.txt') -Encoding utf8
        }
    }
    $nativeLogRoot = Join-Path $env:LOCALAPPDATA 'Metaplasia\logs'
    if (Test-Path -LiteralPath $nativeLogRoot -PathType Container) {
        $resolvedLogRoot = [IO.Path]::GetFullPath($nativeLogRoot).
            TrimEnd('\') + '\'
        $evidenceRoot = Join-Path $ResultRoot 'native-logs'
        New-Item -ItemType Directory -Path $evidenceRoot -Force |
            Out-Null
        foreach ($log in @(Get-ChildItem -LiteralPath $nativeLogRoot `
                -Filter '*.log' -File -ErrorAction SilentlyContinue)) {
            $resolvedLog = [IO.Path]::GetFullPath($log.FullName)
            if ($resolvedLog.StartsWith(
                    $resolvedLogRoot,
                    [StringComparison]::OrdinalIgnoreCase) -and
                ($log.Attributes -band
                    [IO.FileAttributes]::ReparsePoint) -eq 0 -and
                $log.Length -le 4MB) {
                Copy-Item -LiteralPath $resolvedLog `
                    -Destination (Join-Path $evidenceRoot $log.Name)
            }
        }
    }
    $diagnosticSamples | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath (Join-Path $ResultRoot 'diagnostic-samples.json') `
            -Encoding utf8
    $geometrySamples | ConvertTo-Json -Depth 7 |
        Set-Content -LiteralPath (Join-Path $ResultRoot 'geometry-samples.json') `
            -Encoding utf8
    $status = [ordered]@{
        success = $null -eq $failure
        phase = $phase
        startedUtc = $startedUtc.ToString('o')
        completedUtc = [DateTime]::UtcNow.ToString('o')
        sessionId = (Get-Process -Id $PID).SessionId
        error = if ($null -eq $failure) { $null } else { $failure.ToString() }
    }
    $status | ConvertTo-Json -Depth 5 |
        Set-Content -LiteralPath $statusPath -Encoding utf8
}

if ($null -ne $failure) {
    throw $failure
}
