add_executable(
    metaplasia_cli
        src/main.cpp
)

target_link_libraries(
    metaplasia_cli
    PRIVATE
        metaplasia_base
        metaplasia_compatibility
        metaplasia_platform
        metaplasia_protocol
        metaplasia_symbols
        metaplasia_trust
)

target_compile_definitions(
    metaplasia_cli
    PRIVATE
        $<$<CONFIG:Debug>:METAPLASIA_DEVELOPMENT_TRUST=1>
)

set_target_properties(
    metaplasia_cli
    PROPERTIES
        OUTPUT_NAME "metaplasia-cli"
)

metaplasia_configure_target(metaplasia_cli)
