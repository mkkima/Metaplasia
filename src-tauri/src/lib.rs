mod diagnostics;
mod portable_update;
mod protocol;
mod start_menu_policy;

use protocol::{MessageKind, PipeClient, Reader, expect_kind, write_string, write_u32};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::{Duration, Instant};
use tauri::{Manager, WebviewWindow};

const PIPE_TIMEOUT: Duration = Duration::from_millis(2_500);
const HOST_LAUNCH_COOLDOWN: Duration = Duration::from_secs(5);
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const DWMWA_USE_IMMERSIVE_DARK_MODE: u32 = 20;
const DWMWA_BORDER_COLOR: u32 = 34;
const DWMWA_CAPTION_COLOR: u32 = 35;
const DWMWA_TEXT_COLOR: u32 = 36;
static UPDATE_IN_PROGRESS: AtomicBool = AtomicBool::new(false);

#[link(name = "dwmapi")]
unsafe extern "system" {
    fn DwmSetWindowAttribute(
        window: *mut std::ffi::c_void,
        attribute: u32,
        value: *const std::ffi::c_void,
        value_size: u32,
    ) -> i32;
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct TargetSnapshot {
    id: String,
    name: String,
    state: String,
    enabled: bool,
    process_running: bool,
    agent_loaded: bool,
    process_id: u32,
    detail: String,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
struct Settings {
    taskbar_enabled: bool,
    file_explorer_enabled: bool,
    start_menu_enabled: bool,
    taskbar_clock_prefix: String,
    taskbar_opacity: u32,
    taskbar_hide_notification_center: bool,
    taskbar_hide_control_center: bool,
    taskbar_hide_show_desktop: bool,
    taskbar_capsule_enabled: bool,
    taskbar_background_color_enabled: bool,
    taskbar_background_color: u32,
    file_explorer_title_prefix: String,
    file_explorer_background_color_enabled: bool,
    file_explorer_background_color: u32,
    file_explorer_transition_animation: u32,
    file_explorer_custom_scrollbar_enabled: bool,
    start_menu_opacity: u32,
    start_menu_hide_recommended: bool,
    start_menu_background_color_enabled: bool,
    start_menu_background_color: u32,
    start_menu_three_panel_layout_enabled: bool,
    start_menu_three_panel_hide_all_apps: bool,
    start_menu_hide_all_apps: bool,
    start_menu_hide_all_apps_policy_active: bool,
    start_menu_hide_all_apps_supported: bool,
    start_menu_hide_all_apps_editable: bool,
    start_menu_hide_all_apps_detail: String,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            taskbar_enabled: false,
            file_explorer_enabled: false,
            start_menu_enabled: false,
            taskbar_clock_prefix: "M ".into(),
            taskbar_opacity: 100,
            taskbar_hide_notification_center: false,
            taskbar_hide_control_center: false,
            taskbar_hide_show_desktop: false,
            taskbar_capsule_enabled: false,
            taskbar_background_color_enabled: false,
            taskbar_background_color: 0xFF000000,
            file_explorer_title_prefix: "Meta · ".into(),
            file_explorer_background_color_enabled: false,
            file_explorer_background_color: 0xFF000000,
            file_explorer_transition_animation: 1,
            file_explorer_custom_scrollbar_enabled: true,
            start_menu_opacity: 100,
            start_menu_hide_recommended: false,
            start_menu_background_color_enabled: false,
            start_menu_background_color: 0xFF000000,
            start_menu_three_panel_layout_enabled: false,
            start_menu_three_panel_hide_all_apps: false,
            start_menu_hide_all_apps: false,
            start_menu_hide_all_apps_policy_active: false,
            start_menu_hide_all_apps_supported: false,
            start_menu_hide_all_apps_editable: false,
            start_menu_hide_all_apps_detail: "Unable to inspect the Windows policy.".into(),
        }
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct AppState {
    connected: bool,
    connection_detail: String,
    targets: Vec<TargetSnapshot>,
    settings: Settings,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct CommandResult {
    accepted: bool,
    detail: String,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct ToggleTargetRequest {
    target: String,
    enabled: bool,
    confirmed: bool,
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct CustomizationRequest {
    id: String,
    text_value: Option<String>,
    integer_value: Option<u32>,
    boolean_value: Option<bool>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct XamlTypeObservation {
    type_name: String,
    observation_count: u32,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct XamlElementObservation {
    handle: u64,
    parent_handle: u64,
    child_index: u32,
    child_count: u32,
    type_index: u16,
    name: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct XamlDiagnostics {
    dropped_type_count: u32,
    dropped_element_count: u32,
    tracked_element_count: u32,
    types: Vec<XamlTypeObservation>,
    elements: Vec<XamlElementObservation>,
}

#[tauri::command]
async fn get_app_state() -> AppState {
    tauri::async_runtime::spawn_blocking(query_app_state)
        .await
        .unwrap_or_else(|error| disconnected_state(format!("State worker failed: {error}")))
}

#[tauri::command]
async fn set_target_enabled(request: ToggleTargetRequest) -> Result<CommandResult, String> {
    if request.enabled && !request.confirmed {
        return Err("Enabling a shell target requires explicit confirmation".into());
    }
    let target = target_id(&request.target)?;
    tauri::async_runtime::spawn_blocking(move || {
        let client = PipeClient::for_current_session().map_err(|error| error.to_string())?;
        let frame = client
            .transact(
                MessageKind::SetEnabledRequest,
                &[target, u8::from(request.enabled)],
                PIPE_TIMEOUT,
            )
            .map_err(|error| error.to_string())?;
        parse_command_response(frame)
    })
    .await
    .map_err(|error| format!("Command worker failed: {error}"))?
}

#[tauri::command]
async fn set_customization(request: CustomizationRequest) -> Result<CommandResult, String> {
    let payload = encode_customization(request)?;
    tauri::async_runtime::spawn_blocking(move || {
        let client = PipeClient::for_current_session().map_err(|error| error.to_string())?;
        let frame = client
            .transact(MessageKind::SetCustomizationRequest, &payload, PIPE_TIMEOUT)
            .map_err(|error| error.to_string())?;
        parse_command_response(frame)
    })
    .await
    .map_err(|error| format!("Command worker failed: {error}"))?
}

#[tauri::command]
async fn set_start_all_apps_hidden(
    hidden: bool,
    three_panel_enabled: bool,
) -> Result<CommandResult, String> {
    tauri::async_runtime::spawn_blocking(move || {
        if three_panel_enabled {
            let policy_was_active = start_menu_policy::query()?.hidden;
            if policy_was_active {
                // The device policy constrains Start's top-level popup to the
                // native two-column width. Remove it before applying the
                // injected three-panel visibility setting.
                start_menu_policy::set_hidden(false)?;
            }
            let payload = encode_customization(CustomizationRequest {
                id: "startHideAllApps".into(),
                text_value: None,
                integer_value: None,
                boolean_value: Some(hidden),
            })?;
            let client = PipeClient::for_current_session().map_err(|error| error.to_string())?;
            let frame = client
                .transact(MessageKind::SetCustomizationRequest, &payload, PIPE_TIMEOUT)
                .map_err(|error| error.to_string())?;
            let mut result = parse_command_response(frame)?;
            result.detail = if policy_was_active {
                "The incompatible Windows policy was removed and the Three-panel All apps content was updated."
            } else if hidden {
                "Three-panel All apps content hidden."
            } else {
                "Three-panel All apps content restored."
            }
            .into();
            Ok(result)
        } else {
            start_menu_policy::set_hidden(hidden)?;
            Ok(CommandResult {
                accepted: true,
                detail: if hidden {
                    "All apps content hidden. The Windows shell was refreshed."
                } else {
                    "All apps content restored. The Windows shell was refreshed."
                }
                .into(),
            })
        }
    })
    .await
    .map_err(|error| format!("Policy worker failed: {error}"))?
}

#[tauri::command]
async fn get_xaml_diagnostics(target: String) -> Result<XamlDiagnostics, String> {
    let target = target_id(&target)?;
    if target != 1 && target != 3 {
        return Err("XAML diagnostics are available only for Taskbar and Start menu".into());
    }
    tauri::async_runtime::spawn_blocking(move || {
        let client = PipeClient::for_current_session().map_err(|error| error.to_string())?;
        let frame = client
            .transact(
                MessageKind::GetXamlDiagnosticsRequest,
                &[target],
                PIPE_TIMEOUT,
            )
            .map_err(|error| error.to_string())?;
        parse_xaml_diagnostics(frame)
    })
    .await
    .map_err(|error| format!("Diagnostics worker failed: {error}"))?
}

fn query_app_state() -> AppState {
    if UPDATE_IN_PROGRESS.load(Ordering::Acquire) {
        return disconnected_state(
            "Portable update is preparing to restart the application".into(),
        );
    }
    match try_query_app_state() {
        Ok(state) => state,
        Err(first_error) => {
            if let Err(launch_error) = ensure_host_started() {
                return disconnected_state(format!("{first_error}. {launch_error}"));
            }
            std::thread::sleep(Duration::from_millis(150));
            try_query_app_state().unwrap_or_else(disconnected_state)
        }
    }
}

fn try_query_app_state() -> Result<AppState, String> {
    let client = PipeClient::for_current_session().map_err(|error| error.to_string())?;
    let snapshot_frame = client
        .transact(MessageKind::GetSnapshotRequest, &[], PIPE_TIMEOUT)
        .map_err(|error| error.to_string())?;
    expect_kind(&snapshot_frame, MessageKind::SnapshotResponse)
        .map_err(|error| error.to_string())?;
    let targets = parse_snapshots(&snapshot_frame.payload)?;

    let settings_frame = client
        .transact(MessageKind::GetSettingsRequest, &[], PIPE_TIMEOUT)
        .map_err(|error| error.to_string())?;
    expect_kind(&settings_frame, MessageKind::SettingsResponse)
        .map_err(|error| error.to_string())?;
    let settings = with_start_app_list_policy(parse_settings(&settings_frame.payload)?);

    Ok(AppState {
        connected: true,
        connection_detail: "Local host connected".into(),
        targets,
        settings,
    })
}

fn parse_snapshots(payload: &[u8]) -> Result<Vec<TargetSnapshot>, String> {
    let mut reader = Reader::new(payload);
    let count = reader.u8().map_err(|error| error.to_string())?;
    if count > 16 {
        return Err("Host returned too many targets".into());
    }
    let mut snapshots = Vec::with_capacity(count as usize);
    for _ in 0..count {
        let target = reader.u8().map_err(|error| error.to_string())?;
        let state = reader.u8().map_err(|error| error.to_string())?;
        let enabled = read_bool(&mut reader)?;
        let process_running = read_bool(&mut reader)?;
        let agent_loaded = read_bool(&mut reader)?;
        let process_id = reader.u32().map_err(|error| error.to_string())?;
        let detail = reader.string().map_err(|error| error.to_string())?;
        let (id, name) = target_metadata(target)?;
        let state = state_name(state)?;
        snapshots.push(TargetSnapshot {
            id: id.into(),
            name: name.into(),
            state: state.into(),
            enabled,
            process_running,
            agent_loaded,
            process_id,
            detail,
        });
    }
    reader.finish().map_err(|error| error.to_string())?;
    Ok(snapshots)
}

fn parse_settings(payload: &[u8]) -> Result<Settings, String> {
    let mut reader = Reader::new(payload);
    let settings = Settings {
        taskbar_enabled: read_bool(&mut reader)?,
        file_explorer_enabled: read_bool(&mut reader)?,
        start_menu_enabled: read_bool(&mut reader)?,
        taskbar_clock_prefix: reader.string().map_err(|error| error.to_string())?,
        taskbar_opacity: opacity_percent(reader.u32().map_err(|error| error.to_string())?)?,
        taskbar_hide_notification_center: read_bool(&mut reader)?,
        taskbar_hide_control_center: read_bool(&mut reader)?,
        taskbar_hide_show_desktop: read_bool(&mut reader)?,
        taskbar_capsule_enabled: read_bool(&mut reader)?,
        taskbar_background_color_enabled: read_bool(&mut reader)?,
        taskbar_background_color: validated_color(
            reader.u32().map_err(|error| error.to_string())?,
        )?,
        file_explorer_title_prefix: reader.string().map_err(|error| error.to_string())?,
        file_explorer_background_color_enabled: read_bool(&mut reader)?,
        file_explorer_background_color: validated_color(
            reader.u32().map_err(|error| error.to_string())?,
        )?,
        file_explorer_transition_animation: validated_transition_animation(
            reader.u32().map_err(|error| error.to_string())?,
        )?,
        file_explorer_custom_scrollbar_enabled: read_bool(&mut reader)?,
        start_menu_opacity: opacity_percent(reader.u32().map_err(|error| error.to_string())?)?,
        start_menu_hide_recommended: read_bool(&mut reader)?,
        start_menu_background_color_enabled: read_bool(&mut reader)?,
        start_menu_background_color: validated_color(
            reader.u32().map_err(|error| error.to_string())?,
        )?,
        start_menu_three_panel_layout_enabled: read_bool(&mut reader)?,
        start_menu_three_panel_hide_all_apps: read_bool(&mut reader)?,
        ..Settings::default()
    };
    reader.finish().map_err(|error| error.to_string())?;
    Ok(settings)
}

fn parse_command_response(frame: protocol::Frame) -> Result<CommandResult, String> {
    expect_kind(&frame, MessageKind::CommandResponse).map_err(|error| error.to_string())?;
    let mut reader = Reader::new(&frame.payload);
    let accepted = read_bool(&mut reader)?;
    let detail = reader.string().map_err(|error| error.to_string())?;
    reader.finish().map_err(|error| error.to_string())?;
    if !accepted {
        return Err(if detail.is_empty() {
            "Host rejected the command".into()
        } else {
            detail
        });
    }
    Ok(CommandResult { accepted, detail })
}

fn parse_xaml_diagnostics(frame: protocol::Frame) -> Result<XamlDiagnostics, String> {
    expect_kind(&frame, MessageKind::XamlDiagnosticsResponse).map_err(|error| error.to_string())?;
    let mut reader = Reader::new(&frame.payload);
    let target = reader.u8().map_err(|error| error.to_string())?;
    if target != 1 && target != 3 {
        return Err("Host returned diagnostics for an unexpected target".into());
    }
    let dropped_type_count = reader.u32().map_err(|error| error.to_string())?;
    let dropped_element_count = reader.u32().map_err(|error| error.to_string())?;
    let tracked_element_count = reader.u32().map_err(|error| error.to_string())?;
    let type_count = reader.u16().map_err(|error| error.to_string())?;
    if type_count > 64 {
        return Err("Host returned too many XAML diagnostic types".into());
    }
    let mut types = Vec::with_capacity(type_count as usize);
    for _ in 0..type_count {
        types.push(XamlTypeObservation {
            type_name: reader.string().map_err(|error| error.to_string())?,
            observation_count: reader.u32().map_err(|error| error.to_string())?,
        });
    }
    let element_count = reader.u16().map_err(|error| error.to_string())?;
    if element_count > 128 {
        return Err("Host returned too many XAML diagnostic elements".into());
    }
    let mut elements = Vec::with_capacity(element_count as usize);
    for _ in 0..element_count {
        elements.push(XamlElementObservation {
            handle: reader.u64().map_err(|error| error.to_string())?,
            parent_handle: reader.u64().map_err(|error| error.to_string())?,
            child_index: reader.u32().map_err(|error| error.to_string())?,
            child_count: reader.u32().map_err(|error| error.to_string())?,
            type_index: reader.u16().map_err(|error| error.to_string())?,
            name: reader.string().map_err(|error| error.to_string())?,
        });
    }
    reader.finish().map_err(|error| error.to_string())?;
    Ok(XamlDiagnostics {
        dropped_type_count,
        dropped_element_count,
        tracked_element_count,
        types,
        elements,
    })
}

fn encode_customization(request: CustomizationRequest) -> Result<Vec<u8>, String> {
    let mut payload = Vec::new();
    match request.id.as_str() {
        "taskbarClockPrefix" => {
            let value = validated_text(request, 15, 60, "Clock prefix")?;
            payload.push(1);
            write_string(&mut payload, &value).map_err(|error| error.to_string())?;
        }
        "explorerTitlePrefix" => {
            let value = validated_text(request, 31, 124, "Explorer title prefix")?;
            payload.push(2);
            write_string(&mut payload, &value).map_err(|error| error.to_string())?;
        }
        "startOpacity" => {
            payload.push(3);
            write_u32(&mut payload, validated_opacity(request)? * 10);
        }
        "startHideRecommended" => {
            payload.push(4);
            payload.push(u8::from(validated_boolean(request)?));
        }
        "taskbarOpacity" => {
            payload.push(5);
            write_u32(&mut payload, validated_opacity(request)? * 10);
        }
        "taskbarHideNotificationCenter" => {
            payload.push(6);
            payload.push(u8::from(validated_boolean(request)?));
        }
        "taskbarHideControlCenter" => {
            payload.push(7);
            payload.push(u8::from(validated_boolean(request)?));
        }
        "taskbarHideShowDesktop" => {
            payload.push(8);
            payload.push(u8::from(validated_boolean(request)?));
        }
        "taskbarCapsuleEnabled" => {
            payload.push(17);
            payload.push(u8::from(validated_boolean(request)?));
        }
        "taskbarBackgroundColorEnabled" => {
            payload.push(9);
            payload.push(u8::from(validated_boolean(request)?));
        }
        "taskbarBackgroundColor" => {
            payload.push(10);
            write_u32(&mut payload, validated_color_request(request)?);
        }
        "explorerBackgroundColorEnabled" => {
            payload.push(11);
            payload.push(u8::from(validated_boolean(request)?));
        }
        "explorerBackgroundColor" => {
            payload.push(12);
            write_u32(&mut payload, validated_color_request(request)?);
        }
        "startBackgroundColorEnabled" => {
            payload.push(13);
            payload.push(u8::from(validated_boolean(request)?));
        }
        "startBackgroundColor" => {
            payload.push(14);
            write_u32(&mut payload, validated_color_request(request)?);
        }
        "explorerTransitionAnimation" => {
            payload.push(15);
            if request.text_value.is_some() || request.boolean_value.is_some() {
                return Err("Invalid File Explorer transition animation".into());
            }
            write_u32(
                &mut payload,
                validated_transition_animation(
                    request
                        .integer_value
                        .ok_or("File Explorer transition animation is required")?,
                )?,
            );
        }
        "explorerCustomScrollbarEnabled" => {
            payload.push(16);
            payload.push(u8::from(validated_boolean(request)?));
        }
        "startThreePanelLayoutEnabled" => {
            payload.push(18);
            payload.push(u8::from(validated_boolean(request)?));
        }
        "startHideAllApps" => {
            payload.push(19);
            payload.push(u8::from(validated_boolean(request)?));
        }
        _ => return Err("Unknown customization".into()),
    }
    Ok(payload)
}

fn validated_text(
    request: CustomizationRequest,
    max_chars: usize,
    max_bytes: usize,
    label: &str,
) -> Result<String, String> {
    if request.integer_value.is_some() || request.boolean_value.is_some() {
        return Err(format!("Invalid {label} value"));
    }
    let value = request
        .text_value
        .ok_or_else(|| format!("{label} is required"))?;
    if value.chars().count() > max_chars
        || value.len() > max_bytes
        || value.chars().any(char::is_control)
    {
        return Err(format!(
            "{label} is too long or contains control characters"
        ));
    }
    Ok(value)
}

fn validated_opacity(request: CustomizationRequest) -> Result<u32, String> {
    if request.text_value.is_some() || request.boolean_value.is_some() {
        return Err("Invalid opacity value".into());
    }
    let value = request.integer_value.ok_or("Opacity is required")?;
    if !(10..=100).contains(&value) {
        return Err("Opacity must be between 10 and 100 percent".into());
    }
    Ok(value)
}

fn validated_boolean(request: CustomizationRequest) -> Result<bool, String> {
    if request.text_value.is_some() || request.integer_value.is_some() {
        return Err("Invalid boolean value".into());
    }
    request
        .boolean_value
        .ok_or_else(|| "Boolean value is required".into())
}

fn validated_color_request(request: CustomizationRequest) -> Result<u32, String> {
    if request.text_value.is_some() || request.boolean_value.is_some() {
        return Err("Invalid background color value".into());
    }
    validated_color(
        request
            .integer_value
            .ok_or("Background color is required")?,
    )
}

fn validated_color(color: u32) -> Result<u32, String> {
    if color & 0xFF000000 != 0xFF000000 {
        return Err("Background color must be an opaque ARGB value".into());
    }
    Ok(color)
}

fn validated_transition_animation(animation: u32) -> Result<u32, String> {
    if animation > 3 {
        return Err("File Explorer transition animation must be between 0 and 3".into());
    }
    Ok(animation)
}

fn read_bool(reader: &mut Reader<'_>) -> Result<bool, String> {
    match reader.u8().map_err(|error| error.to_string())? {
        0 => Ok(false),
        1 => Ok(true),
        _ => Err("Host returned an invalid boolean".into()),
    }
}

fn opacity_percent(milli: u32) -> Result<u32, String> {
    if !(100..=1000).contains(&milli) || !milli.is_multiple_of(10) {
        return Err("Host returned an invalid opacity".into());
    }
    Ok(milli / 10)
}

fn target_id(target: &str) -> Result<u8, String> {
    match target {
        "taskbar" => Ok(1),
        "explorer" => Ok(2),
        "start" => Ok(3),
        _ => Err("Unknown shell target".into()),
    }
}

fn target_metadata(target: u8) -> Result<(&'static str, &'static str), String> {
    match target {
        1 => Ok(("taskbar", "Taskbar")),
        2 => Ok(("explorer", "File Explorer")),
        3 => Ok(("start", "Start menu")),
        _ => Err("Host returned an unknown target".into()),
    }
}

fn state_name(state: u8) -> Result<&'static str, String> {
    match state {
        0 => Ok("disabled"),
        1 => Ok("stopped"),
        2 => Ok("running"),
        3 => Ok("injecting"),
        4 => Ok("active"),
        5 => Ok("error"),
        6 => Ok("incompatible"),
        _ => Err("Host returned an unknown runtime state".into()),
    }
}

fn disconnected_state(detail: String) -> AppState {
    AppState {
        connected: false,
        connection_detail: detail,
        targets: [
            ("taskbar", "Taskbar"),
            ("start", "Start menu"),
            ("explorer", "File Explorer"),
        ]
        .into_iter()
        .map(|(id, name)| TargetSnapshot {
            id: id.into(),
            name: name.into(),
            state: "stopped".into(),
            enabled: false,
            process_running: false,
            agent_loaded: false,
            process_id: 0,
            detail: "Host unavailable".into(),
        })
        .collect(),
        settings: with_start_app_list_policy(Settings::default()),
    }
}

fn with_start_app_list_policy(mut settings: Settings) -> Settings {
    match start_menu_policy::query() {
        Ok(policy) => {
            settings.start_menu_hide_all_apps_policy_active = policy.hidden;
            if settings.start_menu_three_panel_layout_enabled {
                settings.start_menu_hide_all_apps =
                    settings.start_menu_three_panel_hide_all_apps || policy.hidden;
                settings.start_menu_hide_all_apps_supported = true;
                settings.start_menu_hide_all_apps_editable = !policy.hidden || policy.editable;
                settings.start_menu_hide_all_apps_detail = if policy.hidden {
                    "An incompatible Windows policy is still active. Change this switch once to migrate it to the Three-panel setting."
                        .into()
                } else {
                    "Hide the custom All apps list while keeping the Three-panel Start geometry intact."
                        .into()
                };
            } else {
                settings.start_menu_hide_all_apps = policy.hidden;
                settings.start_menu_hide_all_apps_supported = policy.supported;
                settings.start_menu_hide_all_apps_editable = policy.editable;
                settings.start_menu_hide_all_apps_detail = policy.detail;
            }
        }
        Err(error) => {
            settings.start_menu_hide_all_apps = false;
            settings.start_menu_hide_all_apps_supported = false;
            settings.start_menu_hide_all_apps_editable = false;
            settings.start_menu_hide_all_apps_detail = error;
        }
    }
    settings
}

fn ensure_host_started() -> Result<(), String> {
    static LAST_ATTEMPT: OnceLock<Mutex<Option<Instant>>> = OnceLock::new();
    let attempts = LAST_ATTEMPT.get_or_init(|| Mutex::new(None));
    let mut last_attempt = attempts
        .lock()
        .map_err(|_| "Host launch lock is poisoned")?;
    if last_attempt.is_some_and(|last| last.elapsed() < HOST_LAUNCH_COOLDOWN) {
        return Ok(());
    }
    *last_attempt = Some(Instant::now());

    let host = locate_host_executable()?;
    let mut command = Command::new(&host);
    use std::os::windows::process::CommandExt;
    command.creation_flags(CREATE_NO_WINDOW);
    command
        .spawn()
        .map_err(|error| format!("Could not start {}: {error}", host.display()))?;
    Ok(())
}

fn locate_host_executable() -> Result<PathBuf, String> {
    if let Some(path) = std::env::var_os("METAPLASIA_HOST_PATH").map(PathBuf::from) {
        if is_regular_file(&path) {
            return Ok(path);
        }
        return Err(format!(
            "METAPLASIA_HOST_PATH does not point to a file: {}",
            path.display()
        ));
    }
    let executable = std::env::current_exe()
        .map_err(|error| format!("Could not resolve application path: {error}"))?;
    let directory = executable
        .parent()
        .ok_or("Application path has no parent directory")?;
    let candidates = [
        directory.join("metaplasia-host.exe"),
        directory.join("..\\..\\bin\\metaplasia-host.exe"),
        directory.join("..\\..\\..\\bin\\metaplasia-host.exe"),
    ];
    candidates
        .into_iter()
        .find(|path| is_regular_file(path))
        .map(|path| path.canonicalize().unwrap_or(path))
        .ok_or_else(|| "metaplasia-host.exe was not found beside the application".into())
}

fn is_regular_file(path: &Path) -> bool {
    path.metadata().is_ok_and(|metadata| metadata.is_file())
}

fn show_window(window: &WebviewWindow) {
    apply_dark_window_frame(window);
    let _ = window.show();
    let _ = window.set_focus();
}

fn apply_dark_window_frame(window: &WebviewWindow) {
    let Ok(handle) = window.hwnd() else {
        return;
    };
    let enabled = 1_i32;
    let black = 0_u32;
    let border = 0x001A_1A1A_u32;
    let white = 0x00FF_FFFF_u32;
    let attributes = [
        (
            DWMWA_USE_IMMERSIVE_DARK_MODE,
            (&enabled as *const i32).cast::<std::ffi::c_void>(),
            std::mem::size_of_val(&enabled) as u32,
        ),
        (
            DWMWA_BORDER_COLOR,
            (&border as *const u32).cast::<std::ffi::c_void>(),
            std::mem::size_of_val(&border) as u32,
        ),
        (
            DWMWA_CAPTION_COLOR,
            (&black as *const u32).cast::<std::ffi::c_void>(),
            std::mem::size_of_val(&black) as u32,
        ),
        (
            DWMWA_TEXT_COLOR,
            (&white as *const u32).cast::<std::ffi::c_void>(),
            std::mem::size_of_val(&white) as u32,
        ),
    ];
    for (attribute, value, value_size) in attributes {
        // SAFETY: the HWND belongs to this Tauri window and every value pointer
        // remains valid for the duration and exact byte size of this call.
        let _ = unsafe { DwmSetWindowAttribute(handle.0, attribute, value, value_size) };
    }
}

pub fn run() {
    tauri::Builder::default()
        .manage(portable_update::UpdateManager::default())
        .invoke_handler(tauri::generate_handler![
            get_app_state,
            set_target_enabled,
            set_customization,
            set_start_all_apps_hidden,
            get_xaml_diagnostics,
            diagnostics::get_diagnostic_logs,
            portable_update::get_portable_update_status,
            portable_update::check_portable_update,
            portable_update::download_portable_update,
            portable_update::apply_portable_update
        ])
        .setup(|app| {
            if let Some(window) = app.get_webview_window("main") {
                show_window(&window);
            }
            Ok(())
        })
        .run(tauri::generate_context!())
        .expect("failed to run Metaplasia");
}

pub fn run_portable_update_helper_from_args() -> Option<i32> {
    portable_update::run_helper_from_args()
}

pub fn run_start_all_apps_policy_helper_from_args() -> Option<i32> {
    let arguments: Vec<String> = std::env::args().collect();
    if arguments.get(1).map(String::as_str) != Some("--apply-start-all-apps-policy") {
        return None;
    }
    if arguments.len() != 3 {
        return Some(2);
    }
    let hidden = match arguments[2].as_str() {
        "0" => false,
        "1" => true,
        _ => return Some(2),
    };
    Some(
        match start_menu_policy::set_hidden_from_elevated_helper(hidden) {
            Ok(()) => 0,
            Err(error) => {
                let message = format!("[Metaplasia Policy Helper] {error}\n");
                #[cfg(windows)]
                {
                    use std::os::windows::ffi::OsStrExt;
                    let message: Vec<u16> = std::ffi::OsStr::new(&message)
                        .encode_wide()
                        .chain(std::iter::once(0))
                        .collect();
                    unsafe extern "system" {
                        fn OutputDebugStringW(message: *const u16);
                    }
                    // SAFETY: message is a valid null-terminated UTF-16 buffer.
                    unsafe { OutputDebugStringW(message.as_ptr()) };
                }
                3
            }
        },
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_unconfirmed_enable() {
        let request = ToggleTargetRequest {
            target: "taskbar".into(),
            enabled: true,
            confirmed: false,
        };
        assert!(request.enabled && !request.confirmed);
    }

    #[test]
    fn validates_opacity_boundaries() {
        for value in [10, 100] {
            assert_eq!(
                validated_opacity(CustomizationRequest {
                    id: "taskbarOpacity".into(),
                    text_value: None,
                    integer_value: Some(value),
                    boolean_value: None,
                })
                .unwrap(),
                value
            );
        }
    }

    #[test]
    fn encodes_independent_opaque_shell_color() {
        let payload = encode_customization(CustomizationRequest {
            id: "startBackgroundColor".into(),
            text_value: None,
            integer_value: Some(0xFFAB_CDEF),
            boolean_value: None,
        })
        .unwrap();
        assert_eq!(payload, vec![14, 0xEF, 0xCD, 0xAB, 0xFF]);

        assert!(
            encode_customization(CustomizationRequest {
                id: "explorerBackgroundColor".into(),
                text_value: None,
                integer_value: Some(0x00AB_CDEF),
                boolean_value: None,
            })
            .is_err()
        );
    }

    #[test]
    fn encodes_and_validates_explorer_transition_animation() {
        let payload = encode_customization(CustomizationRequest {
            id: "explorerTransitionAnimation".into(),
            text_value: None,
            integer_value: Some(2),
            boolean_value: None,
        })
        .unwrap();
        assert_eq!(payload, vec![15, 2, 0, 0, 0]);

        assert!(
            encode_customization(CustomizationRequest {
                id: "explorerTransitionAnimation".into(),
                text_value: None,
                integer_value: Some(4),
                boolean_value: None,
            })
            .is_err()
        );
    }

    #[test]
    fn encodes_explorer_custom_scrollbar_toggle() {
        let payload = encode_customization(CustomizationRequest {
            id: "explorerCustomScrollbarEnabled".into(),
            text_value: None,
            integer_value: None,
            boolean_value: Some(false),
        })
        .unwrap();
        assert_eq!(payload, vec![16, 0]);
    }

    #[test]
    fn encodes_taskbar_capsule_toggle() {
        let payload = encode_customization(CustomizationRequest {
            id: "taskbarCapsuleEnabled".into(),
            text_value: None,
            integer_value: None,
            boolean_value: Some(true),
        })
        .unwrap();
        assert_eq!(payload, vec![17, 1]);
    }

    #[test]
    fn encodes_three_panel_start_layout_toggle() {
        let payload = encode_customization(CustomizationRequest {
            id: "startThreePanelLayoutEnabled".into(),
            text_value: None,
            integer_value: None,
            boolean_value: Some(true),
        })
        .unwrap();
        assert_eq!(payload, vec![18, 1]);
    }

    #[test]
    fn encodes_three_panel_all_apps_toggle() {
        let payload = encode_customization(CustomizationRequest {
            id: "startHideAllApps".into(),
            text_value: None,
            integer_value: None,
            boolean_value: Some(true),
        })
        .unwrap();
        assert_eq!(payload, vec![19, 1]);
    }
}
