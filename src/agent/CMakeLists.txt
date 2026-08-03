add_library(
    metaplasia_shell_xaml_style
    STATIC
        src/start_menu_style.cpp
)

target_include_directories(
    metaplasia_shell_xaml_style
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(
    metaplasia_shell_xaml_style
    PUBLIC
        metaplasia_protocol
)

metaplasia_configure_target(metaplasia_shell_xaml_style)

add_library(
    metaplasia_agent
    SHARED
        src/agent.cpp
        src/explorer_window_color.cpp
        src/explorer_winui_adapter.cpp
        src/agent.def
        src/start_menu_xaml_adapter.cpp
)

# File Explorer's command/navigation/tab surfaces use the in-box Windows App
# SDK (WinUI 3), not Windows.UI.Xaml. Generate projections from the exact
# metadata shipped with this Windows image so the injected adapter has no
# redistributable or package dependency of its own.
file(GLOB METAPLASIA_CPPWINRT_CANDIDATES
    "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/cppwinrt.exe")
if(NOT METAPLASIA_CPPWINRT_CANDIDATES)
    message(FATAL_ERROR "cppwinrt.exe was not found in the Windows SDK")
endif()
list(SORT METAPLASIA_CPPWINRT_CANDIDATES COMPARE NATURAL ORDER DESCENDING)
list(GET METAPLASIA_CPPWINRT_CANDIDATES 0 METAPLASIA_CPPWINRT_EXECUTABLE)
set(METAPLASIA_WASDK_METADATA_DIR
    "C:/Windows/SystemApps/Microsoft.WindowsAppRuntime.CBS_8wekyb3d8bbwe")
file(GLOB METAPLASIA_WINDOWS_WINMD_CANDIDATES
    "C:/Program Files (x86)/Windows Kits/10/UnionMetadata/[0-9]*.*/Windows.winmd")
if(NOT METAPLASIA_WINDOWS_WINMD_CANDIDATES)
    message(FATAL_ERROR "Windows.winmd was not found in the Windows SDK")
endif()
list(SORT METAPLASIA_WINDOWS_WINMD_CANDIDATES
    COMPARE NATURAL ORDER DESCENDING)
list(GET METAPLASIA_WINDOWS_WINMD_CANDIDATES 0 METAPLASIA_WINDOWS_WINMD)
set(METAPLASIA_CPPWINRT_OUTPUT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/cppwinrt")
set(METAPLASIA_CPPWINRT_STAMP
    "${METAPLASIA_CPPWINRT_OUTPUT}/metaplasia.stamp")
add_custom_command(
    OUTPUT "${METAPLASIA_CPPWINRT_STAMP}"
    COMMAND "${METAPLASIA_CPPWINRT_EXECUTABLE}"
        -input "${METAPLASIA_WASDK_METADATA_DIR}"
        -reference "${METAPLASIA_WASDK_METADATA_DIR}"
        -reference "${METAPLASIA_WINDOWS_WINMD}"
        -output "${METAPLASIA_CPPWINRT_OUTPUT}"
    COMMAND ${CMAKE_COMMAND} -E touch "${METAPLASIA_CPPWINRT_STAMP}"
    DEPENDS
        "${METAPLASIA_WASDK_METADATA_DIR}/Microsoft.UI.winmd"
        "${METAPLASIA_WASDK_METADATA_DIR}/Microsoft.UI.Xaml.winmd"
    COMMENT "Generating in-box WinUI 3 C++ projections"
    VERBATIM
)
add_custom_target(
    metaplasia_explorer_cppwinrt
    DEPENDS "${METAPLASIA_CPPWINRT_STAMP}"
)
add_dependencies(metaplasia_agent metaplasia_explorer_cppwinrt)

target_include_directories(
    metaplasia_agent
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${METAPLASIA_CPPWINRT_OUTPUT}
)

target_link_libraries(
    metaplasia_agent
    PRIVATE
        metaplasia_protocol
        metaplasia_shell_xaml_style
        minhook
        ole32
        oleaut32
        runtimeobject
        uiautomationcore
        dwmapi
        comctl32
        shell32
        windowscodecs
)

target_compile_definitions(metaplasia_agent PRIVATE METAPLASIA_AGENT_BUILD)
set_target_properties(
    metaplasia_agent
    PROPERTIES
        OUTPUT_NAME "metaplasia-agent"
        PREFIX ""
)

metaplasia_configure_target(metaplasia_agent)
