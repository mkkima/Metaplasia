#pragma once

#include "metaplasia/base/status.hpp"

#include <Windows.h>
#include <guiddef.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace metaplasia::symbols {

struct PdbIdentity final {
    GUID signature{};
    std::uint32_t age{0};
    std::string file_name;

    [[nodiscard]] std::string SymbolServerKey() const;
};

struct ModuleIdentity final {
    std::filesystem::path path;
    std::uint64_t file_size{0};
    std::uint16_t machine{0};
    std::uint32_t timestamp{0};
    std::uint32_t image_size{0};
    std::uint32_t checksum{0};
    std::optional<PdbIdentity> pdb;

    [[nodiscard]] std::string CompatibilityKey() const;
};

[[nodiscard]] Result<ModuleIdentity> InspectPeImage(
    const std::filesystem::path& path);

// Reads identity fields from the PE image actually mapped in a process. This
// avoids approving an old mapped module using a replacement file now present
// at the same on-disk path.
[[nodiscard]] Result<ModuleIdentity> InspectMappedPeImage(
    HANDLE process,
    std::uintptr_t module_base,
    std::uint32_t mapped_image_size,
    const std::filesystem::path& reported_path);

[[nodiscard]] Result<bool> IsExecutableRva(
    const std::filesystem::path& path,
    std::uint32_t rva);

inline constexpr std::size_t kCodeFingerprintSize = 32;

[[nodiscard]] Result<std::array<std::uint8_t, kCodeFingerprintSize>>
FingerprintExecutableCode(
    const std::filesystem::path& path,
    std::uint32_t rva);

[[nodiscard]] std::string HexDigest(
    const std::array<std::uint8_t, kCodeFingerprintSize>& digest);

}  // namespace metaplasia::symbols
