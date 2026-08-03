# Portable updates

Metaplasia remains a portable application. There is no MSI, NSIS installer,
Windows service, or machine-wide update agent. A release is one ZIP containing
the complete runtime set:

- `metaplasia.exe`
- `metaplasia-host.exe`
- `metaplasia-watchdog.exe`
- `metaplasia-cli.exe`
- `metaplasia-agent.dll`

User settings are stored separately under `%LOCALAPPDATA%\Metaplasia` and are
not part of the archive, so a full binary replacement preserves them.
Because releases are complete rather than binary deltas, an updater-enabled
copy may jump directly across multiple versions.

## Update behavior

The control center supports a manual check and an optional automatic background
check/download. Installation is never silent: it requires explicit confirmation
because Explorer and Start must briefly restart to release the injected DLL.

Before any replacement, the application verifies an Ed25519 signature over the
raw manifest, validates the GitHub release URL, downloads with strict size
limits, and verifies the ZIP SHA-256. The helper verifies all of this again,
extracts only the exact allowed filenames, stages them on the portable
directory's volume, and keeps rollback copies until the complete replacement
succeeds.

Only a pushed tag matching `vMAJOR.MINOR.PATCH` is a release flag. Ordinary
pushes to `main`, feature branches, and non-version tags do not publish an
update. The workflow also refuses a tag unless the tag version exactly matches
`CMakeLists.txt`, `src-tauri/Cargo.toml`, and `src-tauri/tauri.conf.json`.

## One-time GitHub repository setup

Generate a dedicated Ed25519 key. Never reuse the Authenticode certificate key
and never commit the private key:

```powershell
openssl genpkey -algorithm Ed25519 -out metaplasia-update-private.pem
openssl pkey -in metaplasia-update-private.pem -pubout -outform DER -out metaplasia-update-public.der
$publicDer = [IO.File]::ReadAllBytes((Resolve-Path .\metaplasia-update-public.der))
$publicRaw = $publicDer[($publicDer.Length - 32)..($publicDer.Length - 1)]
[Convert]::ToBase64String($publicRaw)
[Convert]::ToBase64String([IO.File]::ReadAllBytes((Resolve-Path .\metaplasia-update-private.pem)))
```

Configure these GitHub Actions secrets:

- `METAPLASIA_UPDATE_PUBLIC_KEY`: the first Base64 value (raw 32-byte public key).
- `METAPLASIA_UPDATE_PRIVATE_KEY_PEM_B64`: the second Base64 value.
- `METAPLASIA_CODESIGN_PFX_B64`: Base64 of the Authenticode PFX used for all five binaries.
- `METAPLASIA_CODESIGN_PASSWORD`: the PFX password.

Create a GitHub environment named `release`, require approval for it, and
protect the `v*` tag pattern so an ordinary repository write cannot silently
turn an arbitrary commit into signed binaries. The workflow targets this
environment before any signing secret is materialized.

The workflow derives the public key from the private key and fails closed if it
does not match `METAPLASIA_UPDATE_PUBLIC_KEY`. Release builds embed only the
public key. The private update key and PFX exist only as GitHub secrets and
temporary runner files.

## Publishing a release

First change the version in all three project files, commit it to `main`, and
push normally. That push does not release anything. After the commit is final:

```powershell
git tag v0.2.0
git push origin v0.2.0
```

The tag workflow builds and tests Release, Authenticode-signs every component,
creates the ZIP, creates and signs `portable-update.json`, and publishes the
assets to a GitHub Release. Existing updater-enabled portable copies will then
see the release through the automatic or manual check.

Versions distributed before this update client exists require one final manual
replacement with an updater-enabled signed release. Subsequent versions update
through the portable channel.

Do not rotate the Ed25519 key or remove manifest schema `1` without first
shipping a bridge release that trusts both channels. Older portable copies
know only their embedded public key and supported manifest schema; changing
either abruptly would permanently strand those versions.
