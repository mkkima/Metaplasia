#include "metaplasia/injector/remote_image.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace metaplasia::injector {
namespace {

constexpr std::size_t kMaximumExportNameLength = 127;
constexpr std::uint32_t kMaximumPeSections = 96;
constexpr std::uint32_t kMaximumNamedExports = 65536;

[[nodiscard]] bool IsImageRangeValid(
    const std::uint32_t rva,
    const std::size_t size,
    const std::uint32_t image_size) noexcept {
    return rva <= image_size && size <= image_size - rva;
}

[[nodiscard]] Result<std::uintptr_t> RemoteAddress(
    const platform::ProcessModuleInfo& module,
    const std::uint32_t rva,
    const std::size_t size) {
    if (module.base_address == 0 || module.image_size == 0 ||
        !IsImageRangeValid(rva, size, module.image_size) ||
        module.base_address >
            (std::numeric_limits<std::uintptr_t>::max)() - rva) {
        return Status(ErrorCode::invalid_data, "Remote PE range is invalid");
    }
    return module.base_address + rva;
}

template <typename T>
[[nodiscard]] Result<T> ReadRemoteObject(
    const HANDLE process,
    const platform::ProcessModuleInfo& module,
    const std::uint32_t rva) {
    auto address = RemoteAddress(module, rva, sizeof(T));
    if (!address.ok()) {
        return address.status();
    }
    T value{};
    SIZE_T bytes_read = 0;
    if (!::ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(address.value()),
            &value,
            sizeof(value),
            &bytes_read) ||
        bytes_read != sizeof(value)) {
        return Status::FromWin32("ReadProcessMemory(remote PE)", ::GetLastError());
    }
    return value;
}

template <typename T>
[[nodiscard]] Result<std::vector<T>> ReadRemoteArray(
    const HANDLE process,
    const platform::ProcessModuleInfo& module,
    const std::uint32_t rva,
    const std::uint32_t count) {
    if (count == 0 ||
        count > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) {
        return Status(ErrorCode::invalid_data, "Remote PE array size is invalid");
    }
    const std::size_t byte_count = static_cast<std::size_t>(count) * sizeof(T);
    auto address = RemoteAddress(module, rva, byte_count);
    if (!address.ok()) {
        return address.status();
    }
    std::vector<T> values(count);
    SIZE_T bytes_read = 0;
    if (!::ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(address.value()),
            values.data(),
            byte_count,
            &bytes_read) ||
        bytes_read != byte_count) {
        return Status::FromWin32("ReadProcessMemory(remote PE array)", ::GetLastError());
    }
    return values;
}

[[nodiscard]] Result<bool> RemoteNameEquals(
    const HANDLE process,
    const platform::ProcessModuleInfo& module,
    const std::uint32_t name_rva,
    const std::string_view expected) {
    const std::size_t byte_count = expected.size() + 1U;
    auto address = RemoteAddress(module, name_rva, byte_count);
    if (!address.ok()) {
        return address.status();
    }
    std::array<char, kMaximumExportNameLength + 1U> name{};
    SIZE_T bytes_read = 0;
    if (!::ReadProcessMemory(
            process,
            reinterpret_cast<const void*>(address.value()),
            name.data(),
            byte_count,
            &bytes_read) ||
        bytes_read != byte_count) {
        return Status::FromWin32("ReadProcessMemory(remote export name)", ::GetLastError());
    }
    return name[expected.size()] == '\0' &&
           std::memcmp(name.data(), expected.data(), expected.size()) == 0;
}

