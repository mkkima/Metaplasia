add_library(
    metaplasia_base
    STATIC
        src/status.cpp
        src/utf.cpp
        src/windows_paths.cpp
)

target_include_directories(
    metaplasia_base
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_base
    PUBLIC
        metaplasia_project_options
    PRIVATE
        shell32
)

metaplasia_configure_target(metaplasia_base)
