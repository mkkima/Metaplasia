[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CliPath,
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$cli = Get-Item -LiteralPath $CliPath -Force -ErrorAction Stop
if ($cli.PSIsContainer -or
    ($cli.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'The compatibility-report CLI is not a regular file.'
}

$previousPreference = $ErrorActionPreference
try {
    $ErrorActionPreference = 'Continue'
    $reportLines = @(& $cli.FullName compatibility-report 2>&1 |
        ForEach-Object { [string]$_ })
    $reportExitCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $previousPreference
}
if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    # Retain the original report even when parsing or module inspection fails.
    # It contains shell image identities, never credentials or user documents.
    $reportLines | Set-Content -LiteralPath (
        [IO.Path]::ChangeExtension([IO.Path]::GetFullPath($OutputPath), '.report.txt')) -Encoding utf8
}
if ($reportExitCode -ne 0) {
    throw "compatibility-report failed with exit code $reportExitCode`: $($reportLines -join ' | ')"
}

$adapterNames = [ordered]@{
    'taskbar-clock' = 'taskbar'
    'file-explorer-title' = 'fileExplorer'
    'start-menu-xaml' = 'startMenu'
}
$adapters = [ordered]@{}
$startProcessId = 0
$currentAdapter = $null
$pendingModule = $null
foreach ($line in $reportLines) {
    if ($line -match '^(?<adapter>taskbar-clock|file-explorer-title|start-menu-xaml): target-not-running$') {
        throw ("Cannot fingerprint $($Matches.adapter): target-not-running. " +
            'Run the collector in the logged-on desktop session, not PowerShell Direct session 0.')
    }
    if ($line -match '^(?<adapter>taskbar-clock|file-explorer-title|start-menu-xaml):\s+.*\bwindows=(?<windows>[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+),') {
        $currentAdapter = [string]$adapterNames[$Matches.adapter]
        if ($adapters.Contains($currentAdapter)) {
            throw "compatibility-report repeated adapter $currentAdapter."
        }
        $adapters[$currentAdapter] = [ordered]@{
            windows = [string]$Matches.windows
            modules = [Collections.Generic.List[object]]::new()
        }
        if ($currentAdapter -eq 'startMenu' -and $line -match ', pid=(?<pid>[0-9]+),') {
            $startProcessId = [int]$Matches.pid
        }
        $pendingModule = $null
        continue
    }
    if ($null -ne $currentAdapter -and
        $line -match '^\s{2}(?<name>[^\r\n]+?)\s+->\s+(?<path>[A-Za-z]:\\.+)$') {
        $pendingModule = [ordered]@{
            name = [string]$Matches.name
            path = [IO.Path]::GetFullPath([string]$Matches.path)
        }
        continue
    }
    if ($null -ne $currentAdapter -and $null -ne $pendingModule -and
        $line -match '^\s+key=(?<key>\S+)$') {
        $module = Get-Item -LiteralPath $pendingModule.path -Force `
            -ErrorAction Stop
        if ($module.PSIsContainer -or
            ($module.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "A compatibility module is not a regular file: $($pendingModule.path)"
        }
        $adapters[$currentAdapter].modules.Add([ordered]@{
            name = $pendingModule.name
            path = $module.FullName
            compatibilityKey = [string]$Matches.key
            sha256 = (Get-FileHash -LiteralPath $module.FullName `
                -Algorithm SHA256).Hash
            length = [int64]$module.Length
            fileVersion = [string]$module.VersionInfo.FileVersion
        })
        $pendingModule = $null
        continue
    }
    if ($line -match '^[^\s]') {
        $currentAdapter = $null
        $pendingModule = $null
    }
}

