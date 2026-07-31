#include "metaplasia/base/utf.hpp"

#include <Windows.h>

#include <limits>

namespace metaplasia {

Result<std::string> WideToUtf8(const std::wstring_view value) {
    if (value.empty()) {
        return std::string{};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return Status(ErrorCode::invalid_argument, "Wide string is too large");
    }

    const int input_size = static_cast<int>(value.size());
    const int output_size = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        input_size,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (output_size == 0) {
        return Status::FromWin32("WideCharToMultiByte(size)", ::GetLastError());
    }

    std::string output(static_cast<std::size_t>(output_size), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            input_size,
            output.data(),
            output_size,
            nullptr,
            nullptr) == 0) {
        return Status::FromWin32("WideCharToMultiByte", ::GetLastError());
    }
    return output;
}

Result<std::wstring> Utf8ToWide(const std::string_view value) {
    if (value.empty()) {
        return std::wstring{};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return Status(ErrorCode::invalid_argument, "UTF-8 string is too large");
    }

    const int input_size = static_cast<int>(value.size());
    const int output_size = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        input_size,
        nullptr,
        0);
    if (output_size == 0) {
        return Status::FromWin32("MultiByteToWideChar(size)", ::GetLastError());
    }

    std::wstring output(static_cast<std::size_t>(output_size), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            input_size,
            output.data(),
            output_size) == 0) {
        return Status::FromWin32("MultiByteToWideChar", ::GetLastError());
    }
    return output;
}

}  // namespace metaplasia
