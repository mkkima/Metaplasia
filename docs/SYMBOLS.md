# Symbols and compatibility

Private Windows implementation details change between cumulative updates. A
symbol name alone is therefore insufficient evidence that a hook is safe.
Metaplasia models symbol resolution and compatibility approval as separate
steps.

## Module identity

`module-info` parses a PE image as data and reports:

- machine type;
- COFF timestamp;
- `SizeOfImage` and image checksum;
- CodeView RSDS PDB filename, GUID, and age when present.

```powershell
.\build\debug\bin\metaplasia-cli.exe module-info C:\Windows\System32\ntdll.dll
```

The resulting compatibility key identifies one exact module build. PE offsets,
section counts, raw sizes, debug-directory entries, and CodeView strings are
bounded before access. PDB filenames are reduced to a basename and restricted
to safe ASCII filename characters before being used in a cache path.

`compatibility-report` uses a second parser for images actually mapped in a
shell process. It reads bounded PE/RSDS fields with `ReadProcessMemory` and
requires the mapped `SizeOfImage` to match the process module snapshot. This
prevents a newly serviced on-disk file from approving an older image still
loaded at the same path. See [Windows compatibility catalog](COMPATIBILITY.md).

## Local resolution

```powershell
.\build\debug\bin\metaplasia-cli.exe resolve-symbol `
    C:\Windows\System32\ntdll.dll RtlGetVersion
```

DbgHelp is documented as single-threaded, so every DbgHelp session is guarded
by a process-wide mutex. Metaplasia starts a non-invasive session, loads only
the requested image, rejects mismatched PDB metadata, converts the result to an
RVA, verifies that the RVA is inside an executable PE section, and prints a
SHA-256 fingerprint of the first 32 code bytes.

The resolver does not contact the network. For a module with RSDS metadata it
also checks this exact local cache layout:

```text
%LOCALAPPDATA%\Metaplasia\symbols\
  <pdb-name>\<GUID-without-separators><age-in-hex>\<pdb-name>
```

This is compatible with the downstream layout used by Microsoft symbol-server
tools. Populate it manually from a trusted source and preserve the exact key
printed by `module-info`.

## Approval rules

A successful symbol resolution is diagnostic information, not permission to
inject a private-symbol hook. The current compiled catalog approves the existing
Win32/XAML adapters by exact Windows revision and mapped module identities. A
future private-symbol feature may become eligible only when an installed,
signed compatibility manifest additionally matches all of the following:

1. Windows architecture and target process adapter.
2. Complete module compatibility key.
3. Exact requested decorated symbol name.
4. Resolved RVA in an executable section.
5. Expected SHA-256 code fingerprint or another reviewed semantic invariant.
6. Agent ABI and feature implementation version.

The host remains offline and fail closed. Private symbols must already exist in
the local cache and match a compiled or locally installed signed compatibility
profile before they can be used.

## References

- [SymInitialize](https://learn.microsoft.com/windows/win32/api/dbghelp/nf-dbghelp-syminitialize)
- [SymLoadModuleEx](https://learn.microsoft.com/windows/win32/api/dbghelp/nf-dbghelp-symloadmoduleex)
- [Using a symbol server](https://learn.microsoft.com/windows-hardware/drivers/debugger/using-a-symbol-server)
- [Advanced SymSrv cache layout](https://learn.microsoft.com/windows-hardware/drivers/debugger/advanced-symsrv-use)
