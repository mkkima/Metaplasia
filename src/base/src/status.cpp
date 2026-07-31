#include "metaplasia/base/status.hpp"

#include "metaplasia/base/unique_handle.hpp"
#include "metaplasia/base/utf.hpp"

#include <Windows.h>

#include <algorithm>
#include <string>

namespace metaplasia {
namespace {

ErrorCode MapWin32Error(const std::uint32_t error) noexcept {
    switch (error) {
        case ERROR_SUCCESS:
            return ErrorCode::ok;
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_MOD_NOT_FOUND:
            return ErrorCode::not_found;
        case ERROR_ACCESS_DENIED:
        case ERROR_PRIVILEGE_NOT_HELD:
            return ErrorCode::access_denied;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:
            return ErrorCode::already_exists;
        case ERROR_TIMEOUT:
        case WAIT_TIMEOUT:
            return ErrorCode::timeout;
        case ERROR_OPERATION_ABORTED:
        case ERROR_CANCELLED:
            return ErrorCode::cancelled;
        case ERROR_BROKEN_PIPE:
        case ERROR_PIPE_NOT_CONNECTED:
        case ERROR_NO_DATA:
            return ErrorCode::disconnected;
        case ERROR_INVALID_PARAMETER:
            return ErrorCode::invalid_argument;
        case ERROR_INVALID_DATA:
        case ERROR_BAD_FORMAT:
            return ErrorCode::invalid_data;
        default:
            return ErrorCode::win32_error;
    }
}

std::string FormatWin32Message(const std::uint32_t native_code) {
    wchar_t* raw_message = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        native_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&raw_message),
        0,
        nullptr);

    UniqueLocalMemory memory(reinterpret_cast<HLOCAL>(raw_message));
    if (length == 0 || raw_message == nullptr) {
        return "Windows error " + std::to_string(native_code);
    }

    std::wstring message(raw_message, length);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' ||
            message.back() == L' ' || message.back() == L'.')) {
        message.pop_back();
    }

    auto utf8 = WideToUtf8(message);
    return utf8.ok() ? std::move(utf8).value()
                     : "Windows error " + std::to_string(native_code);
}

}  // namespace

Status::Status(
    const ErrorCode code,
    std::string message,
    const std::uint32_t native_code)
    : code_(code), native_code_(native_code), message_(std::move(message)) {}

Status Status::Ok() noexcept {
    return {};
}

Status Status::FromWin32(
    std::string context,
    const std::uint32_t native_code) {
    if (native_code == ERROR_SUCCESS) {
        if (!context.empty()) {
            context += ": ";
        }
        context += "Windows API failed without setting an error code";
        return {ErrorCode::internal_error, std::move(context), 0};
    }
    if (!context.empty()) {
        context += ": ";
    }
    context += FormatWin32Message(native_code);
    return {MapWin32Error(native_code), std::move(context), native_code};
}

}  // namespace metaplasia
