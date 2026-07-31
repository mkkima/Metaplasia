#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace metaplasia {

enum class ErrorCode : std::uint16_t {
    ok = 0,
    invalid_argument,
    invalid_data,
    not_found,
    access_denied,
    incompatible,
    timeout,
    disconnected,
    already_exists,
    cancelled,
    win32_error,
    internal_error,
};

class [[nodiscard]] Status final {
public:
    Status() = default;
    Status(ErrorCode code, std::string message, std::uint32_t native_code = 0);

    [[nodiscard]] static Status Ok() noexcept;
    [[nodiscard]] static Status FromWin32(
        std::string context,
        std::uint32_t native_code);

    [[nodiscard]] bool ok() const noexcept { return code_ == ErrorCode::ok; }
    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] std::uint32_t native_code() const noexcept {
        return native_code_;
    }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    ErrorCode code_{ErrorCode::ok};
    std::uint32_t native_code_{0};
    std::string message_;
};

template <typename T>
class [[nodiscard]] Result final {
public:
    Result(T value) : storage_(std::move(value)) {}
    Result(Status status) : storage_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] const Status& status() const& {
        return std::get<Status>(storage_);
    }

private:
    std::variant<T, Status> storage_;
};

template <>
class [[nodiscard]] Result<void> final {
public:
    Result() = default;
    Result(Status status) : status_(std::move(status)) {}

    [[nodiscard]] bool ok() const noexcept { return status_.ok(); }
    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    Status status_;
};

}  // namespace metaplasia
