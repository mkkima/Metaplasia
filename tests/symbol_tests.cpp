#include "metaplasia/symbols/pe_image.hpp"
#include "metaplasia/symbols/symbol_resolver.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::filesystem::path ModulePath(const HMODULE module) {
    std::array<wchar_t, 32768> path{};
    const DWORD length = ::GetModuleFileNameW(
        module,
        path.data(),
        static_cast<DWORD>(path.size()));
    Require(length != 0 && length < path.size(), "get module path");
    return std::filesystem::path(std::wstring_view(path.data(), length));
}

}  // namespace

int main() {
    using namespace metaplasia::symbols;

    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    Require(ntdll != nullptr, "locate ntdll");
    const auto module_path = ModulePath(ntdll);

    auto identity = InspectPeImage(module_path);
    Require(identity.ok(), "inspect a system PE image");
    Require(identity.value().machine == IMAGE_FILE_MACHINE_AMD64, "machine identity");
    Require(identity.value().image_size != 0, "image size identity");
    Require(identity.value().timestamp != 0, "timestamp identity");
    Require(!identity.value().CompatibilityKey().empty(), "compatibility key");
    if (identity.value().pdb.has_value()) {
        Require(!identity.value().pdb->file_name.empty(), "PDB filename");
        Require(!identity.value().pdb->SymbolServerKey().empty(), "PDB key");
    }

    auto mapped_identity = InspectMappedPeImage(
        ::GetCurrentProcess(),
        reinterpret_cast<std::uintptr_t>(ntdll),
        identity.value().image_size,
        module_path);
    Require(mapped_identity.ok(), "inspect the actually mapped PE image");
    Require(
        mapped_identity.value().CompatibilityKey() ==
            identity.value().CompatibilityKey(),
        "mapped and on-disk identities match for a stable loaded module");
    auto wrong_mapped_size = InspectMappedPeImage(
        ::GetCurrentProcess(),
        reinterpret_cast<std::uintptr_t>(ntdll),
        identity.value().image_size - 1U,
        module_path);
    Require(
        !wrong_mapped_size.ok(),
        "mapped identity rejects a snapshot size mismatch");

    const FARPROC rtl_get_version = ::GetProcAddress(ntdll, "RtlGetVersion");
    Require(rtl_get_version != nullptr, "locate RtlGetVersion export");
    const auto runtime_rva = static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(rtl_get_version) -
        reinterpret_cast<std::uintptr_t>(ntdll));
    auto executable = IsExecutableRva(module_path, runtime_rva);
    Require(executable.ok() && executable.value(), "export RVA is executable");
    auto headers_executable = IsExecutableRva(module_path, 0);
    Require(
        headers_executable.ok() && !headers_executable.value(),
        "header RVA is not executable");

    SymbolResolver resolver(std::filesystem::temp_directory_path());
    auto resolved = resolver.Resolve(module_path, L"RtlGetVersion");
    Require(resolved.ok(), "resolve exported symbol through DbgHelp");
    Require(resolved.value().rva == runtime_rva, "resolved RVA matches loader export");
    auto fingerprint = FingerprintExecutableCode(module_path, runtime_rva);
    Require(fingerprint.ok(), "fingerprint executable bytes");
    Require(
        fingerprint.value() == resolved.value().code_fingerprint,
        "resolver carries exact code fingerprint");
    Require(
        HexDigest(fingerprint.value()).size() == kCodeFingerprintSize * 2,
        "SHA-256 digest formatting");

    std::cout << "Symbol and PE identity tests passed\n";
    return EXIT_SUCCESS;
}
