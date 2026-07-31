add_library(
    metaplasia_watchdog_protocol
    STATIC
        src/protocol.cpp
)

target_include_directories(
    metaplasia_watchdog_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_watchdog_protocol
    PUBLIC
        metaplasia_base
)

metaplasia_configure_target(metaplasia_watchdog_protocol)
