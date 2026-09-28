[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Server,
    [Parameter(Mandatory)][string]$UserName,
    [Parameter(Mandatory)][string]$CredentialPath,
    [Parameter(Mandatory)][string]$ReadyPath,
    [Parameter(Mandatory)][string]$StopPath,
    [int]$MaximumMinutes = 15
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSVersion.Major -ne 5) {
    throw 'The hidden RDP ActiveX host requires Windows PowerShell 5.1.'
}
if ($Server -notmatch '^192\.168\.(?:2[0-9]{2})\.2$' -or
    $UserName -notmatch '^[A-Za-z0-9_-]{1,32}\\[A-Za-z0-9_-]{1,32}$' -or
    $MaximumMinutes -lt 1 -or $MaximumMinutes -gt 20) {
    throw 'The hidden RDP session arguments are outside the lab contract.'
}
$repositoryRoot = (Resolve-Path -LiteralPath (
        Join-Path $PSScriptRoot '..\..')).Path
$allowedResultRoot = [IO.Path]::GetFullPath(
    (Join-Path $repositoryRoot 'out\e2e\results')).TrimEnd('\') + '\'
foreach ($path in @($ReadyPath, $StopPath)) {
    if (-not [IO.Path]::GetFullPath($path).StartsWith(
            $allowedResultRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'The hidden RDP control path is outside Metaplasia results.'
    }
}
$readyDirectory = Split-Path -Parent $ReadyPath
if (-not (Test-Path -LiteralPath $readyDirectory -PathType Container)) {
    throw 'The hidden RDP result directory does not exist.'
}
$credentialItem = Get-Item -LiteralPath $CredentialPath -Force
if ($credentialItem.PSIsContainer -or
    ($credentialItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw 'The RDP credential source is not a regular file.'
}
$credential = Import-Clixml -LiteralPath $credentialItem.FullName
if ($credential -isnot [PSCredential]) {
    throw 'The RDP credential source is invalid.'
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Windows.Forms,System.Drawing `
    -TypeDefinition @'
using System;
using System.Windows.Forms;

public sealed class MetaplasiaRdpAxHost : AxHost {
    public MetaplasiaRdpAxHost()
        : base("8B918B82-7985-4C24-89DF-C33AD2BBFBCD") { }

    public object OcxObject { get { return GetOcx(); } }
}
'@

$form = $null
$hostControl = $null
$client = $null
$plainPassword = $null
$status = [ordered]@{
    success = $false
    connected = $false
    server = $Server
    processId = $PID
    error = $null
}
try {
    $form = [Windows.Forms.Form]::new()
    $form.ShowInTaskbar = $false
    $form.FormBorderStyle = [Windows.Forms.FormBorderStyle]::None
    $form.StartPosition = [Windows.Forms.FormStartPosition]::Manual
    $form.Location = [Drawing.Point]::new(-32000, -32000)
    $form.Size = [Drawing.Size]::new(1366, 768)
    $form.Opacity = 0.01
    $hostControl = [MetaplasiaRdpAxHost]::new()
    $hostControl.Dock = [Windows.Forms.DockStyle]::Fill
    $form.Controls.Add($hostControl)
    $form.Show()
    $hostControl.CreateControl()
    $client = $hostControl.OcxObject

    $client.Server = $Server
    $client.UserName = $UserName
    $client.DesktopWidth = 1366
    $client.DesktopHeight = 768
    $advanced = $client.AdvancedSettings9
    $plainPassword = $credential.GetNetworkCredential().Password
    $advanced.ClearTextPassword = $plainPassword
    $advanced.EnableCredSspSupport = $true
    $advanced.AuthenticationLevel = 0
    $advanced.NegotiateSecurityLayer = $true
    $advanced.RedirectClipboard = $false
    $advanced.RedirectDrives = $false
    $advanced.RedirectPrinters = $false
    $advanced.RedirectSmartCards = $false
    $advanced.SmartSizing = $false
    try { $client.SecuredSettings2.AudioRedirectionMode = 2 } catch { }
    $plainPassword = $null
    $client.Connect()

    $connectDeadline = [DateTime]::UtcNow.AddSeconds(75)
    do {
        [Windows.Forms.Application]::DoEvents()
        if ([int]$client.Connected -eq 1) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $connectDeadline)
    if ([int]$client.Connected -ne 1) {
        throw 'The isolated hidden RDP session did not connect.'
    }
    $status.success = $true
    $status.connected = $true
    $status.connectedUtc = [DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText(
        $ReadyPath,
        ($status | ConvertTo-Json -Depth 4),
        [Text.UTF8Encoding]::new($false))

    $deadline = [DateTime]::UtcNow.AddMinutes($MaximumMinutes)
    while (-not (Test-Path -LiteralPath $StopPath) -and
        [DateTime]::UtcNow -lt $deadline -and
        [int]$client.Connected -eq 1) {
        [Windows.Forms.Application]::DoEvents()
        Start-Sleep -Milliseconds 100
    }
    $status.connected = [int]$client.Connected -eq 1
    $status.stoppedByHarness = Test-Path -LiteralPath $StopPath
    $status.completedUtc = [DateTime]::UtcNow.ToString('o')
    if (-not $status.connected -and -not $status.stoppedByHarness) {
        $status.success = $false
        $status.error = 'The RDP control disconnected before the harness requested stop.'
        try {
            $status.extendedDisconnectReason =
                [int]$client.ExtendedDisconnectReason
        } catch { }
        try {
            $status.disconnectDescription = [string]$client.GetErrorDescription(
                0, [int]$client.ExtendedDisconnectReason)
        } catch { }
    }
    [IO.File]::WriteAllText(
        $ReadyPath,
        ($status | ConvertTo-Json -Depth 4),
        [Text.UTF8Encoding]::new($false))
} catch {
    $status.error = $_.ToString()
    [IO.File]::WriteAllText(
        $ReadyPath,
        ($status | ConvertTo-Json -Depth 4),
        [Text.UTF8Encoding]::new($false))
    throw
} finally {
    $plainPassword = $null
    if ($null -ne $client) {
        try {
            if ([int]$client.Connected -eq 1) { $client.Disconnect() }
        } catch { }
    }
    if ($null -ne $form) { $form.Close(); $form.Dispose() }
    if ($null -ne $hostControl) { $hostControl.Dispose() }
    if ($null -ne $client -and [Runtime.InteropServices.Marshal]::IsComObject(
            $client)) {
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($client)
    }
}
