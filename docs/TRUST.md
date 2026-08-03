# Component trust and compatibility packs

Metaplasia treats the host, watchdog, and injected agent as one release unit.
Production startup is fail closed: every component must have a valid embedded
Authenticode signature and every leaf signer certificate must have the same
SHA-256 thumbprint. Matching a publisher display name is never sufficient.

## Startup policy

The host verifies these sibling files before changing ACLs, launching the
watchdog, opening its command pipe, or allowing a new agent load:

- `metaplasia-host.exe`;
- `metaplasia-watchdog.exe`;
- `metaplasia-agent.dll`.

`WinVerifyTrust` runs without UI or network retrieval. Revocation is checked
for the signing chain except the root, using only cached data. The verified
file remains open without delete sharing while trust state and signer identity
are read. Only a zero trust result is accepted. Every verify operation closes
its WinTrust state, including failures.

The injector receives the accepted signer thumbprint and verifies the agent
again immediately before a new `LoadLibraryW` operation. Configure-only
teardown does not depend on the current on-disk file's signature: it addresses
only the exact agent path already mapped in the target, so recovery remains
possible after a damaged installation.

Debug builds define `METAPLASIA_DEVELOPMENT_TRUST` for the host and CLI. That
policy accepts a set only when every checked file is unsigned. It rejects
invalid signatures and mixed signed/unsigned sets. Release builds never define
this switch and therefore cannot start from ordinary unsigned local build
output.

Diagnostics do not require a running host:

```powershell
.\build\debug\bin\metaplasia-cli.exe component-trust
.\build\debug\bin\metaplasia-cli.exe authenticode-info C:\absolute\file.dll
```

The verifier is designed for embedded PE Authenticode signatures. Windows
files authenticated only through an operating-system catalog are not the
Metaplasia release model.

## External compatibility pack

An optional `metaplasia-compatibility-pack.dll` may be installed beside the
host. It is a resource-only PE image; Metaplasia maps it with
`LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE`, so no
entry point or initializer runs. A canonical read handle without delete sharing
pins the exact file across mapping, signature verification, and payload parsing.

If the file exists, its embedded Authenticode signature must be trusted and
its exact leaf certificate SHA-256 thumbprint must match the component set.
An invalid, malformed, differently signed, or conflicting pack blocks host
startup. Debug accepts an unsigned pack only together with an entirely
unsigned Debug component set.

The payload is `RCDATA` resource ID `101`, at most 1 MiB. All integers are
little endian and all records are packed without alignment padding:

```text
pack-header:
  u32 magic = 0x5043504D            # MPCP
  u16 format-version = 1
  u16 header-size = 16
  u32 total-size
  u16 profile-count                 # 1..64
  u16 reserved = 0

profile-record:
  u32 record-size                   # includes this field, max 64 KiB
  u8  adapter                       # 1 taskbar, 2 Explorer, 3 Start
  u8  reserved = 0
  u16 module-count                  # 1..16
  u32 windows-major
  u32 windows-minor
  u32 windows-build
  u32 windows-UBR
  u16 profile-id-byte-count
  u16 reserved = 0
  byte profile-id[profile-id-byte-count]
  module-record modules[module-count]

module-record:
  u16 module-name-byte-count
  u16 relative-path-byte-count
  u16 compatibility-key-byte-count
  u16 reserved = 0
  byte module-name[module-name-byte-count]
  byte relative-path[relative-path-byte-count]
  byte compatibility-key[compatibility-key-byte-count]
```

Identifiers and module names are bounded ASCII. Relative paths allow only
ASCII letters, digits, `-`, `_`, `.`, and `\`; they must be relative, contain
no `.`/`..` component, and end in the declared module name. Compatibility keys
are bounded ASCII identifiers. Duplicate module names, duplicate
adapter/version entries, trailing bytes, unsupported versions, and nonzero
reserved fields are rejected.

A pack may add an exact adapter/version profile but cannot replace a compiled
profile. There are no build ranges, wildcards, runtime learning, or override
switches. Profiles are installed in memory only after the complete PE,
signature, payload, and collision checks succeed.

To create a distributable pack, generate the binary payload, embed it with a
resource script such as `101 RCDATA "profiles.bin"`, link a resource-only DLL
with no entry point, then sign that DLL with the same leaf certificate as the
release components. Certificate rotation requires re-signing both the
components and pack as one release set.

## Release requirements

The signing pipeline must:

1. produce the final binaries before signing;
2. apply embedded SHA-256 Authenticode signatures to host, watchdog, agent,
   CLI, UI, and any compatibility pack with one leaf signer certificate;
3. timestamp signatures according to the release policy;
4. run `component-trust` and the Release tests on the signed artifacts;
5. sign the portable update manifest with the separate Ed25519 release key;
6. stage and transactionally replace the complete portable set, never mutating
   an agent while it remains loaded in a shell process.

## Win32 references

- [WinVerifyTrust](https://learn.microsoft.com/en-us/windows/win32/api/wintrust/nf-wintrust-winverifytrust)
- [WINTRUST_DATA](https://learn.microsoft.com/en-us/windows/win32/api/wintrust/ns-wintrust-wintrust_data)
- [CertGetCertificateContextProperty](https://learn.microsoft.com/en-us/windows/win32/api/wincrypt/nf-wincrypt-certgetcertificatecontextproperty)
- [LoadLibraryEx](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)
