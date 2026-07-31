#include "metaplasia/platform/security.hpp"

#include "metaplasia/base/unique_handle.hpp"

#include <Aclapi.h>
#include <Sddl.h>

#include <array>

namespace metaplasia::platform {
namespace {

Result<void> GrantSidReadExecute(
    const std::filesystem::path& path,
    PSID sid,
    const bool is_directory) {
    PACL old_acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD get_result = ::GetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        &old_acl,
        nullptr,
        &descriptor);
    UniqueLocalMemory descriptor_memory(reinterpret_cast<HLOCAL>(descriptor));
    if (get_result != ERROR_SUCCESS) {
        return Status::FromWin32("GetNamedSecurityInfoW", get_result);
    }

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions =
        FILE_GENERIC_READ | FILE_GENERIC_EXECUTE | SYNCHRONIZE;
    // SET_ACCESS replaces our trustee's explicit entry instead of accumulating
    // duplicate ACEs every time the per-user host starts.
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = is_directory
                                ? SUB_CONTAINERS_AND_OBJECTS_INHERIT
                                : NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    access.Trustee.ptstrName = static_cast<LPWSTR>(sid);

    PACL new_acl = nullptr;
    const DWORD acl_result =
        ::SetEntriesInAclW(1, &access, old_acl, &new_acl);
    UniqueLocalMemory acl_memory(reinterpret_cast<HLOCAL>(new_acl));
    if (acl_result != ERROR_SUCCESS) {
        return Status::FromWin32("SetEntriesInAclW", acl_result);
    }

    const DWORD set_result = ::SetNamedSecurityInfoW(
        const_cast<LPWSTR>(path.c_str()),
        SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION,
        nullptr,
        nullptr,
        new_acl,
        nullptr);
    if (set_result != ERROR_SUCCESS) {
        return Status::FromWin32("SetNamedSecurityInfoW", set_result);
    }
    return {};
}

}  // namespace

Result<void> GrantPackagedApplicationReadExecute(
    const std::filesystem::path& path,
    const bool is_directory) {
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        return Status(
            ErrorCode::win32_error,
            "Unable to inspect ACL target: " + filesystem_error.message(),
            static_cast<std::uint32_t>(filesystem_error.value()));
    }
    if (!exists) {
        return Status(ErrorCode::not_found, "ACL target does not exist");
    }

    // ALL APPLICATION PACKAGES and ALL RESTRICTED APPLICATION PACKAGES.
    constexpr std::array<const wchar_t*, 2> package_group_sids{
        L"S-1-15-2-1",
        L"S-1-15-2-2"};

    for (const wchar_t* sid_text : package_group_sids) {
        PSID raw_sid = nullptr;
        if (!::ConvertStringSidToSidW(sid_text, &raw_sid)) {
            return Status::FromWin32(
                "ConvertStringSidToSidW",
                ::GetLastError());
        }
        UniqueLocalMemory sid_memory(reinterpret_cast<HLOCAL>(raw_sid));
        auto grant = GrantSidReadExecute(path, raw_sid, is_directory);
        if (!grant.ok()) {
            return grant.status();
        }
    }
    return {};
}

}  // namespace metaplasia::platform
