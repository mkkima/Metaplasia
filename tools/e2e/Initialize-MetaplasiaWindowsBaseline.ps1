[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$VMName,
    [Parameter(Mandatory)][Guid]$ExpectedId,
    [Parameter(Mandatory)][PSCredential]$Credential,
    [Parameter(Mandatory)][string]$UpdatePath,
    [Parameter(Mandatory)][string]$TargetFingerprintPath,
    [Parameter(Mandatory)][string]$ResultRoot
)

# Called only by the identity-checked adapter after restoring the original
# checkpoint. Its finally block owns rollback, including interrupted servicing.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# Microsoft Update Catalog entry 5cc91450-4f0f-40a8-a67a-63dc58d9dac9,
# KB5129195 x64; the 26200.8037 source already includes checkpoint KB5043080.
$expectedHash = 'B33FF7AAB5BBBBB73867F3F5C2F492FE97C0129B96AA1941BC270610D7876DFA'
$target = Get-Content -LiteralPath $TargetFingerprintPath -Raw | ConvertFrom-Json
if ($target.windows -cne '10.0.26200.9457' -or $target.architecture -cne 'X64') {
    throw 'This pinned servicing package only supports Windows 26200.9457 x64.'
}
$update = Get-Item -LiteralPath $UpdatePath -Force
if ($update.PSIsContainer -or
    ($update.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
    $update.Name -cne 'windows11.0-kb5129195-x64_ed361878ec2b56a7dfdb8f256565263a7fdc8eaa.msu' -or
    (Get-FileHash -LiteralPath $update.FullName -Algorithm SHA256).Hash -cne $expectedHash) {
    throw 'The MSU does not match the SHA-256 published by Microsoft Update Catalog.'
}
$vm = Get-VM -Name $VMName -ErrorAction Stop
if ($vm.Id -ne $ExpectedId -or $vm.State -ne 'Running' -or
    @(Get-VMNetworkAdapter -VMName $VMName | Where-Object SwitchName).Count -ne 0) {
    throw 'Servicing requires the identity-checked, disconnected lab VM.'
}
$session = New-PSSession -VMName $VMName -Credential $Credential
$copyService = $null
$copyServiceWasEnabled = $false
$phase = 'copy-pinned-update'
$progressPath = Join-Path $ResultRoot 'windows-servicing.json'
try {
    [ordered]@{ phase = $phase; startedUtc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $progressPath -Encoding utf8
    Invoke-Command -Session $session -ScriptBlock {
        $computer = Get-CimInstance Win32_ComputerSystem
        $os = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
        if ($computer.Manufacturer -ne 'Microsoft Corporation' -or
            $computer.Model -notmatch 'Virtual Machine' -or
            $os.CurrentBuild -ne '26200' -or [int]$os.UBR -lt 1742 -or
            [int]$os.UBR -ge 9457) {
            throw 'Unexpected guest or servicing baseline; no update was started.'
        }
        if (Test-Path -LiteralPath 'C:\MetaplasiaServicing') {
            throw 'Guest servicing residue exists; restore the clean baseline.'
        }
        if ((Get-PSDrive -Name C).Free -lt 18GB) {
            throw 'The guest needs at least 18 GiB free for this cumulative update.'
        }
        New-Item -ItemType Directory -Path 'C:\MetaplasiaServicing' | Out-Null
    }
    # Use the Hyper-V file transport, not base64 serialization of a 4.6 GB MSU
    # through PowerShell. Restore the service setting before taking a checkpoint.
    $services = @(Get-VMIntegrationService -VMName $VMName | Where-Object {
        $_.Id.ToString().EndsWith('6c09bb55-d683-4da0-8931-c9bf705f6480',
            [StringComparison]::OrdinalIgnoreCase)
    })
    if ($services.Count -ne 1) { throw 'The guest file-copy service identity is ambiguous.' }
    $copyService = $services[0]
    $copyServiceWasEnabled = [bool]$copyService.Enabled
    if (-not $copyServiceWasEnabled) {
        Enable-VMIntegrationService -VMIntegrationService $copyService
    }
    Copy-VMFile -Name $VMName -SourcePath $update.FullName `
        -DestinationPath 'C:\MetaplasiaServicing\target.msu' -FileSource Host
    $phase = 'install-pinned-update'
    Invoke-Command -Session $session -ArgumentList $expectedHash -ScriptBlock {
        param([string]$ExpectedHash)
        if ((Get-FileHash 'C:\MetaplasiaServicing\target.msu' -Algorithm SHA256).Hash -cne $ExpectedHash) {
            throw 'The guest MSU hash changed during transfer.'
        }
        # DISM validates Microsoft's package catalogs; never disable signature checks.
        $global:MetaplasiaServicingProcess = Start-Process -FilePath "$env:SystemRoot\System32\dism.exe" `
            -ArgumentList '/Online /Add-Package /PackagePath:C:\MetaplasiaServicing\target.msu /NoRestart /Quiet /LogPath:C:\MetaplasiaServicing\dism.log' `
            -WindowStyle Hidden -PassThru
    }
    $deadline = [DateTime]::UtcNow.AddMinutes(25)
    do {
        Start-Sleep -Seconds 10
        $progress = Invoke-Command -Session $session -ScriptBlock {
            $global:MetaplasiaServicingProcess.Refresh()
            [pscustomobject]@{
                finished = $global:MetaplasiaServicingProcess.HasExited
                exitCode = if ($global:MetaplasiaServicingProcess.HasExited) {
                    $global:MetaplasiaServicingProcess.ExitCode
                } else { $null }
                logTail = @(Get-Content 'C:\MetaplasiaServicing\dism.log' -Tail 5 -ErrorAction SilentlyContinue |
                    ForEach-Object { [string]$_ })
            }
        }
        [ordered]@{
            phase = $phase; checkedUtc = [DateTime]::UtcNow.ToString('o')
            finished = $progress.finished; exitCode = $progress.exitCode
            logTail = $progress.logTail
        } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $progressPath -Encoding utf8
        if ($progress.finished) { break }
    } while ([DateTime]::UtcNow -lt $deadline)
    Copy-Item -FromSession $session -LiteralPath 'C:\MetaplasiaServicing\dism.log' `
        -Destination (Join-Path $ResultRoot 'windows-servicing-dism.log')
    if (-not $progress.finished -or $progress.exitCode -notin @(0, 3010)) {
        throw "Pinned update failed or exceeded 25 minutes: exit=$($progress.exitCode)."
    }
    $phase = 'restart-after-update'
    [ordered]@{ phase = $phase; checkedUtc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $progressPath -Encoding utf8
    $previousBoot = Invoke-Command -Session $session -ScriptBlock {
        (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    }
    Invoke-Command -Session $session -ScriptBlock {
        & "$env:SystemRoot\System32\shutdown.exe" /r /t 5 /f
        if ($LASTEXITCODE -ne 0) { throw 'Guest restart request failed.' }
    }
    Remove-PSSession $session
    $session = $null
    $deadline = [DateTime]::UtcNow.AddMinutes(12)
    $booted = $false
    do {
        Start-Sleep -Seconds 10
        try {
            $booted = Invoke-MetaplasiaPowerShellDirect -VMName $VMName -Credential $Credential `
                -ArgumentList $previousBoot -ScriptBlock {
                    param([string]$PreviousBoot)
                    $boot = (Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
                    $os = Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
                    $boot -cne $PreviousBoot -and $os.CurrentBuild -eq '26200' -and $os.UBR -eq 9457 -and
                        -not (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending')
                } -Timeout ([TimeSpan]::FromSeconds(20))
        } catch { $booted = $false }
    } while (-not $booted -and [DateTime]::UtcNow -lt $deadline)
    if (-not $booted) { throw 'The serviced VM did not reach 26200.9457 after its restart.' }
    $session = New-PSSession -VMName $VMName -Credential $Credential
    $phase = 'remove-servicing-staging'
    [ordered]@{ phase = $phase; checkedUtc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $progressPath -Encoding utf8
    # PowerShell Direct runs in session 0, without an interactive shell. Save
    # only the serviced OS here. The workload must compare mapped shell images
    # after logon, before it starts Metaplasia; this is not certification.
    Invoke-Command -Session $session -ScriptBlock {
        $root = 'C:\MetaplasiaServicing'
        if ((Resolve-Path -LiteralPath $root).Path -cne $root -or
            ((Get-Item -LiteralPath $root -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            @(Get-ChildItem -LiteralPath $root -Force -Recurse | Where-Object {
                ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
            }).Count -ne 0) { throw 'Unsafe guest staging path; refusing removal.' }
        Remove-Item -LiteralPath $root -Recurse -Force
        if (Test-Path -LiteralPath 'C:\MetaplasiaLab') { throw 'Unexpected test residue before checkpoint.' }
    }
    $phase = 'windows-serviced-only'
    [ordered]@{ phase = $phase; checkedUtc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json | Set-Content -LiteralPath $progressPath -Encoding utf8
} finally {
    if ($null -ne $session) {
        if ($phase -ne 'windows-serviced-only') {
            try {
                $cbsTail = Invoke-Command -Session $session -ScriptBlock {
                    Get-Content -LiteralPath 'C:\Windows\Logs\CBS\CBS.log' -Tail 600 |
                        ForEach-Object { [string]$_ }
                }
                $cbsTail | Set-Content -LiteralPath (
                    Join-Path $ResultRoot 'windows-servicing-cbs-tail.log') -Encoding utf8
            } catch { Write-Warning "Unable to retain guest CBS failure evidence: $_" }
        }
        Remove-PSSession $session -ErrorAction SilentlyContinue
    }
    if ($null -ne $copyService -and -not $copyServiceWasEnabled) {
        Disable-VMIntegrationService -VMIntegrationService $copyService
    }
}
