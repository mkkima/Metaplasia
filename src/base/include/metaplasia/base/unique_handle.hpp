#pragma once

#include <Windows.h>

#include <utility>

namespace metaplasia {

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(other.release()) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    explicit operator bool() const noexcept { return valid(); }

    [[nodiscard]] HANDLE release() noexcept {
        return std::exchange(handle_, nullptr);
    }

    void reset(HANDLE replacement = nullptr) noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_{nullptr};
};

class UniqueLocalMemory final {
public:
    UniqueLocalMemory() noexcept = default;
    explicit UniqueLocalMemory(HLOCAL memory) noexcept : memory_(memory) {}
    ~UniqueLocalMemory() { reset(); }

    UniqueLocalMemory(const UniqueLocalMemory&) = delete;
    UniqueLocalMemory& operator=(const UniqueLocalMemory&) = delete;

    UniqueLocalMemory(UniqueLocalMemory&& other) noexcept
        : memory_(other.release()) {}

    UniqueLocalMemory& operator=(UniqueLocalMemory&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HLOCAL get() const noexcept { return memory_; }
    [[nodiscard]] HLOCAL release() noexcept {
        return std::exchange(memory_, nullptr);
    }

    void reset(HLOCAL replacement = nullptr) noexcept {
        if (memory_ != nullptr) {
            ::LocalFree(memory_);
        }
        memory_ = replacement;
    }

private:
    HLOCAL memory_{nullptr};
};

}  // namespace metaplasia
