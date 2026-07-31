#include "metaplasia/symbols/pe_image.hpp"

#include "metaplasia/base/unique_handle.hpp"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

namespace metaplasia::symbols {
namespace {

constexpr std::uint32_t kRsdsSignature = 0x53445352U;
constexpr std::size_t kMaximumSections = 96;
constexpr std::size_t kMaximumDebugEntries = 64;
constexpr std::size_t kMaximumCodeViewRecordSize = 4096;

bool IsSafePdbFileName(const std::string_view name) noexcept {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](const unsigned char character) {
        return std::isalnum(character) != 0 || character == '.' ||
               character == '_' || character == '-';
    });
}

class MappedFile final {
public:
    ~MappedFile() {
        if (view_ != nullptr) {
            ::UnmapViewOfFile(view_);
        }
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;
    MappedFile(MappedFile&& other) noexcept
        : file_(std::move(other.file_)),
          mapping_(std::move(other.mapping_)),
          view_(std::exchange(other.view_, nullptr)),
          size_(std::exchange(other.size_, 0)) {}
    MappedFile& operator=(MappedFile&&) = delete;

    static Result<MappedFile> Open(const std::filesystem::path& path) {
        UniqueHandle file(::CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr));
        if (!file) {
            return Status::FromWin32("CreateFileW(PE image)", ::GetLastError());
        }

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file.get(), &size)) {
            return Status::FromWin32("GetFileSizeEx(PE image)", ::GetLastError());
        }
        if (size.QuadPart <= 0 ||
            static_cast<unsigned long long>(size.QuadPart) >
                (std::numeric_limits<std::size_t>::max)()) {
            return Status(ErrorCode::invalid_data, "Invalid PE image size");
        }

        UniqueHandle mapping(::CreateFileMappingW(
            file.get(),
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr));
        if (!mapping) {
            return Status::FromWin32(
                "CreateFileMappingW(PE image)",
                ::GetLastError());
        }
        const void* view = ::MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0);
        if (view == nullptr) {
            return Status::FromWin32("MapViewOfFile(PE image)", ::GetLastError());
        }
        return MappedFile(
            std::move(file),
            std::move(mapping),
            view,
            static_cast<std::size_t>(size.QuadPart));
    }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        return {static_cast<const std::byte*>(view_), size_};
    }

private:
    MappedFile(
        UniqueHandle file,
        UniqueHandle mapping,
        const void* view,
        const std::size_t size) noexcept
        : file_(std::move(file)),
          mapping_(std::move(mapping)),
          view_(view),
          size_(size) {}

    UniqueHandle file_;
    UniqueHandle mapping_;
    const void* view_{nullptr};
    std::size_t size_{0};
};

template <typename T>
Result<T> ReadObject(
    const std::span<const std::byte> bytes,
    const std::size_t offset) {
    if (offset > bytes.size() || sizeof(T) > bytes.size() - offset) {
        return Status(ErrorCode::invalid_data, "Truncated PE image");
    }
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

template <typename T>
Result<T> ReadRemoteObject(
    const HANDLE process,
    const std::uintptr_t module_base,
    const std::uint32_t mapped_image_size,
    const std::size_t offset) {
    if (process == nullptr || module_base == 0 || mapped_image_size == 0 ||
        offset > mapped_image_size || sizeof(T) > mapped_image_size - offset ||
        module_base >
            (std::numeric_limits<std::uintptr_t>::max)() - offset) {
        return Status(ErrorCode::invalid_data, "Mapped PE read is out of bounds");
    }
    T value{};
    SIZE_T bytes_read = 0;
    if (!::ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(module_base + offset),
            &value,
            sizeof(value),
            &bytes_read) ||
        bytes_read != sizeof(value)) {
        return Status::FromWin32(
            "ReadProcessMemory(mapped PE)",
            ::GetLastError());
    }
    return value;
}

