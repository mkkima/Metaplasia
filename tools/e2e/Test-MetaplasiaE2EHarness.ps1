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
$uninstallText = Get-Content -LiteralPath (Join-Path $guestRoot 'Uninstall-MetaplasiaE2E.ps1') -Raw
if ($uninstallText.IndexOf('$processDeadline =') -lt 0 -or
    $uninstallText.IndexOf('$processDeadline =') -ge $uninstallText.IndexOf('$agentPath =')) {
    throw 'Uninstall must stop the runtime before restarting agent-bearing shell processes.'
}
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
    'The Metaplasia workload must use the expected limited user token',
    'Interactive workload input is allowed only inside the Hyper-V guest',
    'The workload is not on the active RDP desktop:',
    'The UI Automation Start point is outside the unobstructed guest taskbar.',
    'Refusing Escape input outside the Start foreground window',
    'guest-desktop-evidence.json',
    'Get-MetaplasiaCompatibilityFingerprint.ps1',
    'ExpectedCompatibilityPath',
    'refusing to certify a visual fix from Active alone',
    'A three-panel marker is outside every visible Start window',
    'Refusing to certify this Windows build'
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
$runtimeText = Get-Content -LiteralPath (
    Join-Path $e2eRoot 'Invoke-MetaplasiaHyperVLab.ps1') -Raw
if ($runtimeText -notmatch '\$firstLogonDeadline = ' -or
    $runtimeText -notmatch 'while \(-not \$taskStart.startedMarker -and -not \$rdpProcess.HasExited' -or
    $runtimeText -match '\$rdpProcess.WaitForExit\(10000\)') {
    throw 'First-logon startup must observe late OOBE disconnects until the bounded deadline.'
}
foreach ($contract in @(
    'SourceLabRoot',
    'out\lab\vm-state.json',
    'Global\Metaplasia.ISeeYou-Lab.E2E',
    '$state.schema -ne 1',
    'target-compatibility-fingerprint.json',
    'windows-version-preflight.json',
    'originalCheckpointId',
    'prepared-baseline.json',
    'Restore-VMSnapshot -VMSnapshot $checkpoint -Confirm:$false',
    '$baselineOwned -and $guestPackageStaged',
    '$baselineOwned -and $null -ne $vm -and $null -ne $checkpoint'
)) {
    if ($runtimeText -notmatch [regex]::Escape($contract)) {
        throw "Missing shared-VM safety contract: $contract"
    }
}
if ($runtimeText.IndexOf('Windows version mismatch:') -ge
    $runtimeText.IndexOf('Copy-Item -ToSession $session -Path')) {
    throw 'The Windows version gate must precede package installation.'
}
if ($hostText -match '(?i)\b(New-VM|Import-VM|Export-VMSnapshot)\b|Provision-MetaplasiaE2EVM') {
    throw 'The adapter must reuse the existing VM, not provision another one.'
}
$servicingText = Get-Content -LiteralPath (
    Join-Path $e2eRoot 'Initialize-MetaplasiaWindowsBaseline.ps1') -Raw
if ($servicingText -match 'Microsoft\.Update\.(Session|UpdateColl)|CreateUpdateInstaller' -or
    $servicingText -notmatch 'B33FF7AAB5BBBBB73867F3F5C2F492FE97C0129B96AA1941BC270610D7876DFA' -or
    $servicingText -notmatch 'windows-serviced-only' -or
    $servicingText -match 'Get-Fingerprint.ps1|Get-MetaplasiaCompatibilityFingerprint.ps1') {
    throw 'Pinned guest servicing validation is missing or allows broad updates.'
}
if ($runtimeText -notmatch "verification = 'windows-serviced-only'" -or
    $guestText.IndexOf("'verify-exact-target-fingerprint'") -lt 0 -or
    $guestText.IndexOf("'verify-exact-target-fingerprint'") -ge
        $guestText.IndexOf("'launch-background-runtime'")) {
    throw 'A prepared Windows checkpoint must not bypass interactive fingerprint verification.'
}
foreach ($launcher in @(
    'Start-MetaplasiaEndToEndLab.ps1',
    'Request-MetaplasiaE2EElevation.ps1'
)) {
    $launcherText = Get-Content -LiteralPath (Join-Path $e2eRoot $launcher) -Raw
    if ($launcherText -notmatch [regex]::Escape(
            'System32\WindowsPowerShell\v1.0\powershell.exe') -or
        $launcherText -notmatch [regex]::Escape('-SourceLabRoot')) {
        throw "The one-UAC Windows PowerShell shared-VM launcher regressed: $launcher"
    }
}
# Parsing cannot detect ambiguous provider arguments such as -VM.
if (Get-Module -ListAvailable -Name Hyper-V) {
    $runtimeAst = [Management.Automation.Language.Parser]::ParseInput(
        $runtimeText, [ref]$null, [ref]$null)
    $calls = $runtimeAst.FindAll({ param($node)
        $node -is [Management.Automation.Language.CommandAst] -and
        $node.GetCommandName() -match '^(Get|Set|Start|Stop|Disconnect|Connect|Add|Remove|Restore|Checkpoint|New)-VM'
    }, $true)
    foreach ($call in $calls) {
        $command = Get-Command -Name $call.GetCommandName() -ErrorAction Stop
        foreach ($argument in $call.CommandElements) {
            if ($argument -isnot [Management.Automation.Language.CommandParameterAst]) {
                continue
            }
            $name = $argument.ParameterName
            if ($command.Parameters.ContainsKey($name)) { continue }
            $matches = @($command.Parameters.Keys | Where-Object {
                $_.StartsWith($name, [StringComparison]::OrdinalIgnoreCase)
            })
            if ($matches.Count -ne 1) {
                throw "Invalid or ambiguous Hyper-V argument: $($call.GetCommandName()) -$name"
            }
        }
    }
}

