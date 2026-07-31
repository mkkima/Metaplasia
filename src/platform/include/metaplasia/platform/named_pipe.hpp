#pragma once

#include "metaplasia/base/status.hpp"
#include "metaplasia/protocol/messages.hpp"

#include <Windows.h>

#include <chrono>
#include <functional>
#include <string>

namespace metaplasia::platform {

[[nodiscard]] Result<std::wstring> HostPipeName();

class NamedPipeClient final {
public:
    explicit NamedPipeClient(std::wstring pipe_name);

    [[nodiscard]] Result<protocol::Frame> Transact(
        const protocol::Frame& request,
        std::chrono::milliseconds timeout = std::chrono::seconds(3)) const;

private:
    std::wstring pipe_name_;
};

class NamedPipeServer final {
public:
    using Handler = std::function<Result<protocol::Frame>(const protocol::Frame&)>;

    NamedPipeServer(std::wstring pipe_name, Handler handler);

    NamedPipeServer(const NamedPipeServer&) = delete;
    NamedPipeServer& operator=(const NamedPipeServer&) = delete;

    // Blocks until stop_event is signaled. Requests are serialized deliberately:
    // the control protocol is low-volume and a single handler prevents command
    // races in the host state machine.
    [[nodiscard]] Result<void> Run(HANDLE stop_event);

private:
    std::wstring pipe_name_;
    Handler handler_;
};

}  // namespace metaplasia::platform