$allowedModuleCounts = [ordered]@{
    taskbar = @(5)
    fileExplorer = @(3, 4)
    startMenu = @(2)
}
$requiredModuleNames = [ordered]@{
    taskbar = @('Explorer.EXE', 'user32.dll', 'Taskbar.dll',
        'Taskbar.View.dll', 'twinui.pcshell.dll')
    fileExplorer = @('Explorer.EXE', 'user32.dll', 'dwmapi.dll')
    startMenu = @('StartMenuExperienceHost.exe', 'Windows.UI.Xaml.dll')
}
$windowsVersions = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::Ordinal)
foreach ($adapterName in $allowedModuleCounts.Keys) {
    if (-not $adapters.Contains($adapterName)) {
        throw "compatibility-report omitted adapter $adapterName`: $($reportLines -join ' | ')"
    }
    $adapter = $adapters[$adapterName]
    if ($adapter.modules.Count -notin $allowedModuleCounts[$adapterName]) {
        throw ("compatibility-report returned {0} modules for {1}; allowed {2}." -f
            $adapter.modules.Count, $adapterName,
            ($allowedModuleCounts[$adapterName] -join ', '))
    }
    $actualNames = @($adapter.modules | ForEach-Object { [string]$_.name })
    if (@($actualNames | Sort-Object -Unique).Count -ne $actualNames.Count) {
        throw "compatibility-report repeated a module for $adapterName."
    }
    foreach ($requiredName in $requiredModuleNames[$adapterName]) {
        if (@($actualNames | Where-Object {
                    $_ -ieq $requiredName
                }).Count -ne 1) {
            throw "compatibility-report omitted $requiredName for $adapterName."
        }
    }
    [void]$windowsVersions.Add([string]$adapter.windows)
}
if ($windowsVersions.Count -ne 1) {
    throw 'The shell adapters reported inconsistent Windows revisions.'
}
$currentVersion = Get-ItemProperty -LiteralPath `
    'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
$registryWindows = '10.0.{0}.{1}' -f
    [string]$currentVersion.CurrentBuild,
    [string]$currentVersion.UBR
if ($registryWindows -cne [string]@($windowsVersions)[0]) {
    throw 'The registry and shell adapters reported different Windows revisions.'
}

# The small StartMenuExperienceHost launcher and XAML runtime do not contain
# the entire Start implementation. Compare the loaded UI payloads as well;
# servicing/feature-pack versions can differ even with the same Windows UBR.
if ($startProcessId -le 0) { throw 'The report omitted the interactive Start process ID.' }
$startProcess = Get-Process -Id $startProcessId -ErrorAction Stop
if ($startProcess.ProcessName -ine 'StartMenuExperienceHost' -or
    $startProcess.SessionId -ne (Get-Process -Id $PID).SessionId) {
    throw 'The Start process changed or belongs to a different desktop session.'
}
$payloadNames = @('StartDocked.dll', 'StartMenu.dll', 'Windows.UI.Xaml.Controls.dll')
$payloads = [Collections.Generic.List[object]]::new()
foreach ($mappedModule in @($startProcess.Modules | Where-Object {
    $_.ModuleName -in $payloadNames
} | Sort-Object ModuleName)) {
    $module = Get-Item -LiteralPath $mappedModule.FileName -Force -ErrorAction Stop
    if ($module.PSIsContainer -or
        ($module.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        -not $module.FullName.StartsWith($env:SystemRoot.TrimEnd('\') + '\',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'A loaded Start UI payload is not a regular Windows image.'
    }
    $identityLines = @(& $cli.FullName module-info $module.FullName | ForEach-Object { [string]$_ })
    if ($LASTEXITCODE -ne 0) { throw "Unable to inspect the loaded Start payload: $($module.Name)" }
    $keys = @($identityLines | Where-Object { $_ -match '^compatibility-key: \S+$' })
    if ($keys.Count -ne 1) { throw "Ambiguous PE identity for $($module.Name)." }
    $payloads.Add([ordered]@{
        name = $module.Name
        path = $module.FullName
        compatibilityKey = $keys[0].Substring('compatibility-key: '.Length)
        sha256 = (Get-FileHash -LiteralPath $module.FullName -Algorithm SHA256).Hash
        length = [int64]$module.Length
        fileVersion = [string]$module.VersionInfo.FileVersion
    })
}
if ($payloads.Count -eq 0 -or
    @($payloads.name | Sort-Object -Unique).Count -ne $payloads.Count) {
    throw 'The interactive Start UI payload set is empty or ambiguous.'
}

$fingerprint = [ordered]@{
    schema = 2
    project = 'Metaplasia'
    capturedUtc = [DateTime]::UtcNow.ToString('o')
    architecture = [string][Runtime.InteropServices.RuntimeInformation]::OSArchitecture
    windows = [string]@($windowsVersions)[0]
    operatingSystem = [ordered]@{
        productName = [string]$currentVersion.ProductName
        editionId = [string]$currentVersion.EditionID
        displayVersion = [string]$currentVersion.DisplayVersion
        installationType = [string]$currentVersion.InstallationType
        currentBuild = [string]$currentVersion.CurrentBuild
        ubr = [int64]$currentVersion.UBR
        buildLabEx = [string]$currentVersion.BuildLabEx
    }
    adapters = $adapters
    startMenuPayloads = $payloads
}

if (-not [string]::IsNullOrWhiteSpace($OutputPath)) {
    $fullOutputPath = [IO.Path]::GetFullPath($OutputPath)
    $parent = Split-Path -Parent $fullOutputPath
    if ([string]::IsNullOrWhiteSpace($parent) -or
        -not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw 'The fingerprint output parent does not exist.'
    }
    $fingerprint | ConvertTo-Json -Depth 8 | Set-Content `
        -LiteralPath $fullOutputPath -Encoding utf8
}
$fingerprint
