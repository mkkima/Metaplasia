#include "metaplasia/platform/named_pipe.hpp"

#include "metaplasia/base/unique_handle.hpp"
#include "metaplasia/base/windows_paths.hpp"

#include <Sddl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <iostream>
#include <span>
#include <vector>

namespace metaplasia::platform {
namespace {

constexpr DWORD kIoTimeoutMilliseconds = 5'000;
constexpr std::byte kTransactionAcknowledgement{0xA5};

Result<void> WaitForOverlapped(
    const HANDLE object,
    OVERLAPPED& overlapped,
    const HANDLE stop_event,
    const DWORD timeout,
    DWORD& transferred) {
    std::array<HANDLE, 2> handles{overlapped.hEvent, stop_event};
    const DWORD handle_count = stop_event != nullptr ? 2U : 1U;
    const DWORD wait_result =
        ::WaitForMultipleObjects(handle_count, handles.data(), FALSE, timeout);

    if (wait_result == WAIT_OBJECT_0) {
        if (!::GetOverlappedResult(object, &overlapped, &transferred, FALSE)) {
            return Status::FromWin32("GetOverlappedResult", ::GetLastError());
        }
        return {};
    }

    ::CancelIoEx(object, &overlapped);
    ::WaitForSingleObject(overlapped.hEvent, INFINITE);
    DWORD ignored = 0;
    ::GetOverlappedResult(object, &overlapped, &ignored, FALSE);

    if (stop_event != nullptr && wait_result == WAIT_OBJECT_0 + 1U) {
        return Status(ErrorCode::cancelled, "Named-pipe operation cancelled");
    }
    if (wait_result == WAIT_TIMEOUT) {
        return Status(ErrorCode::timeout, "Named-pipe operation timed out");
    }
    return Status::FromWin32("WaitForMultipleObjects", ::GetLastError());
}

Result<void> TransferExact(
    const HANDLE pipe,
    const std::span<std::byte> bytes,
    const bool write,
    const HANDLE stop_event,
    const DWORD timeout) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        UniqueHandle completion_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!completion_event) {
            return Status::FromWin32("CreateEventW", ::GetLastError());
        }

        OVERLAPPED overlapped{};
        overlapped.hEvent = completion_event.get();
        const auto remaining = bytes.size() - offset;
        const DWORD amount = static_cast<DWORD>((std::min)(
            remaining,
            static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));

        DWORD transferred = 0;
        const BOOL started = write
                                 ? ::WriteFile(
                                       pipe,
                                       bytes.data() + offset,
                                       amount,
                                       &transferred,
                                       &overlapped)
                                 : ::ReadFile(
                                       pipe,
                                       bytes.data() + offset,
                                       amount,
                                       &transferred,
                                       &overlapped);
        if (!started) {
            const DWORD error = ::GetLastError();
            if (error != ERROR_IO_PENDING) {
                return Status::FromWin32(
                    write ? "WriteFile(named pipe)" : "ReadFile(named pipe)",
                    error);
            }
            auto wait = WaitForOverlapped(
                pipe,
                overlapped,
                stop_event,
                timeout,
                transferred);
            if (!wait.ok()) {
                return wait.status();
            }
        }

        if (transferred == 0) {
            return Status(ErrorCode::disconnected, "Named pipe was closed");
        }
        offset += transferred;
    }
    return {};
}

Result<void> WriteExact(
    const HANDLE pipe,
    const std::span<const std::byte> bytes,
    const HANDLE stop_event = nullptr,
    const DWORD timeout = kIoTimeoutMilliseconds) {
    return TransferExact(
        pipe,
        {const_cast<std::byte*>(bytes.data()), bytes.size()},
        true,
        stop_event,
        timeout);
}

Result<void> ReadExact(
    const HANDLE pipe,
    const std::span<std::byte> bytes,
    const HANDLE stop_event = nullptr,
    const DWORD timeout = kIoTimeoutMilliseconds) {
    return TransferExact(pipe, bytes, false, stop_event, timeout);
}

