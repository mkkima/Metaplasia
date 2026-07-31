# Third-party software

Native dependencies are pinned to immutable Git commits in the root
`CMakeLists.txt`; Rust dependencies are fixed by `src-tauri/Cargo.lock`. Review
both files whenever a revision changes.

## Tauri

- Upstream: <https://github.com/tauri-apps/tauri>
- Direct crate versions: `tauri 2.11.5`, `tauri-build 2.6.3`
- Used by: `metaplasia.exe`
- License: Apache-2.0 or MIT; transitive versions and checksums are recorded in
  `src-tauri/Cargo.lock`

Tauri uses the system Microsoft Edge WebView2 Runtime on Windows. WebView2 and
its distribution terms are governed by Microsoft.

## MinHook

- Upstream: <https://github.com/TsudaKageyu/minhook>
- Pinned revision: `c3fcafdc10146beb5919319d0683e44e3c30d537`
- Used by: `metaplasia-agent.dll`
- License: 2-clause BSD license, included in the fetched source tree

## Platform SDKs

Metaplasia uses Microsoft Windows SDK and MSVC runtime interfaces. Their
redistribution terms are governed by the installed Visual Studio and Windows
SDK licenses. A release package must include the appropriate MSVC runtime or
use an approved deployment mechanism.
