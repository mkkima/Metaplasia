#pragma once

#include "metaplasia/base/status.hpp"
#include "metaplasia/trust/authenticode.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <span>
#include <vector>

namespace metaplasia::compatibility {

enum class AdapterId : std::uint8_t {
    taskbar_clock = 1,
    file_explorer_title = 2,
    start_menu_xaml = 3,
};

struct WindowsVersion final {
    std::uint32_t major{0};
    std::uint32_t minor{0};
    std::uint32_t build{0};
    std::uint32_t revision{0};

    [[nodiscard]] std::string ToString() const;
    [[nodiscard]] bool operator==(const WindowsVersion&) const noexcept = default;
};

struct ModuleRequirement final {
    std::wstring module_name;
    std::filesystem::path path_relative_to_windows;
    std::string compatibility_key;
};

struct CompatibilityProfile final {
    std::string id;
    AdapterId adapter{AdapterId::taskbar_clock};
    WindowsVersion windows;
    std::vector<ModuleRequirement> modules;
};

struct ModuleObservation final {
    std::wstring module_name;
    std::filesystem::path path;
    std::string compatibility_key;
};

struct CompatibilityDecision final {
    AdapterId adapter{AdapterId::taskbar_clock};
    WindowsVersion windows;
    bool supported{false};
    std::string profile_id;
    std::string detail;
    std::vector<ModuleObservation> modules;
};

struct ProfilePackLoadResult final {
    bool present{false};
    bool loaded{false};
    std::size_t profile_count{0};
    std::string detail;
};

[[nodiscard]] std::string_view AdapterName(AdapterId adapter) noexcept;
// Returns true only for startup observations that can become valid without a
// Windows or Metaplasia update. Permanent identity/profile mismatches remain
// fail-closed.
[[nodiscard]] bool IsTransientRejection(
    const CompatibilityDecision& decision) noexcept;
[[nodiscard]] Result<WindowsVersion> QueryWindowsVersion();

// Pure profile evaluation used by runtime inspection and deterministic tests.
[[nodiscard]] CompatibilityDecision EvaluateProfile(
    const CompatibilityProfile& profile,
    const WindowsVersion& windows,
    const std::vector<ModuleObservation>& observations,
    const std::filesystem::path& windows_directory);

// Inspects PE identities from the images mapped in `process_id`, never from
// potentially replaced files at their reported paths.
[[nodiscard]] Result<CompatibilityDecision> EvaluateProcess(
    AdapterId adapter,
    std::uint32_t process_id);

// Strict little-endian parser for the signed pack payload (RCDATA id 101).
// Exposed for deterministic fuzz/boundary tests; runtime callers should use
// LoadExternalProfilePack so signature and publisher policy cannot be skipped.
[[nodiscard]] Result<std::vector<CompatibilityProfile>> ParseProfilePack(
    std::span<const std::byte> bytes);

[[nodiscard]] Result<ProfilePackLoadResult> LoadExternalProfilePack(
    const std::filesystem::path& pack_path,
    const trust::PublisherThumbprint& expected_publisher,
    bool allow_unsigned_development);

}  // namespace metaplasia::compatibility
