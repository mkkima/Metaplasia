add_executable(
    metaplasia_watchdog
        src/main.cpp
)

target_link_libraries(
    metaplasia_watchdog
    PRIVATE
        metaplasia_base
        metaplasia_host_diagnostics
        metaplasia_host_settings
        metaplasia_injector
        metaplasia_platform
        metaplasia_protocol
        metaplasia_watchdog_protocol
)

set_target_properties(
    metaplasia_watchdog
    PROPERTIES
        OUTPUT_NAME "metaplasia-watchdog"
)

metaplasia_configure_target(metaplasia_watchdog)