# Exercise the fingerprint gate, not only its presence in the launcher.
$workloadAst = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $guestRoot 'Invoke-MetaplasiaE2EWorkload.ps1'), [ref]$null, [ref]$null)
if ($workloadAst.Extent.Text -match '-PreparationTargetOnly' -or
    $workloadAst.Extent.Text -match 'RootElement.FindAll') {
    throw 'Workload must use full fingerprint validation and scope UI Automation to Start windows.'
}
# Compile the native window probe without invoking it or touching the desktop.
$probeSource = $workloadAst.Find({ param($node)
    $node -is [Management.Automation.Language.StringConstantExpressionAst] -and
    $node.Value -match 'public static class MetaplasiaDesktopProbe'
}, $true)
if ($null -eq $probeSource) { throw 'The native window probe is missing.' }
if ($probeSource.Value -notmatch 'EnumDesktopWindows' -or
    $probeSource.Value -match '\bEnumWindows\(' -or
    $probeSource.Value -notmatch 'ForegroundProcessId') {
    throw 'The guest window probe must not omit packaged shell windows or foreground ownership.'
}
if ($probeSource.Value -notmatch 'WindowFromPoint' -or
    $probeSource.Value -notmatch 'WTSQuerySessionInformation' -or
    $workloadAst.Extent.Text -notmatch 'Current.BoundingRectangle' -or
    $workloadAst.Extent.Text -match '\[MetaplasiaDesktopProbe\]::OpenStart\(') {
    throw 'Guest Start activation must use a verified UIA point in the active RDP session.'
}
$visibilityCheck = $workloadAst.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Test-MetaplasiaStartVisible'
}, $false)
if ($null -eq $visibilityCheck -or $visibilityCheck.Extent.Text -notmatch 'ProcessAtPoint' -or
    $visibilityCheck.Extent.Text -notmatch 'IsOffscreen' -or
    $visibilityCheck.Extent.Text -notmatch 'Cloaked -eq 0') {
    throw 'A preloaded or obstructed Start CoreWindow must not count as an open menu.'
}
Add-Type -TypeDefinition $probeSource.Value -ErrorAction Stop
$rdpAst = [Management.Automation.Language.Parser]::ParseFile(
    (Join-Path $e2eRoot 'Open-MetaplasiaE2ERdpSession.ps1'), [ref]$null, [ref]$null)
