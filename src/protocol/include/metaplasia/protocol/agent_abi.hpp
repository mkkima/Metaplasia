#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace metaplasia::protocol {

inline constexpr std::uint32_t kAgentConfigMagic = 0x4741504D;  // "MPAG"
inline constexpr std::uint16_t kAgentAbiVersion = 11;
inline constexpr std::size_t kMaximumClockPrefixLength = 15;
inline constexpr std::size_t kMaximumExplorerTitlePrefixLength = 31;
inline constexpr std::uint32_t kDefaultTaskbarOpacityMilli = 1000;
inline constexpr std::uint32_t kMinimumTaskbarOpacityMilli = 100;
inline constexpr std::uint32_t kMaximumTaskbarOpacityMilli = 1000;
inline constexpr std::uint32_t kDefaultStartMenuOpacityMilli = 940;
inline constexpr std::uint32_t kMinimumStartMenuOpacityMilli = 100;
inline constexpr std::uint32_t kMaximumStartMenuOpacityMilli = 1000;
inline constexpr std::uint32_t kDefaultShellBackgroundColor = 0xFF000000U;
inline constexpr std::uint32_t kExplorerTransitionNone = 0;
inline constexpr std::uint32_t kExplorerTransitionFade = 1;
inline constexpr std::uint32_t kExplorerTransitionSlideFade = 2;
inline constexpr std::uint32_t kExplorerTransitionScaleFade = 3;
inline constexpr std::uint32_t kDefaultExplorerTransition =
    kExplorerTransitionFade;
inline constexpr std::uint32_t kMaximumExplorerTransition =
    kExplorerTransitionScaleFade;

enum class AgentTarget : std::uint16_t {
    explorer_shell = 1,
    start_menu = 2,
};

enum AgentFeature : std::uint32_t {
    agent_feature_none = 0,
    agent_feature_taskbar_clock_prefix = 1U << 0U,
    agent_feature_file_explorer_title_prefix = 1U << 1U,
    agent_feature_start_menu_root_opacity = 1U << 2U,
    agent_feature_taskbar_background_color = 1U << 3U,
    agent_feature_file_explorer_background_color = 1U << 4U,
    agent_feature_start_menu_background_color = 1U << 5U,
    agent_feature_file_explorer_custom_scrollbar = 1U << 6U,
    agent_feature_taskbar_capsule = 1U << 7U,
    agent_feature_start_menu_three_panel_layout = 1U << 8U,
};

enum class AgentResult : std::uint32_t {
    success = 0,
    invalid_configuration = 1,
    incompatible_process = 2,
    initialization_failed = 3,
    hook_failed = 4,
    not_initialized = 5,
    busy = 6,
    adapter_unavailable = 7,
};

// Diagnostic progress for adapters that depend on undocumented shell-host
// behavior. This is intentionally separate from AgentResult: callers can
// handle stable result codes while still receiving actionable compatibility
// details after a Windows update.
enum class AgentDiagnosticStage : std::uint32_t {
    none = 0,
    initialize_xaml_diagnostics = 1,
    request_class_factory = 2,
    create_tap_instance = 3,
    set_tap_site = 4,
    query_xaml_diagnostics = 5,
    query_visual_tree_service = 6,
    attach_controller = 7,
    advise_visual_tree = 8,
    controller_attached = 9,
    get_tap_site = 10,
    lock_class_factory = 11,
    tap_site_released = 12,
    tap_site_detached = 13,
};

