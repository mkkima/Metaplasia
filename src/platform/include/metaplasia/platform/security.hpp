#pragma once

#include "metaplasia/base/status.hpp"

#include <Windows.h>

#include <filesystem>

namespace metaplasia::platform {

// Grants read/execute (and directory traversal) only to Windows packaged app
// groups. This is required for loading the agent into packaged shell hosts such
// as StartMenuExperienceHost. Existing ACL entries are preserved.
[[nodiscard]] Result<void> GrantPackagedApplicationReadExecute(
    const std::filesystem::path& path,
    bool is_directory);

}  // namespace metaplasia::platform
