#include "metaplasia/symbols/symbol_resolver.hpp"

#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <vector>

namespace metaplasia::symbols {
namespace {

constexpr std::size_t kMaximumSymbolNameLength = 4096;
constexpr DWORD64 kSyntheticModuleBase = 0x0000000180000000ULL;
std::mutex g_dbghelp_mutex;

class SymbolSession final {
public:
    explicit SymbolSession(std::wstring search_path) {
        const DWORD options = ::SymGetOptions();
        ::SymSetOptions(
            options | SYMOPT_CASE_INSENSITIVE | SYMOPT_DEFERRED_LOADS |
            SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS | SYMOPT_SECURE |
            SYMOPT_UNDNAME);
        initialized_ = ::SymInitializeW(
                           ::GetCurrentProcess(),
                           search_path.empty() ? nullptr : search_path.c_str(),
                           FALSE) != FALSE;
        if (!initialized_) {
            error_ = ::GetLastError();
        }
    }

    ~SymbolSession() {
        if (initialized_) {
            ::SymCleanup(::GetCurrentProcess());
        }
    }

    SymbolSession(const SymbolSession&) = delete;
    SymbolSession& operator=(const SymbolSession&) = delete;

    [[nodiscard]] bool initialized() const noexcept { return initialized_; }
    [[nodiscard]] DWORD error() const noexcept { return error_; }

private:
    bool initialized_{false};
    DWORD error_{ERROR_SUCCESS};
};

bool EqualGuid(const GUID& left, const GUID& right) noexcept {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

}  // namespace

SymbolResolver::SymbolResolver(std::filesystem::path cache_directory)
    : cache_directory_(std::move(cache_directory)) {}

std::filesystem::path SymbolResolver::ExpectedPdbPath(
    const ModuleIdentity& module) const {
    if (!module.pdb.has_value()) {
        return {};
    }
    const auto& file_name = module.pdb->file_name;
    const bool safe_name = !file_name.empty() && file_name.size() <= 255 &&
                           file_name != "." && file_name != ".." &&
                           std::all_of(
                               file_name.begin(),
                               file_name.end(),
                               [](const unsigned char character) {
                                   return std::isalnum(character) != 0 ||
                                          character == '.' || character == '_' ||
                                          character == '-';
                               });
    if (!safe_name) {
        return {};
    }
    return cache_directory_ / file_name / module.pdb->SymbolServerKey() /
           file_name;
}

Result<ResolvedSymbol> SymbolResolver::Resolve(
    const std::filesystem::path& module_path,
    const std::wstring_view symbol_name) const {
    if (symbol_name.empty() || symbol_name.size() > kMaximumSymbolNameLength ||
        symbol_name.find(L'!') != std::wstring_view::npos) {
        return Status(ErrorCode::invalid_argument, "Invalid symbol name");
    }
    auto identity = InspectPeImage(module_path);
    if (!identity.ok()) {
        return identity.status();
    }

    std::wstring search_path = identity.value().path.parent_path().native();
    const auto expected_pdb = ExpectedPdbPath(identity.value());
    std::error_code filesystem_error;
    if (!expected_pdb.empty() &&
        std::filesystem::is_regular_file(expected_pdb, filesystem_error) &&
        !filesystem_error) {
        search_path += L";" + expected_pdb.parent_path().native();
    }

    std::lock_guard lock(g_dbghelp_mutex);
    SymbolSession session(std::move(search_path));
    if (!session.initialized()) {
        return Status::FromWin32("SymInitializeW", session.error());
    }

    static std::atomic<std::uint64_t> alias_sequence{0};
    const std::wstring alias =
        L"metaplasia_" + std::to_wstring(alias_sequence.fetch_add(
                              1,
                              std::memory_order_relaxed));
    const DWORD64 module_base = ::SymLoadModuleExW(
        ::GetCurrentProcess(),
        nullptr,
        identity.value().path.c_str(),
        alias.c_str(),
        kSyntheticModuleBase,
        identity.value().image_size,
        nullptr,
        0);
    if (module_base == 0) {
        return Status::FromWin32("SymLoadModuleExW", ::GetLastError());
    }

    IMAGEHLP_MODULEW64 module_info{};
    module_info.SizeOfStruct = sizeof(module_info);
    if (!::SymGetModuleInfoW64(
            ::GetCurrentProcess(),
            module_base,
            &module_info)) {
        return Status::FromWin32("SymGetModuleInfoW64", ::GetLastError());
    }
    if (module_info.PdbUnmatched || module_info.DbgUnmatched) {
        return Status(ErrorCode::incompatible, "DbgHelp loaded mismatched symbols");
    }
    if (identity.value().pdb.has_value() &&
        module_info.SymType == SymPdb &&
        (!EqualGuid(
             identity.value().pdb->signature,
             module_info.PdbSig70) ||
         identity.value().pdb->age != module_info.PdbAge)) {
        return Status(ErrorCode::incompatible, "Loaded PDB identity does not match PE image");
    }

    const std::wstring qualified_name = alias + L"!" + std::wstring(symbol_name);
    std::vector<std::byte> storage(
        sizeof(SYMBOL_INFOW) +
        kMaximumSymbolNameLength * sizeof(wchar_t));
    auto* symbol = reinterpret_cast<SYMBOL_INFOW*>(storage.data());
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = static_cast<ULONG>(kMaximumSymbolNameLength);
    if (!::SymFromNameW(
            ::GetCurrentProcess(),
            qualified_name.c_str(),
            symbol)) {
        return Status::FromWin32("SymFromNameW", ::GetLastError());
    }
    if (symbol->Address < module_base ||
        symbol->Address - module_base >
            (std::numeric_limits<std::uint32_t>::max)()) {
        return Status(ErrorCode::invalid_data, "Resolved symbol address is outside module range");
    }
    const auto rva = static_cast<std::uint32_t>(symbol->Address - module_base);
    if (rva >= identity.value().image_size) {
        return Status(ErrorCode::invalid_data, "Resolved symbol RVA is outside image");
    }
    auto executable = IsExecutableRva(identity.value().path, rva);
    if (!executable.ok()) {
        return executable.status();
    }
    if (!executable.value()) {
        return Status(ErrorCode::incompatible, "Resolved symbol is not in an executable section");
    }

    ResolvedSymbol resolved;
    resolved.module = std::move(identity).value();
    resolved.name.assign(symbol->Name, symbol->NameLen);
    resolved.rva = rva;
    resolved.size = symbol->Size;
    auto code_fingerprint = FingerprintExecutableCode(
        resolved.module.path,
        resolved.rva);
    if (!code_fingerprint.ok()) {
        return code_fingerprint.status();
    }
    resolved.code_fingerprint = code_fingerprint.value();
    return resolved;
}

}  // namespace metaplasia::symbols
