#pragma once

#include "metaplasia/base/status.hpp"

#include <string>
#include <string_view>

namespace metaplasia {

[[nodiscard]] Result<std::string> WideToUtf8(std::wstring_view value);
[[nodiscard]] Result<std::wstring> Utf8ToWide(std::string_view value);

}  // namespace metaplasia
