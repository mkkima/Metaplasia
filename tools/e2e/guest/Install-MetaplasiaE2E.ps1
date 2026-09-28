[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [string]$InstallRoot = 'C:\MetaplasiaLab\install',
    [string]$IsolatedRdpAddress
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'MetaplasiaE2E.Common.ps1')

Assert-MetaplasiaE2EGuest
$manifest = Assert-MetaplasiaE2EPackage -PackageRoot $PackageRoot
if (-not [IO.Path]::GetFullPath($InstallRoot).Equals(
        'C:\MetaplasiaLab\install', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Metaplasia E2E install path is not the isolated path.'
}

$existingProcesses = @(Get-CimInstance Win32_Process | Where-Object {
        $_.Name -like 'metaplasia*'
    })
if ($existingProcesses.Count -ne 0) {
    throw 'Metaplasia processes already exist in the clean baseline.'
}
foreach ($path in @(
    $InstallRoot,
    (Join-Path $env:LOCALAPPDATA 'Metaplasia')
)) {
    if (Test-Path -LiteralPath $path) {
        throw "Metaplasia state already exists in the clean baseline: $path"
    }
}
if (Get-ScheduledTask -TaskName 'Metaplasia-E2E-*' `
        -ErrorAction SilentlyContinue) {
    throw 'A Metaplasia E2E scheduled task already exists.'
}

New-Item -ItemType Directory -Path $InstallRoot -Force | Out-Null
$installItem = Get-Item -LiteralPath $InstallRoot -Force
if (-not $installItem.PSIsContainer -or
    ($installItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'Metaplasia install root is not a regular directory.'
}
$componentNames = @(
    'metaplasia.exe',
    'metaplasia-host.exe',
    'metaplasia-watchdog.exe',
    'metaplasia-cli.exe',
    'metaplasia-agent.dll'
)
foreach ($name in $componentNames) {
    Copy-Item -LiteralPath (Join-Path $PackageRoot "bin\$name") `
        -Destination (Join-Path $InstallRoot $name)
}

$rdpState = $null
if ($IsolatedRdpAddress) {
    if ($IsolatedRdpAddress -notmatch '^192\.168\.(?:2[0-9]{2})\.2$') {
        throw 'The isolated RDP address is outside the lab subnet pool.'
    }
    $statePath = 'C:\MetaplasiaLab\rdp-state.json'
    if (Test-Path -LiteralPath $statePath) {
        throw 'Isolated RDP state already exists in the clean baseline.'
    }
    $adapterDeadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $adapters = @(Get-NetAdapter -Physical -ErrorAction SilentlyContinue |
            Where-Object {
                $_.InterfaceDescription -match
                    '(?i)Hyper-V.*(Ethernet|Network) Adapter' -and
                $_.Status -eq 'Up'
            })
        if ($adapters.Count -eq 1) { break }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $adapterDeadline)
    if ($adapters.Count -ne 1) {
        throw 'Expected exactly one connected Hyper-V network adapter.'
    }
    $adapter = $adapters[0]
    if (Get-NetIPAddress -AddressFamily IPv4 `
            -IPAddress $IsolatedRdpAddress -ErrorAction SilentlyContinue) {
        throw 'The isolated guest RDP address is already assigned.'
    }
    $terminalServerPath =
        'HKLM:\SYSTEM\CurrentControlSet\Control\Terminal Server'
    $rdpTcpPath = Join-Path $terminalServerPath `
        'WinStations\RDP-Tcp'
    $terminalServer = Get-ItemProperty -LiteralPath $terminalServerPath
    $rdpTcp = Get-ItemProperty -LiteralPath $rdpTcpPath
    $firewallRuleName = 'Metaplasia-E2E-RDP'
    if (Get-NetFirewallRule -Name $firewallRuleName `
            -ErrorAction SilentlyContinue) {
        throw 'The isolated RDP firewall rule already exists.'
    }
    $service = Get-Service -Name TermService -ErrorAction Stop
    $rdpState = [ordered]@{
        schema = 1
        project = 'Metaplasia'
        address = $IsolatedRdpAddress
        prefixLength = 30
        interfaceIndex = $adapter.ifIndex
        interfaceGuid = [string]$adapter.InterfaceGuid
        fDenyTSConnections = [int]$terminalServer.fDenyTSConnections
        userAuthentication = [int]$rdpTcp.UserAuthentication
        serviceWasRunning = $service.Status -eq 'Running'
        firewallRuleName = $firewallRuleName
    }
    $rdpState | ConvertTo-Json -Depth 6 |
        Set-Content -LiteralPath $statePath -Encoding utf8
    New-NetIPAddress -InterfaceIndex $adapter.ifIndex `
        -IPAddress $IsolatedRdpAddress -PrefixLength 30 `
        -AddressFamily IPv4 | Out-Null
    Set-ItemProperty -LiteralPath $terminalServerPath `
        -Name fDenyTSConnections -Type DWord -Value 0
    Set-ItemProperty -LiteralPath $rdpTcpPath `
        -Name UserAuthentication -Type DWord -Value 1
    $hostAddress = $IsolatedRdpAddress -replace '\.2$', '.1'
    New-NetFirewallRule -Name $firewallRuleName `
        -DisplayName 'Metaplasia isolated E2E RDP' `
        -Direction Inbound -Action Allow -Enabled True -Profile Any `
        -Protocol TCP -LocalPort 3389 -RemoteAddress $hostAddress `
        -InterfaceAlias $adapter.Name | Out-Null
    if ($service.Status -ne 'Running') {
        Start-Service -Name TermService
        (Get-Service -Name TermService).WaitForStatus(
            'Running', [TimeSpan]::FromSeconds(15))
    }
}

$verifier = Get-MetaplasiaVerifierState
[ordered]@{
    success = $true
    project = [string]$manifest.project
    version = [string]$manifest.version
    installedFiles = $componentNames
    isolatedRdp = $rdpState
    driverVerifier = $verifier
} | ConvertTo-Json -Depth 5 -Compress
