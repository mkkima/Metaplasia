[CmdletBinding()]
param(
    [string]$SourceLabRoot = 'C:\workspace\ISeeYou',
    [string]$PackageRoot,
    [string]$WindowsUpdatePath,
    [switch]$VisibleVM,
    [switch]$KeepFailedVM
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath (
        Join-Path $PSScriptRoot '..\..')).Path
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Hyper-V E2E orchestration requires one elevated host process.'
}
Import-Module Hyper-V -ErrorAction Stop

$mutex = [Threading.Mutex]::new(
    $false, 'Global\Metaplasia.ISeeYou-Lab.E2E')
$ownsMutex = $false
try {
    $ownsMutex = $mutex.WaitOne(0)
} catch [Threading.AbandonedMutexException] {
    $ownsMutex = $true
}
if (-not $ownsMutex) {
    $mutex.Dispose()
    throw 'Another Metaplasia E2E run already owns the shared VM adapter.'
}

$session = $null
$resultRoot = $null
$transcriptStarted = $false
$succeeded = $false
$failure = $null
$baselineRestored = $false
$baselineRecreated = $false
$cleanupVerified = $false
$guestSecretStaged = $false
$taskName = $null
$guestResultRoot = $null
$vm = $null
$vmName = $null
$checkpoint = $null
$credential = $null
$networkState = $null
$rdpProcess = $null
$rdpReadyPath = $null
$rdpStopPath = $null
$targetFingerprintPath = $null
$baselineOwned = $false
$guestPackageStaged = $false
$preparedCheckpoint = $null
$preparedStatePath = Join-Path $repositoryRoot 'out\e2e\prepared-baseline.json'
$preparedFingerprintPath = Join-Path $repositoryRoot 'out\e2e\prepared-target-fingerprint.json'

function Invoke-MetaplasiaPowerShellDirect {
    param(
        [Parameter(Mandatory)][string]$VMName,
        [Parameter(Mandatory)][PSCredential]$Credential,
        [Parameter(Mandatory)][scriptblock]$ScriptBlock,
        [object[]]$ArgumentList = @(),
        [TimeSpan]$Timeout = [TimeSpan]::FromSeconds(30)
    )
    $parameters = @{
        VMName = $VMName
        Credential = $Credential
        ScriptBlock = $ScriptBlock
        AsJob = $true
        ErrorAction = 'Stop'
    }
    if ($ArgumentList.Count -ne 0) {
        $parameters.ArgumentList = $ArgumentList
    }
    $job = Invoke-Command @parameters
    try {
        $completed = Wait-Job -Job $job `
            -Timeout ([int][Math]::Ceiling($Timeout.TotalSeconds))
        if ($null -eq $completed) {
            throw "PowerShell Direct exceeded $($Timeout.TotalSeconds) seconds."
        }
        if ($job.State -ne 'Completed') {
            $reason = $job.ChildJobs[0].JobStateInfo.Reason
            $detail = if ($null -eq $reason) {
                "state=$($job.State)"
            } else { $reason.Message }
            throw "PowerShell Direct failed: $detail"
        }
        return Receive-Job -Job $job -ErrorAction Stop
    } finally {
        if ($job.State -in @('NotStarted', 'Running')) {
            Stop-Job -Job $job -ErrorAction SilentlyContinue
        }
        Remove-Job -Job $job -Force -ErrorAction SilentlyContinue
    }
}

function Wait-MetaplasiaPowerShellDirect {
    param(
        [Parameter(Mandatory)][string]$VMName,
        [Parameter(Mandatory)][PSCredential]$Credential,
        [TimeSpan]$Timeout = [TimeSpan]::FromMinutes(12)
    )
    $deadline = [DateTime]::UtcNow + $Timeout
    do {
        try {
            $probe = Invoke-MetaplasiaPowerShellDirect -VMName $VMName `
                -Credential $Credential -ScriptBlock { $env:COMPUTERNAME } `
                -Timeout ([TimeSpan]::FromSeconds(20))
            if ($probe) { return }
        } catch {
            Start-Sleep -Seconds 4
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "PowerShell Direct did not become available for $VMName."
}

function Wait-MetaplasiaVmOff {
    param(
        [Parameter(Mandatory)][string]$VMName,
        [Parameter(Mandatory)][Guid]$ExpectedId,
        [TimeSpan]$Timeout = [TimeSpan]::FromSeconds(90)
    )
    $deadline = [DateTime]::UtcNow + $Timeout
    do {
        $current = Get-VM -Name $VMName -ErrorAction Stop
        if ($current.Id -ne $ExpectedId) {
            throw 'Refusing to observe a replacement VM with the same name.'
        }
        if ($current.State -eq 'Off') { return $true }
        Start-Sleep -Seconds 2
    } while ([DateTime]::UtcNow -lt $deadline)
    return $false
}

function Stop-MetaplasiaVmSafely {
    param(
        [Parameter(Mandatory)][string]$VMName,
        [Parameter(Mandatory)][Guid]$ExpectedId,
        [Parameter(Mandatory)][PSCredential]$Credential
    )
    $current = Get-VM -Name $VMName -ErrorAction Stop
    if ($current.Id -ne $ExpectedId) {
        throw 'Refusing to stop a replacement VM with the same name.'
    }
    if ($current.State -eq 'Off') { return }
    try {
        Invoke-MetaplasiaPowerShellDirect -VMName $VMName `
            -Credential $Credential -ScriptBlock { Stop-Computer -Force } `
            -Timeout ([TimeSpan]::FromSeconds(30))
    } catch {
        Write-Verbose "Guest shutdown command disconnected: $_"
    }
    if (Wait-MetaplasiaVmOff -VMName $VMName -ExpectedId $ExpectedId) {
        return
    }
    Stop-VM -Name $VMName -TurnOff -Force -Confirm:$false
    if (-not (Wait-MetaplasiaVmOff -VMName $VMName `
            -ExpectedId $ExpectedId -Timeout ([TimeSpan]::FromSeconds(30)))) {
        throw "VM $VMName did not reach the Off state."
    }
}

function Start-MetaplasiaVm {
    param(
        [Parameter(Mandatory)][string]$VMName,
        [Parameter(Mandatory)][Guid]$ExpectedId
    )
    try {
        Start-VM -Name $VMName -ErrorAction Stop | Out-Null
    } catch {
        $startFailure = $_
        if (($startFailure | Out-String) -notmatch '(?i)0x8007000E') {
            throw
        }
        $current = Get-VM -Name $VMName -ErrorAction Stop
        $memory = Get-VMMemory -VMName $VMName -ErrorAction Stop
        $allowedMaximum = $memory.Maximum -eq 4GB -or
            $memory.Maximum -eq 8GB
        if ($current.Id -ne $ExpectedId -or $current.State -ne 'Off' -or
            -not $memory.DynamicMemoryEnabled -or
            $memory.Startup -ne 2GB -or $memory.Minimum -ne 1GB -or
            -not $allowedMaximum -or $memory.Buffer -ne 20) {
            throw $startFailure
        }
        Set-VMMemory -VMName $VMName -DynamicMemoryEnabled $true `
            -StartupBytes 1536MB -MinimumBytes 1GB `
            -MaximumBytes $memory.Maximum `
            -Buffer 20
        Start-VM -Name $VMName -ErrorAction Stop | Out-Null
    }
}

function Get-MetaplasiaVmSnapshots {
    param([Parameter(Mandatory)][string]$VMName)

    $snapshotErrors = @()
    $snapshots = @(Get-VMSnapshot -VMName $VMName `
        -ErrorAction SilentlyContinue -ErrorVariable +snapshotErrors)
    $unexpected = @($snapshotErrors | Where-Object {
            $_.FullyQualifiedErrorId -notmatch '^ObjectNotFound,'
        })
    if ($unexpected.Count -ne 0) {
        throw $unexpected[0]
    }
    return $snapshots
}

