[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string]$PrivateKeyPath,

    [ValidatePattern('^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$')]
    [string]$Repository = 'mkkima/Metaplasia',

    [ValidateNotNullOrEmpty()]
    [string]$ReleaseNotes = 'Development portable release.',

    [switch]$Publish
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Description,

        [Parameter(Mandatory = $true)]
        [scriptblock]$Command
    )

    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Description failed with exit code $LASTEXITCODE."
    }
}

function Get-ProjectVersion {
    param([Parameter(Mandatory = $true)][string]$Root)

    $cmake = Get-Content -LiteralPath (Join-Path $Root 'CMakeLists.txt') -Raw
    if ($cmake -notmatch 'project\(Metaplasia VERSION (?<version>[0-9]+\.[0-9]+\.[0-9]+)') {
        throw 'Could not read the CMake project version.'
    }
    $version = $Matches.version

    $cargo = Get-Content -LiteralPath (Join-Path $Root 'src-tauri\Cargo.toml') -Raw
    if ($cargo -notmatch '(?m)^version = "(?<version>[0-9]+\.[0-9]+\.[0-9]+)"\r?$') {
        throw 'Could not read the Cargo package version.'
    }
    $cargoVersion = $Matches.version
    $tauriVersion = (
        Get-Content -LiteralPath (Join-Path $Root 'src-tauri\tauri.conf.json') -Raw |
            ConvertFrom-Json
    ).version

    if ($cargoVersion -ne $version -or $tauriVersion -ne $version) {
        throw "Project versions differ (CMake=$version, Cargo=$cargoVersion, Tauri=$tauriVersion)."
    }
    return $version
}

function Get-OpenSsl {
    $command = Get-Command openssl -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }
    $gitOpenSsl = 'C:\Program Files\Git\usr\bin\openssl.exe'
    if (Test-Path -LiteralPath $gitOpenSsl -PathType Leaf) {
        return $gitOpenSsl
    }
    throw 'OpenSSL was not found.'
}

function Get-GitHubToken {
    if (-not [string]::IsNullOrWhiteSpace($env:GITHUB_TOKEN)) {
        return $env:GITHUB_TOKEN.Trim()
    }

    $credential = @(
        'protocol=https'
        'host=github.com'
        ''
    ) | & git credential fill
    if ($LASTEXITCODE -ne 0) {
        throw 'Git credential helper could not provide GitHub credentials.'
    }
    $passwordLine = $credential |
        Where-Object { $_ -like 'password=*' } |
        Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($passwordLine)) {
        throw 'No GitHub token is available. Configure the Git credential helper or GITHUB_TOKEN.'
    }
    return $passwordLine.Substring('password='.Length)
}

function Invoke-GitHubJson {
    param(
        [Parameter(Mandatory = $true)][string]$Method,
        [Parameter(Mandatory = $true)][string]$Uri,
        [Parameter(Mandatory = $true)][hashtable]$Headers,
        [object]$Body
    )

    $parameters = @{
        Method = $Method
        Uri = $Uri
        Headers = $Headers
        ContentType = 'application/json'
    }
    if ($null -ne $Body) {
        $parameters.Body = $Body | ConvertTo-Json -Depth 8 -Compress
    }
    return Invoke-RestMethod @parameters
}

function Get-GitHubReleaseByTag {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryName,
        [Parameter(Mandatory = $true)][string]$Tag,
        [Parameter(Mandatory = $true)][hashtable]$Headers
    )

    $escapedTag = [Uri]::EscapeDataString($Tag)
    try {
        return Invoke-GitHubJson `
            -Method Get `
            -Uri "https://api.github.com/repos/$RepositoryName/releases/tags/$escapedTag" `
            -Headers $Headers
    } catch {
        $response = $_.Exception.Response
        if ($null -ne $response -and [int]$response.StatusCode -eq 404) {
            $releases = @(
                Invoke-GitHubJson `
                    -Method Get `
                    -Uri "https://api.github.com/repos/${RepositoryName}/releases?per_page=100" `
                    -Headers $Headers
            )
            $matches = @(
                $releases | Where-Object { $_.tag_name -ceq $Tag }
            )
            if ($matches.Count -gt 1) {
                throw "GitHub returned multiple releases for tag $Tag."
            }
            return $matches | Select-Object -First 1
        }
        throw
    }
}

