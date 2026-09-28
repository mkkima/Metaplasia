[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'MetaplasiaE2E.Common.ps1')

Assert-MetaplasiaE2EGuest
$remainingProcesses = @(Get-CimInstance Win32_Process | Where-Object {
        $_.Name -like 'metaplasia*'
    })
if ($remainingProcesses.Count -ne 0) {
    throw 'Metaplasia processes remain after uninstall and reboot.'
}
foreach ($path in @(
    'C:\MetaplasiaLab\install',
    (Join-Path $env:LOCALAPPDATA 'Metaplasia')
)) {
    if (Test-Path -LiteralPath $path) {
        throw "Metaplasia state remains after uninstall: $path"
    }
}
if (Get-ScheduledTask -TaskName 'Metaplasia-E2E-*' `
        -ErrorAction SilentlyContinue) {
    throw 'A Metaplasia E2E task remains after uninstall.'
}
$isolatedRdpAddress = @(Get-NetIPAddress -AddressFamily IPv4 `
        -ErrorAction SilentlyContinue | Where-Object {
            $_.IPAddress -match '^192\.168\.2[0-9]{2}\.2$'
        })
if ((Test-Path -LiteralPath 'C:\MetaplasiaLab\rdp-state.json') -or
    $isolatedRdpAddress.Count -ne 0 -or
    (Get-NetFirewallRule -Name 'Metaplasia-E2E-RDP' `
        -ErrorAction SilentlyContinue)) {
    throw 'Temporary Metaplasia isolated RDP state remains after uninstall.'
}
$runKey = Get-Item -LiteralPath `
    'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
    -ErrorAction SilentlyContinue
$runValue = if ($null -eq $runKey) { $null } else {
    $runKey.GetValue('Metaplasia', $null, 'DoNotExpandEnvironmentNames')
}
if ($null -ne $runValue) {
    throw 'Metaplasia startup registration remains after uninstall.'
}
if (Test-Path -LiteralPath 'HKCU:\Software\Metaplasia\PolicyOwnership') {
    throw 'Metaplasia policy ownership remains after uninstall.'
}
$winlogon = Get-ItemProperty -LiteralPath `
    'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
$autoAdminLogon = if ($winlogon.PSObject.Properties['AutoAdminLogon']) {
    [string]$winlogon.AutoAdminLogon
} else { '' }
$defaultPassword = if ($winlogon.PSObject.Properties['DefaultPassword']) {
    [string]$winlogon.DefaultPassword
} else { '' }
$forceAutoLogon = if ($winlogon.PSObject.Properties['ForceAutoLogon']) {
    [string]$winlogon.ForceAutoLogon
} else { '' }
if ($autoAdminLogon -eq '1' -or
    -not [string]::IsNullOrEmpty($defaultPassword) -or
    $forceAutoLogon -eq '1') {
    throw 'Temporary E2E automatic logon state remains after uninstall.'
}
$boot = & bcdedit.exe /enum all 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -or $boot -match '(?im)^testsigning\s+Yes\s*$') {
    throw 'Unexpected test-signing state after Metaplasia E2E cleanup.'
}

[ordered]@{
    success = $true
    processesRemoved = $true
    startupRemoved = $true
    settingsRemoved = $true
    policyOwnershipRemoved = $true
    temporaryLogonRemoved = $true
    isolatedRdpRemoved = $true
    testSigningDisabled = $true
    driverVerifier = Get-MetaplasiaVerifierState
} | ConvertTo-Json -Depth 5 -Compress
