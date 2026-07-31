add_library(
    metaplasia_trust
    STATIC
        src/authenticode.cpp
)

target_include_directories(
    metaplasia_trust
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_trust
    PUBLIC
        metaplasia_base
    PRIVATE
        crypt32
        wintrust
)

metaplasia_configure_target(metaplasia_trust)
