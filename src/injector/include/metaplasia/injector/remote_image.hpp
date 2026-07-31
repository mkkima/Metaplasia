#pragma once

#include "metaplasia/base/status.hpp"
#include "metaplasia/platform/process.hpp"

#include <Windows.h>

#include <cstdint>
#include <string_view>

namespace metaplasia::injector {

// Resolves an export from the PE image that is actually mapped in the target
// process. The returned address is accepted only when every referenced table
// stays inside the mapped image and the export RVA belongs to an executable
// section. Forwarded exports are intentionally rejected.
[[nodiscard]] Result<std::uintptr_t> ResolveRemoteExportAddress(
    HANDLE process,
    const platform::ProcessModuleInfo& module,
    std::string_view export_name);

}  // namespace metaplasia::injector
