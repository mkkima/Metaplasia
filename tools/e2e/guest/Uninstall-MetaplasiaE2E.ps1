[CmdletBinding()]
param(
    [string]$InstallRoot = 'C:\MetaplasiaLab\install',
    [string]$TaskName
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'MetaplasiaE2E.Common.ps1')

Assert-MetaplasiaE2EGuest
if (-not [IO.Path]::GetFullPath($InstallRoot).Equals(
        'C:\MetaplasiaLab\install', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Refusing to uninstall outside the Metaplasia E2E install root.'
}
if ($TaskName -and $TaskName -notmatch '^Metaplasia-E2E-[a-f0-9]{32}$') {
    throw 'Unexpected Metaplasia E2E task name.'
}

if ($TaskName) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false `
        -ErrorAction SilentlyContinue
}
$rdpStatePath = 'C:\MetaplasiaLab\rdp-state.json'
if (Test-Path -LiteralPath $rdpStatePath) {
    $rdpState = Get-Content -LiteralPath $rdpStatePath -Raw |
        ConvertFrom-Json
    if ($rdpState.schema -ne 1 -or $rdpState.project -cne 'Metaplasia' -or
        [string]$rdpState.address -notmatch
            '^192\.168\.(?:2[0-9]{2})\.2$' -or
        [int]$rdpState.prefixLength -ne 30 -or
        [int]$rdpState.interfaceIndex -le 0 -or
        [string]$rdpState.firewallRuleName -cne
            'Metaplasia-E2E-RDP') {
        throw 'The isolated RDP restore state is invalid.'
    }
    $adapter = Get-NetAdapter -InterfaceIndex `
        ([int]$rdpState.interfaceIndex) -ErrorAction Stop
    if ([string]$adapter.InterfaceGuid -ine
        [string]$rdpState.interfaceGuid) {
        throw 'The isolated RDP adapter identity changed during the run.'
    }
    Remove-NetIPAddress -InterfaceIndex $adapter.ifIndex `
        -IPAddress ([string]$rdpState.address) -Confirm:$false `
        -ErrorAction Stop
    $terminalServerPath =
        'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server'
    $rdpTcpPath = Join-Path $terminalServerPath 'WinStations\RDP-Tcp'
    Set-ItemProperty -LiteralPath $terminalServerPath `
        -Name fDenyTSConnections -Type DWord `
        -Value ([int]$rdpState.fDenyTSConnections)
    Set-ItemProperty -LiteralPath $rdpTcpPath `
        -Name UserAuthentication -Type DWord `
        -Value ([int]$rdpState.userAuthentication)
    Remove-NetFirewallRule -Name ([string]$rdpState.firewallRuleName) `
        -ErrorAction Stop
    if (-not [bool]$rdpState.serviceWasRunning) {
        Stop-Service -Name TermService -Force -ErrorAction Stop
    }
    Remove-Item -LiteralPath $rdpStatePath -Force
}
$cli = Join-Path $InstallRoot 'metaplasia-cli.exe'
if (Test-Path -LiteralPath $cli) {
    # A host that never started has no named pipe. That is an expected cleanup
    # state, not an uninstall failure; the process sweep below is authoritative.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $cli disable start-menu 2>&1 | Out-Null
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    Start-Sleep -Seconds 2
}
$agentPath = [IO.Path]::GetFullPath(
    (Join-Path $InstallRoot 'metaplasia-agent.dll'))
$shellProcessesRestarted = [Collections.Generic.List[uint32]]::new()
foreach ($shellProcess in @(Get-Process -Name @(
            'StartMenuExperienceHost', 'explorer') `
        -ErrorAction SilentlyContinue)) {
    try {
        $loadedFromInstall = @($shellProcess.Modules | Where-Object {
                $_.FileName -and
                [IO.Path]::GetFullPath($_.FileName).Equals(
                    $agentPath,
                    [StringComparison]::OrdinalIgnoreCase)
            }).Count -ne 0
    } catch {
        if (Get-Process -Id $shellProcess.Id -ErrorAction SilentlyContinue) {
            throw
        }
        continue
    }
    if (-not $loadedFromInstall) {
        continue
    }
    $shellProcessesRestarted.Add([uint32]$shellProcess.Id)
    Stop-Process -Id $shellProcess.Id -Force -ErrorAction Stop
}
if ($shellProcessesRestarted.Count -ne 0) {
    $shellDeadline = [DateTime]::UtcNow.AddSeconds(10)
    do {
        $loadedShells = @(Get-Process -Id $shellProcessesRestarted `
            -ErrorAction SilentlyContinue)
        if ($loadedShells.Count -eq 0) { break }
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $shellDeadline)
    if ($loadedShells.Count -ne 0) {
        throw 'A shell process retaining the Metaplasia agent did not exit.'
    }
}
$processDeadline = [DateTime]::UtcNow.AddSeconds(15)
do {
    $remaining = @(Get-CimInstance Win32_Process | Where-Object {
            $_.Name -like 'metaplasia*' -and $_.ExecutablePath
        })
    foreach ($process in $remaining) {
        $path = [IO.Path]::GetFullPath([string]$process.ExecutablePath)
        if (-not $path.StartsWith(
                [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\') + '\',
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Refusing to stop a foreign Metaplasia process: $path"
        }
        Stop-Process -Id $process.ProcessId -Force -ErrorAction Stop
    }
    if ($remaining.Count -ne 0) { Start-Sleep -Milliseconds 500 }
} while ($remaining.Count -ne 0 -and
    [DateTime]::UtcNow -lt $processDeadline)
if (@(Get-Process -Name 'metaplasia*' -ErrorAction SilentlyContinue).Count `
        -ne 0) {
    throw 'Metaplasia processes did not terminate during uninstall.'
}

$runPath = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
if (Test-Path -LiteralPath $runPath) {
    Remove-ItemProperty -LiteralPath $runPath -Name 'Metaplasia' `
        -ErrorAction SilentlyContinue
}
$ownershipPath = 'HKCU:\Software\Metaplasia\PolicyOwnership'
if (Test-Path -LiteralPath $ownershipPath) {
    $owned = (Get-ItemProperty -LiteralPath $ownershipPath `
            -ErrorAction Stop).NoStartMenuMorePrograms -eq 1
    if ($owned) {
        Remove-ItemProperty -LiteralPath `
            'HKLM:\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer' `
            -Name 'NoStartMenuMorePrograms' -ErrorAction SilentlyContinue
    }
    Remove-Item -LiteralPath $ownershipPath -Recurse -Force
}

foreach ($path in @(
    (Join-Path $env:LOCALAPPDATA 'Metaplasia'),
    $InstallRoot
)) {
    if (Test-Path -LiteralPath $path) {
        $resolved = [IO.Path]::GetFullPath($path)
        $allowed = if ($path -eq $InstallRoot) {
            'C:\MetaplasiaLab'
        } else {
            [IO.Path]::GetFullPath($env:LOCALAPPDATA)
        }
        $prefix = [IO.Path]::GetFullPath($allowed).TrimEnd('\') + '\'
        $item = Get-Item -LiteralPath $resolved -Force
        if (-not $resolved.StartsWith(
                $prefix, [StringComparison]::OrdinalIgnoreCase) -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing to remove unsafe Metaplasia path: $resolved"
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}

$winlogon = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
foreach ($name in @(
    'AutoAdminLogon', 'DefaultUserName', 'DefaultDomainName',
    'DefaultPassword', 'AutoLogonCount', 'ForceAutoLogon'
)) {
    Remove-ItemProperty -LiteralPath $winlogon -Name $name `
        -ErrorAction SilentlyContinue
}

[ordered]@{
    success = $true
    rebootRequired = $true
    shellProcessesRestarted = @($shellProcessesRestarted)
    driverVerifier = Get-MetaplasiaVerifierState
} | ConvertTo-Json -Depth 5 -Compress