Result<void> WriteFrame(
    const HANDLE pipe,
    const protocol::Frame& frame,
    const HANDLE stop_event = nullptr,
    const DWORD timeout = kIoTimeoutMilliseconds) {
    if (frame.payload.size() > protocol::kMaximumPayloadSize) {
        return Status(ErrorCode::invalid_argument, "Frame payload is too large");
    }
    auto header = frame.header;
    header.payload_size = static_cast<std::uint32_t>(frame.payload.size());
    auto encoded_header = protocol::EncodeFrameHeader(header);
    if (!encoded_header.ok()) {
        return encoded_header.status();
    }
    auto write_header =
        WriteExact(pipe, encoded_header.value(), stop_event, timeout);
    if (!write_header.ok()) {
        return write_header.status();
    }
    if (!frame.payload.empty()) {
        auto write_payload =
            WriteExact(pipe, frame.payload, stop_event, timeout);
        if (!write_payload.ok()) {
            return write_payload.status();
        }
    }
    return {};
}

Result<protocol::Frame> ReadFrame(
    const HANDLE pipe,
    const HANDLE stop_event = nullptr,
    const DWORD timeout = kIoTimeoutMilliseconds) {
    std::array<std::byte, protocol::kFrameHeaderSize> header_bytes{};
    auto read_header = ReadExact(pipe, header_bytes, stop_event, timeout);
    if (!read_header.ok()) {
        return read_header.status();
    }
    auto decoded_header = protocol::DecodeFrameHeader(header_bytes);
    if (!decoded_header.ok()) {
        return decoded_header.status();
    }

    protocol::Frame frame;
    frame.header = decoded_header.value();
    frame.payload.resize(frame.header.payload_size);
    if (!frame.payload.empty()) {
        auto read_payload = ReadExact(pipe, frame.payload, stop_event, timeout);
        if (!read_payload.ok()) {
            return read_payload.status();
        }
    }
    return frame;
}

Result<bool> ClientBelongsToCurrentUser(const HANDLE pipe) {
    ULONG client_process_id = 0;
    if (!::GetNamedPipeClientProcessId(pipe, &client_process_id)) {
        return Status::FromWin32(
            "GetNamedPipeClientProcessId",
            ::GetLastError());
    }

    UniqueHandle client_process(::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        client_process_id));
    if (!client_process) {
        return Status::FromWin32("OpenProcess(pipe client)", ::GetLastError());
    }

    HANDLE raw_client_token = nullptr;
    if (!::OpenProcessToken(client_process.get(), TOKEN_QUERY, &raw_client_token)) {
        return Status::FromWin32(
            "OpenProcessToken(pipe client)",
            ::GetLastError());
    }
    UniqueHandle client_token(raw_client_token);

    HANDLE raw_server_token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_server_token)) {
        return Status::FromWin32("OpenProcessToken(server)", ::GetLastError());
    }
    UniqueHandle server_token(raw_server_token);

    auto read_sid = [](const HANDLE token) -> Result<std::vector<std::byte>> {
        DWORD required = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &required);
        if (required == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return Status::FromWin32(
                "GetTokenInformation(size)",
                ::GetLastError());
        }
        std::vector<std::byte> storage(required);
        if (!::GetTokenInformation(
                token,
                TokenUser,
                storage.data(),
                required,
                &required)) {
            return Status::FromWin32("GetTokenInformation", ::GetLastError());
        }
        return storage;
    };

    auto client_sid = read_sid(client_token.get());
    if (!client_sid.ok()) {
        return client_sid.status();
    }
    auto server_sid = read_sid(server_token.get());
    if (!server_sid.ok()) {
        return server_sid.status();
    }

    const auto* client_user =
        reinterpret_cast<const TOKEN_USER*>(client_sid.value().data());
    const auto* server_user =
        reinterpret_cast<const TOKEN_USER*>(server_sid.value().data());
    return ::EqualSid(client_user->User.Sid, server_user->User.Sid) != FALSE;
}

