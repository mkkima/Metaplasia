cmake_minimum_required(VERSION 3.24)

project(Metaplasia VERSION 0.1.5 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Metaplasia currently supports Windows only")
endif()

if(MSVC)
    # Keep the five-file release self-contained. Requiring a separately
    # installed VC++ redistributable would violate the portable distribution
    # contract and could also split CRT ownership across injected components.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(METAPLASIA_BUILD_UI "Build the Tauri control center" ON)
option(METAPLASIA_BUILD_TESTS "Build unit tests" ON)
option(METAPLASIA_ENABLE_HARDENING "Enable compiler and linker hardening" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)

include(FetchContent)

# Pin third-party dependencies to immutable commits. Updating them is an
# intentional compatibility decision, especially for code loaded into Explorer.
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG c3fcafdc10146beb5919319d0683e44e3c30d537
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(minhook)

add_subdirectory(src)

if(METAPLASIA_BUILD_UI)
    find_program(METAPLASIA_CARGO_EXECUTABLE cargo REQUIRED)

    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(METAPLASIA_TAURI_PROFILE "release")
        set(METAPLASIA_TAURI_RELEASE_ARG "--release")
    else()
        set(METAPLASIA_TAURI_PROFILE "debug")
        set(METAPLASIA_TAURI_RELEASE_ARG "")
    endif()

    set(METAPLASIA_TAURI_MANIFEST
        "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/Cargo.toml")
    set(METAPLASIA_TAURI_TARGET_DIR
        "${CMAKE_BINARY_DIR}/tauri-target")
    file(GLOB_RECURSE METAPLASIA_TAURI_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/src/*.rs"
        "${CMAKE_CURRENT_SOURCE_DIR}/frontend/*.html"
        "${CMAKE_CURRENT_SOURCE_DIR}/frontend/*.css"
        "${CMAKE_CURRENT_SOURCE_DIR}/frontend/*.js")

    add_custom_target(
        metaplasia ALL
        COMMAND
            ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${METAPLASIA_TAURI_TARGET_DIR}"
            ${METAPLASIA_CARGO_EXECUTABLE} build
            --manifest-path "${METAPLASIA_TAURI_MANIFEST}"
            --locked
            ${METAPLASIA_TAURI_RELEASE_ARG}
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${METAPLASIA_TAURI_TARGET_DIR}/${METAPLASIA_TAURI_PROFILE}/metaplasia.exe"
            "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/metaplasia.exe"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        DEPENDS
            ${METAPLASIA_TAURI_SOURCES}
            "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/Cargo.lock"
            "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/Cargo.toml"
            "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/build.rs"
            "${CMAKE_CURRENT_SOURCE_DIR}/src-tauri/tauri.conf.json"
            metaplasia_agent
            metaplasia_host
            metaplasia_watchdog
        COMMENT "Building the Tauri control center"
        VERBATIM
    )
endif()

if(METAPLASIA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
