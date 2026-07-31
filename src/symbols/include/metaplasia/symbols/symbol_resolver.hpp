#pragma once

#include "metaplasia/base/status.hpp"
#include "metaplasia/symbols/pe_image.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace metaplasia::symbols {

struct ResolvedSymbol final {
    ModuleIdentity module;
    std::wstring name;
    std::uint32_t rva{0};
    std::uint32_t size{0};
    std::array<std::uint8_t, kCodeFingerprintSize> code_fingerprint{};
};

class SymbolResolver final {
public:
    explicit SymbolResolver(std::filesystem::path cache_directory);

    [[nodiscard]] Result<ResolvedSymbol> Resolve(
        const std::filesystem::path& module_path,
        std::wstring_view symbol_name) const;

    [[nodiscard]] std::filesystem::path ExpectedPdbPath(
        const ModuleIdentity& module) const;

private:
    std::filesystem::path cache_directory_;
};

}  // namespace metaplasia::symbols