$rdpSource = $rdpAst.Find({ param($node)
    $node -is [Management.Automation.Language.StringConstantExpressionAst] -and
    $node.Value -match 'public sealed class MetaplasiaRdpForm'
}, $true)
if ($null -eq $rdpSource -or $rdpSource.Value -notmatch 'ShowWithoutActivation' -or
    $rdpSource.Value -notmatch '0x08000000') {
    throw 'The hidden VM transport must never activate a host desktop window.'
}
if ($PSVersionTable.PSVersion.Major -eq 5) {
    Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing `
        -TypeDefinition $rdpSource.Value -ErrorAction Stop
}
# Load only the pure assertion, never the desktop workload or its input.
$geometryAssertion = $workloadAst.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Assert-MetaplasiaThreePanelGeometry'
}, $false)
if ($null -eq $geometryAssertion) { throw 'The independent geometry assertion is missing.' }
. ([scriptblock]::Create($geometryAssertion.Extent.Text))
foreach ($case in @('visible', 'missing', 'partial', 'clipped-right', 'clipped-top', 'cloaked', 'wrong-process', 'occluded')) {
    $geometry = [ordered]@{
        processId = 42
        windows = @([pscustomobject]@{ Left = 0; Top = 0; Width = 1246; Height = 624; Cloaked = 0 })
        allAppsLabel = [ordered]@{ left = 54; top = 48; width = 140; height = 20; processId = 42; hitProcessId = 42 }
        recommendedLabel = [ordered]@{ left = 920; top = 48; width = 140; height = 20; processId = 42; hitProcessId = 42 }
    }
    switch ($case) {
        'missing' { $geometry.allAppsLabel = $null; $geometry.recommendedLabel = $null }
        'partial' { $geometry.recommendedLabel = $null }
        'clipped-right' { $geometry.windows[0].Width = 800 }
        'clipped-top' { $geometry.allAppsLabel.top = -1 }
        'cloaked' { $geometry.windows[0].Cloaked = 1 }
        'wrong-process' { $geometry.recommendedLabel.processId = 43 }
        'occluded' { $geometry.recommendedLabel.hitProcessId = 43 }
    }
    $accepted = $true
    try { Assert-MetaplasiaThreePanelGeometry -Geometry $geometry } catch { $accepted = $false }
    if ($accepted -ne ($case -eq 'visible')) {
        throw "Independent Start geometry regression: $case; accepted=$accepted."
    }
}

$taskbarAssertion = $workloadAst.Find({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
    $node.Name -eq 'Assert-MetaplasiaTaskbarGeometry'
}, $false)
if ($null -eq $taskbarAssertion) { throw 'The independent Taskbar assertion is missing.' }
. ([scriptblock]::Create($taskbarAssertion.Extent.Text))
foreach ($case in @('capsule', 'restored', 'unchanged', 'clipped', 'occluded', 'missing', 'hide-applied', 'hide-ignored')) {
    $baseline = [ordered]@{ clock = @{ left = 1220; width = 100 } }
    $geometry = [ordered]@{
        processId = 42; screenWidth = 1366; screenHeight = 768; showDesktopCount = 1
        clock = @{ left = 1208; top = 720; width = 100; height = 48; hitProcessId = 42 }
        start = @{ left = 400; top = 720; width = 48; height = 48; hitProcessId = 42 }
    }
    switch ($case) {
        'restored' { $geometry.clock.left = 1220 }
        'unchanged' { $geometry.clock.left = 1220 }
        'clipped' { $geometry.clock.left = 1360 }
        'occluded' { $geometry.clock.hitProcessId = 43 }
        'missing' { $geometry.clock = $null }
        'hide-applied' { $geometry.showDesktopCount = 0 }
    }
    $accepted = $true
    try {
        Assert-MetaplasiaTaskbarGeometry -Geometry $geometry -Baseline $baseline `
            -Capsule ($case -ne 'restored') -HideShowDesktop ($case -like 'hide-*')
    } catch { $accepted = $false }
    if ($accepted -ne ($case -in @('capsule', 'restored', 'hide-applied'))) {
        throw "Independent Taskbar geometry regression: $case; accepted=$accepted."
    }
}
if ($workloadAst.Extent.Text -notmatch '-Target shell' -or
    $workloadAst.Extent.Text -notmatch 'taskbar-explorer-restart') {
    throw 'Taskbar certification must require exact images and Explorer restart recovery.'
}