[[nodiscard]] bool IsExecutableRva(
    const std::span<const IMAGE_SECTION_HEADER> sections,
    const std::uint32_t rva) noexcept {
    for (const auto& section : sections) {
        const std::uint32_t span =
            (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
        if (rva >= section.VirtualAddress &&
            rva - section.VirtualAddress < span) {
            return (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        }
    }
    return false;
}

}  // namespace

Result<std::uintptr_t> ResolveRemoteExportAddress(
    const HANDLE process,
    const platform::ProcessModuleInfo& module,
    const std::string_view export_name) {
    if (process == nullptr || module.base_address == 0 ||
        module.image_size == 0 || export_name.empty() ||
        export_name.size() > kMaximumExportNameLength) {
        return Status(ErrorCode::invalid_argument, "Invalid remote export lookup");
    }

    const auto dos = ReadRemoteObject<IMAGE_DOS_HEADER>(process, module, 0);
    if (!dos.ok()) {
        return dos.status();
    }
    if (dos.value().e_magic != IMAGE_DOS_SIGNATURE ||
        dos.value().e_lfanew < 0) {
        return Status(ErrorCode::invalid_data, "Invalid remote DOS header");
    }
    const auto nt_rva = static_cast<std::uint32_t>(dos.value().e_lfanew);
    const auto signature = ReadRemoteObject<DWORD>(process, module, nt_rva);
    if (!signature.ok() || signature.value() != IMAGE_NT_SIGNATURE) {
        return Status(ErrorCode::invalid_data, "Invalid remote PE signature");
    }

    constexpr std::uint32_t file_header_offset = sizeof(DWORD);
    const auto file_header = ReadRemoteObject<IMAGE_FILE_HEADER>(
        process,
        module,
        nt_rva + file_header_offset);
    if (!file_header.ok()) {
        return file_header.status();
    }
    if (file_header.value().NumberOfSections == 0 ||
        file_header.value().NumberOfSections > kMaximumPeSections) {
        return Status(ErrorCode::invalid_data, "Invalid remote PE section count");
    }

    const std::uint32_t optional_header_rva =
        nt_rva + file_header_offset + sizeof(IMAGE_FILE_HEADER);
    const auto optional_magic =
        ReadRemoteObject<WORD>(process, module, optional_header_rva);
    if (!optional_magic.ok()) {
        return optional_magic.status();
    }

    IMAGE_DATA_DIRECTORY export_directory{};
    if (optional_magic.value() == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (file_header.value().SizeOfOptionalHeader <
            sizeof(IMAGE_OPTIONAL_HEADER64)) {
            return Status(ErrorCode::invalid_data, "Truncated remote PE32+ header");
        }
        const auto optional = ReadRemoteObject<IMAGE_OPTIONAL_HEADER64>(
            process,
            module,
            optional_header_rva);
        if (!optional.ok()) {
            return optional.status();
        }
        if (optional.value().SizeOfImage != module.image_size ||
            optional.value().NumberOfRvaAndSizes <=
                IMAGE_DIRECTORY_ENTRY_EXPORT) {
            return Status(ErrorCode::invalid_data, "Remote PE32+ image identity is invalid");
        }
        export_directory = optional.value().DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else if (optional_magic.value() == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        if (file_header.value().SizeOfOptionalHeader <
            sizeof(IMAGE_OPTIONAL_HEADER32)) {
            return Status(ErrorCode::invalid_data, "Truncated remote PE32 header");
        }
        const auto optional = ReadRemoteObject<IMAGE_OPTIONAL_HEADER32>(
            process,
            module,
            optional_header_rva);
        if (!optional.ok()) {
            return optional.status();
        }
        if (optional.value().SizeOfImage != module.image_size ||
            optional.value().NumberOfRvaAndSizes <=
                IMAGE_DIRECTORY_ENTRY_EXPORT) {
            return Status(ErrorCode::invalid_data, "Remote PE32 image identity is invalid");
        }
        export_directory = optional.value().DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    } else {
        return Status(ErrorCode::incompatible, "Unsupported remote PE format");
    }

    const std::uint32_t section_table_rva =
        optional_header_rva + file_header.value().SizeOfOptionalHeader;
    auto sections = ReadRemoteArray<IMAGE_SECTION_HEADER>(
        process,
        module,
        section_table_rva,
        file_header.value().NumberOfSections);
    if (!sections.ok()) {
        return sections.status();
    }

    if (export_directory.VirtualAddress == 0 || export_directory.Size == 0 ||
        !IsImageRangeValid(
            export_directory.VirtualAddress,
            export_directory.Size,
            module.image_size)) {
        return Status(ErrorCode::invalid_data, "Remote PE export directory is invalid");
    }
    const auto exports = ReadRemoteObject<IMAGE_EXPORT_DIRECTORY>(
        process,
        module,
        export_directory.VirtualAddress);
    if (!exports.ok()) {
        return exports.status();
    }
    if (exports.value().NumberOfFunctions == 0 ||
        exports.value().NumberOfNames == 0 ||
        exports.value().NumberOfNames > kMaximumNamedExports ||
        exports.value().NumberOfFunctions > kMaximumNamedExports) {
        return Status(ErrorCode::invalid_data, "Remote PE export counts are invalid");
    }

    auto name_rvas = ReadRemoteArray<DWORD>(
        process,
        module,
        exports.value().AddressOfNames,
        exports.value().NumberOfNames);
    if (!name_rvas.ok()) {
        return name_rvas.status();
    }
    auto ordinals = ReadRemoteArray<WORD>(
        process,
        module,
        exports.value().AddressOfNameOrdinals,
        exports.value().NumberOfNames);
    if (!ordinals.ok()) {
        return ordinals.status();
    }

    for (std::uint32_t index = 0;
         index < exports.value().NumberOfNames;
         ++index) {
        auto matches = RemoteNameEquals(
            process,
            module,
            name_rvas.value()[index],
            export_name);
        if (!matches.ok()) {
            return matches.status();
        }
        if (!matches.value()) {
            continue;
        }

        const std::uint32_t ordinal = ordinals.value()[index];
        if (ordinal >= exports.value().NumberOfFunctions) {
            return Status(ErrorCode::invalid_data, "Remote export ordinal is invalid");
        }
        const std::uint64_t function_entry_rva =
            static_cast<std::uint64_t>(exports.value().AddressOfFunctions) +
            static_cast<std::uint64_t>(ordinal) * sizeof(DWORD);
        if (function_entry_rva > (std::numeric_limits<std::uint32_t>::max)()) {
            return Status(ErrorCode::invalid_data, "Remote export table overflow");
        }
        const auto function_rva = ReadRemoteObject<DWORD>(
            process,
            module,
            static_cast<std::uint32_t>(function_entry_rva));
        if (!function_rva.ok()) {
            return function_rva.status();
        }
        const std::uint32_t export_end =
            export_directory.VirtualAddress + export_directory.Size;
        if (function_rva.value() >= export_directory.VirtualAddress &&
            function_rva.value() < export_end) {
            return Status(ErrorCode::incompatible, "Forwarded remote export is unsupported");
        }
        if (!IsExecutableRva(sections.value(), function_rva.value())) {
            return Status(ErrorCode::incompatible, "Remote export is not executable");
        }
        return RemoteAddress(module, function_rva.value(), 1);
    }

    return Status(ErrorCode::not_found, "Remote export was not found");
}

}  // namespace metaplasia::injector