function New-MetaplasiaValidatedBaselineCheckpoint {
    param(
        [Parameter(Mandatory)][Microsoft.HyperV.PowerShell.VirtualMachine]$VM,
        [Parameter(Mandatory)][string]$BaselineName,
        [Parameter(Mandatory)][PSCredential]$Credential
    )

    $vmName = $VM.Name
    $expectedId = $VM.Id
    if ($VM.State -ne 'Off') {
        throw 'A missing baseline can only be recovered from a powered-off VM.'
    }
    if (@(Get-MetaplasiaVmSnapshots -VMName $vmName).Count -ne 0) {
        throw 'Refusing to recover a baseline while any checkpoint exists.'
    }
    if (@(Get-VMNetworkAdapter -VMName $vmName |
            Where-Object SwitchName).Count -ne 0) {
        throw 'Refusing to recover a baseline from a connected VM.'
    }
    $memory = Get-VMMemory -VMName $vmName -ErrorAction Stop
    $expectedMaximum = $memory.Maximum
    $allowedMaximum = $expectedMaximum -eq 4GB -or
        $expectedMaximum -eq 8GB
    $normalMemory = $memory.DynamicMemoryEnabled -and
        $memory.Startup -eq 2GB -and $memory.Minimum -eq 1GB -and
        $allowedMaximum -and $memory.Buffer -eq 20
    $knownInterruptedFallback = $memory.DynamicMemoryEnabled -and
        $memory.Startup -eq 1536MB -and $memory.Minimum -eq 1GB -and
        $allowedMaximum -and $memory.Buffer -eq 20
    if (-not $normalMemory -and -not $knownInterruptedFallback) {
        throw ('The checkpoint-free VM has an unknown memory profile: ' +
            "dynamic=$($memory.DynamicMemoryEnabled), " +
            "startup=$($memory.Startup), minimum=$($memory.Minimum), " +
            "maximum=$($memory.Maximum), buffer=$($memory.Buffer).")
    }
    if ($knownInterruptedFallback) {
        Set-VMMemory -VMName $vmName -DynamicMemoryEnabled $true `
            -StartupBytes 2GB -MinimumBytes 1GB `
            -MaximumBytes $expectedMaximum `
            -Buffer 20
    }

    $started = $false
    try {
        Start-VM -Name $vmName -ErrorAction Stop | Out-Null
        $started = $true
        Wait-MetaplasiaPowerShellDirect -VMName $vmName `
            -Credential $Credential
        $evidence = Invoke-MetaplasiaPowerShellDirect -VMName $vmName `
            -Credential $Credential -ScriptBlock {
            Set-StrictMode -Version Latest
            $ErrorActionPreference = 'Stop'

            $computer = Get-CimInstance Win32_ComputerSystem
            if ($computer.Manufacturer -ne 'Microsoft Corporation' -or
                $computer.Model -notmatch 'Virtual Machine') {
                throw 'Baseline recovery is not running inside a Hyper-V VM.'
            }
            $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
            $principal = [Security.Principal.WindowsPrincipal]::new($identity)
            if (-not $principal.IsInRole(
                    [Security.Principal.WindowsBuiltInRole]::Administrator)) {
                throw 'Baseline recovery requires the provisioned guest administrator.'
            }

            $forbiddenPaths = @(
                'C:\MetaplasiaLab',
                'C:\ProgramData\Metaplasia',
                'C:\ProgramData\ISeeYou',
                (Join-Path $env:LOCALAPPDATA 'Metaplasia')
            )
            foreach ($path in $forbiddenPaths) {
                if (Test-Path -LiteralPath $path) {
                    throw "Checkpoint-free VM contains test residue: $path"
                }
            }
            $sourceLabStaging = 'C:\ISeeYouLab'
            $sourceLabItem = Get-Item -LiteralPath $sourceLabStaging `
                -Force -ErrorAction Stop
            if (-not $sourceLabItem.PSIsContainer -or
                ($sourceLabItem.Attributes -band
                    [IO.FileAttributes]::ReparsePoint) -ne 0 -or
                -not (Resolve-Path -LiteralPath $sourceLabStaging).Path.Equals(
                    $sourceLabStaging,
                    [StringComparison]::OrdinalIgnoreCase)) {
                throw 'The source lab staging root is not the provisioned directory.'
            }
            $sourceLabChildren = @(Get-ChildItem -LiteralPath `
                $sourceLabStaging -Force)
            if ($sourceLabChildren.Count -ne 0) {
                $names = ($sourceLabChildren | Select-Object -ExpandProperty Name) `
                    -join ', '
                throw "The source lab staging root is not empty: $names"
            }
            if (@(Get-CimInstance Win32_Process | Where-Object {
                        $_.Name -like 'metaplasia*' -or
                        $_.Name -like 'iseeyou*'
                    }).Count -ne 0) {
                throw 'Checkpoint-free VM contains a project test process.'
            }
            foreach ($serviceName in @('ISeeYouCollector', 'ISeeYouFs')) {
                if (Get-Service -Name $serviceName -ErrorAction SilentlyContinue) {
                    throw "Checkpoint-free VM contains service $serviceName."
                }
            }
            if (Get-ScheduledTask -TaskName 'Metaplasia-E2E-*' `
                    -ErrorAction SilentlyContinue) {
                throw 'Checkpoint-free VM contains a Metaplasia test task.'
            }
            if (Get-NetFirewallRule -Name 'Metaplasia-E2E-RDP' `
                    -ErrorAction SilentlyContinue) {
                throw 'Checkpoint-free VM contains the temporary RDP rule.'
            }
            if (@(Get-NetIPAddress -AddressFamily IPv4 `
                    -ErrorAction SilentlyContinue | Where-Object {
                        $_.IPAddress -match '^192\.168\.2[0-9]{2}\.2$'
                    }).Count -ne 0) {
                throw 'Checkpoint-free VM contains a temporary E2E address.'
            }

            $runKey = Get-Item -LiteralPath `
                'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' `
                -ErrorAction SilentlyContinue
            if ($null -ne $runKey -and
                $null -ne $runKey.GetValue(
                    'Metaplasia', $null, 'DoNotExpandEnvironmentNames')) {
                throw 'Checkpoint-free VM contains Metaplasia startup state.'
            }
            if (Test-Path -LiteralPath `
                    'HKCU:\Software\Metaplasia\PolicyOwnership') {
                throw 'Checkpoint-free VM contains Metaplasia policy ownership.'
            }

            $boot = & bcdedit.exe /enum all 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0 -or
                $boot -match '(?im)^testsigning\s+Yes\s*$') {
                throw 'Checkpoint-free VM has an unsafe test-signing state.'
            }
            $filters = & fltmc.exe filters 2>&1 | Out-String
            if ($LASTEXITCODE -ne 0 -or
                $filters -match '(?im)^\s*ISeeYouFs\s') {
                throw 'Checkpoint-free VM has an unsafe filter state.'
            }
            $verifierSettings = & verifier.exe /querysettings 2>&1 |
                Out-String
            $settingsExitCode = $LASTEXITCODE
            $verifierActive = & verifier.exe /query 2>&1 | Out-String
            $activeExitCode = $LASTEXITCODE
            if ($settingsExitCode -ne 0 -or $activeExitCode -ne 0 -or
                $verifierSettings -match '(?i)metaplasia|iseeyou_fs\.sys' -or
                $verifierActive -match '(?i)metaplasia|iseeyou_fs\.sys') {
                throw 'Checkpoint-free VM has an unsafe Driver Verifier state.'
            }

            [ordered]@{
                clean = $true
                computerName = $env:COMPUTERNAME
                userSid = $identity.User.Value
                projectPathsAbsent = $true
                projectProcessesAbsent = $true
                projectServicesAbsent = $true
                temporaryNetworkStateAbsent = $true
                testSigningDisabled = $true
                verifierProjectTargetsAbsent = $true
            }
        } -Timeout ([TimeSpan]::FromMinutes(2))
    } finally {
        if ($started) {
            Stop-MetaplasiaVmSafely -VMName $vmName `
                -ExpectedId $expectedId -Credential $Credential
        }
    }

    $current = Get-VM -Name $vmName -ErrorAction Stop
    $recoveredMemory = Get-VMMemory -VMName $vmName -ErrorAction Stop
    if ($current.Id -ne $expectedId -or $current.State -ne 'Off' -or
        @(Get-VMNetworkAdapter -VMName $vmName |
            Where-Object SwitchName).Count -ne 0 -or
        -not $recoveredMemory.DynamicMemoryEnabled -or
        $recoveredMemory.Startup -ne 2GB -or
        $recoveredMemory.Minimum -ne 1GB -or
        $recoveredMemory.Maximum -ne $expectedMaximum -or
        $recoveredMemory.Buffer -ne 20) {
        throw 'The validated VM identity or isolation changed during baseline recovery.'
    }
    Checkpoint-VM -Name $vmName -SnapshotName $BaselineName `
        -Confirm:$false -ErrorAction Stop
    $deadline = [DateTime]::UtcNow.AddSeconds(90)
    do {
        Start-Sleep -Milliseconds 500
        $created = @(Get-VMSnapshot -VMName $vmName `
            -ErrorAction SilentlyContinue)
    } while (($created.Count -ne 1 -or
            $created[0].Name -cne $BaselineName) -and
        [DateTime]::UtcNow -lt $deadline)
    if ($created.Count -ne 1 -or $created[0].Name -cne $BaselineName) {
        throw 'The validated clean baseline checkpoint was not created.'
    }
    return [pscustomobject]@{
        checkpoint = $created[0]
        evidence = $evidence
    }
}

function New-MetaplasiaIsolatedNetwork {
    param(
        [Parameter(Mandatory)][string]$VMName,
        [Parameter(Mandatory)][Guid]$ExpectedId
    )

    if (Get-VMSwitch -Name 'Metaplasia-E2E-*' `
            -ErrorAction SilentlyContinue) {
        throw 'A stale Metaplasia E2E virtual switch already exists.'
    }
    $thirdOctet = $null
    $hostAddresses = @(Get-NetIPAddress -AddressFamily IPv4 `
        -ErrorAction Stop | Select-Object -ExpandProperty IPAddress)
    $routes = @(Get-NetRoute -AddressFamily IPv4 -ErrorAction Stop |
        Select-Object -ExpandProperty DestinationPrefix)
    foreach ($candidate in 240..249) {
        $prefix = "192.168.$candidate."
        if (@($hostAddresses | Where-Object {
                    $_.StartsWith(
                        $prefix, [StringComparison]::OrdinalIgnoreCase)
                }).Count -eq 0 -and
            @($routes | Where-Object {
                    $_ -match "^192\.168\.$candidate\."
                }).Count -eq 0) {
            $thirdOctet = $candidate
            break
        }
    }
    if ($null -eq $thirdOctet) {
        throw 'No collision-free isolated Metaplasia /30 subnet is available.'
    }
    $switchName = 'Metaplasia-E2E-' + [Guid]::NewGuid().ToString('N')
    $switch = New-VMSwitch -Name $switchName -SwitchType Internal `
        -ErrorAction Stop
    try {
        $adapterName = "vEthernet ($switchName)"
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        do {
            $hostAdapter = Get-NetAdapter -Name $adapterName `
                -ErrorAction SilentlyContinue
            if ($null -ne $hostAdapter) { break }
            Start-Sleep -Milliseconds 250
        } while ([DateTime]::UtcNow -lt $deadline)
        if ($null -eq $hostAdapter) {
            throw 'The isolated Hyper-V host adapter was not created.'
        }
        $hostAddress = "192.168.$thirdOctet.1"
        $guestAddress = "192.168.$thirdOctet.2"
        New-NetIPAddress -InterfaceIndex $hostAdapter.ifIndex `
            -IPAddress $hostAddress -PrefixLength 30 `
            -AddressFamily IPv4 | Out-Null
        $current = Get-VM -Name $VMName -ErrorAction Stop
        if ($current.Id -ne $ExpectedId) {
            throw 'Refusing to connect a replacement VM to the E2E switch.'
        }
        $guestAdapters = @(Get-VMNetworkAdapter -VMName $VMName)
        if ($guestAdapters.Count -ne 1 -or $guestAdapters[0].SwitchName) {
            throw 'The lab VM network adapter is not in its disconnected baseline state.'
        }
        Connect-VMNetworkAdapter -VMNetworkAdapter $guestAdapters[0] `
            -SwitchName $switchName
        return [ordered]@{
            switchName = $switchName
            switchId = [string]$switch.Id
            hostAddress = $hostAddress
            guestAddress = $guestAddress
            hostInterfaceIndex = $hostAdapter.ifIndex
        }
    } catch {
        Remove-VMSwitch -Name $switchName -Force -Confirm:$false `
            -ErrorAction SilentlyContinue
        throw
    }
}

function Remove-MetaplasiaIsolatedNetwork {
    param(
        [Parameter(Mandatory)]$State,
        [Parameter(Mandatory)][string]$VMName,
        [Parameter(Mandatory)][Guid]$ExpectedId
    )

    $current = Get-VM -Name $VMName -ErrorAction Stop
    if ($current.Id -ne $ExpectedId) {
        throw 'Refusing to disconnect a replacement VM from the E2E switch.'
    }
    $switch = Get-VMSwitch -Name ([string]$State.switchName) `
        -ErrorAction Stop
    if ([string]$switch.Id -ine [string]$State.switchId -or
        $switch.SwitchType -ne 'Internal' -or
        $switch.Name -notmatch '^Metaplasia-E2E-[a-f0-9]{32}$') {
        throw 'The isolated Metaplasia virtual switch identity changed.'
    }
    foreach ($adapter in @(Get-VMNetworkAdapter -VMName $VMName |
            Where-Object SwitchName -eq $switch.Name)) {
        Disconnect-VMNetworkAdapter -VMNetworkAdapter $adapter
    }
    Remove-VMSwitch -VMSwitch $switch -Force -Confirm:$false
    if (Get-VMSwitch -Name $switch.Name -ErrorAction SilentlyContinue) {
        throw 'The isolated Metaplasia virtual switch was not removed.'
    }
}

function Start-MetaplasiaHiddenRdp {
    param(
        [Parameter(Mandatory)][string]$Server,
        [Parameter(Mandatory)][string]$UserName,
        [Parameter(Mandatory)][string]$CredentialPath,
        [Parameter(Mandatory)][string]$ReadyPath,
        [Parameter(Mandatory)][string]$StopPath
    )

    $windowsPowerShell = Join-Path $env:SystemRoot `
        'System32\WindowsPowerShell\v1.0\powershell.exe'
    $helper = Join-Path $PSScriptRoot 'Open-MetaplasiaE2ERdpSession.ps1'
    $arguments = @(
        '-NoProfile', '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $helper),
        '-Server', $Server,
        '-UserName', ('"{0}"' -f $UserName),
        '-CredentialPath', ('"{0}"' -f $CredentialPath),
        '-ReadyPath', ('"{0}"' -f $ReadyPath),
        '-StopPath', ('"{0}"' -f $StopPath)
    )
    if ($VisibleVM) { $arguments += '-VisibleVM' }
    $arguments = $arguments -join ' '
    $process = Start-Process -FilePath $windowsPowerShell `
        -ArgumentList $arguments -WindowStyle Hidden -PassThru
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(90)
        do {
            if (Test-Path -LiteralPath $ReadyPath) {
                $ready = Get-Content -LiteralPath $ReadyPath -Encoding utf8 -Raw |
                    ConvertFrom-Json
                if (-not $ready.success -or -not $ready.connected) {
                    throw "The hidden RDP helper failed: $($ready.error)"
                }
                return $process
            }
            if ($process.HasExited) {
                throw "The hidden RDP helper exited with $($process.ExitCode)."
            }
            Start-Sleep -Milliseconds 250
        } while ([DateTime]::UtcNow -lt $deadline)
        throw 'The hidden RDP helper did not report connection readiness.'
    } catch {
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
            $process.WaitForExit(5000)
        }
        throw
    }
}

function Stop-MetaplasiaHiddenRdp {
    param(
        [Diagnostics.Process]$Process,
        [string]$StopPath
    )
    if ($null -eq $Process) { return }
    if (-not $Process.HasExited) {
        [IO.File]::WriteAllText(
            $StopPath, [DateTime]::UtcNow.ToString('o'),
            [Text.UTF8Encoding]::new($false))
        if (-not $Process.WaitForExit(30000)) {
            Stop-Process -Id $Process.Id -Force -ErrorAction Stop
            $Process.WaitForExit(5000)
        }
    }
}

function Restart-MetaplasiaGuest {
    param(
        [Parameter(Mandatory)][string]$VMName,
        [Parameter(Mandatory)][Guid]$ExpectedId,
        [Parameter(Mandatory)][PSCredential]$Credential,
        [Parameter(Mandatory)]
        [Management.Automation.Runspaces.PSSession]$Session,
        [switch]$DeferPowerShellDirect,
        [TimeSpan]$Timeout = [TimeSpan]::FromMinutes(12)
    )
    $previousBoot = Invoke-Command -Session $Session -ScriptBlock {
        ([DateTime](Get-CimInstance Win32_OperatingSystem).LastBootUpTime).
            ToUniversalTime().Ticks
    }
    try {
        $marker = Invoke-Command -Session $Session -ScriptBlock {
            $process = Start-Process -FilePath `
                (Join-Path $env:SystemRoot 'System32\shutdown.exe') `
                -ArgumentList @('/r', '/t', '3', '/f', '/d', 'p:0:0') `
                -WindowStyle Hidden -Wait -PassThru
            if ($process.ExitCode -ne 0) {
                throw "shutdown.exe failed with $($process.ExitCode)."
            }
            'METAPLASIA_RESTART_SCHEDULED'
        }
        if ([string]$marker -ne 'METAPLASIA_RESTART_SCHEDULED') {
            throw 'The guest did not confirm its scheduled restart.'
        }
    } finally {
        Remove-PSSession $Session -ErrorAction SilentlyContinue
    }
    if ($DeferPowerShellDirect) {
        # A PowerShell Direct connection is recorded as Logon Type 2 and can
        # win the race against Winlogon. Keep the guest completely untouched
        # while its console auto-logon and AtLogOn task establish the desktop.
        $notBefore = [DateTime]::UtcNow.AddSeconds(60)
        do {
            Start-Sleep -Seconds 3
            $current = Get-VM -Name $VMName -ErrorAction Stop
            if ($current.Id -ne $ExpectedId) {
                throw 'Refusing to observe a replacement VM with the same name.'
            }
        } while ([DateTime]::UtcNow -lt $notBefore)
        if ($current.State -ne 'Running') {
            throw 'The guest is not running after the interactive reboot quiet period.'
        }
        return
    }
    $deadline = [DateTime]::UtcNow + $Timeout
    do {
        Start-Sleep -Seconds 3
        $current = Get-VM -Name $VMName -ErrorAction Stop
        if ($current.Id -ne $ExpectedId) {
            throw 'Refusing to observe a replacement VM with the same name.'
        }
        if ($current.State -ne 'Running') { continue }
        $probe = $null
        try {
            $probe = New-PSSession -VMName $VMName -Credential $Credential `
                -ErrorAction Stop
            $currentBoot = Invoke-Command -Session $probe -ScriptBlock {
                ([DateTime](Get-CimInstance Win32_OperatingSystem).
                    LastBootUpTime).ToUniversalTime().Ticks
            }
            if ([int64]$currentBoot -gt [int64]$previousBoot) { return }
        } catch {
            Write-Verbose "Waiting for guest restart: $_"
        } finally {
            if ($null -ne $probe) {
                Remove-PSSession $probe -ErrorAction SilentlyContinue
            }
        }
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "The guest $VMName did not complete a graceful restart."
}

function Test-MetaplasiaVhdChain {
    param(
        [Parameter(Mandatory)][string]$AttachedPath,
        [Parameter(Mandatory)][string]$ExpectedBasePath,
        [Parameter(Mandatory)][string]$AllowedRoot
    )
    $currentPath = [IO.Path]::GetFullPath($AttachedPath)
    $basePath = [IO.Path]::GetFullPath($ExpectedBasePath)
    $allowedPrefix = [IO.Path]::GetFullPath($AllowedRoot).TrimEnd('\') + '\'
    $visited = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($depth in 0..16) {
        if (-not $visited.Add($currentPath) -or
            -not $currentPath.StartsWith(
                $allowedPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            return $false
        }
        $item = Get-Item -LiteralPath $currentPath -Force
        if ($item.PSIsContainer -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            return $false
        }
        if ($currentPath.Equals($basePath,
                [StringComparison]::OrdinalIgnoreCase)) {
            return $true
        }
        $vhd = Get-VHD -Path $currentPath -ErrorAction Stop
        if ($vhd.VhdType -ne 'Differencing' -or
            [string]::IsNullOrWhiteSpace([string]$vhd.ParentPath)) {
            return $false
        }
        $currentPath = [IO.Path]::GetFullPath([string]$vhd.ParentPath)
    }
    return $false
}

function Assert-MetaplasiaHostPackage {
    param([Parameter(Mandatory)][string]$Root)

    $manifestPath = Join-Path $Root 'manifest.json'
    $manifestItem = Get-Item -LiteralPath $manifestPath -Force
    if ($manifestItem.PSIsContainer -or
        ($manifestItem.Attributes -band [IO.FileAttributes]::ReparsePoint) `
            -ne 0) {
        throw 'The Metaplasia E2E manifest is not a regular file.'
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.project -cne 'Metaplasia') {
        throw 'The host package manifest is invalid or belongs to another project.'
    }
    $required = @(
        'bin\metaplasia.exe',
        'bin\metaplasia-host.exe',
        'bin\metaplasia-watchdog.exe',
        'bin\metaplasia-cli.exe',
        'bin\metaplasia-agent.dll',
        'guest\MetaplasiaE2E.Common.ps1',
        'guest\Get-MetaplasiaCompatibilityFingerprint.ps1',
        'guest\Compare-MetaplasiaCompatibilityFingerprint.ps1',
        'guest\Install-MetaplasiaE2E.ps1',
        'guest\Run-MetaplasiaE2EWorkload.ps1',
        'guest\Invoke-MetaplasiaE2EWorkload.ps1',
        'guest\Uninstall-MetaplasiaE2E.ps1',
        'guest\Confirm-MetaplasiaE2EClean.ps1'
    )
    $manifestPaths = @($manifest.files | ForEach-Object { [string]$_.path })
    if ($manifestPaths.Count -ne $required.Count) {
        throw 'The Metaplasia E2E package has an unexpected file count.'
    }
    foreach ($path in $required) {
        if ($manifestPaths -cnotcontains $path) {
            throw "The Metaplasia E2E package is missing $path."
        }
    }
    $rootPrefix = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $manifest.files) {
        $relative = [string]$file.path
        if (-not $seen.Add($relative) -or
            [IO.Path]::GetExtension($relative) -ieq '.sys') {
            throw "The package contains a duplicate or driver entry: $relative"
        }
        $candidate = [IO.Path]::GetFullPath((Join-Path $Root $relative))
        if (-not $candidate.StartsWith(
                $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "The package entry escapes its root: $relative"
        }
        $item = Get-Item -LiteralPath $candidate -Force
        if ($item.PSIsContainer -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
            $item.Length -ne [int64]$file.length -or
            (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash `
                -cne [string]$file.sha256) {
            throw "The package entry failed integrity validation: $relative"
        }
    }
    $actual = @(Get-ChildItem -LiteralPath $Root -File -Recurse |
        Where-Object Name -ne 'manifest.json')
    if ($actual.Count -ne $manifest.files.Count) {
        throw 'Unmanifested files exist in the Metaplasia E2E package.'
    }
    return $manifest
}

try {
    $SourceLabRoot = (Resolve-Path -LiteralPath $SourceLabRoot).Path
    $sourceStatePath = Join-Path $SourceLabRoot 'out\lab\vm-state.json'
    $stateItem = Get-Item -LiteralPath $sourceStatePath -Force
    if ($stateItem.PSIsContainer -or
        ($stateItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'The source lab state is not a regular file.'
    }
    $state = Get-Content -LiteralPath $sourceStatePath -Raw | ConvertFrom-Json
    if ($state.schema -ne 1 -or
        [string]$state.vmName -notmatch '^ISeeYou-Lab(?:-[A-Za-z0-9_-]{1,32})?$' -or
        [string]$state.baselineCheckpoint -notmatch
            '^ISeeYou-[A-Za-z0-9_-]{1,64}$') {
        throw 'The source ISeeYou lab state is invalid.'
    }
    $vmName = [string]$state.vmName
    $baselineName = [string]$state.baselineCheckpoint
    $credentialPath = [IO.Path]::GetFullPath([string]$state.credentialPath)
    $expectedCredentialRoot = [IO.Path]::GetFullPath(
        (Join-Path $SourceLabRoot 'out\lab')).TrimEnd('\') + '\'
    if (-not $credentialPath.StartsWith(
            $expectedCredentialRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The source credential is outside the ISeeYou state boundary.'
    }
    $credential = Import-Clixml -LiteralPath $credentialPath
    if ($credential -isnot [PSCredential]) {
        throw 'The source lab credential is invalid.'
    }

    if (-not $PackageRoot) {
        $latest = Get-Content -LiteralPath (
            Join-Path $repositoryRoot 'out\e2e\latest-package.json') -Raw |
            ConvertFrom-Json
        if ($latest.schema -ne 1 -or $latest.project -cne 'Metaplasia') {
            throw 'The latest Metaplasia E2E package pointer is invalid.'
        }
        $PackageRoot = [string]$latest.packageRoot
    }
    $PackageRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
    $allowedPackageRoot = [IO.Path]::GetFullPath(
        (Join-Path $repositoryRoot 'out\e2e\packages')).TrimEnd('\') + '\'
    $packageItem = Get-Item -LiteralPath $PackageRoot -Force
    if (-not $PackageRoot.StartsWith(
            $allowedPackageRoot, [StringComparison]::OrdinalIgnoreCase) -or
        -not $packageItem.PSIsContainer -or
        ($packageItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'The package is outside the Metaplasia E2E package boundary.'
    }
    [void](Assert-MetaplasiaHostPackage -Root $PackageRoot)

    $resultId = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $resultRoot = Join-Path $repositoryRoot "out\e2e\results\$resultId"
    New-Item -ItemType Directory -Path $resultRoot -Force | Out-Null
    $resultItem = Get-Item -LiteralPath $resultRoot -Force
    if (-not $resultItem.PSIsContainer -or
        ($resultItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'The host E2E result root is not a regular directory.'
    }
    Start-Transcript -LiteralPath (Join-Path $resultRoot 'host-transcript.txt') |
        Out-Null
    $transcriptStarted = $true

    $targetFingerprintPath = Join-Path $resultRoot `
        'target-compatibility-fingerprint.json'
    & (Join-Path $PSScriptRoot `
        'guest\Get-MetaplasiaCompatibilityFingerprint.ps1') `
        -CliPath (Join-Path $PackageRoot 'bin\metaplasia-cli.exe') `
        -OutputPath $targetFingerprintPath | Out-Null

    $vm = Get-VM -Name $vmName -ErrorAction Stop
    if ($vm.Generation -ne 2 -or [string]$state.vmId -ne $vm.Id.Guid) {
        throw 'The VM does not match the read-only ISeeYou provisioning state.'
    }
    if ($vm.State -ne 'Off') {
        throw 'The shared lab VM is not Off; refusing to interrupt another run.'
    }
    $expectedVhd = [IO.Path]::GetFullPath([string]$state.vhdPath)
    $expectedConfiguration = [IO.Path]::GetFullPath(
        [string]$state.vmConfigurationPath)
    if (-not [IO.Path]::GetFullPath($vm.ConfigurationLocation).Equals(
            $expectedConfiguration, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The shared VM configuration path does not match its identity state.'
    }
    $expectedLabRoot = Split-Path -Parent (Split-Path -Parent $expectedVhd)
    $attachedVhds = @(Get-VMHardDiskDrive -VMName $vmName)
    if ($attachedVhds.Count -ne 1 -or
        -not (Test-MetaplasiaVhdChain -AttachedPath $attachedVhds[0].Path `
            -ExpectedBasePath $expectedVhd -AllowedRoot $expectedLabRoot)) {
        throw 'The shared VM VHD chain does not match the ISeeYou baseline.'
    }
    if (@(Get-VMNetworkAdapter -VMName $vmName |
            Where-Object SwitchName).Count -ne 0) {
        throw 'The shared VM must remain disconnected from virtual switches.'
    }
    $checkpoints = @(Get-MetaplasiaVmSnapshots -VMName $vmName)
    $originalCheckpoints = @($checkpoints | Where-Object Name -CEQ $baselineName)
    if (Test-Path -LiteralPath $preparedStatePath) {
        $prepared = Get-Content -LiteralPath $preparedStatePath -Raw | ConvertFrom-Json
        if ($prepared.schema -ne 2 -or $prepared.project -cne 'Metaplasia' -or
            $prepared.verification -cne 'windows-serviced-only' -or
            [string]$prepared.vmId -cne $vm.Id.ToString() -or
            $originalCheckpoints.Count -ne 1 -or
            [string]$prepared.originalCheckpointId -cne $originalCheckpoints[0].Id.ToString() -or
            [string]$prepared.checkpointName -notmatch '^Metaplasia-Windows-[0-9]+\.[0-9]+$' -or
            (Get-FileHash -LiteralPath $preparedFingerprintPath -Algorithm SHA256).Hash -cne
                [string]$prepared.targetFingerprintSha256) {
            throw 'The prepared Windows checkpoint does not match this VM and original baseline.'
        }
        & (Join-Path $PSScriptRoot 'guest\Compare-MetaplasiaCompatibilityFingerprint.ps1') `
            -ExpectedPath $preparedFingerprintPath -ActualPath $targetFingerprintPath `
            -PreparationTargetOnly | Out-Null
        $preparedMatches = @($checkpoints | Where-Object {
            $_.Id.ToString() -ceq [string]$prepared.checkpointId -and
            $_.Name -ceq [string]$prepared.checkpointName
        })
        if ($preparedMatches.Count -ne 1) { throw 'The prepared Windows checkpoint is missing.' }
        $preparedCheckpoint = $preparedMatches[0]
    }
    $expectedCount = if ($null -ne $preparedCheckpoint) { 2 } else { 1 }
    if ($checkpoints.Count -gt $expectedCount -or $originalCheckpoints.Count -gt 1 -or
        ($checkpoints.Count -ne 0 -and $originalCheckpoints.Count -ne 1)) {
        throw 'The shared VM has an ambiguous or foreign checkpoint set.'
    }
    if ($checkpoints.Count -eq 0) {
        $recovery = New-MetaplasiaValidatedBaselineCheckpoint `
            -VM $vm -BaselineName $baselineName -Credential $credential
        $checkpoint = $recovery.checkpoint
        $recovery.evidence | ConvertTo-Json -Depth 5 | Set-Content `
            -LiteralPath (Join-Path $resultRoot `
                'baseline-recovery-clean-state.json') -Encoding utf8
        $baselineRecreated = $true
    } else {
        $checkpoint = $originalCheckpoints[0]
    }

    $baselineOwned = $true
    $testCheckpoint = if ($null -ne $preparedCheckpoint) { $preparedCheckpoint } else { $checkpoint }
    Restore-VMSnapshot -VMSnapshot $testCheckpoint -Confirm:$false
    if ($WindowsUpdatePath -and $null -eq $preparedCheckpoint) {
        $freeMemory = [long](Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory * 1KB
        if ($freeMemory -lt 6GB) { throw 'Servicing requires 6 GiB of free host RAM.' }
        $storageDrive = [IO.Path]::GetPathRoot($expectedVhd).TrimEnd('\').TrimEnd(':')
        if ((Get-PSDrive -Name $storageDrive).Free -lt 30GB) {
            throw 'Servicing requires 30 GiB free on the existing VM storage drive.'
        }
        Set-VMMemory -VMName $vmName -DynamicMemoryEnabled $false -StartupBytes 4GB
    }
    Start-MetaplasiaVm -VMName $vmName -ExpectedId $vm.Id
    Wait-MetaplasiaPowerShellDirect -VMName $vmName -Credential $credential
    if ($WindowsUpdatePath -and $null -eq $preparedCheckpoint) {
        & (Join-Path $PSScriptRoot 'Initialize-MetaplasiaWindowsBaseline.ps1') `
            -VMName $vmName -ExpectedId $vm.Id -Credential $credential `
            -UpdatePath $WindowsUpdatePath `
            -TargetFingerprintPath $targetFingerprintPath -ResultRoot $resultRoot
        Stop-MetaplasiaVmSafely -VMName $vmName -ExpectedId $vm.Id -Credential $credential
        $preparedName = 'Metaplasia-Windows-26200.9457'
        Checkpoint-VM -Name $vmName -SnapshotName $preparedName -Confirm:$false
        $created = @(Get-VMSnapshot -VMName $vmName -Name $preparedName)
        if ($created.Count -ne 1) { throw 'The prepared checkpoint was not uniquely created.' }
        $preparedCheckpoint = $created[0]
        # This records the intended host target, NOT a verified guest match.
        # Every run still compares the guest's loaded modules after logon.
        Copy-Item -LiteralPath $targetFingerprintPath -Destination $preparedFingerprintPath
        $prepared = [ordered]@{
            schema = 2; project = 'Metaplasia'; vmId = $vm.Id.ToString()
            verification = 'windows-serviced-only'
            originalCheckpointId = $checkpoint.Id.ToString()
            checkpointId = $preparedCheckpoint.Id.ToString(); checkpointName = $preparedName
            targetFingerprintSha256 = (Get-FileHash -LiteralPath $preparedFingerprintPath -Algorithm SHA256).Hash
        }
        $pendingStatePath = $preparedStatePath + '.pending'
        $prepared | ConvertTo-Json | Set-Content -LiteralPath $pendingStatePath -Encoding utf8
        Move-Item -LiteralPath $pendingStatePath -Destination $preparedStatePath
        Start-MetaplasiaVm -VMName $vmName -ExpectedId $vm.Id
        Wait-MetaplasiaPowerShellDirect -VMName $vmName -Credential $credential
    }
    # Reject the wrong Windows revision before copying or installing anything.
    # The interactive workload also compares every relevant shell binary hash.
    $guestVersion = Invoke-MetaplasiaPowerShellDirect -VMName $vmName `
        -Credential $credential -ScriptBlock {
            $os = Get-ItemProperty -LiteralPath `
                'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
            "10.0.$($os.CurrentBuildNumber).$($os.UBR)"
        }
    $hostVersion = Get-ItemProperty -LiteralPath `
        'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
    $expectedVersion = "10.0.$($hostVersion.CurrentBuildNumber).$($hostVersion.UBR)"
    [ordered]@{
        expected = $expectedVersion
        actual = [string]$guestVersion
        matches = [string]$guestVersion -ceq $expectedVersion
    } | ConvertTo-Json | Set-Content -LiteralPath (
        Join-Path $resultRoot 'windows-version-preflight.json') -Encoding utf8
    if ([string]$guestVersion -cne $expectedVersion) {
        throw "Windows version mismatch: guest=$guestVersion, host=$expectedVersion. No installation or injection was attempted."
    }
    $session = New-PSSession -VMName $vmName -Credential $credential
    $networkState = New-MetaplasiaIsolatedNetwork -VMName $vmName `
        -ExpectedId $vm.Id

    Invoke-Command -Session $session -ScriptBlock {
        if (Test-Path -LiteralPath 'C:\MetaplasiaLab') {
            throw 'The clean baseline already contains Metaplasia guest state.'
        }
        New-Item -ItemType Directory -Path 'C:\MetaplasiaLab\package' `
            -Force | Out-Null
    }
    Copy-Item -ToSession $session -Path (Join-Path $PackageRoot '*') `
        -Destination 'C:\MetaplasiaLab\package' -Recurse -Force
    $guestPackageStaged = $true

    $install = Invoke-Command -Session $session `
        -ArgumentList ([string]$networkState.guestAddress) -ScriptBlock {
        param([string]$GuestAddress)
        Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
        & 'C:\MetaplasiaLab\package\guest\Install-MetaplasiaE2E.ps1' `
            -PackageRoot 'C:\MetaplasiaLab\package' `
            -IsolatedRdpAddress $GuestAddress
    }
    $install | Set-Content -LiteralPath (
        Join-Path $resultRoot 'guest-install.json') -Encoding utf8

    $guestUser = $credential.UserName
    $guestResultRoot = "C:\MetaplasiaLab\results\$resultId"
    Invoke-Command -Session $session -ArgumentList $guestResultRoot `
        -ScriptBlock {
            param([string]$ResultRoot)
            New-Item -ItemType Directory -Path $ResultRoot -Force | Out-Null
        }
    $guestExpectedCompatibilityPath = Join-Path $guestResultRoot `
        'expected-compatibility-fingerprint.json'
    Copy-Item -ToSession $session -LiteralPath $targetFingerprintPath `
        -Destination $guestExpectedCompatibilityPath -Force
    $securePassword = $credential.Password
    $interactivePreflight = Invoke-Command -Session $session `
        -ArgumentList $guestUser, $securePassword `
        -ScriptBlock {
            param([string]$UserName, [SecureString]$Password)
            $account = $UserName
            if ($UserName.Contains('\')) {
                $parts = $UserName.Split('\', 2)
                $account = $parts[1]
            }
            $localUser = Get-LocalUser -Name $account -ErrorAction Stop
            if (-not $localUser.Enabled) {
                throw "The E2E account is disabled: $account"
            }
            # PowerShell Direct accepts the localhost alias, but Winlogon needs
            # the actual computer name for a local interactive logon.
            $domain = $env:COMPUTERNAME
            $plain = [PSCredential]::new('credential-probe', $Password).
                GetNetworkCredential().Password
            try {
                Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class MetaplasiaLogonProbe {
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode,
        SetLastError = true)]
    private static extern bool LogonUser(
        string userName, string domain, string password,
        int logonType, int provider, out IntPtr token);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    public static int Validate(string userName, string domain, string password) {
        IntPtr token;
        if (!LogonUser(userName, domain, password, 2, 0, out token)) {
            return Marshal.GetLastWin32Error();
        }
        CloseHandle(token);
        return 0;
    }
}
'@
                $validationError = [MetaplasiaLogonProbe]::Validate(
                    $account, $domain, $plain)
                if ($validationError -ne 0) {
                    throw "Interactive credential validation failed with Win32 $validationError."
                }
                $path = 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
                $currentWinlogon = Get-ItemProperty -LiteralPath $path
                $shell = [string]$currentWinlogon.Shell
                $userinit = [string]$currentWinlogon.Userinit
                if ($shell -ine 'explorer.exe' -or
                    $userinit -notmatch '(?i)\\userinit\.exe,?$') {
                    throw "The guest Winlogon shell contract is unsafe: Shell=$shell; Userinit=$userinit"
                }
            } finally {
                $plain = $null
            }
            [ordered]@{
                taskUser = "$domain\$account"
                account = $account
                domain = $domain
                shell = $shell
                userinit = $userinit
                userSid = $localUser.SID.Value
                credentialValidated = $true
            }
        }
    $interactivePreflight | ConvertTo-Json -Depth 3 | Set-Content `
        -LiteralPath (Join-Path $resultRoot `
            'guest-interactive-preflight.json') -Encoding utf8
    $guestUser = [string]$interactivePreflight.taskUser
    $taskName = 'Metaplasia-E2E-' + [Guid]::NewGuid().ToString('N')
    $expectedUserSid = [string]$interactivePreflight.userSid
    Invoke-Command -Session $session -ArgumentList @(
        $taskName, $guestUser, $expectedUserSid, $guestResultRoot,
        $guestExpectedCompatibilityPath) -ScriptBlock {
        param(
            [string]$TaskName,
            [string]$UserName,
            [string]$ExpectedUserSid,
            [string]$ResultRoot,
            [string]$ExpectedCompatibilityPath
        )
        $installRoot = 'C:\MetaplasiaLab\install'
        $installAcl = Get-Acl -LiteralPath $installRoot
        $installRule = [Security.AccessControl.FileSystemAccessRule]::new(
            $UserName,
            [Security.AccessControl.FileSystemRights]::FullControl,
            ([Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
                [Security.AccessControl.InheritanceFlags]::ObjectInherit),
            [Security.AccessControl.PropagationFlags]::None,
            [Security.AccessControl.AccessControlType]::Allow)
        $installAcl.SetAccessRule($installRule)
        Set-Acl -LiteralPath $installRoot -AclObject $installAcl

        $acl = Get-Acl -LiteralPath $ResultRoot
        $rule = [Security.AccessControl.FileSystemAccessRule]::new(
            $UserName,
            [Security.AccessControl.FileSystemRights]::Modify,
            ([Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
                [Security.AccessControl.InheritanceFlags]::ObjectInherit),
            [Security.AccessControl.PropagationFlags]::None,
            [Security.AccessControl.AccessControlType]::Allow)
        $acl.SetAccessRule($rule)
        Set-Acl -LiteralPath $ResultRoot -AclObject $acl

        $powerShell = Join-Path $env:SystemRoot `
            'System32\WindowsPowerShell\v1.0\powershell.exe'
        $arguments = @(
            '-NoProfile', '-MTA', '-ExecutionPolicy', 'Bypass',
            '-File', '"C:\MetaplasiaLab\package\guest\Run-MetaplasiaE2EWorkload.ps1"',
            '-ResultRoot', ('"{0}"' -f $ResultRoot),
            '-ExpectedUserSid', ('"{0}"' -f $ExpectedUserSid),
            '-ExpectedCompatibilityPath',
            ('"{0}"' -f $ExpectedCompatibilityPath)
        ) -join ' '
        $action = New-ScheduledTaskAction -Execute $powerShell `
            -Argument $arguments
        $principal = New-ScheduledTaskPrincipal -UserId $UserName `
            -LogonType Interactive -RunLevel Limited
        $settings = New-ScheduledTaskSettingsSet `
            -ExecutionTimeLimit ([TimeSpan]::FromMinutes(10)) `
            -MultipleInstances IgnoreNew -StartWhenAvailable
        Register-ScheduledTask -TaskName $TaskName -Action $action `
            -Principal $principal -Settings $settings `
            -Force | Out-Null
        $task = Get-ScheduledTask -TaskName $TaskName
        $logonType = [string]$task.Principal.LogonType
        $taskSid = ([Security.Principal.NTAccount]::new(
                [string]$task.Principal.UserId)).Translate(
            [Security.Principal.SecurityIdentifier]).Value
        if ($logonType -notin @('Interactive', 'InteractiveToken') -or
            $taskSid -ine $ExpectedUserSid) {
            throw "The workload task has an unsafe principal: $($task.Principal.UserId)/$logonType"
        }
    }
    $restartSession = $session
    $session = $null
    Restart-MetaplasiaGuest -VMName $vmName -ExpectedId $vm.Id `
        -Credential $credential -Session $restartSession
    Wait-MetaplasiaPowerShellDirect -VMName $vmName -Credential $credential
    $session = New-PSSession -VMName $vmName -Credential $credential
    $guestStartedPath = Join-Path $guestResultRoot 'workload-started.json'
    $guestStatusPath = Join-Path $guestResultRoot 'workload-status.json'
    $rdpReadyPath = Join-Path $resultRoot 'rdp-session-oobe.json'
    $rdpStopPath = Join-Path $resultRoot 'rdp-session-oobe.stop'
    $rdpProcess = Start-MetaplasiaHiddenRdp `
        -Server ([string]$networkState.guestAddress) `
        -UserName $guestUser -CredentialPath $credentialPath `
        -ReadyPath $rdpReadyPath -StopPath $rdpStopPath
    $taskStartScript = {
            param([string]$TaskName, [string]$StartedPath)
            $deadline = [DateTime]::UtcNow.AddSeconds(20)
            $attempts = 0
            $attemptErrors = [Collections.Generic.List[string]]::new()
            $existing = Get-ScheduledTask -TaskName $TaskName
            if ([string]$existing.State -eq 'Queued') {
                try {
                    Stop-ScheduledTask -TaskName $TaskName
                    Start-Sleep -Milliseconds 250
                } catch {
                    $attemptErrors.Add($_.Exception.Message)
                }
            }
            do {
                ++$attempts
                try {
                    Start-ScheduledTask -TaskName $TaskName
                } catch {
                    $attemptErrors.Add($_.Exception.Message)
                }
                Start-Sleep -Milliseconds 200
                $task = Get-ScheduledTask -TaskName $TaskName
                $info = Get-ScheduledTaskInfo -TaskName $TaskName
                if ((Test-Path -LiteralPath $StartedPath) -or
                    [string]$task.State -eq 'Running') {
                    break
                }
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $deadline)
            [ordered]@{
                attempts = $attempts
                attemptErrors = @($attemptErrors | Select-Object -Unique)
                startedMarker = Test-Path -LiteralPath $StartedPath
                state = [string]$task.State
                lastRunTime = $info.LastRunTime
                lastTaskResult = $info.LastTaskResult
            }
        }
    $collectSessionEvents = {
        $since = (Get-Date).AddMinutes(-5)
        $logs = @(
            'Microsoft-Windows-TerminalServices-LocalSessionManager/Operational',
            'Microsoft-Windows-TerminalServices-RemoteConnectionManager/Operational',
            'Microsoft-Windows-RemoteDesktopServices-RdpCoreTS/Operational',
            'Microsoft-Windows-User Profile Service/Operational',
            'Microsoft-Windows-Shell-Core/Operational',
            'System',
            'Application'
        )
        foreach ($log in $logs) {
            $events = @(Get-WinEvent -FilterHashtable @{
                LogName = $log
                StartTime = $since
            } -ErrorAction SilentlyContinue | Select-Object -First 50)
            if ($log -eq 'Microsoft-Windows-Shell-Core/Operational') {
                # First-logon AppResolver activity can produce more than fifty
                # newer records and otherwise evict the OOBE evidence used to
                # distinguish the expected one-time disconnect from an RDP
                # failure. Query the bounded CloudExperienceHost IDs directly.
                $events += @(Get-WinEvent -FilterHashtable @{
                    LogName = $log
                    StartTime = $since
                    Id = @(62401, 62403, 62404, 62405)
                } -ErrorAction SilentlyContinue | Select-Object -First 100)
            }
            $events | Sort-Object RecordId -Unique | ForEach-Object {
                [ordered]@{
                    logName = $log
                    timeCreated = $_.TimeCreated
                    recordId = $_.RecordId
                    id = $_.Id
                    level = $_.LevelDisplayName
                    message = $_.Message
                }
            }
        }
    }
    $taskStart = Invoke-Command -Session $session -ArgumentList @(
        $taskName, $guestStartedPath) -ScriptBlock $taskStartScript
    $taskStart | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (
        Join-Path $resultRoot 'guest-workload-task-start-oobe.json') `
        -Encoding utf8
    # Connection readiness precedes first-logon OOBE. Observe either actual
    # workload startup or disconnect throughout the bounded transition; a
    # single ten-second exit check can miss a later OOBE logoff entirely.
    $firstLogonDeadline = [DateTime]::UtcNow.AddSeconds(90)
    $rdpProcess.Refresh()
    while (-not $taskStart.startedMarker -and -not $rdpProcess.HasExited -and
        [DateTime]::UtcNow -lt $firstLogonDeadline) {
        $taskStart.startedMarker = Invoke-Command -Session $session `
            -ArgumentList $guestStartedPath -ScriptBlock {
                param([string]$StartedPath)
                Test-Path -LiteralPath $StartedPath
            }
        if (-not $taskStart.startedMarker) {
            [void]$rdpProcess.WaitForExit(1000)
            $rdpProcess.Refresh()
        }
    }
    if (-not $taskStart.startedMarker -and -not $rdpProcess.HasExited) {
        throw 'First logon neither started the workload nor completed OOBE within 90 seconds.'
    }
    if (-not $taskStart.startedMarker -and $rdpProcess.HasExited) {
        # RDP disconnect notification can precede the guest's logoff event.
        # Wait for bounded evidence, not a blind reconnect or an immediate fail.
        $eventDeadline = [DateTime]::UtcNow.AddSeconds(20)
        do {
            $sessionEvents = Invoke-Command -Session $session `
                -ScriptBlock $collectSessionEvents
            $logoffObserved = @($sessionEvents | Where-Object {
                    $_.logName -eq 'Microsoft-Windows-TerminalServices-LocalSessionManager/Operational' -and
                    $_.id -eq 23
                }).Count -ne 0
            if ($logoffObserved) { break }
            Start-Sleep -Seconds 2
        } while ([DateTime]::UtcNow -lt $eventDeadline)
        $sessionEvents | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (
            Join-Path $resultRoot 'guest-oobe-session-events.json') `
            -Encoding utf8
        $rdpState = Get-Content -LiteralPath $rdpReadyPath -Encoding utf8 -Raw |
            ConvertFrom-Json
        $oobeObserved = @($sessionEvents | Where-Object {
                $_.logName -eq 'Microsoft-Windows-Shell-Core/Operational' -and
                $_.message -match "CXID: '(?:Oobe|Reboot)[^']*'"
            }).Count -ne 0
        $logoffObserved = @($sessionEvents | Where-Object {
                $_.logName -eq
                    'Microsoft-Windows-TerminalServices-LocalSessionManager/Operational' -and
                $_.id -eq 23
            }).Count -ne 0
        if (-not $oobeObserved -or -not $logoffObserved -or
            [int]$rdpState.extendedDisconnectReason -ne 12) {
            throw ('The first hidden RDP session ended without the expected ' +
                "one-time OOBE contract; extendedReason=$($rdpState.extendedDisconnectReason); " +
                "description=$($rdpState.disconnectDescription)")
        }
        $rdpProcess = $null
        $rdpReadyPath = Join-Path $resultRoot 'rdp-session.json'
        $rdpStopPath = Join-Path $resultRoot 'rdp-session.stop'
        $rdpProcess = Start-MetaplasiaHiddenRdp `
            -Server ([string]$networkState.guestAddress) `
            -UserName $guestUser -CredentialPath $credentialPath `
            -ReadyPath $rdpReadyPath -StopPath $rdpStopPath
        $taskStart = Invoke-Command -Session $session -ArgumentList @(
            $taskName, $guestStartedPath) -ScriptBlock $taskStartScript
        $taskStart | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (
            Join-Path $resultRoot 'guest-workload-task-start.json') `
            -Encoding utf8
        $rdpProcess.Refresh()
        if (-not $taskStart.startedMarker -and $rdpProcess.HasExited) {
            $secondEvents = Invoke-Command -Session $session `
                -ScriptBlock $collectSessionEvents
            $secondEvents | ConvertTo-Json -Depth 5 | Set-Content `
                -LiteralPath (Join-Path $resultRoot `
                    'guest-interactive-session-events.json') -Encoding utf8
            $secondRdpState = Get-Content -LiteralPath $rdpReadyPath -Encoding utf8 -Raw |
                ConvertFrom-Json
            throw ('The post-OOBE RDP session ended before the limited ' +
                "workload started; extendedReason=$($secondRdpState.extendedDisconnectReason); " +
                "description=$($secondRdpState.disconnectDescription); " +
                "taskErrors=$($taskStart.attemptErrors -join ' | ')")
        }
    }

    $workloadStarted = $false
    $workloadStatus = $null
    $interactive = Invoke-Command -Session $session -ArgumentList @(
        $guestResultRoot, $guestStartedPath, $guestStatusPath, $taskName) `
        -ScriptBlock {
        param(
            [string]$ResultRoot,
            [string]$StartedPath,
            [string]$StatusPath,
            [string]$TaskName
        )
        $deadline = [DateTime]::UtcNow.AddMinutes(3)
        $explorer = @()
        do {
            $explorer = @(Get-Process explorer -ErrorAction SilentlyContinue |
                Where-Object SessionId -gt 0)
            if (Test-Path -LiteralPath $StartedPath) {
                return [ordered]@{
                    started = $true
                    completed = Test-Path -LiteralPath $StatusPath
                    explorerCount = $explorer.Count
                }
            }
            Start-Sleep -Seconds 2
        } while ([DateTime]::UtcNow -lt $deadline)
        $winlogonPath =
            'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon'
        $winlogon = Get-ItemProperty -LiteralPath $winlogonPath
        $systemEvents = @(Get-WinEvent -FilterHashtable @{
                LogName = 'System'
                StartTime = (Get-Date).AddMinutes(-10)
            } -ErrorAction SilentlyContinue | Where-Object {
                $_.ProviderName -in @(
                    'Microsoft-Windows-Winlogon',
                    'Microsoft-Windows-User Profiles Service',
                    'USER32')
            } | Select-Object -First 30 TimeCreated, ProviderName, Id,
                LevelDisplayName, Message)
        $winlogonEvents = @(Get-WinEvent -LogName `
                'Microsoft-Windows-Winlogon/Operational' `
                -ErrorAction SilentlyContinue | Where-Object {
                    $_.TimeCreated -ge (Get-Date).AddMinutes(-10)
                } | Select-Object -First 50 TimeCreated, Id,
                    LevelDisplayName, Message)
        $securityEvents = @(Get-WinEvent -FilterHashtable @{
                LogName = 'Security'
                StartTime = (Get-Date).AddMinutes(-10)
                Id = @(4624, 4625, 4634, 4647)
            } -ErrorAction SilentlyContinue | Select-Object -First 100 |
            ForEach-Object {
                $event = $_
                $values = @{}
                ([xml]$event.ToXml()).Event.EventData.Data |
                    ForEach-Object {
                        if ($_.Name) {
                            $values[[string]$_.Name] = [string]$_.'#text'
                        }
                    }
                [ordered]@{
                    timeCreated = $event.TimeCreated
                    id = $event.Id
                    targetUserName = $values.TargetUserName
                    targetDomainName = $values.TargetDomainName
                    logonType = $values.LogonType
                    targetLogonId = $values.TargetLogonId
                    status = $values.Status
                    subStatus = $values.SubStatus
                    processName = $values.ProcessName
                    authenticationPackage = $values.AuthenticationPackageName
                }
            })
        $previousPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            $queryUser = (& query.exe user 2>&1 | Out-String).Trim()
            $queryUserExitCode = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
        }
        $task = Get-ScheduledTask -TaskName $TaskName `
            -ErrorAction SilentlyContinue
        $taskInfo = Get-ScheduledTaskInfo -TaskName $TaskName `
            -ErrorAction SilentlyContinue
        $processLogPath = Join-Path $ResultRoot 'workload-process.log'
        $processLog = if (Test-Path -LiteralPath $processLogPath) {
            (Get-Content -LiteralPath $processLogPath -Raw).Trim()
        } else { $null }
        $diagnostic = [ordered]@{
            capturedUtc = [DateTime]::UtcNow.ToString('o')
            explorerProcesses = @($explorer | Select-Object Id, SessionId,
                StartTime, Path)
            queryUser = $queryUser
            queryUserExitCode = $queryUserExitCode
            winlogon = [ordered]@{
                autoAdminLogon = [string]$winlogon.AutoAdminLogon
                defaultUserName = [string]$winlogon.DefaultUserName
                defaultDomainName = [string]$winlogon.DefaultDomainName
                autoLogonCount = $winlogon.AutoLogonCount
                forceAutoLogon = [string]$winlogon.ForceAutoLogon
                shell = [string]$winlogon.Shell
                userinit = [string]$winlogon.Userinit
                defaultPasswordPresent = -not [string]::IsNullOrEmpty(
                    [string]$winlogon.DefaultPassword)
            }
            task = if ($null -eq $task) { $null } else {
                [ordered]@{
                    state = [string]$task.State
                    logonType = [string]$task.Principal.LogonType
                    userId = [string]$task.Principal.UserId
                    lastRunTime = $taskInfo.LastRunTime
                    lastTaskResult = $taskInfo.LastTaskResult
                }
            }
            workloadProcessLog = $processLog
            systemEvents = $systemEvents
            winlogonEvents = $winlogonEvents
            securityEvents = $securityEvents
        }
        $diagnostic | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (
            Join-Path $ResultRoot 'interactive-session-diagnostics.json') `
            -Encoding utf8
        $taskDetail = if ([string]::IsNullOrWhiteSpace($processLog)) {
            "task state=$($task.State); result=$($taskInfo.LastTaskResult); workload log is missing"
        } else {
            "workload log=$processLog"
        }
        throw ('The isolated RDP session did not start the interactive workload; ' +
            "explorerCount=$($explorer.Count); queryUser=$queryUser; " +
            $taskDetail)
    }
    $deadline = [DateTime]::UtcNow.AddMinutes(10)
    do {
        Start-Sleep -Seconds 2
        $workloadStatus = Invoke-Command -Session $session `
            -ArgumentList $guestStatusPath -ScriptBlock {
                param([string]$Path)
                if (Test-Path -LiteralPath $Path) {
                    Get-Content -LiteralPath $Path -Raw
                }
            }
        if ($workloadStatus) { break }
    } while ([DateTime]::UtcNow -lt $deadline)
    if (-not $workloadStatus) {
        throw 'The interactive Metaplasia workload timed out.'
    }
    Copy-Item -FromSession $session -LiteralPath $guestResultRoot `
        -Destination $resultRoot -Recurse -Force
    $parsedWorkloadStatus = [string]$workloadStatus | ConvertFrom-Json
    if (-not $parsedWorkloadStatus.success) {
        throw "Interactive workload failed in phase $($parsedWorkloadStatus.phase): $($parsedWorkloadStatus.error)"
    }
    $succeeded = $true
} catch {
    $failure = $_
} finally {
    if ($null -ne $rdpProcess) {
        try {
            Stop-MetaplasiaHiddenRdp -Process $rdpProcess `
                -StopPath $rdpStopPath
        } catch {
            Write-Warning "Unable to stop the hidden RDP session cleanly: $_"
            if ($null -eq $failure) { $failure = $_ }
        } finally {
            $rdpProcess = $null
        }
    }
    if ($baselineOwned -and $guestPackageStaged -and
        $null -eq $session -and $null -ne $vm -and
        (Get-VM -Name $vmName -ErrorAction SilentlyContinue).State -eq 'Running') {
        try {
            Wait-MetaplasiaPowerShellDirect -VMName $vmName `
                -Credential $credential -Timeout ([TimeSpan]::FromMinutes(3))
            $session = New-PSSession -VMName $vmName -Credential $credential
        } catch {
            Write-Warning "Unable to reconnect for cleanup: $_"
        }
    }
    if ($guestPackageStaged -and $null -ne $session) {
        try {
            $guestResults = Invoke-Command -Session $session -ScriptBlock {
                if (Test-Path -LiteralPath 'C:\MetaplasiaLab\results') {
                    Get-ChildItem -LiteralPath 'C:\MetaplasiaLab\results' `
                        -Directory | Select-Object -ExpandProperty FullName
                }
            }
            foreach ($guestResult in @($guestResults)) {
                $destination = Join-Path $resultRoot 'failure-artifacts'
                New-Item -ItemType Directory -Path $destination -Force |
                    Out-Null
                Copy-Item -FromSession $session -LiteralPath $guestResult `
                    -Destination $destination -Recurse -Force `
                    -ErrorAction SilentlyContinue
            }
            $uninstall = Invoke-Command -Session $session `
                -ArgumentList $taskName -ScriptBlock {
                    param([string]$TaskName)
                    Set-ExecutionPolicy -Scope Process `
                        -ExecutionPolicy Bypass -Force
                    & 'C:\MetaplasiaLab\package\guest\Uninstall-MetaplasiaE2E.ps1' `
                        -TaskName $TaskName
                }
            $uninstall | Set-Content -LiteralPath (
                Join-Path $resultRoot 'guest-uninstall.json') -Encoding utf8
            $restartSession = $session
            $session = $null
            Restart-MetaplasiaGuest -VMName $vmName -ExpectedId $vm.Id `
                -Credential $credential -Session $restartSession
            Wait-MetaplasiaPowerShellDirect -VMName $vmName `
                -Credential $credential
            $session = New-PSSession -VMName $vmName -Credential $credential
            $clean = Invoke-Command -Session $session -ScriptBlock {
                Set-ExecutionPolicy -Scope Process `
                    -ExecutionPolicy Bypass -Force
                & 'C:\MetaplasiaLab\package\guest\Confirm-MetaplasiaE2EClean.ps1'
            }
            $clean | Set-Content -LiteralPath (
                Join-Path $resultRoot 'guest-clean-state.json') -Encoding utf8
            $cleanupVerified = $true
            $guestSecretStaged = $false
        } catch {
            Write-Warning "Guest uninstall or clean-state verification failed: $_"
            if ($null -eq $failure) { $failure = $_ }
        }
    }
    if ($null -ne $session) {
        Remove-PSSession $session -ErrorAction SilentlyContinue
        $session = $null
    }
    if ($null -ne $networkState -and $null -ne $vm) {
        try {
            Remove-MetaplasiaIsolatedNetwork -State $networkState `
                -VMName $vmName -ExpectedId $vm.Id
            $networkState = $null
        } catch {
            Write-Warning "Unable to remove the isolated E2E network: $_"
            if ($null -eq $failure) { $failure = $_ }
        }
    }
    if (($succeeded -or -not $KeepFailedVM -or
            ($guestSecretStaged -and -not $cleanupVerified) -or
            $null -ne $networkState) -and
        $baselineOwned -and $null -ne $vm -and $null -ne $checkpoint) {
        try {
            Stop-MetaplasiaVmSafely -VMName $vmName -ExpectedId $vm.Id `
                -Credential $credential
            Restore-VMSnapshot -VMSnapshot $checkpoint -Confirm:$false
            if ($null -ne $networkState) {
                Remove-MetaplasiaIsolatedNetwork -State $networkState `
                    -VMName $vmName -ExpectedId $vm.Id
                $networkState = $null
            }
            $baselineRestored = $true
        } catch {
            Write-Warning "Automatic baseline rollback failed: $_"
            if ($null -eq $failure) { $failure = $_ }
        }
    }
    if ($null -ne $resultRoot) {
        $status = [ordered]@{
            success = $succeeded -and $null -eq $failure
            project = 'Metaplasia'
            sourceLab = $SourceLabRoot
            vmName = $vmName
            packageRoot = $PackageRoot
            resultRoot = $resultRoot
            cleanupVerified = $cleanupVerified
            baselineRecreated = $baselineRecreated
            baselineRestored = $baselineRestored
            preparedCheckpoint = if ($null -eq $preparedCheckpoint) { $null } else { $preparedCheckpoint.Name }
            isolatedNetworkRemoved = $null -eq $networkState
            error = if ($null -eq $failure) { $null } else {
                $failure.ToString()
            }
        }
        $status | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (
            Join-Path $resultRoot 'host-status.json') -Encoding utf8
    }
    if ($transcriptStarted) {
        Stop-Transcript | Out-Null
    }
    if ($ownsMutex) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}

if ($null -ne $failure) { throw $failure }
Write-Output $resultRoot
