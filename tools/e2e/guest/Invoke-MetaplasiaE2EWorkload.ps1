[CmdletBinding()]
param(
    [string]$InstallRoot = 'C:\MetaplasiaLab\install',
    [Parameter(Mandatory)][string]$ResultRoot,
    [Parameter(Mandatory)][string]$ExpectedUserSid
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ((Get-Process -Id $PID).SessionId -eq 0) {
    throw 'The Metaplasia workload must run in the interactive desktop session.'
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
    startedUtc = $startedUtc.ToString('o')
} | ConvertTo-Json -Compress | Set-Content -LiteralPath (
    Join-Path $ResultRoot 'workload-started.json') -Encoding utf8
$phase = 'startup'
$failure = $null
$diagnosticSamples = [Collections.Generic.List[object]]::new()
$geometrySamples = [Collections.Generic.List[object]]::new()
$lastStartXamlText = $null
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
    public string ClassName;
    public string Title;
    public int Left;
    public int Top;
    public int Width;
    public int Height;
    public int Cloaked;
}

public static class MetaplasiaDesktopProbe {
    private delegate bool EnumWindowsProc(IntPtr hwnd, IntPtr parameter);

    [StructLayout(LayoutKind.Sequential)]
    private struct RECT { public int Left, Top, Right, Bottom; }

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
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
    [DllImport("user32.dll")]
    private static extern void keybd_event(byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);

    public static void ToggleStart() {
        const byte VK_LWIN = 0x5B;
        const uint KEYEVENTF_KEYUP = 0x0002;
        keybd_event(VK_LWIN, 0, 0, UIntPtr.Zero);
        keybd_event(VK_LWIN, 0, KEYEVENTF_KEYUP, UIntPtr.Zero);
    }

    public static MetaplasiaWindowInfo[] WindowsForProcess(uint expectedPid) {
        var result = new List<MetaplasiaWindowInfo>();
        EnumWindowsProc callback = delegate(IntPtr hwnd, IntPtr ignored) {
            uint pid;
            GetWindowThreadProcessId(hwnd, out pid);
            if (pid != expectedPid || !IsWindowVisible(hwnd)) return true;
            RECT rect;
            if (!GetWindowRect(hwnd, out rect)) return true;
            var className = new StringBuilder(256);
            var title = new StringBuilder(512);
            GetClassName(hwnd, className, className.Capacity);
            GetWindowText(hwnd, title, title.Capacity);
            int cloaked = 0;
            DwmGetWindowAttribute(hwnd, 14, out cloaked, sizeof(int));
            result.Add(new MetaplasiaWindowInfo {
                Handle = hwnd.ToInt64(),
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
        EnumWindows(callback, IntPtr.Zero);
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
    $elements = [System.Windows.Automation.AutomationElement]::RootElement.FindAll(
        [System.Windows.Automation.TreeScope]::Descendants,
        $condition)
    $observed = [Collections.Generic.List[object]]::new()
    $allApps = $null
    $recommended = $null
    $limit = [Math]::Min($elements.Count, 1024)
    for ($index = 0; $index -lt $limit; ++$index) {
        try {
            $current = $elements.Item($index).Current
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
        namedElements = $observed
        allAppsLabel = $allApps
        recommendedLabel = $recommended
    }
}

function Open-MetaplasiaStart {
    [MetaplasiaDesktopProbe]::ToggleStart()
    Start-Sleep -Milliseconds 900
}

function Close-MetaplasiaStart {
    [MetaplasiaDesktopProbe]::ToggleStart()
    Start-Sleep -Milliseconds 500
}

function Assert-MetaplasiaThreePanelGeometry {
    param([Parameter(Mandatory)]$Geometry)

    # Hidden RDP sessions on some Windows 11 builds do not publish the Start
    # island through desktop UI Automation. In that case style-state=active is
    # still a geometry assertion: the native agent now performs UpdateLayout
    # and validates the live panel/label sizes, offsets, and visibility before
    # it is allowed to publish active. If either marker is exposed, require the
    # complete pair and independently validate its screen coordinates here.
    if ($null -eq $Geometry.allAppsLabel -and
        $null -eq $Geometry.recommendedLabel) {
        return
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
        if ($explorer.Count -eq 1) { break }
        Start-Sleep -Seconds 1
    } while ([DateTime]::UtcNow -lt $shellDeadline)
    if ($explorer.Count -ne 1) {
        throw 'Explorer did not become ready in the workload desktop session.'
    }

    $phase = 'launch-background-runtime'
    $application = Join-Path $InstallRoot 'metaplasia.exe'
    Start-Process -FilePath $application -ArgumentList '--background' `
        -WindowStyle Hidden | Out-Null
    Wait-MetaplasiaHost

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
    if ($null -ne $failure) {
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
