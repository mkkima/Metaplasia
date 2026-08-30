add_executable(
    metaplasia_protocol_tests
        protocol_tests.cpp
)

target_link_libraries(
    metaplasia_protocol_tests
    PRIVATE
        metaplasia_protocol
)

metaplasia_configure_target(metaplasia_protocol_tests)
add_test(NAME protocol_codec COMMAND metaplasia_protocol_tests)

add_executable(
    metaplasia_trust_tests
        trust_tests.cpp
)

target_link_libraries(
    metaplasia_trust_tests
    PRIVATE
        metaplasia_trust
)

metaplasia_configure_target(metaplasia_trust_tests)
add_test(NAME component_trust COMMAND metaplasia_trust_tests)

add_executable(
    metaplasia_named_pipe_tests
        named_pipe_tests.cpp
)

target_link_libraries(
    metaplasia_named_pipe_tests
    PRIVATE
        metaplasia_platform
        metaplasia_protocol
)

metaplasia_configure_target(metaplasia_named_pipe_tests)
add_test(NAME named_pipe_transactions COMMAND metaplasia_named_pipe_tests)

add_executable(
    metaplasia_settings_tests
        settings_store_tests.cpp
)

target_link_libraries(
    metaplasia_settings_tests
    PRIVATE
        metaplasia_host_settings
)

metaplasia_configure_target(metaplasia_settings_tests)
add_test(NAME settings_store COMMAND metaplasia_settings_tests)

add_executable(
    metaplasia_diagnostic_log_tests
        diagnostic_log_tests.cpp
)

target_link_libraries(
    metaplasia_diagnostic_log_tests
    PRIVATE
        metaplasia_host_diagnostics
)

metaplasia_configure_target(metaplasia_diagnostic_log_tests)
add_test(NAME diagnostic_log COMMAND metaplasia_diagnostic_log_tests)

add_executable(
    metaplasia_reconciliation_policy_tests
        reconciliation_policy_tests.cpp
)

target_link_libraries(
    metaplasia_reconciliation_policy_tests
    PRIVATE
        metaplasia_host_settings
)

metaplasia_configure_target(metaplasia_reconciliation_policy_tests)
add_test(
    NAME reconciliation_policy
    COMMAND metaplasia_reconciliation_policy_tests
)

add_executable(
    metaplasia_watchdog_protocol_tests
        watchdog_protocol_tests.cpp
)

target_link_libraries(
    metaplasia_watchdog_protocol_tests
    PRIVATE
        metaplasia_watchdog_protocol
)

metaplasia_configure_target(metaplasia_watchdog_protocol_tests)
add_test(NAME watchdog_protocol COMMAND metaplasia_watchdog_protocol_tests)

add_executable(
    metaplasia_symbol_tests
        symbol_tests.cpp
)

target_link_libraries(
    metaplasia_symbol_tests
    PRIVATE
        metaplasia_symbols
)

metaplasia_configure_target(metaplasia_symbol_tests)
add_test(NAME symbols COMMAND metaplasia_symbol_tests)

add_executable(
    metaplasia_compatibility_tests
        compatibility_tests.cpp
)

add_library(
    metaplasia_compatibility_pack_fixture
    MODULE
        compatibility_pack_fixture.rc
)

set_target_properties(
    metaplasia_compatibility_pack_fixture
    PROPERTIES
        LINKER_LANGUAGE CXX
        PREFIX ""
        OUTPUT_NAME "metaplasia-compatibility-pack-fixture"
)

target_link_options(
    metaplasia_compatibility_pack_fixture
    PRIVATE
        /NOENTRY
        /DYNAMICBASE
        /INCREMENTAL:NO
        /MANIFEST:NO
        /NXCOMPAT
)

target_link_libraries(
    metaplasia_compatibility_tests
    PRIVATE
        metaplasia_compatibility
)

metaplasia_configure_target(metaplasia_compatibility_tests)
add_test(
    NAME compatibility_catalog
    COMMAND
        metaplasia_compatibility_tests
        $<TARGET_FILE:metaplasia_compatibility_pack_fixture>
)

add_executable(
    metaplasia_injector_policy_tests
        injector_policy_tests.cpp
)

target_link_libraries(
    metaplasia_injector_policy_tests
    PRIVATE
        metaplasia_injector
)

metaplasia_configure_target(metaplasia_injector_policy_tests)
add_test(NAME injector_policy COMMAND metaplasia_injector_policy_tests)

add_executable(
    metaplasia_shell_xaml_style_tests
        start_menu_style_tests.cpp
)

target_link_libraries(
    metaplasia_shell_xaml_style_tests
    PRIVATE
        metaplasia_shell_xaml_style
)

metaplasia_configure_target(metaplasia_shell_xaml_style_tests)
add_test(NAME shell_xaml_style COMMAND metaplasia_shell_xaml_style_tests)

add_executable(
    metaplasia_agent_tests
        agent_tests.cpp
)

target_link_libraries(
    metaplasia_agent_tests
    PRIVATE
        metaplasia_protocol
)

target_include_directories(
    metaplasia_agent_tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/agent/include
)

set_target_properties(
    metaplasia_agent_tests
    PROPERTIES
        OUTPUT_NAME "explorer"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/agent-harness"
        PDB_NAME "metaplasia-agent-tests"
)

metaplasia_configure_target(metaplasia_agent_tests)
add_dependencies(metaplasia_agent_tests metaplasia_agent)
add_test(
    NAME agent_hooks
    COMMAND
        $<TARGET_FILE:metaplasia_agent_tests>
        $<TARGET_FILE:metaplasia_agent>
)