Result<std::vector<std::byte>> ReadRemoteBytes(
    const HANDLE process,
    const std::uintptr_t module_base,
    const std::uint32_t mapped_image_size,
    const std::size_t offset,
    const std::size_t size) {
    if (process == nullptr || module_base == 0 || mapped_image_size == 0 ||
        size == 0 || offset > mapped_image_size ||
        size > mapped_image_size - offset ||
        module_base >
            (std::numeric_limits<std::uintptr_t>::max)() - offset) {
        return Status(ErrorCode::invalid_data, "Mapped PE read is out of bounds");
    }
    std::vector<std::byte> bytes(size);
    SIZE_T bytes_read = 0;
    if (!::ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(module_base + offset),
            bytes.data(),
            bytes.size(),
            &bytes_read) ||
        bytes_read != bytes.size()) {
        return Status::FromWin32(
            "ReadProcessMemory(mapped PE data)",
            ::GetLastError());
    }
    return bytes;
}

struct ParsedImage final {
    ModuleIdentity identity;
    std::uint32_t size_of_headers{0};
    std::vector<IMAGE_SECTION_HEADER> sections;
};

Result<std::size_t> RvaToFileOffset(
    const ParsedImage& image,
    const std::uint32_t rva,
    const std::size_t required_size,
    const std::size_t file_size) {
    if (rva < image.size_of_headers) {
        if (rva > file_size || required_size > file_size - rva) {
            return Status(ErrorCode::invalid_data, "PE header RVA is out of bounds");
        }
        return static_cast<std::size_t>(rva);
    }

    for (const auto& section : image.sections) {
        const std::uint32_t section_span =
            (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (rva < section.VirtualAddress ||
            rva - section.VirtualAddress >= section_span) {
            continue;
        }
        const std::uint32_t delta = rva - section.VirtualAddress;
        if (delta > section.SizeOfRawData ||
            required_size > section.SizeOfRawData - delta) {
            return Status(
                ErrorCode::invalid_data,
                "PE RVA points outside section raw data");
        }
        const std::uint64_t offset =
            static_cast<std::uint64_t>(section.PointerToRawData) + delta;
        if (offset > file_size || required_size > file_size - offset) {
            return Status(ErrorCode::invalid_data, "PE section offset is out of bounds");
        }
        return static_cast<std::size_t>(offset);
    }
    return Status(ErrorCode::invalid_data, "PE RVA does not map to a section");
}

Result<ParsedImage> ParseHeaders(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes) {
    auto dos = ReadObject<IMAGE_DOS_HEADER>(bytes, 0);
    if (!dos.ok()) {
        return dos.status();
    }
    if (dos.value().e_magic != IMAGE_DOS_SIGNATURE ||
        dos.value().e_lfanew < 0) {
        return Status(ErrorCode::invalid_data, "Invalid DOS header");
    }
    const auto nt_offset = static_cast<std::size_t>(dos.value().e_lfanew);
    auto signature = ReadObject<DWORD>(bytes, nt_offset);
    if (!signature.ok() || signature.value() != IMAGE_NT_SIGNATURE) {
        return Status(ErrorCode::invalid_data, "Invalid PE signature");
    }
    auto file_header = ReadObject<IMAGE_FILE_HEADER>(
        bytes,
        nt_offset + sizeof(DWORD));
    if (!file_header.ok()) {
        return file_header.status();
    }
    if (file_header.value().NumberOfSections == 0 ||
        file_header.value().NumberOfSections > kMaximumSections) {
        return Status(ErrorCode::invalid_data, "Invalid PE section count");
    }

    const std::size_t optional_offset =
        nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    auto optional_magic = ReadObject<WORD>(bytes, optional_offset);
    if (!optional_magic.ok()) {
        return optional_magic.status();
    }

    ParsedImage parsed;
    parsed.identity.path = path;
    parsed.identity.file_size = bytes.size();
    parsed.identity.machine = file_header.value().Machine;
    parsed.identity.timestamp = file_header.value().TimeDateStamp;
    IMAGE_DATA_DIRECTORY debug_directory{};
    if (optional_magic.value() == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (file_header.value().SizeOfOptionalHeader <
            sizeof(IMAGE_OPTIONAL_HEADER64)) {
            return Status(ErrorCode::invalid_data, "Truncated PE32+ optional header");
        }
        auto optional = ReadObject<IMAGE_OPTIONAL_HEADER64>(bytes, optional_offset);
        if (!optional.ok()) {
            return optional.status();
        }
        parsed.identity.image_size = optional.value().SizeOfImage;
        parsed.identity.checksum = optional.value().CheckSum;
        parsed.size_of_headers = optional.value().SizeOfHeaders;
        if (optional.value().NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DEBUG) {
            debug_directory =
                optional.value().DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        }
    } else if (optional_magic.value() == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (file_header.value().SizeOfOptionalHeader <
            sizeof(IMAGE_OPTIONAL_HEADER32)) {
            return Status(ErrorCode::invalid_data, "Truncated PE32 optional header");
        }
        auto optional = ReadObject<IMAGE_OPTIONAL_HEADER32>(bytes, optional_offset);
        if (!optional.ok()) {
            return optional.status();
        }
        parsed.identity.image_size = optional.value().SizeOfImage;
        parsed.identity.checksum = optional.value().CheckSum;
        parsed.size_of_headers = optional.value().SizeOfHeaders;
        if (optional.value().NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DEBUG) {
            debug_directory =
                optional.value().DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        }
    } else {
        return Status(ErrorCode::incompatible, "Unsupported PE optional header");
    }
    if (parsed.identity.image_size == 0 || parsed.size_of_headers == 0) {
        return Status(ErrorCode::invalid_data, "Invalid PE image dimensions");
    }

    const std::size_t section_offset =
        optional_offset + file_header.value().SizeOfOptionalHeader;
    parsed.sections.reserve(file_header.value().NumberOfSections);
    for (std::size_t index = 0;
         index < file_header.value().NumberOfSections;
         ++index) {
        auto section = ReadObject<IMAGE_SECTION_HEADER>(
            bytes,
            section_offset + index * sizeof(IMAGE_SECTION_HEADER));
        if (!section.ok()) {
            return section.status();
        }
        parsed.sections.push_back(section.value());
    }

    if (debug_directory.VirtualAddress == 0 || debug_directory.Size == 0) {
        return parsed;
    }
    if (debug_directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0) {
        return Status(ErrorCode::invalid_data, "Invalid PE debug directory size");
    }
    auto debug_offset = RvaToFileOffset(
        parsed,
        debug_directory.VirtualAddress,
        debug_directory.Size,
        bytes.size());
    if (!debug_offset.ok()) {
        return debug_offset.status();
    }

    const std::size_t debug_count =
        debug_directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (std::size_t index = 0; index < debug_count; ++index) {
        auto entry = ReadObject<IMAGE_DEBUG_DIRECTORY>(
            bytes,
            debug_offset.value() + index * sizeof(IMAGE_DEBUG_DIRECTORY));
        if (!entry.ok()) {
            return entry.status();
        }
        if (entry.value().Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
            entry.value().SizeOfData < sizeof(DWORD) + sizeof(GUID) + sizeof(DWORD)) {
            continue;
        }
        const std::size_t codeview_offset = entry.value().PointerToRawData;
        const std::size_t codeview_size = entry.value().SizeOfData;
        if (codeview_offset > bytes.size() ||
            codeview_size > bytes.size() - codeview_offset) {
            return Status(ErrorCode::invalid_data, "CodeView record is out of bounds");
        }
        auto cv_signature = ReadObject<DWORD>(bytes, codeview_offset);
        if (!cv_signature.ok() || cv_signature.value() != kRsdsSignature) {
            continue;
        }
        auto pdb_guid = ReadObject<GUID>(bytes, codeview_offset + sizeof(DWORD));
        auto pdb_age = ReadObject<DWORD>(
            bytes,
            codeview_offset + sizeof(DWORD) + sizeof(GUID));
        if (!pdb_guid.ok() || !pdb_age.ok()) {
            return Status(ErrorCode::invalid_data, "Truncated RSDS record");
        }

        const std::size_t name_offset =
            codeview_offset + sizeof(DWORD) + sizeof(GUID) + sizeof(DWORD);
        const std::size_t name_capacity =
            codeview_offset + codeview_size - name_offset;
        const char* name = reinterpret_cast<const char*>(bytes.data() + name_offset);
        const auto terminator = std::find(name, name + name_capacity, '\0');
        if (terminator == name + name_capacity || terminator == name) {
            return Status(ErrorCode::invalid_data, "Invalid RSDS PDB path");
        }
        std::string_view pdb_path(name, static_cast<std::size_t>(terminator - name));
        const auto separator = pdb_path.find_last_of("\\/");
        const auto basename = separator == std::string_view::npos
                                  ? pdb_path
                                  : pdb_path.substr(separator + 1);
        if (!IsSafePdbFileName(basename)) {
            return Status(ErrorCode::invalid_data, "Invalid RSDS PDB filename");
        }
        parsed.identity.pdb =
            PdbIdentity{pdb_guid.value(), pdb_age.value(), std::string(basename)};
        break;
    }
    return parsed;
}

std::string HexValue(const std::uint64_t value, const std::size_t width) {
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0')
           << std::setw(static_cast<int>(width)) << value;
    return output.str();
}

}  // namespace

