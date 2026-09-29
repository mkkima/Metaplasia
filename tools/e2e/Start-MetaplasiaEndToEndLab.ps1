[CmdletBinding()]
param(
    [string]$SourceLabRoot = 'C:\workspace\ISeeYou',
    [string]$WindowsUpdatePath,
    [switch]$KeepFailedVM,
    [switch]$VisibleVM,
    [Parameter(DontShow)][switch]$ElevatedStage,
    [Parameter(DontShow)][string]$PackageRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath (
        Join-Path $PSScriptRoot '..\..')).Path
$SourceLabRoot = [IO.Path]::GetFullPath($SourceLabRoot)
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
$isAdministrator = $principal.IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
$windowsPowerShell = Join-Path $env:SystemRoot `
    'System32\WindowsPowerShell\v1.0\powershell.exe'
if (-not (Test-Path -LiteralPath $windowsPowerShell -PathType Leaf)) {
    throw "Windows PowerShell 5.1 was not found at $windowsPowerShell."
}

if (-not $ElevatedStage) {
    & (Join-Path $PSScriptRoot 'New-MetaplasiaE2EPackage.ps1')
    $latest = Get-Content -LiteralPath (
        Join-Path $repositoryRoot 'out\e2e\latest-package.json') -Raw |
        ConvertFrom-Json
    $PackageRoot = [string]$latest.packageRoot
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $PSCommandPath),
        '-SourceLabRoot', ('"{0}"' -f $SourceLabRoot),
        '-ElevatedStage',
        '-PackageRoot', ('"{0}"' -f $PackageRoot)
    )
    if ($KeepFailedVM) { $arguments += '-KeepFailedVM' }
    if ($VisibleVM) { $arguments += '-VisibleVM' }
    if ($WindowsUpdatePath) {
        $arguments += @('-WindowsUpdatePath', ('"{0}"' -f ([IO.Path]::GetFullPath($WindowsUpdatePath))))
    }
    $startParameters = @{
        FilePath = $windowsPowerShell
        ArgumentList = ($arguments -join ' ')
        WindowStyle = 'Hidden'
        Wait = $true
        PassThru = $true
    }
    if (-not $isAdministrator) {
        $startParameters.Verb = 'RunAs'
    }
    $process = Start-Process @startParameters
    if ($process.ExitCode -ne 0) {
        throw "The elevated E2E stage failed with exit code $($process.ExitCode)."
    }
    return
}

if (-not $isAdministrator) {
    throw 'The elevated E2E stage requires administrator rights.'
}
$elevatedRoot = Join-Path $repositoryRoot 'out\e2e\elevated'
New-Item -ItemType Directory -Path $elevatedRoot -Force | Out-Null
$id = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$transcriptPath = Join-Path $elevatedRoot "$id-transcript.txt"
$errorPath = Join-Path $elevatedRoot "$id-error.txt"
Start-Transcript -LiteralPath $transcriptPath | Out-Null
try {
    & (Join-Path $PSScriptRoot 'Invoke-MetaplasiaHyperVLab.ps1') `
        -SourceLabRoot $SourceLabRoot -PackageRoot $PackageRoot `
        -WindowsUpdatePath $WindowsUpdatePath `
        -VisibleVM:$VisibleVM `
        -KeepFailedVM:$KeepFailedVM
} catch {
    $_ | Format-List * -Force | Out-String |
        Set-Content -LiteralPath $errorPath -Encoding utf8
    throw
} finally {
    Stop-Transcript | Out-Null
}
