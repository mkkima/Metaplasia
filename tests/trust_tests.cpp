#include "metaplasia/trust/authenticode.hpp"

#include <Windows.h>

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::filesystem::path ExecutablePath() {
    std::array<wchar_t, 32768> path{};
    const DWORD length = ::GetModuleFileNameW(
        nullptr,
        path.data(),
        static_cast<DWORD>(path.size()));
    Require(length != 0 && length < path.size(), "resolve test executable path");
    return std::filesystem::path(std::wstring_view(path.data(), length));
}

metaplasia::trust::ComponentSignature Trusted(
    std::string name,
    const std::uint8_t thumbprint_byte) {
    metaplasia::trust::AuthenticodeResult signature;
    signature.state = metaplasia::trust::SignatureState::trusted;
    signature.publisher_thumbprint.fill(thumbprint_byte);
    signature.detail = "trusted";
    return {std::move(name), std::move(signature)};
}

metaplasia::trust::ComponentSignature Unsigned(std::string name) {
    metaplasia::trust::AuthenticodeResult signature;
    signature.state = metaplasia::trust::SignatureState::unsigned_file;
    signature.detail = "unsigned";
    return {std::move(name), std::move(signature)};
}

}  // namespace

int main() {
    using namespace metaplasia::trust;

    std::vector<ComponentSignature> signed_set{
        Trusted("host", 0x42),
        Trusted("watchdog", 0x42),
        Trusted("agent", 0x42)};
    auto signed_decision = EvaluateComponentTrust(signed_set, false);
    Require(signed_decision.accepted, "same trusted publisher accepted");
    Require(!signed_decision.development_override, "signed set is production trust");
    Require(
        signed_decision.publisher_thumbprint.front() == 0x42,
        "publisher thumbprint retained");

    auto mismatched = signed_set;
    mismatched.back() = Trusted("agent", 0x24);
    Require(
        !EvaluateComponentTrust(mismatched, true).accepted,
        "publisher mismatch rejected even in development");

    std::vector<ComponentSignature> unsigned_set{
        Unsigned("host"),
        Unsigned("watchdog"),
        Unsigned("agent")};
    auto development = EvaluateComponentTrust(unsigned_set, true);
    Require(development.accepted, "entirely unsigned Debug set accepted");
    Require(development.development_override, "development override reported");
    Require(
        !EvaluateComponentTrust(unsigned_set, false).accepted,
        "unsigned production set rejected");

    auto mixed = signed_set;
    mixed.back() = Unsigned("agent");
    Require(
        !EvaluateComponentTrust(mixed, true).accepted,
        "mixed signed and unsigned set rejected");

    auto invalid = signed_set;
    invalid.back().signature.state = SignatureState::invalid;
    invalid.back().signature.detail = "bad digest";
    Require(
        !EvaluateComponentTrust(invalid, true).accepted,
        "invalid signature rejected");
    Require(
        !EvaluateComponentTrust({}, true).accepted,
        "empty component set rejected");

    const auto executable = ExecutablePath();
    auto actual = VerifyAuthenticode(executable);
    Require(actual.ok(), "inspect current PE Authenticode state");
    Require(
        actual.value().state != SignatureState::invalid,
        "fresh local test binary is unsigned or validly signed");
    auto verified_files = VerifyComponentFiles(
        {{"first", executable}, {"second", executable}},
        true);
    Require(
        verified_files.ok() && verified_files.value().accepted,
        "file-set verification composes Authenticode and set policy");

    auto missing = VerifyAuthenticode(
        executable.parent_path() / L"definitely-missing-component.exe");
    Require(!missing.ok(), "missing component rejected before trust evaluation");
    Require(
        HexThumbprint(signed_decision.publisher_thumbprint).size() == 64,
        "SHA-256 publisher thumbprint formatting");

    std::cout << "Component trust tests passed\n";
    return EXIT_SUCCESS;
}
