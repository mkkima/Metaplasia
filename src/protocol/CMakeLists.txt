add_library(
    metaplasia_protocol
    STATIC
        src/codec.cpp
)

target_include_directories(
    metaplasia_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_protocol
    PUBLIC
        metaplasia_base
)

metaplasia_configure_target(metaplasia_protocol)
