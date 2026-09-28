Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-MetaplasiaE2EGuest {
    $computer = Get-CimInstance Win32_ComputerSystem
    if ($computer.Manufacturer -ne 'Microsoft Corporation' -or
        $computer.Model -notmatch 'Virtual Machine') {
        throw 'Refusing to run Metaplasia E2E actions outside a Hyper-V VM.'
    }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Metaplasia E2E guest actions require an administrator account.'
    }
}

function Assert-MetaplasiaE2EPath {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$AllowedRoot,
        [switch]$Directory
    )

    $resolved = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetFullPath($AllowedRoot).TrimEnd('\')
    if (-not $resolved.Equals($root, [StringComparison]::OrdinalIgnoreCase) -and
        -not $resolved.StartsWith(
            $root + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path escapes the Metaplasia E2E boundary: $Path"
    }
    $item = Get-Item -LiteralPath $resolved -Force -ErrorAction Stop
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        ($Directory -and -not $item.PSIsContainer) -or
        (-not $Directory -and $item.PSIsContainer)) {
        throw "Unexpected Metaplasia E2E path type: $Path"
    }
    return $resolved
}

function Assert-MetaplasiaE2EPackage {
    param([Parameter(Mandatory)][string]$PackageRoot)

    $resolvedRoot = Assert-MetaplasiaE2EPath -Path $PackageRoot `
        -AllowedRoot 'C:\MetaplasiaLab\package' -Directory
    if (-not $resolvedRoot.Equals(
            'C:\MetaplasiaLab\package',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The guest package must use the exact isolated staging path.'
    }
    $manifestPath = Assert-MetaplasiaE2EPath `
        -Path (Join-Path $resolvedRoot 'manifest.json') `
        -AllowedRoot $resolvedRoot
    $manifest = Get-Content -LiteralPath $manifestPath -Raw |
        ConvertFrom-Json
    if ($manifest.schema -ne 1 -or $manifest.project -cne 'Metaplasia') {
        throw 'Unsupported or foreign E2E package manifest.'
    }

    $required = @(
        'bin\metaplasia.exe',
        'bin\metaplasia-host.exe',
        'bin\metaplasia-watchdog.exe',
        'bin\metaplasia-cli.exe',
        'bin\metaplasia-agent.dll',
        'guest\MetaplasiaE2E.Common.ps1',
        'guest\Install-MetaplasiaE2E.ps1',
        'guest\Run-MetaplasiaE2EWorkload.ps1',
        'guest\Invoke-MetaplasiaE2EWorkload.ps1',
        'guest\Uninstall-MetaplasiaE2E.ps1',
        'guest\Confirm-MetaplasiaE2EClean.ps1'
    )
    $manifestPaths = @($manifest.files | ForEach-Object { [string]$_.path })
    foreach ($requiredPath in $required) {
        if ($manifestPaths -cnotcontains $requiredPath) {
            throw "Required Metaplasia package entry is missing: $requiredPath"
        }
    }
    if (@($manifest.files | Where-Object {
                [IO.Path]::GetExtension([string]$_.path) -ieq '.sys'
            }).Count -ne 0) {
        throw 'Metaplasia must not import a driver from the ISeeYou package.'
    }

    $rootPrefix = $resolvedRoot.TrimEnd('\') + '\'
    $seen = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::OrdinalIgnoreCase)
    foreach ($file in $manifest.files) {
        $relative = [string]$file.path
        if (-not $seen.Add($relative)) {
            throw "Duplicate E2E manifest path: $relative"
        }
        $candidate = [IO.Path]::GetFullPath((Join-Path $resolvedRoot $relative))
        if (-not $candidate.StartsWith(
                $rootPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "E2E manifest path escapes package root: $relative"
        }
        $item = Get-Item -LiteralPath $candidate -Force -ErrorAction Stop
        if ($item.PSIsContainer -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "E2E package entry is not a regular file: $relative"
        }
        if ($item.Length -ne [int64]$file.length) {
            throw "E2E package length mismatch: $relative"
        }
        $hash = (Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
        if ($hash -cne [string]$file.sha256) {
            throw "E2E package hash mismatch: $relative"
        }
    }
    return $manifest
}

function Invoke-MetaplasiaNative {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [string[]]$Arguments = @(),
        [int[]]$AllowedExitCodes = @(0)
    )

    $output = & $FilePath @Arguments 2>&1
    $exitCode = $LASTEXITCODE
    $text = ($output | Out-String).Trim()
    if ($AllowedExitCodes -notcontains $exitCode) {
        throw "$FilePath exited with $exitCode. $text"
    }
    return $text
}

function Get-MetaplasiaVerifierState {
    $settings = & verifier.exe /querysettings 2>&1 | Out-String
    $settingsExitCode = $LASTEXITCODE
    $active = & verifier.exe /query 2>&1 | Out-String
    $activeExitCode = $LASTEXITCODE
    if ($settingsExitCode -ne 0 -or $activeExitCode -ne 0) {
        throw 'Driver Verifier state could not be queried.'
    }
    if ($settings -match '(?i)metaplasia' -or
        $active -match '(?i)metaplasia') {
        throw 'Driver Verifier unexpectedly references Metaplasia.'
    }
    return [ordered]@{
        applicable = $false
        reason = 'Metaplasia contains no kernel driver; verifier state is checked but not modified.'
        settingsSha256 = Get-MetaplasiaTextSha256 -Text $settings
        activeSha256 = Get-MetaplasiaTextSha256 -Text $active
    }
}

function Get-MetaplasiaTextSha256 {
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)

    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha256.ComputeHash($bytes)
        return ([BitConverter]::ToString($hash) -replace '-', '')
    } finally {
        $sha256.Dispose()
        [Array]::Clear($bytes, 0, $bytes.Length)
    }
}
