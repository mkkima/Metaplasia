add_library(
    metaplasia_host_settings
    STATIC
        src/settings_store.cpp
)

target_include_directories(
    metaplasia_host_settings
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_host_settings
    PUBLIC
        metaplasia_base
        metaplasia_protocol
)

metaplasia_configure_target(metaplasia_host_settings)

add_library(
    metaplasia_host_diagnostics
    STATIC
        src/diagnostic_log.cpp
)

target_include_directories(
    metaplasia_host_diagnostics
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_host_diagnostics
    PUBLIC
        metaplasia_base
)

metaplasia_configure_target(metaplasia_host_diagnostics)

add_executable(
    metaplasia_host
        src/engine_controller.cpp
        src/main.cpp
        src/watchdog_client.cpp
)

target_include_directories(
    metaplasia_host
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_host
    PRIVATE
        metaplasia_base
        metaplasia_compatibility
        metaplasia_host_diagnostics
        metaplasia_host_settings
        metaplasia_injector
        metaplasia_platform
        metaplasia_protocol
        metaplasia_trust
        metaplasia_watchdog_protocol
        bcrypt
)

target_compile_definitions(
    metaplasia_host
    PRIVATE
        $<$<CONFIG:Debug>:METAPLASIA_DEVELOPMENT_TRUST=1>
)

set_target_properties(
    metaplasia_host
    PROPERTIES
        OUTPUT_NAME "metaplasia-host"
)

metaplasia_configure_target(metaplasia_host)