Result<UniqueLocalMemory> BuildPipeSecurityDescriptor() {
    auto sid = CurrentUserSidString();
    if (!sid.ok()) {
        return sid.status();
    }

    const std::wstring sddl =
        L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid.value() + L")";
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(),
            SDDL_REVISION_1,
            &raw_descriptor,
            nullptr)) {
        return Status::FromWin32(
            "ConvertStringSecurityDescriptorToSecurityDescriptorW",
            ::GetLastError());
    }
    return UniqueLocalMemory(reinterpret_cast<HLOCAL>(raw_descriptor));
}

}  // namespace

Result<std::wstring> HostPipeName() {
    auto session = CurrentSessionId();
    if (!session.ok()) {
        return session.status();
    }
    return L"\\\\.\\pipe\\Metaplasia.Host.v1." +
           std::to_wstring(session.value());
}

NamedPipeClient::NamedPipeClient(std::wstring pipe_name)
    : pipe_name_(std::move(pipe_name)) {}

Result<protocol::Frame> NamedPipeClient::Transact(
    const protocol::Frame& request,
    const std::chrono::milliseconds timeout) const {
    if (timeout.count() <= 0 ||
        timeout.count() > static_cast<long long>((std::numeric_limits<DWORD>::max)())) {
        return Status(ErrorCode::invalid_argument, "Invalid pipe timeout");
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    Status last_error(ErrorCode::disconnected, "Named pipe is unavailable");

    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return last_error.code() == ErrorCode::not_found
                       ? Status(ErrorCode::timeout, last_error.message())
                       : last_error;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        const DWORD remaining_ms = static_cast<DWORD>((std::max)(
            std::int64_t{1},
            remaining.count()));

        if (!::WaitNamedPipeW(pipe_name_.c_str(), remaining_ms)) {
            const DWORD error = ::GetLastError();
            last_error = Status::FromWin32("WaitNamedPipeW", error);
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_SEM_TIMEOUT &&
                error != ERROR_PIPE_BUSY) {
                return last_error;
            }
            ::Sleep(10);
            continue;
        }

        UniqueHandle pipe(::CreateFileW(
            pipe_name_.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
                SECURITY_IDENTIFICATION,
            nullptr));
        if (!pipe) {
            const DWORD error = ::GetLastError();
            last_error = Status::FromWin32("CreateFileW(named pipe)", error);
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PIPE_BUSY) {
                return last_error;
            }
            ::Sleep(10);
            continue;
        }

        auto write = WriteFrame(pipe.get(), request, nullptr, remaining_ms);
        if (!write.ok()) {
            last_error = write.status();
        } else {
            auto response = ReadFrame(pipe.get(), nullptr, remaining_ms);
            if (response.ok()) {
                const std::array acknowledgement{
                    kTransactionAcknowledgement};
                // The response is already valid. The acknowledgement only
                // tells the server it can disconnect without discarding bytes
                // that the client has not consumed yet.
                static_cast<void>(WriteExact(
                    pipe.get(),
                    acknowledgement,
                    nullptr,
                    remaining_ms));
                return response;
            }
            last_error = response.status();
        }

        if (last_error.code() != ErrorCode::disconnected &&
            last_error.code() != ErrorCode::timeout &&
            last_error.code() != ErrorCode::not_found) {
            return last_error;
        }
        ::Sleep(10);
    }
}

NamedPipeServer::NamedPipeServer(std::wstring pipe_name, Handler handler)
    : pipe_name_(std::move(pipe_name)), handler_(std::move(handler)) {}