std::string PdbIdentity::SymbolServerKey() const {
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0')
           << std::setw(8) << signature.Data1
           << std::setw(4) << signature.Data2
           << std::setw(4) << signature.Data3;
    for (const auto byte : signature.Data4) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    output << std::uppercase << std::hex << age;
    return output.str();
}

std::string ModuleIdentity::CompatibilityKey() const {
    std::string key = HexValue(machine, 4) + "-" + HexValue(timestamp, 8) +
                      "-" + HexValue(image_size, 8) + "-" +
                      HexValue(checksum, 8);
    if (pdb.has_value()) {
        key += "-" + pdb->file_name + "-" + pdb->SymbolServerKey();
    } else {
        key += "-no-pdb";
    }
    return key;
}

Result<ModuleIdentity> InspectPeImage(const std::filesystem::path& path) {
    if (path.empty() || !path.is_absolute()) {
        return Status(ErrorCode::invalid_argument, "PE image path must be absolute");
    }
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error || !std::filesystem::is_regular_file(canonical, error) || error) {
        return Status(ErrorCode::not_found, "PE image does not exist");
    }
    auto mapped = MappedFile::Open(canonical);
    if (!mapped.ok()) {
        return mapped.status();
    }
    auto parsed = ParseHeaders(canonical, mapped.value().bytes());
    if (!parsed.ok()) {
        return parsed.status();
    }
    return std::move(parsed).value().identity;
}

