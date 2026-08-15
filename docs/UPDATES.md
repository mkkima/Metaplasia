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

The control center supports two modes. With **Automatic updates** enabled, it
checks the configured channel in the background, verifies and downloads a newer
complete package, then installs it and restarts Metaplasia plus the required
Windows shell components. Turning the switch off disables that automatic path.
The manual **Check now**, **Download update**, and **Install and restart** flow
remains available and requires confirmation before installation.

The dedicated **Updates** page also shows the running version and channel,
verification state, downloaded-cache size, the local metadata directory, and a
bounded installation history. History is stored in
`%LOCALAPPDATA%\Metaplasia\updates\update-history.json`; it contains at most 50
events and never contains the portable binaries themselves. **Clear downloads**
removes only recognized updater cache files and deliberately leaves history,
policy metadata, settings, and unknown files untouched.

## Signed rollback

The rollback list is built from older releases on the running build's channel.
A release is offered only when its versioned GitHub Release contains the
channel's complete portable ZIP, `portable-update.json`, and
`portable-update.json.sig`. Selecting a version does not trust the catalog by
itself: Metaplasia downloads that exact version's manifest, verifies it with
the channel's embedded Ed25519 public key, validates its canonical version and
GitHub URL, then verifies the ZIP size and SHA-256 before using the same staged,
transactional replacement helper as a forward update.

A rollback preserves `%LOCALAPPDATA%\Metaplasia` settings and history, but an
older binary may not understand settings introduced by a newer version. The UI
therefore requires explicit confirmation and disables automatic updates before
replacement. The departed version is also recorded as skipped for that channel
so it is not immediately installed again; **Allow again** clears this policy.
Stable and development skip policies are isolated. No permanent copy of an old
binary package is retained: rollback downloads the selected signed release
when requested.

Before any replacement, the application verifies an Ed25519 signature over the
raw manifest, validates the GitHub release URL, downloads with strict size
limits, and verifies the ZIP SHA-256. The helper verifies all of this again,
extracts only the exact allowed filenames, stages them on the portable
directory's volume, and keeps rollback copies until the complete replacement
succeeds.

Only an explicit release command for a version tag is a release flag. Ordinary
pushes to `main`, feature branches, and non-version tags do not publish an
update. The publisher also refuses a tag unless the tag version exactly matches
`CMakeLists.txt`, `src-tauri/Cargo.toml`, and `src-tauri/tauri.conf.json`.

The local development publisher creates and pushes an explicit
`dev-vMAJOR.MINOR.PATCH` tag, publishes an unsigned Debug portable ZIP as a
GitHub pre-release through the GitHub REST API, and advances the `development`
channel. It does not use GitHub Actions or `gh`. The package manifest is signed
with an isolated development Ed25519 key, so development portable copies support
the same automatic check/download and manual install flow. Development copies
never consume stable metadata, stable copies never consume development metadata,
and the development channel does not weaken the fail-closed `v*` release
workflow.

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

Generate a second, independent Ed25519 key for development updates. Commit only
its raw Base64 public key in `src-tauri/development-update-public-key.txt`. Keep
the private PEM outside the repository and pass its path to the local publisher.

Never reuse either private key across channels. A compromised development key
must not authorize a stable update.

Create a GitHub environment named `release`, require approval for it, and
protect the `v*` tag pattern so an ordinary repository write cannot silently
turn an arbitrary commit into signed binaries. The workflow targets this
environment before any signing secret is materialized.

Each publisher derives the public key from the supplied private key and fails
closed if it does not match the key trusted by the corresponding channel.
Release builds embed only a public key. Private signing material must remain
outside the repository.

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

A development portable build is built, tested, signed, tagged, and published
locally. The private PEM must be the same key already trusted by installed
development copies:

```powershell
.\tools\publish-development-portable.ps1 `
  -PrivateKeyPath D:\secure\metaplasia-development-update-private.pem `
  -Publish
```

The script requires a clean `main` commit already present at `origin/main`,
validates that all project versions match, derives and compares the public key,
builds the complete five-file Debug runtime, runs native tests, signs and verifies
the manifest, creates the annotated tag, publishes the pre-release, and advances
the channel metadata signature-first. Omitting `-Publish` performs all local
validation and writes assets below `out/releases` without changing GitHub.

The original `dev-v0.1.0` package was published without an embedded development
key and cannot be changed retroactively. It requires one manual replacement with
`dev-v0.1.1` or newer. Every development version from `0.1.1` onward can update
within the development channel.

Versions distributed before this update client exists require one final manual
replacement with an updater-enabled signed release. Subsequent versions update
through the portable channel.

Rollback has the same compatibility boundary. Releases without a signed
versioned manifest are intentionally omitted, so `dev-v0.1.0` cannot be used as
an in-app rollback target. The first development rollback target is
`dev-v0.1.1`.

Do not rotate the Ed25519 key or remove manifest schema `1` without first
shipping a bridge release that trusts both channels. Older portable copies
know only their embedded public key and supported manifest schema; changing
either abruptly would permanently strand those versions.
