[CmdletBinding()]
param([Parameter(DontShow)][switch]$WindowsPowerShellStage)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$e2eRoot = $PSScriptRoot
$scripts = @(Get-ChildItem -LiteralPath $e2eRoot -Filter '*.ps1' `
    -File -Recurse)
if ($scripts.Count -lt 7) {
    throw 'The Metaplasia E2E harness is incomplete.'
}
foreach ($script in $scripts) {
    $tokens = $null
    $errors = $null
    [void][Management.Automation.Language.Parser]::ParseFile(
        $script.FullName, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) {
        $rendered = ($errors | ForEach-Object {
                "$($_.Extent.File):$($_.Extent.StartLineNumber): $($_.Message)"
            }) -join [Environment]::NewLine
        throw "PowerShell parse errors were found.$([Environment]::NewLine)$rendered"
    }
}

$guestRoot = Join-Path $e2eRoot 'guest'
$utf8 = [Text.UTF8Encoding]::new($false, $true)
foreach ($guestScript in Get-ChildItem -LiteralPath $guestRoot `
        -Filter '*.ps1' -File) {
    $bytes = [IO.File]::ReadAllBytes($guestScript.FullName)
    [void]$utf8.GetString($bytes)
    if (@($bytes | Where-Object { $_ -gt 0x7f }).Count -ne 0) {
        throw "Guest script must remain ASCII for Windows PowerShell 5.1: $($guestScript.FullName)"
    }
}
$guestText = (Get-ChildItem -LiteralPath $guestRoot -Filter '*.ps1' -File |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
if ($guestText -match '(?i)C:\\ISeeYouLab|ISeeYouCollector|iseeyou_fs') {
    throw 'Metaplasia guest scripts reference ISeeYou guest artifacts.'
}
if ($guestText -notmatch [regex]::Escape('C:\MetaplasiaLab') -or
    $guestText -notmatch 'Metaplasia-E2E-') {
    throw 'Metaplasia guest namespace guards are missing.'
}
foreach ($requiredInteractiveContract in @(
    'The Metaplasia workload must use the expected limited user token'
)) {
    if ($guestText -notmatch [regex]::Escape($requiredInteractiveContract)) {
        throw "The limited interactive-launch contract is missing: $requiredInteractiveContract"
    }
}
foreach ($requiredIsolationContract in @(
    'Metaplasia-E2E-RDP',
    'RemoteAddress $hostAddress',
    'C:\MetaplasiaLab\rdp-state.json'
)) {
    if ($guestText -notmatch [regex]::Escape($requiredIsolationContract)) {
        throw "The isolated RDP guest contract is missing: $requiredIsolationContract"
    }
}

$hostText = (Get-ChildItem -LiteralPath $e2eRoot -Filter '*.ps1' -File |
    Where-Object FullName -ne $PSCommandPath |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
if ($hostText -match '(?i)out\\lab\\(packages|results|latest-package)') {
    throw 'Metaplasia host scripts would mix output with ISeeYou lab artifacts.'
}
if ($hostText -notmatch [regex]::Escape('out\e2e')) {
    throw 'Metaplasia host output namespace is missing.'
}
if ($hostText -match '(?i)Save-ISeeYouVmScreen|CaptureScreen|Get-VMVideo') {
    throw 'The Metaplasia E2E harness must not capture VM or host screenshots.'
}
if ($hostText -notmatch 'Start-ScheduledTask -TaskName' -or
    $hostText -notmatch 'LogonType Interactive -RunLevel Limited' -or
    $hostText -match 'New-ScheduledTaskTrigger -AtLogOn' -or
    $hostText -match 'LogonType ServiceAccount') {
    throw 'The on-demand limited workload task contract is missing or regressed.'
}
if ($hostText -notmatch 'New-VMSwitch -Name .* -SwitchType Internal' -or
    $hostText -notmatch 'Open-MetaplasiaE2ERdpSession.ps1' -or
    $hostText -match '(?i)New-NetNat|InternetSharing|Default Switch') {
    throw 'The isolated host-to-VM RDP transport contract is missing or unsafe.'
}
if ($hostText -notmatch '62401, 62403, 62404, 62405' -or
    $hostText -notmatch 'Sort-Object RecordId -Unique') {
    throw 'The bounded first-logon OOBE evidence query is missing.'
}
foreach ($requiredBaselineRecoveryContract in @(
    'Refusing to recover a baseline while any checkpoint exists.',
    'Checkpoint-free VM contains test residue:',
    'The source lab staging root is not empty:',
    'Checkpoint-free VM has an unsafe Driver Verifier state.',
    'baseline-recovery-clean-state.json',
    'baselineRecreated = $baselineRecreated'
)) {
    if ($hostText -notmatch
        [regex]::Escape($requiredBaselineRecoveryContract)) {
        throw "The guarded baseline recovery contract is missing: $requiredBaselineRecoveryContract"
    }
}

if (-not $WindowsPowerShellStage -and
    $PSVersionTable.PSVersion.Major -ne 5) {
    $windowsPowerShell = Join-Path $env:SystemRoot `
        'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $windowsPowerShell -NoProfile -ExecutionPolicy Bypass `
        -File $PSCommandPath -WindowsPowerShellStage
    if ($LASTEXITCODE -ne 0) {
        throw "Windows PowerShell 5.1 harness validation failed with $LASTEXITCODE."
    }
}

Write-Output (
    "Validated $($scripts.Count) isolated Metaplasia E2E scripts with " +
    "PowerShell $($PSVersionTable.PSVersion.Major).")
