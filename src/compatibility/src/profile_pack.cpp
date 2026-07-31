#include "metaplasia/compatibility/catalog.hpp"

#include "metaplasia/base/unique_handle.hpp"
#include "metaplasia/base/utf.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace metaplasia::compatibility {
namespace {

constexpr std::uint32_t kPackMagic = 0x5043504DU;  // "MPCP"
constexpr std::uint16_t kPackVersion = 1;
constexpr std::uint16_t kPackHeaderSize = 16;
constexpr std::size_t kMaximumPackSize = 1024U * 1024U;
constexpr std::size_t kMaximumProfiles = 64;
constexpr std::size_t kMaximumModulesPerProfile = 16;
constexpr std::size_t kMaximumProfileRecordSize = 64U * 1024U;
constexpr std::size_t kMaximumProfileIdLength = 127;
constexpr std::size_t kMaximumModuleNameLength = 127;
constexpr std::size_t kMaximumRelativePathLength = 511;
constexpr std::size_t kMaximumCompatibilityKeyLength = 511;
constexpr int kProfileResourceId = 101;

class Reader final {
public:
    explicit Reader(const std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    template <typename T>
    [[nodiscard]] bool Read(T& value) noexcept {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
        if (sizeof(T) > remaining()) {
            return false;
        }
        value = 0;
        for (std::size_t index = 0; index < sizeof(T); ++index) {
            value |= static_cast<T>(
                         std::to_integer<unsigned int>(bytes_[offset_ + index]))
                     << (index * 8U);
        }
        offset_ += sizeof(T);
        return true;
    }

    [[nodiscard]] bool ReadBytes(
        const std::size_t length,
        std::span<const std::byte>& value) noexcept {
        if (length > remaining()) {
            return false;
        }
        value = bytes_.subspan(offset_, length);
        offset_ += length;
        return true;
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }
    [[nodiscard]] bool empty() const noexcept { return remaining() == 0; }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{0};
};

class ResourceModule final {
public:
    explicit ResourceModule(HMODULE module) noexcept : module_(module) {}
    ~ResourceModule() {
        if (module_ != nullptr) {
            ::FreeLibrary(module_);
        }
    }

    ResourceModule(const ResourceModule&) = delete;
    ResourceModule& operator=(const ResourceModule&) = delete;

    [[nodiscard]] HMODULE get() const noexcept { return module_; }

private:
    HMODULE module_{nullptr};
};

bool IsSafeIdentifier(
    const std::string_view value,
    const bool allow_dot) noexcept {
    if (value.empty()) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [allow_dot](const unsigned char character) {
            return std::isalnum(character) != 0 || character == '-' ||
                   character == '_' || (allow_dot && character == '.');
        });
}

Result<std::string> ReadAscii(
    Reader& reader,
    const std::size_t length,
    const std::size_t maximum,
    const bool allow_dot,
    const char* description) {
    if (length == 0 || length > maximum) {
        return Status(
            ErrorCode::invalid_data,
            std::string("Invalid ") + description + " length");
    }
    std::span<const std::byte> bytes;
    if (!reader.ReadBytes(length, bytes)) {
        return Status(
            ErrorCode::invalid_data,
            std::string("Truncated ") + description);
    }
    std::string value(length, '\0');
    std::memcpy(value.data(), bytes.data(), bytes.size());
    if (!IsSafeIdentifier(value, allow_dot)) {
        return Status(
            ErrorCode::invalid_data,
            std::string("Unsafe ") + description);
    }
    return value;
}

Result<std::filesystem::path> ReadRelativePath(
    Reader& reader,
    const std::size_t length) {
    if (length == 0 || length > kMaximumRelativePathLength) {
        return Status(ErrorCode::invalid_data, "Invalid module path length");
    }
    std::span<const std::byte> bytes;
    if (!reader.ReadBytes(length, bytes)) {
        return Status(ErrorCode::invalid_data, "Truncated module path");
    }
    std::string utf8(length, '\0');
    std::memcpy(utf8.data(), bytes.data(), bytes.size());
    const bool safe_path_text = std::all_of(
        utf8.begin(),
        utf8.end(),
        [](const unsigned char character) {
            return std::isalnum(character) != 0 || character == '-' ||
                   character == '_' || character == '.' ||
                   character == '\\';
        });
    if (!safe_path_text) {
        return Status(
            ErrorCode::invalid_data,
            "Module path contains an unsafe character");
    }
    auto wide = Utf8ToWide(utf8);
    if (!wide.ok()) {
        return wide.status();
    }
    std::filesystem::path path(wide.value());
    if (path.empty() || path.is_absolute() || path.has_root_path()) {
        return Status(ErrorCode::invalid_data, "Module path must be relative");
    }
    for (const auto& component : path) {
        if (component == L"." || component == L"..") {
            return Status(
                ErrorCode::invalid_data,
                "Module path contains traversal components");
        }
    }
    return path.lexically_normal();
}

bool EqualsIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    return _wcsicmp(std::wstring(left).c_str(), std::wstring(right).c_str()) == 0;
}