Result<void> NamedPipeServer::Run(const HANDLE stop_event) {
    if (stop_event == nullptr || stop_event == INVALID_HANDLE_VALUE) {
        return Status(ErrorCode::invalid_argument, "A valid stop event is required");
    }
    if (!handler_) {
        return Status(ErrorCode::invalid_argument, "A pipe handler is required");
    }

    auto descriptor = BuildPipeSecurityDescriptor();
    if (!descriptor.ok()) {
        return descriptor.status();
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor.value().get();
    attributes.bInheritHandle = FALSE;

    while (::WaitForSingleObject(stop_event, 0) != WAIT_OBJECT_0) {
        UniqueHandle pipe(::CreateNamedPipeW(
            pipe_name_.c_str(),
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                PIPE_REJECT_REMOTE_CLIENTS,
            1,
            protocol::kMaximumPayloadSize,
            protocol::kMaximumPayloadSize,
            0,
            &attributes));
        if (!pipe) {
            return Status::FromWin32("CreateNamedPipeW", ::GetLastError());
        }

        UniqueHandle connected_event(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!connected_event) {
            return Status::FromWin32("CreateEventW", ::GetLastError());
        }
        OVERLAPPED connect_overlapped{};
        connect_overlapped.hEvent = connected_event.get();

        bool connected = ::ConnectNamedPipe(pipe.get(), &connect_overlapped) != FALSE;
        if (!connected) {
            const DWORD error = ::GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                DWORD ignored = 0;
                auto wait = WaitForOverlapped(
                    pipe.get(),
                    connect_overlapped,
                    stop_event,
                    INFINITE,
                    ignored);
                if (!wait.ok()) {
                    if (wait.status().code() == ErrorCode::cancelled) {
                        return {};
                    }
                    return wait.status();
                }
                connected = true;
            } else {
                return Status::FromWin32("ConnectNamedPipe", error);
            }
        }

        if (!connected) {
            continue;
        }

        auto same_user = ClientBelongsToCurrentUser(pipe.get());
        if (!same_user.ok() || !same_user.value()) {
#ifndef NDEBUG
            std::cerr << "Named pipe client rejected: "
                      << (same_user.ok() ? "different user"
                                         : same_user.status().message())
                      << '\n';
#endif
            ::DisconnectNamedPipe(pipe.get());
            continue;
        }

        auto request = ReadFrame(pipe.get(), stop_event);
        if (!request.ok()) {
#ifndef NDEBUG
            std::cerr << "Named pipe request failed: "
                      << request.status().message() << '\n';
#endif
            ::DisconnectNamedPipe(pipe.get());
            if (request.status().code() == ErrorCode::cancelled) {
                return {};
            }
            continue;
        }

        auto response = handler_(request.value());
        protocol::Frame response_frame;
        if (response.ok()) {
            response_frame = std::move(response).value();
        } else {
            protocol::ErrorResponse error{
                response.status().code(),
                response.status().native_code(),
                response.status().message()};
            auto payload = protocol::EncodeErrorResponse(error);
            if (!payload.ok()) {
                ::DisconnectNamedPipe(pipe.get());
                continue;
            }
            response_frame.header.kind = protocol::MessageKind::error_response;
            response_frame.header.request_id = request.value().header.request_id;
            response_frame.payload = std::move(payload).value();
        }

        auto write_response = WriteFrame(pipe.get(), response_frame, stop_event);
#ifndef NDEBUG
        if (!write_response.ok()) {
            std::cerr << "Named pipe response failed: "
                      << write_response.status().message() << '\n';
        }
#endif
        if (write_response.ok()) {
            std::array<std::byte, 1> acknowledgement{};
            auto read_acknowledgement =
                ReadExact(pipe.get(), acknowledgement, stop_event);
#ifndef NDEBUG
            if (read_acknowledgement.ok() &&
                acknowledgement.front() != kTransactionAcknowledgement) {
                std::cerr << "Named pipe client sent an invalid acknowledgement\n";
            }
#endif
            if (!read_acknowledgement.ok() &&
                read_acknowledgement.status().code() == ErrorCode::cancelled) {
                return {};
            }
        }
        ::DisconnectNamedPipe(pipe.get());
    }
    return {};
}

}  // namespace metaplasia::platform
