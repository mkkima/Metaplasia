[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ResultRoot,
    [Parameter(Mandatory)][string]$ExpectedUserSid,
    [Parameter(Mandatory)][string]$ExpectedCompatibilityPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not [IO.Path]::GetFullPath($ResultRoot).StartsWith(
        'C:\MetaplasiaLab\results\',
        [StringComparison]::OrdinalIgnoreCase) -or
    -not [IO.Path]::GetFullPath($ExpectedCompatibilityPath).StartsWith(
        'C:\MetaplasiaLab\results\',
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The workload wrapper result path is outside the lab boundary.'
}
$logPath = Join-Path $ResultRoot 'workload-process.log'
try {
    & 'C:\MetaplasiaLab\package\guest\Invoke-MetaplasiaE2EWorkload.ps1' `
        -ResultRoot $ResultRoot -ExpectedUserSid $ExpectedUserSid `
        -ExpectedCompatibilityPath $ExpectedCompatibilityPath *>&1 |
        Out-File -LiteralPath $logPath -Encoding utf8
} catch {
    $_ | Format-List * -Force | Out-String |
        Out-File -LiteralPath $logPath -Encoding utf8 -Append
    exit 1
}
