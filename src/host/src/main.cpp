#include "metaplasia/base/unique_handle.hpp"
#include "metaplasia/base/windows_paths.hpp"
#include "metaplasia/compatibility/catalog.hpp"
#include "metaplasia/host/diagnostic_log.hpp"
#include "metaplasia/host/engine_controller.hpp"
#include "metaplasia/host/settings_store.hpp"
#include "metaplasia/host/watchdog_client.hpp"
#include "metaplasia/platform/named_pipe.hpp"
#include "metaplasia/platform/security.hpp"
#include "metaplasia/protocol/messages.hpp"
#include "metaplasia/trust/authenticode.hpp"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>

namespace {

metaplasia::UniqueHandle g_stop_event;

BOOL WINAPI ConsoleHandler(const DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT ||
        signal == CTRL_CLOSE_EVENT || signal == CTRL_LOGOFF_EVENT ||
        signal == CTRL_SHUTDOWN_EVENT) {
        if (g_stop_event) {
            ::SetEvent(g_stop_event.get());
        }
        return TRUE;
    }
    return FALSE;
}

int Run() {
    using namespace metaplasia;
    // Cold portable starts can be delayed by Defender while it scans the
    // newly copied watchdog binary. Keep the handshake bounded, but allow
    // enough time for that first launch on supported Windows 11 systems.
    constexpr auto watchdog_startup_timeout = std::chrono::seconds(15);
    constexpr auto previous_recovery_timeout = std::chrono::seconds(15);

    auto session = CurrentSessionId();
    if (!session.ok()) {
        return 10;
    }
    const std::wstring mutex_name =
        L"Local\\Metaplasia.Host.v1." + std::to_wstring(session.value());
    UniqueHandle instance_mutex(::CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
    if (!instance_mutex) {
        return 11;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    auto prior_recovery = host::WaitForPriorRecovery(
        session.value(),
        previous_recovery_timeout);
    if (!prior_recovery.ok()) {
        ::OutputDebugStringW(
            L"[Metaplasia Host] A previous recovery did not finish; "
            L"refusing to start without a safe settings snapshot.\n");
        return 12;
    }

    g_stop_event.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!g_stop_event) {
        return 13;
    }
    ::SetConsoleCtrlHandler(&ConsoleHandler, TRUE);

    auto executable_directory = ExecutableDirectory();
    auto data_directory = MetaplasiaDataDirectory();
    auto pipe_name = platform::HostPipeName();
    if (!executable_directory.ok() || !data_directory.ok() || !pipe_name.ok()) {
        return 14;
    }

    host::DiagnosticLog diagnostic_log(data_directory.value() / L"logs");
    diagnostic_log.Write(
        host::DiagnosticLevel::info,
        "host",
        "startup",
        "host",
        ::GetCurrentProcessId(),
        "Native host startup began for session " +
            std::to_string(session.value()));

    const auto agent_path =
        executable_directory.value() / L"metaplasia-agent.dll";
    std::error_code filesystem_error;
    if (!std::filesystem::is_regular_file(agent_path, filesystem_error) ||
        filesystem_error) {
        diagnostic_log.Write(
            host::DiagnosticLevel::error,
            "host",
            "agent-file-missing",
            "host",
            0,
            "The native agent DLL is missing or inaccessible beside the host executable");
        return 15;
    }
    const auto watchdog_path =
        executable_directory.value() / L"metaplasia-watchdog.exe";
    const auto settings_path = data_directory.value() / L"settings.conf";

    auto executable_path = ExecutablePath();
    if (!executable_path.ok()) {
        return 16;
    }
#if defined(METAPLASIA_DEVELOPMENT_TRUST)
    constexpr bool allow_unsigned_development = true;
#else
    constexpr bool allow_unsigned_development = false;
#endif
    auto component_trust = trust::VerifyComponentFiles(
        {
            {"host", executable_path.value()},
            {"watchdog", watchdog_path},
            {"agent", agent_path},
        },
        allow_unsigned_development);
    if (!component_trust.ok() || !component_trust.value().accepted) {
        ::OutputDebugStringW(
            L"[Metaplasia Host] Component Authenticode trust failed; "
            L"refusing to activate shell customizations.\n");
        diagnostic_log.Write(
            host::DiagnosticLevel::error,
            "host",
            "component-trust-failed",
            "host",
            0,
            component_trust.ok()
                ? component_trust.value().detail
                : component_trust.status().message());
        ::SetConsoleCtrlHandler(&ConsoleHandler, FALSE);
        return 17;
    }
    if (component_trust.value().development_override) {
        ::OutputDebugStringW(
            L"[Metaplasia Host] WARNING: Debug-only unsigned component "
            L"trust policy is active.\n");
        diagnostic_log.Write(
            host::DiagnosticLevel::warning,
            "host",
            "development-trust-active",
            "host",
            ::GetCurrentProcessId(),
            "Debug-only unsigned component trust policy is active");
    }

    auto profile_pack = compatibility::LoadExternalProfilePack(
        executable_directory.value() /
            L"metaplasia-compatibility-pack.dll",
        component_trust.value().publisher_thumbprint,
        allow_unsigned_development);
    if (!profile_pack.ok()) {
        ::OutputDebugStringW(
            L"[Metaplasia Host] External compatibility pack verification "
            L"failed; refusing to start.\n");
        diagnostic_log.Write(
            host::DiagnosticLevel::error,
            "host",
            "compatibility-pack-failed",
            "host",
            0,
            profile_pack.status().message());
        ::SetConsoleCtrlHandler(&ConsoleHandler, FALSE);
        return 18;
    }
    if (profile_pack.value().loaded) {
        ::OutputDebugStringW(
            L"[Metaplasia Host] Verified external compatibility pack loaded.\n");
    }

    // Packaged shell processes need explicit read/execute access to the agent
    // and traversal access to its directory. Only our own build/install output
    // is modified and existing ACL entries are preserved.
    auto directory_acl = platform::GrantPackagedApplicationReadExecute(
        executable_directory.value(),
        true);
    auto agent_acl =
        platform::GrantPackagedApplicationReadExecute(agent_path, false);
    if (!directory_acl.ok() || !agent_acl.ok()) {
        ::OutputDebugStringW(
            L"[Metaplasia Host] Unable to prepare packaged-app ACLs; "
            L"Start menu injection may be unavailable.\n");
        diagnostic_log.Write(
            host::DiagnosticLevel::warning,
            "host",
            "agent-acl-failed",
            "host",
            0,
            !directory_acl.ok() ? directory_acl.status().message()
                                : agent_acl.status().message());
    }

    auto watchdog = host::WatchdogClient::Start(
        watchdog_path,
        agent_path,
        settings_path,
        session.value(),
        watchdog_startup_timeout);
    if (!watchdog.ok()) {
        ::OutputDebugStringW(
            L"[Metaplasia Host] Watchdog readiness failed; refusing to "
            L"activate shell customizations.\n");
        diagnostic_log.Write(
            host::DiagnosticLevel::error,
            "host",
            "watchdog-start-failed",
            "host",
            0,
            watchdog.status().message());
        ::SetConsoleCtrlHandler(&ConsoleHandler, FALSE);
        return 19;
    }

    const HANDLE watchdog_process = watchdog.value().process_handle();
    const HANDLE watchdog_graceful =
        watchdog.value().graceful_event_handle();
    std::jthread watchdog_monitor(
        [watchdog_process, watchdog_graceful, &diagnostic_log](
            const std::stop_token stop_token) noexcept {
            while (!stop_token.stop_requested()) {
                const DWORD wait =
                    ::WaitForSingleObject(watchdog_process, 250);
                if (wait == WAIT_TIMEOUT) {
                    continue;
                }
                if (wait == WAIT_OBJECT_0 &&
                    ::WaitForSingleObject(watchdog_graceful, 0) !=
                        WAIT_OBJECT_0 &&
                    g_stop_event) {
                    ::OutputDebugStringW(
                        L"[Metaplasia Host] Watchdog exited unexpectedly; "
                        L"stopping the host safely.\n");
                    diagnostic_log.Write(
                        host::DiagnosticLevel::error,
                        "host",
                        "watchdog-exited",
                        "host",
                        0,
                        "Watchdog exited unexpectedly; stopping the host safely");
                    ::SetEvent(g_stop_event.get());
                } else if (wait != WAIT_OBJECT_0 && g_stop_event) {
                    ::OutputDebugStringW(
                        L"[Metaplasia Host] Watchdog supervision failed; "
                        L"stopping the host safely.\n");
                    diagnostic_log.Write(
                        host::DiagnosticLevel::error,
                        "host",
                        "watchdog-supervision-failed",
                        "host",
                        0,
                        "Watchdog process supervision failed; stopping the host safely");
                    ::SetEvent(g_stop_event.get());
                }
                return;
            }
        });

    host::SettingsStore settings_store(settings_path);
    auto settings = settings_store.Load();
    host::HostSettings initial_settings;
    if (!settings.ok()) {
        ::OutputDebugStringW(
            L"[Metaplasia Host] Settings are invalid; starting fail-safe with "
            L"all customizations disabled.\n");
        diagnostic_log.Write(
            host::DiagnosticLevel::error,
            "host",
            "settings-load-failed",
            "host",
            0,
            settings.status().message());
    } else {
        initial_settings = settings.value();
    }

    host::EngineController controller(
        agent_path,
        std::move(settings_store),
        initial_settings,
        session.value(),
        diagnostic_log,
        injector::AgentTrustPolicy{
            component_trust.value().publisher_thumbprint,
            component_trust.value().development_override});
    controller.Start();

    platform::NamedPipeServer server(
        pipe_name.value(),
        [&controller](const protocol::Frame& request)
            -> Result<protocol::Frame> {
            protocol::Frame response;
            response.header.request_id = request.header.request_id;

            switch (request.header.kind) {
                case protocol::MessageKind::get_snapshot_request: {
                    if (!request.payload.empty()) {
                        return Status(
                            ErrorCode::invalid_data,
                            "Snapshot request payload must be empty");
                    }
                    const auto snapshots = controller.Snapshot();
                    auto payload = protocol::EncodeSnapshotResponse(snapshots);
                    if (!payload.ok()) {
                        return payload.status();
                    }
                    response.header.kind =
                        protocol::MessageKind::snapshot_response;
                    response.payload = std::move(payload).value();
                    return response;
                }
                case protocol::MessageKind::set_enabled_request: {
                    auto command =
                        protocol::DecodeSetEnabledRequest(request.payload);
                    if (!command.ok()) {
                        return command.status();
                    }
                    auto update = controller.SetEnabled(
                        command.value().target,
                        command.value().enabled);
                    if (!update.ok()) {
                        return update.status();
                    }
                    auto payload = protocol::EncodeCommandResponse(
                        {true, "Configuration persisted"});
                    if (!payload.ok()) {
                        return payload.status();
                    }
                    response.header.kind =
                        protocol::MessageKind::command_response;
                    response.payload = std::move(payload).value();
                    return response;
                }
                case protocol::MessageKind::get_settings_request: {
                    if (!request.payload.empty()) {
                        return Status(
                            ErrorCode::invalid_data,
                            "Settings request payload must be empty");
                    }
                    auto payload = protocol::EncodeSettingsResponse(
                        controller.Settings());
                    if (!payload.ok()) {
                        return payload.status();
                    }
                    response.header.kind =
                        protocol::MessageKind::settings_response;
                    response.payload = std::move(payload).value();
                    return response;
                }
                case protocol::MessageKind::set_customization_request: {
                    auto command =
                        protocol::DecodeSetCustomizationRequest(
                            request.payload);
                    if (!command.ok()) {
                        return command.status();
                    }
                    auto update = controller.SetCustomization(command.value());
                    if (!update.ok()) {
                        return update.status();
                    }
                    auto payload = protocol::EncodeCommandResponse(
                        {true, "Customization persisted"});
                    if (!payload.ok()) {
                        return payload.status();
                    }
                    response.header.kind =
                        protocol::MessageKind::command_response;
                    response.payload = std::move(payload).value();
                    return response;
                }
                case protocol::MessageKind::get_xaml_diagnostics_request: {
                    auto target = protocol::DecodeXamlDiagnosticsRequest(
                        request.payload);
                    if (!target.ok()) {
                        return target.status();
                    }
                    auto diagnostics =
                        controller.XamlDiagnostics(target.value());
                    if (!diagnostics.ok()) {
                        return diagnostics.status();
                    }
                    auto payload = protocol::EncodeXamlDiagnosticsResponse(
                        diagnostics.value());
                    if (!payload.ok()) {
                        return payload.status();
                    }
                    response.header.kind =
                        protocol::MessageKind::xaml_diagnostics_response;
                    response.payload = std::move(payload).value();
                    return response;
                }
                case protocol::MessageKind::prepare_update_request: {
                    if (!request.payload.empty()) {
                        return Status(
                            ErrorCode::invalid_data,
                            "Prepare-update request payload must be empty");
                    }
                    auto payload = protocol::EncodeCommandResponse(
                        {true, "Host is ready to stop for a verified update"});
                    if (!payload.ok()) {
                        return payload.status();
                    }
                    response.header.kind =
                        protocol::MessageKind::command_response;
                    response.payload = std::move(payload).value();
                    return response;
                }
                default:
                    return Status(
                        ErrorCode::invalid_data,
                        "Unsupported protocol message");
            }
        },
        [](const protocol::Frame& request,
           const protocol::Frame& response,
           const bool acknowledged) {
            if (acknowledged &&
                request.header.kind ==
                    protocol::MessageKind::prepare_update_request &&
                response.header.kind ==
                    protocol::MessageKind::command_response &&
                g_stop_event) {
                // Stop only after the caller has received and acknowledged the
                // success response. This preserves named-pipe transaction
                // semantics while allowing the host to unload its controllers
                // before the portable update helper replaces the binaries.
                ::SetEvent(g_stop_event.get());
            }
        });

    auto server_result = server.Run(g_stop_event.get());
    controller.Stop();
    auto graceful_shutdown = watchdog.value().SignalGracefulShutdown();
    if (!graceful_shutdown.ok()) {
        ::OutputDebugStringW(
            L"[Metaplasia Host] Unable to signal graceful watchdog "
            L"shutdown; watchdog recovery will verify safe mode.\n");
        diagnostic_log.Write(
            host::DiagnosticLevel::warning,
            "host",
            "watchdog-shutdown-signal-failed",
            "host",
            0,
            graceful_shutdown.status().message());
    } else {
        ::WaitForSingleObject(watchdog_process, 5000);
    }
    watchdog_monitor.request_stop();
    watchdog_monitor.join();
    ::SetConsoleCtrlHandler(&ConsoleHandler, FALSE);
    diagnostic_log.Write(
        server_result.ok() ? host::DiagnosticLevel::info
                           : host::DiagnosticLevel::error,
        "host",
        "shutdown",
        "host",
        ::GetCurrentProcessId(),
        server_result.ok() ? "Native host stopped cleanly"
                           : server_result.status().message());
    return server_result.ok() ? 0 : 20;
}

}  // namespace

int wmain() {
    return Run();
}
