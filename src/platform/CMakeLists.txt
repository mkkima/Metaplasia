add_library(
    metaplasia_platform
    STATIC
        src/named_pipe.cpp
        src/process.cpp
        src/security.cpp
)

target_include_directories(
    metaplasia_platform
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_platform
    PUBLIC
        metaplasia_base
        metaplasia_protocol
    PRIVATE
        advapi32
)

metaplasia_configure_target(metaplasia_platform)
