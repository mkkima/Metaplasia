#pragma once

#include "metaplasia/base/status.hpp"
#include "metaplasia/protocol/agent_abi.hpp"
#include "metaplasia/trust/authenticode.hpp"

#include <Windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>

namespace metaplasia::injector {

struct InjectionResult final {
    bool newly_loaded{false};
    protocol::AgentResult agent_result{protocol::AgentResult::initialization_failed};
};

struct AgentTrustPolicy final {
    trust::PublisherThumbprint expected_publisher{};
    bool allow_unsigned_development{false};
};

class Injector final {
public:
    explicit Injector(AgentTrustPolicy trust_policy = {}) noexcept
        : trust_policy_(trust_policy) {}

    [[nodiscard]] Result<InjectionResult> LoadAndConfigure(
        std::uint32_t process_id,
        const std::filesystem::path& agent_path,
        const protocol::AgentConfiguration& configuration,
        std::chrono::milliseconds timeout = std::chrono::seconds(10)) const;

    // Configures an exact, already mapped agent image. This operation never
    // calls LoadLibrary, even if the module disappears after discovery.
    [[nodiscard]] Result<InjectionResult> ConfigureLoaded(
        std::uint32_t process_id,
        const std::filesystem::path& agent_path,
        const protocol::AgentConfiguration& configuration,
        std::chrono::milliseconds timeout = std::chrono::seconds(10)) const;

    // Queries a bounded snapshot from an exact already mapped agent. This
    // operation never loads the DLL or enables a feature.
    [[nodiscard]] Result<protocol::XamlDiagnosticsSnapshot>
    QueryLoadedXamlDiagnostics(
        std::uint32_t process_id,
        const std::filesystem::path& agent_path,
        protocol::AgentTarget target,
        std::chrono::milliseconds timeout = std::chrono::seconds(5)) const;

private:
    [[nodiscard]] Result<InjectionResult> ConfigureInternal(
        std::uint32_t process_id,
        const std::filesystem::path& agent_path,
        const protocol::AgentConfiguration& configuration,
        std::chrono::milliseconds timeout,
        bool allow_load) const;

    [[nodiscard]] Result<void> ValidateTarget(
        HANDLE process,
        std::uint32_t process_id,
        protocol::AgentTarget target) const;
    [[nodiscard]] Result<void> ValidateAgentTrust(
        const std::filesystem::path& agent_path) const;

    AgentTrustPolicy trust_policy_;
};

}  // namespace metaplasia::injector
