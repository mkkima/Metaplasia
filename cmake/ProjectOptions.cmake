add_library(metaplasia_project_options INTERFACE)
target_compile_features(metaplasia_project_options INTERFACE cxx_std_20)
target_compile_definitions(
    metaplasia_project_options
    INTERFACE
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        STRICT
)

if(MSVC)
    target_compile_options(
        metaplasia_project_options
        INTERFACE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /Zc:preprocessor
            /Zc:inline
            /volatile:iso
    )

    if(METAPLASIA_ENABLE_HARDENING)
        target_compile_options(
            metaplasia_project_options
            INTERFACE
                /sdl
                /guard:cf
        )
        target_link_options(
            metaplasia_project_options
            INTERFACE
                /DYNAMICBASE
                /NXCOMPAT
                /guard:cf
                /CETCOMPAT
        )
    endif()
endif()

function(metaplasia_configure_target target)
    target_link_libraries(${target} PRIVATE metaplasia_project_options)
    set_target_properties(
        ${target}
        PROPERTIES
            CXX_STANDARD 20
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO
    )
endfunction()
