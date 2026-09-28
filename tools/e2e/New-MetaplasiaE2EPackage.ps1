[CmdletBinding()]
param(
    [ValidateSet('Debug')]
    [string]$Configuration = 'Debug'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param(
        [Parameter(Mandatory)][string]$Description,
        [Parameter(Mandatory)][scriptblock]$Command
    )
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
Push-Location $root
try {
    & (Join-Path $PSScriptRoot 'Test-MetaplasiaE2EHarness.ps1')

    Invoke-Checked -Description 'CMake configuration' -Command {
        & cmake --preset windows-msvc-debug
    }
    Invoke-Checked -Description 'Metaplasia Debug build' -Command {
        & cmake --build --preset windows-msvc-debug
    }
    Invoke-Checked -Description 'Native tests' -Command {
        & ctest --preset windows-msvc-debug --output-on-failure
    }
    Invoke-Checked -Description 'Rust formatting' -Command {
        & cargo fmt --manifest-path src-tauri\Cargo.toml -- --check
    }
    Invoke-Checked -Description 'Rust tests' -Command {
        & cargo test --manifest-path src-tauri\Cargo.toml --locked
    }
    Invoke-Checked -Description 'Rust lints' -Command {
        & cargo clippy --manifest-path src-tauri\Cargo.toml --locked `
            --all-targets -- -D warnings
    }
    $javaScript = @(Get-ChildItem -LiteralPath (Join-Path $root 'frontend') `
        -Filter '*.js' -File -Recurse)
    if ($javaScript.Count -eq 0) {
        throw 'No frontend JavaScript files were found.'
    }
    foreach ($file in $javaScript) {
        Invoke-Checked -Description "JavaScript syntax: $($file.Name)" `
            -Command { & node --check $file.FullName }
    }

    $id = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $packageRoot = Join-Path $root "out\e2e\packages\$id"
    $binRoot = Join-Path $packageRoot 'bin'
    $guestRoot = Join-Path $packageRoot 'guest'
    New-Item -ItemType Directory -Path $binRoot -Force | Out-Null
    New-Item -ItemType Directory -Path $guestRoot -Force | Out-Null

    $componentNames = @(
        'metaplasia.exe',
        'metaplasia-host.exe',
        'metaplasia-watchdog.exe',
        'metaplasia-cli.exe',
        'metaplasia-agent.dll'
    )
    foreach ($name in $componentNames) {
        $source = Join-Path $root "build\debug\bin\$name"
        $item = Get-Item -LiteralPath $source -Force -ErrorAction Stop
        if ($item.PSIsContainer -or
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Invalid portable component: $source"
        }
        Copy-Item -LiteralPath $source -Destination (Join-Path $binRoot $name)
    }
    foreach ($name in @(
        'MetaplasiaE2E.Common.ps1',
        'Install-MetaplasiaE2E.ps1',
        'Run-MetaplasiaE2EWorkload.ps1',
        'Invoke-MetaplasiaE2EWorkload.ps1',
        'Uninstall-MetaplasiaE2E.ps1',
        'Confirm-MetaplasiaE2EClean.ps1'
    )) {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot "guest\$name") `
            -Destination (Join-Path $guestRoot $name)
    }

    $cmakeText = Get-Content -LiteralPath (Join-Path $root 'CMakeLists.txt') -Raw
    if ($cmakeText -notmatch
        'project\(Metaplasia VERSION (?<version>[0-9]+\.[0-9]+\.[0-9]+)') {
        throw 'Could not resolve the Metaplasia version.'
    }
    $version = $Matches.version
    $commit = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $commit -notmatch '^[0-9a-f]{40}$') {
        throw 'Could not resolve the source commit.'
    }
    $dirty = [bool](& git status --porcelain)
    $files = @(Get-ChildItem -LiteralPath $packageRoot -File -Recurse |
        Sort-Object FullName | ForEach-Object {
            [ordered]@{
                path = [IO.Path]::GetRelativePath($packageRoot, $_.FullName)
                length = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName `
                    -Algorithm SHA256).Hash
            }
        })
    $manifest = [ordered]@{
        schema = 1
        project = 'Metaplasia'
        version = $version
        configuration = $Configuration
        sourceCommit = $commit
        workingTreeDirty = $dirty
        createdUtc = [DateTime]::UtcNow.ToString('o')
        files = $files
    }
    $manifestPath = Join-Path $packageRoot 'manifest.json'
    [IO.File]::WriteAllText(
        $manifestPath,
        ($manifest | ConvertTo-Json -Depth 6),
        [Text.UTF8Encoding]::new($false))

    $latestRoot = Join-Path $root 'out\e2e'
    New-Item -ItemType Directory -Path $latestRoot -Force | Out-Null
    $latest = [ordered]@{
        schema = 1
        project = 'Metaplasia'
        packageRoot = $packageRoot
        manifestPath = $manifestPath
    }
    [IO.File]::WriteAllText(
        (Join-Path $latestRoot 'latest-package.json'),
        ($latest | ConvertTo-Json),
        [Text.UTF8Encoding]::new($false))
    Write-Output $packageRoot
} finally {
    Pop-Location
}
