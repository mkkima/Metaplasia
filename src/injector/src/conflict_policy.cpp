#include "metaplasia/injector/conflict_policy.hpp"

#include <algorithm>
#include <array>
#include <cwctype>

namespace metaplasia::injector {
namespace {

bool EqualsIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    return left.size() == right.size() &&
           std::ranges::equal(left, right, [](const wchar_t lhs, const wchar_t rhs) {
               return std::towlower(lhs) == std::towlower(rhs);
           });
}

struct ConflictIdentity final {
    std::wstring_view module_name;
    ConflictingShellCustomizer customizer;
};

// These are exact loader identities, not substring heuristics. Broad matching
// would make an unrelated module a reason to reject shell configuration.
constexpr std::array<ConflictIdentity, 4> kConflictingModules{
    ConflictIdentity{L"windhawk.dll", ConflictingShellCustomizer::windhawk},
    ConflictIdentity{
        L"wincorlib_orig.dll",
        ConflictingShellCustomizer::explorer_patcher},
    ConflictIdentity{
        L"ExplorerPatcher.amd64.dll",
        ConflictingShellCustomizer::explorer_patcher},
    ConflictIdentity{
        L"ep_taskbar.2.amd64.dll",
        ConflictingShellCustomizer::explorer_patcher},
};

}  // namespace

std::optional<ConflictingShellCustomizer> IdentifyConflictingShellModule(
    const std::wstring_view module_name) noexcept {
    const auto match = std::ranges::find_if(
        kConflictingModules,
        [module_name](const ConflictIdentity& identity) {
            return EqualsIgnoreCase(module_name, identity.module_name);
        });
    return match == kConflictingModules.end()
               ? std::nullopt
               : std::optional(match->customizer);
}

std::string_view ConflictingShellCustomizerName(
    const ConflictingShellCustomizer customizer) noexcept {
    switch (customizer) {
        case ConflictingShellCustomizer::windhawk:
            return "Windhawk";
        case ConflictingShellCustomizer::explorer_patcher:
            return "ExplorerPatcher";
        default:
            return "unknown shell customizer";
    }
}

}  // namespace metaplasia::injector
