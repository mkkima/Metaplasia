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
    std::atomic<std::uint32_t> acknowledged{0};
    NamedPipeServer server(
        pipe_name,
        [&handled](const Frame& request) -> metaplasia::Result<Frame> {
            handled.fetch_add(1, std::memory_order_relaxed);
            if (request.header.kind == MessageKind::set_customization_request) {
                // Simulate an atomic durable settings flush exceeding the
                // CLI's former two-second read-only response budget.
                std::this_thread::sleep_for(2200ms);
            }
            Frame response;
            response.header.kind = MessageKind::command_response;
            response.header.request_id = request.header.request_id;
            return response;
        },
        [&acknowledged](const Frame& request,
                        const Frame& response,
                        const bool was_acknowledged) {
            if (was_acknowledged &&
                request.header.request_id == response.header.request_id) {
                acknowledged.fetch_add(1, std::memory_order_relaxed);
            }
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

    Frame persisted_command;
    persisted_command.header.kind = MessageKind::set_customization_request;
    persisted_command.header.request_id = transaction_count + 1;
    auto persisted_response = client.Transact(persisted_command, 15s);
    Require(persisted_response.ok(), "wait for a delayed durable-write acknowledgement");
    Require(
        persisted_response.value().header.request_id == transaction_count + 1,
        "preserve the delayed mutation request identity");
    constexpr auto expected_count = transaction_count + 1;

    const auto acknowledgement_deadline =
        std::chrono::steady_clock::now() + 2s;
    while (acknowledged.load(std::memory_order_relaxed) < expected_count &&
           std::chrono::steady_clock::now() < acknowledgement_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ::SetEvent(stop_event.get());
    server_thread.join();
    Require(server_status.ok(), "stop named-pipe server cleanly");
    Require(
        handled.load(std::memory_order_relaxed) == expected_count,
        "handle each acknowledged request exactly once");
    Require(
        acknowledged.load(std::memory_order_relaxed) == expected_count,
        "observe each transaction only after its acknowledgement");

    std::cout << "Named-pipe transaction tests passed\n";
    return EXIT_SUCCESS;
}