Result<CompatibilityProfile> ParseProfileRecord(
    const std::span<const std::byte> bytes) {
    Reader reader(bytes);
    std::uint8_t adapter_value = 0;
    std::uint8_t reserved_byte = 0;
    std::uint16_t module_count = 0;
    WindowsVersion windows;
    std::uint16_t profile_id_length = 0;
    std::uint16_t reserved_word = 0;
    if (!reader.Read(adapter_value) || !reader.Read(reserved_byte) ||
        !reader.Read(module_count) || !reader.Read(windows.major) ||
        !reader.Read(windows.minor) || !reader.Read(windows.build) ||
        !reader.Read(windows.revision) || !reader.Read(profile_id_length) ||
        !reader.Read(reserved_word)) {
        return Status(ErrorCode::invalid_data, "Truncated profile record");
    }
    if (adapter_value < static_cast<std::uint8_t>(AdapterId::taskbar_clock) ||
        adapter_value > static_cast<std::uint8_t>(AdapterId::start_menu_xaml) ||
        reserved_byte != 0 || reserved_word != 0 || module_count == 0 ||
        module_count > kMaximumModulesPerProfile || windows.major == 0 ||
        windows.build == 0) {
        return Status(ErrorCode::invalid_data, "Invalid profile record header");
    }
    auto profile_id = ReadAscii(
        reader,
        profile_id_length,
        kMaximumProfileIdLength,
        true,
        "profile id");
    if (!profile_id.ok()) {
        return profile_id.status();
    }

    CompatibilityProfile profile;
    profile.id = std::move(profile_id).value();
    profile.adapter = static_cast<AdapterId>(adapter_value);
    profile.windows = windows;
    profile.modules.reserve(module_count);
    for (std::size_t index = 0; index < module_count; ++index) {
        std::uint16_t name_length = 0;
        std::uint16_t path_length = 0;
        std::uint16_t key_length = 0;
        std::uint16_t module_reserved = 0;
        if (!reader.Read(name_length) || !reader.Read(path_length) ||
            !reader.Read(key_length) || !reader.Read(module_reserved) ||
            module_reserved != 0) {
            return Status(ErrorCode::invalid_data, "Invalid module record header");
        }
        auto name = ReadAscii(
            reader,
            name_length,
            kMaximumModuleNameLength,
            true,
            "module name");
        auto path = ReadRelativePath(reader, path_length);
        auto key = ReadAscii(
            reader,
            key_length,
            kMaximumCompatibilityKeyLength,
            true,
            "compatibility key");
        if (!name.ok()) {
            return name.status();
        }
        if (!path.ok()) {
            return path.status();
        }
        if (!key.ok()) {
            return key.status();
        }
        auto wide_name = Utf8ToWide(name.value());
        if (!wide_name.ok()) {
            return wide_name.status();
        }
        if (!EqualsIgnoreCase(
                path.value().filename().native(),
                wide_name.value())) {
            return Status(
                ErrorCode::invalid_data,
                "Module name does not match its relative path");
        }
        const auto duplicate = std::find_if(
            profile.modules.begin(),
            profile.modules.end(),
            [&wide_name](const ModuleRequirement& requirement) {
                return EqualsIgnoreCase(
                    requirement.module_name,
                    wide_name.value());
            });
        if (duplicate != profile.modules.end()) {
            return Status(
                ErrorCode::invalid_data,
                "Profile contains a duplicate module name");
        }
        profile.modules.push_back(ModuleRequirement{
            std::move(wide_name).value(),
            std::move(path).value(),
            std::move(key).value()});
    }
    if (!reader.empty()) {
        return Status(ErrorCode::invalid_data, "Profile record has trailing data");
    }
    return profile;
}