// Fixed-size, versioned ABI copied to the target process by the injector.
// Keep this structure trivially copyable and do not add pointers.
struct AgentConfiguration final {
    std::uint32_t magic{kAgentConfigMagic};
    std::uint16_t version{kAgentAbiVersion};
    std::uint16_t size{sizeof(AgentConfiguration)};
    AgentTarget target{AgentTarget::explorer_shell};
    std::uint16_t reserved{0};
    std::uint32_t feature_flags{agent_feature_none};
    wchar_t taskbar_clock_prefix[kMaximumClockPrefixLength + 1]{};
    wchar_t explorer_title_prefix[kMaximumExplorerTitlePrefixLength + 1]{};
    std::uint32_t taskbar_opacity_milli{kDefaultTaskbarOpacityMilli};
    std::uint8_t taskbar_hide_notification_center{0};
    std::uint8_t taskbar_hide_control_center{0};
    std::uint8_t taskbar_hide_show_desktop{0};
    std::uint8_t taskbar_capsule_enabled{0};
    std::uint8_t file_explorer_transition_animation{
        static_cast<std::uint8_t>(kDefaultExplorerTransition)};
    std::uint32_t start_menu_opacity_milli{kDefaultStartMenuOpacityMilli};
    std::uint8_t start_menu_hide_recommended{0};
    std::uint8_t taskbar_background_color_enabled{0};
    std::uint8_t file_explorer_background_color_enabled{0};
    std::uint8_t start_menu_background_color_enabled{0};
    std::uint32_t taskbar_background_color{kDefaultShellBackgroundColor};
    std::uint32_t file_explorer_background_color{kDefaultShellBackgroundColor};
    std::uint32_t start_menu_background_color{kDefaultShellBackgroundColor};
    std::uint8_t start_menu_three_panel_layout_enabled{0};
    std::uint8_t start_menu_hide_all_apps{0};
    std::uint8_t reserved_tail[2]{};
};

static_assert(sizeof(wchar_t) == 2, "Agent ABI requires Windows UTF-16 wchar_t");
static_assert(sizeof(AgentConfiguration) == 148);

using AgentEntryPoint = std::uint32_t(__stdcall*)(void* configuration);

inline constexpr char kAgentStartExport[] = "MetaplasiaAgentStart";
inline constexpr char kAgentConfigureExport[] = "MetaplasiaAgentConfigure";
inline constexpr char kAgentStopExport[] = "MetaplasiaAgentStop";
inline constexpr char kAgentGetLastErrorExport[] =
    "MetaplasiaAgentGetLastError";
inline constexpr char kAgentGetDiagnosticStageExport[] =
    "MetaplasiaAgentGetDiagnosticStage";
inline constexpr char kAgentGetStartMenuStateExport[] =
    "MetaplasiaAgentGetStartMenuState";
inline constexpr char kAgentGetXamlDiagnosticsExport[] =
    "MetaplasiaAgentGetXamlDiagnostics";

inline constexpr std::uint32_t kXamlDiagnosticsMagic =
    0x4458504D;  // "MPXD"
inline constexpr std::uint16_t kXamlDiagnosticsVersion = 1;
inline constexpr std::size_t kMaximumXamlDiagnosticTypes = 64;
inline constexpr std::size_t kMaximumXamlDiagnosticTypeNameLength = 127;
inline constexpr std::size_t kMaximumXamlDiagnosticElements = 128;
inline constexpr std::size_t kMaximumXamlDiagnosticElementNameLength = 63;

struct XamlTypeDiagnostic final {
    wchar_t type_name[kMaximumXamlDiagnosticTypeNameLength + 1]{};
    std::uint32_t observation_count{0};
};

struct XamlElementDiagnostic final {
    std::uint64_t handle{0};
    std::uint64_t parent_handle{0};
    std::uint32_t child_index{0};
    std::uint32_t child_count{0};
    std::uint16_t type_index{0};
    std::uint16_t reserved{0};
    wchar_t name[kMaximumXamlDiagnosticElementNameLength + 1]{};
};

struct XamlDiagnosticsSnapshot final {
    std::uint32_t magic{kXamlDiagnosticsMagic};
    std::uint16_t version{kXamlDiagnosticsVersion};
    std::uint16_t size{sizeof(XamlDiagnosticsSnapshot)};
    AgentTarget target{AgentTarget::start_menu};
    std::uint16_t reserved{0};
    std::uint32_t type_count{0};
    std::uint32_t dropped_type_count{0};
    std::uint32_t element_count{0};
    std::uint32_t dropped_element_count{0};
    std::uint32_t tracked_element_count{0};
    std::uint32_t reserved_state{0};
    XamlTypeDiagnostic types[kMaximumXamlDiagnosticTypes]{};
    XamlElementDiagnostic elements[kMaximumXamlDiagnosticElements]{};
};

static_assert(sizeof(XamlTypeDiagnostic) == 260);
static_assert(sizeof(XamlElementDiagnostic) == 160);
static_assert(std::is_trivially_copyable_v<XamlDiagnosticsSnapshot>);
static_assert(sizeof(XamlDiagnosticsSnapshot) < 64U * 1024U);

}  // namespace metaplasia::protocol
