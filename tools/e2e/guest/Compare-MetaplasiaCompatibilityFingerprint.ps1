[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ExpectedPath,
    [Parameter(Mandatory)][string]$ActualPath,
    [ValidateSet('all', 'start-menu', 'shell')][string]$Target = 'all',
    # A serviced checkpoint records an intended target, not a tested shell.
    # Allow older target metadata here only; workload verification never uses it.
    [switch]$PreparationTargetOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-MetaplasiaCanonicalFingerprint {
    param([Parameter(Mandatory)]$Fingerprint)

    if (($Fingerprint.schema -ne 2 -and
            -not ($PreparationTargetOnly -and $Fingerprint.schema -eq 1)) -or
        [string]$Fingerprint.project -cne 'Metaplasia') {
        throw 'The compatibility fingerprint schema is invalid.'
    }
    $canonical = [Collections.Generic.List[string]]::new()
    $canonical.Add("architecture=$([string]$Fingerprint.architecture)")
    $canonical.Add("windows=$([string]$Fingerprint.windows)")
    foreach ($propertyName in @('productName', 'editionId', 'displayVersion',
            'installationType', 'currentBuild', 'ubr', 'buildLabEx')) {
        $properties = @($Fingerprint.operatingSystem.PSObject.Properties |
            Where-Object { $_.Name -ceq $propertyName })
        if ($properties.Count -ne 1) {
            throw "The compatibility fingerprint omitted operatingSystem.$propertyName."
        }
        if ($propertyName -notin @('productName', 'editionId')) {
            $canonical.Add(
                "operatingSystem.$propertyName=$([string]$properties[0].Value)")
        }
    }
    $adapterNames = switch ($Target) {
        'start-menu' { @('startMenu') }
        'shell' { @('taskbar', 'startMenu') }
        default { @('taskbar', 'fileExplorer', 'startMenu') }
    }
    foreach ($adapterName in $adapterNames) {
        $properties = @($Fingerprint.adapters.PSObject.Properties |
            Where-Object { $_.Name -ceq $adapterName })
        if ($properties.Count -ne 1) {
            throw "The compatibility fingerprint omitted $adapterName."
        }
        $adapter = $properties[0].Value
        $canonical.Add("adapter=$adapterName|windows=$([string]$adapter.windows)")
        foreach ($module in @($adapter.modules | Sort-Object `
                -Property @{ Expression = { [string]$_.name } })) {
            $canonical.Add((
                'module={0}|path={1}|key={2}|sha256={3}|length={4}' -f
                [string]$module.name,
                [IO.Path]::GetFullPath([string]$module.path).ToUpperInvariant(),
                [string]$module.compatibilityKey,
                [string]$module.sha256,
                [int64]$module.length))
        }
    }
    if (-not $PreparationTargetOnly) {
        if (@($Fingerprint.startMenuPayloads).Count -eq 0) {
            throw 'The fingerprint omitted the loaded Start UI payloads.'
        }
        foreach ($module in @($Fingerprint.startMenuPayloads | Sort-Object name)) {
            $canonical.Add((
                'start-payload={0}|path={1}|key={2}|sha256={3}|length={4}' -f
                [string]$module.name,
                [IO.Path]::GetFullPath([string]$module.path).ToUpperInvariant(),
                [string]$module.compatibilityKey,
                [string]$module.sha256,
                [int64]$module.length))
        }
    }
    return $canonical.ToArray()
}

$expected = Get-Content -LiteralPath $ExpectedPath -Raw | ConvertFrom-Json
$actual = Get-Content -LiteralPath $ActualPath -Raw | ConvertFrom-Json
# FileVersion is a localized resource string, not the identity of the image.
# Real captures had identical SHA-256/PE keys but different version strings
# on Russian and English Windows. Preserve it as evidence, not a match gate.
$expectedLines = @(Get-MetaplasiaCanonicalFingerprint `
    -Fingerprint $expected)
$actualLines = @(Get-MetaplasiaCanonicalFingerprint -Fingerprint $actual)
$difference = @(Compare-Object -ReferenceObject $expectedLines `
    -DifferenceObject $actualLines -CaseSensitive)
if ($difference.Count -ne 0) {
    $detail = ($difference | Select-Object -First 6 | ForEach-Object {
            "[$($_.SideIndicator)] $($_.InputObject)"
        }) -join '; '
    throw ("The VM shell fingerprint does not exactly match the target " +
        "system. Refusing to certify this Windows build. " +
        "targetWindows=$([string]$expected.windows); " +
        "vmWindows=$([string]$actual.windows); " +
        "differences=$($difference.Count); sample=$detail")
}

[ordered]@{
    success = $true
    target = $Target
    windows = [string]$actual.windows
    architecture = [string]$actual.architecture
}
