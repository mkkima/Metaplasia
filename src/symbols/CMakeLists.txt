add_library(
    metaplasia_symbols
    STATIC
        src/pe_image.cpp
        src/symbol_resolver.cpp
)

target_include_directories(
    metaplasia_symbols
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_symbols
    PUBLIC
        metaplasia_base
    PRIVATE
        Bcrypt
        Dbghelp
)

metaplasia_configure_target(metaplasia_symbols)