bool IsZeroThumbprint(
    const trust::PublisherThumbprint& thumbprint) noexcept {
    return std::all_of(
        thumbprint.begin(),
        thumbprint.end(),
        [](const std::uint8_t value) { return value == 0; });
}

}  // namespace

Result<std::vector<CompatibilityProfile>> ParseProfilePack(
    const std::span<const std::byte> bytes) {
    if (bytes.size() < kPackHeaderSize || bytes.size() > kMaximumPackSize) {
        return Status(ErrorCode::invalid_data, "Invalid profile pack size");
    }
    Reader reader(bytes);
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t header_size = 0;
    std::uint32_t total_size = 0;
    std::uint16_t profile_count = 0;
    std::uint16_t reserved = 0;
    if (!reader.Read(magic) || !reader.Read(version) ||
        !reader.Read(header_size) || !reader.Read(total_size) ||
        !reader.Read(profile_count) || !reader.Read(reserved)) {
        return Status(ErrorCode::invalid_data, "Truncated profile pack header");
    }
    if (magic != kPackMagic || version != kPackVersion ||
        header_size != kPackHeaderSize || total_size != bytes.size() ||
        profile_count == 0 || profile_count > kMaximumProfiles || reserved != 0) {
        return Status(ErrorCode::invalid_data, "Invalid profile pack header");
    }

    std::vector<CompatibilityProfile> profiles;
    profiles.reserve(profile_count);
    for (std::size_t index = 0; index < profile_count; ++index) {
        std::uint32_t record_size = 0;
        if (!reader.Read(record_size) || record_size < sizeof(record_size) ||
            record_size > kMaximumProfileRecordSize ||
            record_size - sizeof(record_size) > reader.remaining()) {
            return Status(ErrorCode::invalid_data, "Invalid profile record size");
        }
        std::span<const std::byte> record;
        if (!reader.ReadBytes(record_size - sizeof(record_size), record)) {
            return Status(ErrorCode::invalid_data, "Truncated profile record");
        }
        auto profile = ParseProfileRecord(record);
        if (!profile.ok()) {
            return profile.status();
        }
        const auto duplicate = std::find_if(
            profiles.begin(),
            profiles.end(),
            [&profile](const CompatibilityProfile& existing) {
                return existing.adapter == profile.value().adapter &&
                       existing.windows == profile.value().windows;
            });
        if (duplicate != profiles.end()) {
            return Status(
                ErrorCode::invalid_data,
                "Profile pack contains duplicate adapter/version entries");
        }
        profiles.push_back(std::move(profile).value());
    }
    if (!reader.empty()) {
        return Status(ErrorCode::invalid_data, "Profile pack has trailing data");
    }
    return profiles;
}

// Defined in catalog.cpp; installation is kept private to the compatibility
// library so callers cannot inject unverified profiles.
Result<void> InstallVerifiedExternalProfiles(
    std::vector<CompatibilityProfile> profiles);

