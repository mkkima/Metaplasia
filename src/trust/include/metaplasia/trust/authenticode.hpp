#pragma once

#include "metaplasia/base/status.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace metaplasia::trust {

inline constexpr std::size_t kPublisherThumbprintSize = 32;
using PublisherThumbprint =
    std::array<std::uint8_t, kPublisherThumbprintSize>;

enum class SignatureState : std::uint8_t {
    trusted = 1,
    unsigned_file = 2,
    invalid = 3,
};

struct AuthenticodeResult final {
    SignatureState state{SignatureState::invalid};
    std::uint32_t native_status{0};
    PublisherThumbprint publisher_thumbprint{};
    std::wstring publisher_name;
    std::string detail;
};

struct ComponentSignature final {
    std::string component_name;
    AuthenticodeResult signature;
};

struct ComponentTrustDecision final {
    bool accepted{false};
    bool development_override{false};
    PublisherThumbprint publisher_thumbprint{};
    std::string detail;
};

// Uses the generic Authenticode policy without UI or network retrieval. A
// trusted result includes the SHA-256 thumbprint of the leaf signer.
[[nodiscard]] Result<AuthenticodeResult> VerifyAuthenticode(
    const std::filesystem::path& path);

// Pure set policy: production accepts only trusted files signed by one leaf
// publisher. Development may accept an entirely unsigned set, never a mixed
// or invalid set.
[[nodiscard]] ComponentTrustDecision EvaluateComponentTrust(
    std::span<const ComponentSignature> components,
    bool allow_unsigned_development);

[[nodiscard]] Result<ComponentTrustDecision> VerifyComponentFiles(
    const std::vector<std::pair<std::string, std::filesystem::path>>& files,
    bool allow_unsigned_development);

[[nodiscard]] std::string HexThumbprint(
    const PublisherThumbprint& thumbprint);

}  // namespace metaplasia::trust