Result<ModuleIdentity> InspectMappedPeImage(
    const HANDLE process,
    const std::uintptr_t module_base,
    const std::uint32_t mapped_image_size,
    const std::filesystem::path& reported_path) {
    if (process == nullptr || module_base == 0 || mapped_image_size == 0 ||
        reported_path.empty() || !reported_path.is_absolute()) {
        return Status(
            ErrorCode::invalid_argument,
            "Invalid mapped PE inspection arguments");
    }

    auto dos = ReadRemoteObject<IMAGE_DOS_HEADER>(
        process,
        module_base,
        mapped_image_size,
        0);
    if (!dos.ok()) {
        return dos.status();
    }
    if (dos.value().e_magic != IMAGE_DOS_SIGNATURE ||
        dos.value().e_lfanew < 0) {
        return Status(ErrorCode::invalid_data, "Invalid mapped DOS header");
    }
    const auto nt_offset = static_cast<std::size_t>(dos.value().e_lfanew);
    auto signature = ReadRemoteObject<DWORD>(
        process,
        module_base,
        mapped_image_size,
        nt_offset);
    if (!signature.ok() || signature.value() != IMAGE_NT_SIGNATURE) {
        return Status(ErrorCode::invalid_data, "Invalid mapped PE signature");
    }
    auto file_header = ReadRemoteObject<IMAGE_FILE_HEADER>(
        process,
        module_base,
        mapped_image_size,
        nt_offset + sizeof(DWORD));
    if (!file_header.ok()) {
        return file_header.status();
    }
    if (file_header.value().NumberOfSections == 0 ||
        file_header.value().NumberOfSections > kMaximumSections) {
        return Status(ErrorCode::invalid_data, "Invalid mapped PE section count");
    }

    const std::size_t optional_offset =
        nt_offset + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
    auto optional_magic = ReadRemoteObject<WORD>(
        process,
        module_base,
        mapped_image_size,
        optional_offset);
    if (!optional_magic.ok()) {
        return optional_magic.status();
    }

    ModuleIdentity identity;
    identity.path = reported_path;
    identity.machine = file_header.value().Machine;
    identity.timestamp = file_header.value().TimeDateStamp;
    IMAGE_DATA_DIRECTORY debug_directory{};
    if (optional_magic.value() == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (file_header.value().SizeOfOptionalHeader <
            sizeof(IMAGE_OPTIONAL_HEADER64)) {
            return Status(
                ErrorCode::invalid_data,
                "Truncated mapped PE32+ optional header");
        }
        auto optional = ReadRemoteObject<IMAGE_OPTIONAL_HEADER64>(
            process,
            module_base,
            mapped_image_size,
            optional_offset);
        if (!optional.ok()) {
            return optional.status();
        }
        identity.image_size = optional.value().SizeOfImage;
        identity.checksum = optional.value().CheckSum;
        if (optional.value().NumberOfRvaAndSizes >
            IMAGE_DIRECTORY_ENTRY_DEBUG) {
            debug_directory =
                optional.value().DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        }
    } else if (optional_magic.value() == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (file_header.value().SizeOfOptionalHeader <
            sizeof(IMAGE_OPTIONAL_HEADER32)) {
            return Status(
                ErrorCode::invalid_data,
                "Truncated mapped PE32 optional header");
        }
        auto optional = ReadRemoteObject<IMAGE_OPTIONAL_HEADER32>(
            process,
            module_base,
            mapped_image_size,
            optional_offset);
        if (!optional.ok()) {
            return optional.status();
        }
        identity.image_size = optional.value().SizeOfImage;
        identity.checksum = optional.value().CheckSum;
        if (optional.value().NumberOfRvaAndSizes >
            IMAGE_DIRECTORY_ENTRY_DEBUG) {
            debug_directory =
                optional.value().DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
        }
    } else {
        return Status(
            ErrorCode::incompatible,
            "Unsupported mapped PE optional header");
    }
    if (identity.image_size == 0 ||
        identity.image_size != mapped_image_size) {
        return Status(
            ErrorCode::invalid_data,
            "Mapped PE image size does not match the module snapshot");
    }

    if (debug_directory.VirtualAddress == 0 || debug_directory.Size == 0) {
        return identity;
    }
    if (debug_directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0 ||
        debug_directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY) >
            kMaximumDebugEntries) {
        return Status(
            ErrorCode::invalid_data,
            "Invalid mapped PE debug directory size");
    }

    const std::size_t debug_count =
        debug_directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    for (std::size_t index = 0; index < debug_count; ++index) {
        auto entry = ReadRemoteObject<IMAGE_DEBUG_DIRECTORY>(
            process,
            module_base,
            mapped_image_size,
            static_cast<std::size_t>(debug_directory.VirtualAddress) +
                index * sizeof(IMAGE_DEBUG_DIRECTORY));
        if (!entry.ok()) {
            return entry.status();
        }
        constexpr std::size_t codeview_header_size =
            sizeof(DWORD) + sizeof(GUID) + sizeof(DWORD);
        if (entry.value().Type != IMAGE_DEBUG_TYPE_CODEVIEW ||
            entry.value().AddressOfRawData == 0 ||
            entry.value().SizeOfData < codeview_header_size ||
            entry.value().SizeOfData > kMaximumCodeViewRecordSize) {
            continue;
        }
        auto codeview = ReadRemoteBytes(
            process,
            module_base,
            mapped_image_size,
            entry.value().AddressOfRawData,
            entry.value().SizeOfData);
        if (!codeview.ok()) {
            return codeview.status();
        }
        auto cv_signature = ReadObject<DWORD>(codeview.value(), 0);
        if (!cv_signature.ok() || cv_signature.value() != kRsdsSignature) {
            continue;
        }
        auto pdb_guid = ReadObject<GUID>(codeview.value(), sizeof(DWORD));
        auto pdb_age = ReadObject<DWORD>(
            codeview.value(),
            sizeof(DWORD) + sizeof(GUID));
        if (!pdb_guid.ok() || !pdb_age.ok()) {
            return Status(
                ErrorCode::invalid_data,
                "Truncated mapped RSDS record");
        }

        const auto name_offset = codeview_header_size;
        const char* name = reinterpret_cast<const char*>(
            codeview.value().data() + name_offset);
        const auto name_capacity = codeview.value().size() - name_offset;
        const auto terminator = std::find(name, name + name_capacity, '\0');
        if (terminator == name + name_capacity || terminator == name) {
            return Status(
                ErrorCode::invalid_data,
                "Invalid mapped RSDS PDB path");
        }
        const std::string_view pdb_path(
            name,
            static_cast<std::size_t>(terminator - name));
        const auto separator = pdb_path.find_last_of("\\/");
        const auto basename = separator == std::string_view::npos
                                  ? pdb_path
                                  : pdb_path.substr(separator + 1);
        if (!IsSafePdbFileName(basename)) {
            return Status(
                ErrorCode::invalid_data,
                "Invalid mapped RSDS PDB filename");
        }
        identity.pdb = PdbIdentity{
            pdb_guid.value(),
            pdb_age.value(),
            std::string(basename)};
        break;
    }
    return identity;
}

