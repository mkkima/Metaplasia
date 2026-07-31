#pragma once

#include "metaplasia/base/status.hpp"

#include <filesystem>
#include <string>

namespace metaplasia {

[[nodiscard]] Result<std::filesystem::path> ExecutablePath();
[[nodiscard]] Result<std::filesystem::path> ExecutableDirectory();
[[nodiscard]] Result<std::filesystem::path> LocalAppDataDirectory();
[[nodiscard]] Result<std::filesystem::path> MetaplasiaDataDirectory();
[[nodiscard]] Result<std::wstring> CurrentUserSidString();
[[nodiscard]] Result<std::uint32_t> CurrentSessionId();

}  // namespace metaplasia