function Add-GitHubReleaseAsset {
    param(
        [Parameter(Mandatory = $true)][string]$UploadUrl,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ContentType,
        [Parameter(Mandatory = $true)][hashtable]$Headers
    )

    $baseUrl = $UploadUrl -replace '\{.*$', ''
    $name = [Uri]::EscapeDataString((Split-Path -Leaf $Path))
    $parameters = @{
        Method = 'Post'
        Uri = "${baseUrl}?name=$name"
        Headers = $Headers
        ContentType = $ContentType
        InFile = $Path
    }
    Invoke-RestMethod @parameters | Out-Null
}

function Set-GitHubReleaseAsset {
    param(
        [Parameter(Mandatory = $true)][string]$RepositoryName,
        [Parameter(Mandatory = $true)][object]$Release,
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$ContentType,
        [Parameter(Mandatory = $true)][hashtable]$Headers
    )

    $name = Split-Path -Leaf $Path
    foreach ($asset in @($Release.assets | Where-Object { $_.name -eq $name })) {
        Invoke-GitHubJson `
            -Method Delete `
            -Uri "https://api.github.com/repos/$RepositoryName/releases/assets/$($asset.id)" `
            -Headers $Headers | Out-Null
    }
    Add-GitHubReleaseAsset `
        -UploadUrl $Release.upload_url `
        -Path $Path `
        -ContentType $ContentType `
        -Headers $Headers
}

$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$resolvedPrivateKey = (Resolve-Path -LiteralPath $PrivateKeyPath).Path
if ($resolvedPrivateKey.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The private update key must be stored outside the repository.'
}

Push-Location $root
$temporaryRoot = $null
$githubToken = $null
try {
    $dirty = & git status --porcelain
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not inspect the Git working tree.'
    }
    if ($dirty) {
        throw 'The Git working tree must be clean before packaging a release.'
    }

    $version = Get-ProjectVersion -Root $root
    $tag = "dev-v$version"
    $head = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-f]{40}$') {
        throw 'Could not resolve the release commit.'
    }

    $publicKeyPath = Join-Path $root 'src-tauri\development-update-public-key.txt'
    $expectedPublicKey = (Get-Content -LiteralPath $publicKeyPath -Raw).Trim()
    if ($expectedPublicKey -notmatch '^[A-Za-z0-9+/]{43}=$') {
        throw 'The committed development public key is invalid.'
    }

    $openssl = Get-OpenSsl
    $temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'metaplasia-release-' + [Guid]::NewGuid().ToString('N')
    )
    $payloadDirectory = Join-Path $temporaryRoot 'payload'
    New-Item -ItemType Directory -Path $payloadDirectory -Force | Out-Null
    $publicDer = Join-Path $temporaryRoot 'public.der'
    Invoke-Checked -Description 'Development key validation' -Command {
        & $openssl pkey -in $resolvedPrivateKey -pubout -outform DER -out $publicDer
    }
    $publicBytes = [IO.File]::ReadAllBytes($publicDer)
    if ($publicBytes.Length -lt 32) {
        throw 'The development private key produced an invalid public key.'
    }
    $rawPublic = $publicBytes[($publicBytes.Length - 32)..($publicBytes.Length - 1)]
    $derivedPublicKey = [Convert]::ToBase64String($rawPublic)
    if ($derivedPublicKey -cne $expectedPublicKey) {
        throw 'The private key does not match the public key trusted by installed development copies.'
    }

    $previousChannel = $env:METAPLASIA_UPDATE_CHANNEL
    try {
        $env:METAPLASIA_UPDATE_CHANNEL = 'development'
        Invoke-Checked -Description 'CMake configuration' -Command {
            & cmake --preset windows-msvc-debug
        }
        Invoke-Checked -Description 'Development build' -Command {
            & cmake --build --preset windows-msvc-debug
        }
        Invoke-Checked -Description 'Native tests' -Command {
            & ctest --preset windows-msvc-debug --output-on-failure
        }
        Invoke-Checked -Description 'Rust formatting check' -Command {
            & cargo fmt --manifest-path src-tauri\Cargo.toml -- --check
        }
        Invoke-Checked -Description 'Rust tests' -Command {
            & cargo test --manifest-path src-tauri\Cargo.toml --locked
        }
        Invoke-Checked -Description 'Rust lints' -Command {
            & cargo clippy --manifest-path src-tauri\Cargo.toml --locked --all-targets -- -D warnings
        }
        $javaScriptFiles = @(
            Get-ChildItem `
                -LiteralPath (Join-Path $root 'frontend') `
                -Filter '*.js' `
                -File `
                -Recurse
        )
        if ($javaScriptFiles.Count -eq 0) {
            throw 'No frontend JavaScript files were found.'
        }
        foreach ($javaScriptFile in $javaScriptFiles) {
            Invoke-Checked -Description "JavaScript syntax check for $($javaScriptFile.FullName)" -Command {
                & node --check $javaScriptFile.FullName
            }
        }
    } finally {
        $env:METAPLASIA_UPDATE_CHANNEL = $previousChannel
    }

    $componentNames = @(
        'metaplasia.exe'
        'metaplasia-host.exe'
        'metaplasia-watchdog.exe'
        'metaplasia-cli.exe'
        'metaplasia-agent.dll'
    )
    foreach ($name in $componentNames) {
        $source = Join-Path $root "build\debug\bin\$name"
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Required portable component is missing: $source"
        }
        Copy-Item -LiteralPath $source -Destination (Join-Path $payloadDirectory $name)
    }

    $outputRoot = Join-Path $root "out\releases\$tag"
    $allowedOutputParent = [IO.Path]::GetFullPath((Join-Path $root 'out\releases'))
    $resolvedOutput = [IO.Path]::GetFullPath($outputRoot)
    if (-not $resolvedOutput.StartsWith(
        $allowedOutputParent + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Resolved release output escaped the expected directory.'
    }
    if (Test-Path -LiteralPath $resolvedOutput) {
        Remove-Item -LiteralPath $resolvedOutput -Recurse -Force
    }
    New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null

    $zipName = "Metaplasia-$version-windows-x64-portable-dev.zip"
    $zipPath = Join-Path $resolvedOutput $zipName
    $sourcePaths = $componentNames | ForEach-Object {
        Join-Path $payloadDirectory $_
    }
    Compress-Archive `
        -LiteralPath $sourcePaths `
        -DestinationPath $zipPath `
        -CompressionLevel Optimal
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $zipPath).Length

    $manifestPath = Join-Path $resolvedOutput 'portable-update.json'
    $signaturePath = Join-Path $resolvedOutput 'portable-update.json.sig'
    $checksumPath = Join-Path $resolvedOutput 'SHA256SUMS.txt'
    $manifest = [ordered]@{
        schema = 1
        version = $version
        published_at = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        notes = $ReleaseNotes
        package = [ordered]@{
            url = "https://github.com/$Repository/releases/download/$tag/$zipName"
            sha256 = $hash
            size = $size
        }
    }
    $manifestJson = $manifest | ConvertTo-Json -Depth 4 -Compress
    [IO.File]::WriteAllText(
        $manifestPath,
        $manifestJson,
        [Text.UTF8Encoding]::new($false)
    )
    Invoke-Checked -Description 'Manifest signing' -Command {
        & $openssl pkeyutl -sign -rawin -inkey $resolvedPrivateKey -in $manifestPath -out $signaturePath
    }
    Invoke-Checked -Description 'Manifest signature verification' -Command {
        & $openssl pkeyutl -verify -pubin -inkey $publicDer -keyform DER -rawin -in $manifestPath -sigfile $signaturePath
    }
    [IO.File]::WriteAllText(
        $checksumPath,
        "$hash  $zipName",
        [Text.ASCIIEncoding]::new()
    )

    if (-not $Publish) {
        Write-Host "Validated release assets: $resolvedOutput"
        return
    }

    $originMain = (& git rev-parse origin/main).Trim()
    if ($LASTEXITCODE -ne 0 -or $originMain -ne $head) {
        throw 'Publish requires the release commit to be present at origin/main.'
    }
    $existingTagCommit = & git rev-parse "$tag^{commit}" 2>$null
    if ($LASTEXITCODE -eq 0) {
        if ($existingTagCommit.Trim() -ne $head) {
            throw "Existing tag $tag does not point to HEAD."
        }
    } else {
        Invoke-Checked -Description 'Release tag creation' -Command {
            & git tag -a $tag -m "Metaplasia $version Development Portable"
        }
    }
    Invoke-Checked -Description 'Release tag push' -Command {
        & git push origin "refs/tags/$tag"
    }

    $githubToken = Get-GitHubToken
    $headers = @{
        Accept = 'application/vnd.github+json'
        Authorization = "Bearer $githubToken"
        'X-GitHub-Api-Version' = '2022-11-28'
        'User-Agent' = 'Metaplasia-local-release-publisher'
    }
    $release = Get-GitHubReleaseByTag `
        -RepositoryName $Repository `
        -Tag $tag `
        -Headers $headers
    if ($null -eq $release) {
        $release = Invoke-GitHubJson `
            -Method Post `
            -Uri "https://api.github.com/repos/$Repository/releases" `
            -Headers $headers `
            -Body ([ordered]@{
                tag_name = $tag
                target_commitish = $head
                name = "Metaplasia $version Development Portable"
                body = "$ReleaseNotes`n`nLocally built and tested. The package manifest is signed by the isolated development update key."
                draft = $true
                prerelease = $true
            })
    } elseif (-not $release.prerelease) {
        throw "Existing release $tag is not a development pre-release."
    }
    foreach ($asset in @(
        @($zipPath, 'application/zip'),
        @($manifestPath, 'application/json'),
        @($signaturePath, 'application/octet-stream'),
        @($checksumPath, 'text/plain')
    )) {
        Set-GitHubReleaseAsset `
            -RepositoryName $Repository `
            -Release $release `
            -Path $asset[0] `
            -ContentType $asset[1] `
            -Headers $headers
    }
    if ($release.draft) {
        Invoke-GitHubJson `
            -Method Patch `
            -Uri "https://api.github.com/repos/$Repository/releases/$($release.id)" `
            -Headers $headers `
            -Body @{ draft = $false; prerelease = $true } | Out-Null
    }

    $channelRelease = Get-GitHubReleaseByTag `
        -RepositoryName $Repository `
        -Tag 'development' `
        -Headers $headers
    if ($null -eq $channelRelease) {
        throw 'The development update channel release does not exist.'
    }
    Set-GitHubReleaseAsset `
        -RepositoryName $Repository `
        -Release $channelRelease `
        -Path $signaturePath `
        -ContentType 'application/octet-stream' `
        -Headers $headers
    $channelRelease = Get-GitHubReleaseByTag `
        -RepositoryName $Repository `
        -Tag 'development' `
        -Headers $headers
    Set-GitHubReleaseAsset `
        -RepositoryName $Repository `
        -Release $channelRelease `
        -Path $manifestPath `
        -ContentType 'application/json' `
        -Headers $headers

    Write-Host "Published $tag and advanced the signed development channel."
} finally {
    $githubToken = $null
    if ($null -ne $temporaryRoot -and (Test-Path -LiteralPath $temporaryRoot)) {
        Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
    Pop-Location
}
