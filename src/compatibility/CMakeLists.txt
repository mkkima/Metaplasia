add_library(
    metaplasia_compatibility
    STATIC
        src/catalog.cpp
        src/profile_pack.cpp
)

target_include_directories(
    metaplasia_compatibility
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_compatibility
    PUBLIC
        metaplasia_base
        metaplasia_platform
        metaplasia_symbols
        metaplasia_trust
    PRIVATE
        advapi32
)

metaplasia_configure_target(metaplasia_compatibility)
