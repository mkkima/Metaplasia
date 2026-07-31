#include "metaplasia/base/unique_handle.hpp"
#include "metaplasia/platform/named_pipe.hpp"
#include "metaplasia/protocol/messages.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

void Require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

}  // namespace

int main() {
    using namespace std::chrono_literals;
    using metaplasia::platform::NamedPipeClient;
    using metaplasia::platform::NamedPipeServer;
    using metaplasia::protocol::Frame;
    using metaplasia::protocol::MessageKind;

    const std::wstring pipe_name =
        L"\\\\.\\pipe\\Metaplasia.TransactionTest." +
        std::to_wstring(::GetCurrentProcessId()) + L"." +
        std::to_wstring(::GetTickCount64());
    metaplasia::UniqueHandle stop_event(
        ::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    Require(static_cast<bool>(stop_event), "create server stop event");

    std::atomic<std::uint32_t> handled{0};
    NamedPipeServer server(
        pipe_name,
        [&handled](const Frame& request) -> metaplasia::Result<Frame> {
            handled.fetch_add(1, std::memory_order_relaxed);
            Frame response;
            response.header.kind = MessageKind::command_response;
            response.header.request_id = request.header.request_id;
            return response;
        });

    metaplasia::Status server_status;
    std::jthread server_thread([&] {
        auto result = server.Run(stop_event.get());
        if (!result.ok()) {
            server_status = result.status();
        }
    });

    NamedPipeClient client(pipe_name);
    constexpr std::uint32_t transaction_count = 64;
    for (std::uint32_t index = 1; index <= transaction_count; ++index) {
        Frame request;
        request.header.kind = MessageKind::get_snapshot_request;
        request.header.request_id = index;
        auto response = client.Transact(request, 2s);
        Require(response.ok(), "complete named-pipe transaction");
        Require(
            response.value().header.request_id == index,
            "preserve request id across reconnect");
    }

    ::SetEvent(stop_event.get());
    server_thread.join();
    Require(server_status.ok(), "stop named-pipe server cleanly");
    Require(
        handled.load(std::memory_order_relaxed) == transaction_count,
        "handle each acknowledged request exactly once");

    std::cout << "Named-pipe transaction tests passed\n";
    return EXIT_SUCCESS;
}