$fixtureRoot = Join-Path ([IO.Path]::GetTempPath()) ('Metaplasia-FingerprintTest-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixtureRoot | Out-Null
try {
    # Regression: session 0 has no Explorer. This used to discard a successful
    # Windows update with an unhelpful "omitted adapter" parse error.
    $stubCli = Join-Path $fixtureRoot 'no-shell-cli.ps1'
    @('
param([string]$Action)
if ($Action -ne ''compatibility-report'') { throw ''Unexpected CLI action.'' }
''external-pack: not-loaded''
''taskbar-clock: target-not-running''
''file-explorer-title: target-not-running''
''start-menu-xaml: target-not-running''
$global:LASTEXITCODE = 0
') | Set-Content -LiteralPath $stubCli -Encoding utf8
    $noShellOutput = Join-Path $fixtureRoot 'no-shell.json'
    $collectorError = $null
    try {
        & (Join-Path $guestRoot 'Get-MetaplasiaCompatibilityFingerprint.ps1') `
            -CliPath $stubCli -OutputPath $noShellOutput | Out-Null
    } catch { $collectorError = $_.Exception.Message }
    if ($null -eq $collectorError -or $collectorError -notmatch 'logged-on desktop session' -or
        (Test-Path -LiteralPath $noShellOutput) -or
        (Get-Content -LiteralPath (Join-Path $fixtureRoot 'no-shell.report.txt') -Raw) -notmatch
            'taskbar-clock: target-not-running') {
        throw 'Session-0 fingerprint failures must retain the raw report and explain the missing shell.'
    }
    $fixture = [ordered]@{
        schema = 2; project = 'Metaplasia'; windows = '10.0.26200.9457'; architecture = 'X64'
        operatingSystem = [ordered]@{
            productName = 'Windows'; editionId = 'Professional'; displayVersion = '25H2'
            installationType = 'Client'; currentBuild = '26200'; ubr = 9457; buildLabEx = 'fixture'
        }
        adapters = [ordered]@{}
        startMenuPayloads = @([ordered]@{
            name = 'StartMenu.dll'; path = 'C:\Windows\StartMenu.dll'; compatibilityKey = 'payload-key'
            sha256 = ('C' * 64); length = 2048; fileVersion = '2.3.4.5'
        })
    }
    foreach ($name in @('taskbar', 'fileExplorer', 'startMenu')) {
        $fixture.adapters[$name] = [ordered]@{
            windows = '10.0.26200.9457'
            modules = @([ordered]@{
                name = 'fixture.dll'; path = 'C:\Windows\fixture.dll'; compatibilityKey = 'key'
                sha256 = ('A' * 64); length = 1024; fileVersion = '1.2.3.4'
            })
        }
    }
    $expectedPath = Join-Path $fixtureRoot 'expected.json'
    $actualPath = Join-Path $fixtureRoot 'actual.json'
    $fixtureJson = $fixture | ConvertTo-Json -Depth 8
    $fixtureJson | Set-Content -LiteralPath $expectedPath -Encoding utf8
    foreach ($case in @('equal', 'edition', 'path-case', 'build', 'hash', 'key', 'version', 'other-adapter', 'taskbar-hash', 'payload-hash', 'missing-payload', 'legacy-schema')) {
        $actual = $fixtureJson | ConvertFrom-Json
        switch ($case) {
            'edition' { $actual.operatingSystem.editionId = 'ProfessionalWorkstation' }
            'path-case' { $actual.adapters.startMenu.modules[0].path = 'c:\WINDOWS\fixture.dll' }
            'build' { $actual.operatingSystem.ubr = 8037 }
            'hash' { $actual.adapters.startMenu.modules[0].sha256 = ('B' * 64) }
            'key' { $actual.adapters.startMenu.modules[0].compatibilityKey = 'other' }
            'version' { $actual.adapters.startMenu.modules[0].fileVersion = '1.2.3.5' }
            'other-adapter' { $actual.adapters.fileExplorer.modules[0].sha256 = ('E' * 64) }
            'taskbar-hash' { $actual.adapters.taskbar.modules[0].sha256 = ('F' * 64) }
            'payload-hash' { $actual.startMenuPayloads[0].sha256 = ('D' * 64) }
            'missing-payload' { $actual.startMenuPayloads = @() }
            'legacy-schema' { $actual.schema = 1 }
        }
        $actual | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $actualPath -Encoding utf8
        foreach ($target in @('all', 'start-menu', 'shell')) {
            $accepted = $true
            try {
                & (Join-Path $guestRoot 'Compare-MetaplasiaCompatibilityFingerprint.ps1') `
                    -ExpectedPath $expectedPath -ActualPath $actualPath -Target $target | Out-Null
            } catch { $accepted = $false }
            $shouldPass = $case -in @('equal', 'edition', 'path-case', 'version') -or
                ($target -in @('start-menu', 'shell') -and $case -eq 'other-adapter') -or
                ($target -eq 'start-menu' -and $case -eq 'taskbar-hash')
            if ($accepted -ne $shouldPass) {
                throw "Fingerprint gate regression: target=$target; case=$case; accepted=$accepted."
            }
        }
    }
    # Old metadata may select an OS preparation checkpoint, but the same
    # legacy-schema input above must never pass the normal workload gate.
    & (Join-Path $guestRoot 'Compare-MetaplasiaCompatibilityFingerprint.ps1') `
        -ExpectedPath $expectedPath -ActualPath $actualPath -PreparationTargetOnly | Out-Null
} finally {
    $fixtureItem = Get-Item -LiteralPath $fixtureRoot -Force
    if ((Resolve-Path -LiteralPath $fixtureRoot).Path -ine $fixtureRoot -or
        ($fixtureItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        @(Get-ChildItem -LiteralPath $fixtureRoot -Force -Recurse | Where-Object {
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        }).Count -ne 0) { throw 'Refusing to remove an unexpected test fixture path.' }
    Remove-Item -LiteralPath $fixtureRoot -Recurse -Force
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