Result<ProfilePackLoadResult> LoadExternalProfilePack(
    const std::filesystem::path& pack_path,
    const trust::PublisherThumbprint& expected_publisher,
    const bool allow_unsigned_development) {
    if (pack_path.empty() || !pack_path.is_absolute()) {
        return Status(ErrorCode::invalid_argument, "Profile pack path must be absolute");
    }
    std::error_code filesystem_error;
    if (!std::filesystem::exists(pack_path, filesystem_error)) {
        if (filesystem_error) {
            return Status(
                ErrorCode::win32_error,
                "Unable to inspect profile pack: " + filesystem_error.message(),
                static_cast<std::uint32_t>(filesystem_error.value()));
        }
        return ProfilePackLoadResult{
            false,
            false,
            0,
            "No external compatibility pack is installed"};
    }
    if (!std::filesystem::is_regular_file(pack_path, filesystem_error) ||
        filesystem_error) {
        return Status(ErrorCode::invalid_data, "Profile pack is not a regular file");
    }

    auto canonical_path =
        std::filesystem::weakly_canonical(pack_path, filesystem_error);
    if (filesystem_error || canonical_path.empty() ||
        !canonical_path.is_absolute()) {
        return Status(
            ErrorCode::invalid_data,
            "Unable to canonicalize the profile pack path");
    }

    // Pin the canonical file against deletion or replacement until the mapped
    // resource, Authenticode signer, and parsed payload have all been accepted.
    UniqueHandle pinned_file(::CreateFileW(
        canonical_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!pinned_file) {
        return Status::FromWin32(
            "CreateFileW(profile pack pin)",
            ::GetLastError());
    }

    ResourceModule resource_module(::LoadLibraryExW(
        canonical_path.c_str(),
        nullptr,
        LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE));
    if (resource_module.get() == nullptr) {
        return Status::FromWin32(
            "LoadLibraryExW(profile pack resource)",
            ::GetLastError());
    }

    auto signature = trust::VerifyAuthenticode(canonical_path);
    if (!signature.ok()) {
        return signature.status();
    }
    if (IsZeroThumbprint(expected_publisher)) {
        if (!allow_unsigned_development ||
            signature.value().state != trust::SignatureState::unsigned_file) {
            return Status(
                ErrorCode::access_denied,
                "Profile pack has no pinned component publisher");
        }
    } else if (
        signature.value().state != trust::SignatureState::trusted ||
        signature.value().publisher_thumbprint != expected_publisher) {
        return Status(
            ErrorCode::access_denied,
            "Profile pack Authenticode publisher does not match the components");
    }

    const HRSRC resource = ::FindResourceW(
        resource_module.get(),
        MAKEINTRESOURCEW(kProfileResourceId),
        RT_RCDATA);
    if (resource == nullptr) {
        return Status::FromWin32(
            "FindResourceW(profile pack)",
            ::GetLastError());
    }
    const DWORD resource_size = ::SizeofResource(resource_module.get(), resource);
    if (resource_size == 0 || resource_size > kMaximumPackSize) {
        return Status(ErrorCode::invalid_data, "Invalid profile pack resource size");
    }
    const HGLOBAL loaded_resource =
        ::LoadResource(resource_module.get(), resource);
    const void* resource_data = loaded_resource != nullptr
                                    ? ::LockResource(loaded_resource)
                                    : nullptr;
    if (resource_data == nullptr) {
        return Status::FromWin32(
            "LoadResource(profile pack)",
            ::GetLastError());
    }
    const auto bytes = std::span(
        static_cast<const std::byte*>(resource_data),
        static_cast<std::size_t>(resource_size));
    auto profiles = ParseProfilePack(bytes);
    if (!profiles.ok()) {
        return profiles.status();
    }
    const std::size_t profile_count = profiles.value().size();
    auto installed =
        InstallVerifiedExternalProfiles(std::move(profiles).value());
    if (!installed.ok()) {
        return installed.status();
    }
    return ProfilePackLoadResult{
        true,
        true,
        profile_count,
        "Verified external compatibility pack loaded"};
}

}  // namespace metaplasia::compatibility
