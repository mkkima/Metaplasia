[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$SourceLabRoot,
    [Parameter(Mandatory)][string]$PackageRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$entryPoint = Join-Path $PSScriptRoot 'Start-MetaplasiaEndToEndLab.ps1'
$powerShell = (Get-Process -Id $PID).Path
$repositoryRoot = (Resolve-Path -LiteralPath (
        Join-Path $PSScriptRoot '..\..')).Path
$statusPath = Join-Path $repositoryRoot 'out\e2e\elevation-request.json'
$statusDirectory = Split-Path -Parent $statusPath
New-Item -ItemType Directory -Path $statusDirectory -Force | Out-Null
[IO.File]::WriteAllText(
    $statusPath,
    ([ordered]@{
            state = 'waiting-for-consent'
            startedUtc = [DateTime]::UtcNow.ToString('o')
            launcherProcessId = $PID
        } | ConvertTo-Json),
    [Text.UTF8Encoding]::new($false))

$arguments = @(
    '-NoProfile', '-ExecutionPolicy', 'Bypass',
    '-File', ('"{0}"' -f $entryPoint),
    '-SourceLabRoot', ('"{0}"' -f $SourceLabRoot),
    '-ElevatedStage',
    '-PackageRoot', ('"{0}"' -f $PackageRoot)
) -join ' '
try {
    $process = Start-Process -FilePath $powerShell -Verb RunAs `
        -ArgumentList $arguments -WindowStyle Hidden -Wait -PassThru
    $status = [ordered]@{
        state = 'completed'
        completedUtc = [DateTime]::UtcNow.ToString('o')
        exitCode = $process.ExitCode
    }
} catch {
    $status = [ordered]@{
        state = 'launch-failed'
        completedUtc = [DateTime]::UtcNow.ToString('o')
        error = $_.Exception.Message
        hresult = ('0x{0:X8}' -f ($_.Exception.HResult -band 0xffffffffL))
    }
}
[IO.File]::WriteAllText(
    $statusPath,
    ($status | ConvertTo-Json),
    [Text.UTF8Encoding]::new($false))
if ($status.state -eq 'completed') {
    exit $status.exitCode
}
exit 1
