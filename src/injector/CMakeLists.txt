add_library(
    metaplasia_injector
    STATIC
        src/conflict_policy.cpp
        src/injector.cpp
        src/remote_image.cpp
)

target_include_directories(
    metaplasia_injector
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_injector
    PUBLIC
        metaplasia_base
        metaplasia_platform
        metaplasia_protocol
        metaplasia_trust
)

metaplasia_configure_target(metaplasia_injector)
