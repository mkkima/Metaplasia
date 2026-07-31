#pragma once

#include <optional>
#include <string_view>

namespace metaplasia::injector {

enum class ConflictingShellCustomizer {
    windhawk,
    explorer_patcher,
};

[[nodiscard]] std::optional<ConflictingShellCustomizer>
IdentifyConflictingShellModule(std::wstring_view module_name) noexcept;

[[nodiscard]] std::string_view ConflictingShellCustomizerName(
    ConflictingShellCustomizer customizer) noexcept;

}  // namespace metaplasia::injector