Result<bool> IsExecutableRva(
    const std::filesystem::path& path,
    const std::uint32_t rva) {
    auto mapped = MappedFile::Open(path);
    if (!mapped.ok()) {
        return mapped.status();
    }
    auto parsed = ParseHeaders(path, mapped.value().bytes());
    if (!parsed.ok()) {
        return parsed.status();
    }
    if (rva >= parsed.value().identity.image_size) {
        return false;
    }
    for (const auto& section : parsed.value().sections) {
        const auto size = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (rva >= section.VirtualAddress &&
            rva - section.VirtualAddress < size) {
            return (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        }
    }
    return false;
}

Result<std::array<std::uint8_t, kCodeFingerprintSize>>
FingerprintExecutableCode(
    const std::filesystem::path& path,
    const std::uint32_t rva) {
    auto mapped = MappedFile::Open(path);
    if (!mapped.ok()) {
        return mapped.status();
    }
    auto parsed = ParseHeaders(path, mapped.value().bytes());
    if (!parsed.ok()) {
        return parsed.status();
    }

    bool executable = false;
    for (const auto& section : parsed.value().sections) {
        const auto size = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (rva >= section.VirtualAddress &&
            rva - section.VirtualAddress < size) {
            executable =
                (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
            break;
        }
    }
    if (!executable) {
        return Status(
            ErrorCode::incompatible,
            "Code fingerprint RVA is not executable");
    }
    auto offset = RvaToFileOffset(
        parsed.value(),
        rva,
        kCodeFingerprintSize,
        mapped.value().bytes().size());
    if (!offset.ok()) {
        return offset.status();
    }

    std::array<std::uint8_t, kCodeFingerprintSize> digest{};
    const NTSTATUS hash_status = ::BCryptHash(
        BCRYPT_SHA256_ALG_HANDLE,
        nullptr,
        0,
        reinterpret_cast<PUCHAR>(
            const_cast<std::byte*>(
                mapped.value().bytes().data() + offset.value())),
        static_cast<ULONG>(kCodeFingerprintSize),
        digest.data(),
        static_cast<ULONG>(digest.size()));
    if (hash_status < 0) {
        return Status(
            ErrorCode::internal_error,
            "BCryptHash(SHA-256) failed",
            static_cast<std::uint32_t>(hash_status));
    }
    return digest;
}

std::string HexDigest(
    const std::array<std::uint8_t, kCodeFingerprintSize>& digest) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : digest) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

}  // namespace metaplasia::symbols
