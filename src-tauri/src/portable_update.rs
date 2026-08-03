use base64::Engine as _;
use ed25519_dalek::{Signature, VerifyingKey};
use reqwest::blocking::{Client, Response};
use semver::Version;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::os::windows::ffi::OsStrExt;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};
use tauri::{AppHandle, State};

use crate::protocol::{MessageKind, PipeClient};

const UPDATE_MANIFEST_URL: &str =
    "https://github.com/mkkima/Metaplasia/releases/latest/download/portable-update.json";
const UPDATE_SIGNATURE_URL: &str =
    "https://github.com/mkkima/Metaplasia/releases/latest/download/portable-update.json.sig";
const UPDATE_PUBLIC_KEY_B64: Option<&str> = option_env!("METAPLASIA_UPDATE_PUBLIC_KEY");
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const MAX_MANIFEST_BYTES: usize = 64 * 1024;
const MAX_SIGNATURE_BYTES: usize = 512;
const MAX_PACKAGE_BYTES: u64 = 768 * 1024 * 1024;
const MAX_EXPANDED_BYTES: u64 = 1024 * 1024 * 1024;
const HELPER_WAIT_TIMEOUT: Duration = Duration::from_secs(30);
const EXPECTED_FILES: [&str; 5] = [
    "metaplasia.exe",
    "metaplasia-host.exe",
    "metaplasia-watchdog.exe",
    "metaplasia-cli.exe",
    "metaplasia-agent.dll",
];

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CloseHandle(handle: isize) -> i32;
    fn GetCurrentProcessId() -> u32;
    fn OpenProcess(access: u32, inherit_handle: i32, process_id: u32) -> isize;
    fn WaitForSingleObject(handle: isize, timeout_ms: u32) -> u32;
}

const SYNCHRONIZE: u32 = 0x0010_0000;
const WAIT_OBJECT_0: u32 = 0;
const WAIT_TIMEOUT: u32 = 258;
const ERROR_INVALID_PARAMETER: i32 = 87;
struct OwnedHandle(isize);

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        // SAFETY: this wrapper exclusively owns a valid Windows handle.
        let _ = unsafe { CloseHandle(self.0) };
    }
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct ReleaseManifest {
    schema: u32,
    version: String,
    published_at: String,
    notes: String,
    package: ReleasePackage,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(deny_unknown_fields)]
struct ReleasePackage {
    url: String,
    sha256: String,
    size: u64,
}

#[derive(Debug, Clone)]
struct PendingUpdate {
    manifest: ReleaseManifest,
    manifest_bytes: Vec<u8>,
    signature: Vec<u8>,
    package_path: Option<PathBuf>,
    manifest_path: Option<PathBuf>,
    signature_path: Option<PathBuf>,
}

#[derive(Debug)]
struct ManagerState {
    phase: &'static str,
    detail: String,
    pending: Option<PendingUpdate>,
}

impl Default for ManagerState {
    fn default() -> Self {
        Self {
            phase: "idle",
            detail: "Ready to check GitHub Releases for a signed portable update.".into(),
            pending: None,
        }
    }
}

#[derive(Clone)]
pub struct UpdateManager(Arc<Mutex<ManagerState>>);

impl Default for UpdateManager {
    fn default() -> Self {
        let mut state = ManagerState::default();
        if let Some(result) = take_last_update_result() {
            state.phase = if result.success { "current" } else { "error" };
            state.detail = result.detail;
        }
        Self(Arc::new(Mutex::new(state)))
    }
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct LastUpdateResult {
    success: bool,
    detail: String,
}

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateStatus {
    configured: bool,
    current_version: String,
    available_version: Option<String>,
    notes: String,
    phase: String,
    detail: String,
    downloaded: bool,
}

impl UpdateManager {
    fn status(&self) -> UpdateStatus {
        let configured = verification_key().is_ok();
        match self.0.lock() {
            Ok(state) => status_from_state(&state, configured),
            Err(_) => UpdateStatus {
                configured,
                current_version: env!("CARGO_PKG_VERSION").into(),
                available_version: None,
                notes: String::new(),
                phase: "error".into(),
                detail: "The update state lock is poisoned.".into(),
                downloaded: false,
            },
        }
    }

    fn check(&self) -> Result<UpdateStatus, String> {
        let key = verification_key()?;
        let mut state = self
            .0
            .lock()
            .map_err(|_| "The update state lock is poisoned".to_string())?;
        state.phase = "checking";
        state.detail = "Checking the signed GitHub release manifest…".into();

        match fetch_pending_update(&key) {
            Ok(Some(pending)) => {
                state.phase = "available";
                state.detail =
                    format!("Portable update {} is available.", pending.manifest.version);
                state.pending = Some(pending);
                Ok(status_from_state(&state, true))
            }
            Ok(None) => {
                state.phase = "current";
                state.detail = "This portable copy is up to date.".into();
                state.pending = None;
                Ok(status_from_state(&state, true))
            }
            Err(error) => {
                state.phase = "error";
                state.detail = error.clone();
                Err(error)
            }
        }
    }

    fn download(&self) -> Result<UpdateStatus, String> {
        let key = verification_key()?;
        let mut state = self
            .0
            .lock()
            .map_err(|_| "The update state lock is poisoned".to_string())?;
        let mut pending = state
            .pending
            .clone()
            .ok_or("Check for an available update before downloading it")?;
        state.phase = "downloading";
        state.detail = format!("Downloading portable update {}…", pending.manifest.version);

        let result = download_pending_update(&mut pending, &key);
        match result {
            Ok(()) => {
                state.phase = "ready";
                state.detail = format!(
                    "Portable update {} is verified and ready to install.",
                    pending.manifest.version
                );
                state.pending = Some(pending);
                Ok(status_from_state(&state, true))
            }
            Err(error) => {
                state.phase = "error";
                state.detail = error.clone();
                Err(error)
            }
        }
    }

    fn ready_update(&self) -> Result<PendingUpdate, String> {
        let state = self
            .0
            .lock()
            .map_err(|_| "The update state lock is poisoned".to_string())?;
        let pending = state
            .pending
            .clone()
            .ok_or("No portable update is available")?;
        if pending.package_path.is_none()
            || pending.manifest_path.is_none()
            || pending.signature_path.is_none()
        {
            return Err("Download and verify the portable update before installing it".into());
        }
        Ok(pending)
    }
}

fn status_from_state(state: &ManagerState, configured: bool) -> UpdateStatus {
    let pending = state.pending.as_ref();
    UpdateStatus {
        configured,
        current_version: env!("CARGO_PKG_VERSION").into(),
        available_version: pending.map(|value| value.manifest.version.clone()),
        notes: pending
            .map(|value| value.manifest.notes.clone())
            .unwrap_or_default(),
        phase: if configured {
            state.phase
        } else {
            "unconfigured"
        }
        .into(),
        detail: if configured {
            state.detail.clone()
        } else {
            "This build has no embedded update verification key. Use a signed release build.".into()
        },
        downloaded: pending.is_some_and(|value| value.package_path.is_some()),
    }
}

#[tauri::command]
pub fn get_portable_update_status(state: State<'_, UpdateManager>) -> UpdateStatus {
    state.status()
}

#[tauri::command]
pub async fn check_portable_update(
    state: State<'_, UpdateManager>,
) -> Result<UpdateStatus, String> {
    let manager = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || manager.check())
        .await
        .map_err(|error| format!("Update check worker failed: {error}"))?
}

#[tauri::command]
pub async fn download_portable_update(
    state: State<'_, UpdateManager>,
) -> Result<UpdateStatus, String> {
    let manager = state.inner().clone();
    tauri::async_runtime::spawn_blocking(move || manager.download())
        .await
        .map_err(|error| format!("Update download worker failed: {error}"))?
}

#[tauri::command]
pub async fn apply_portable_update(
    app: AppHandle,
    state: State<'_, UpdateManager>,
) -> Result<(), String> {
    let pending = state.ready_update()?;
    tauri::async_runtime::spawn_blocking(move || launch_update_helper(&pending))
        .await
        .map_err(|error| format!("Update launcher worker failed: {error}"))??;
    crate::UPDATE_IN_PROGRESS.store(true, std::sync::atomic::Ordering::Release);
    app.exit(0);
    Ok(())
}

fn verification_key() -> Result<VerifyingKey, String> {
    let encoded = UPDATE_PUBLIC_KEY_B64.ok_or(
        "This build has no embedded update verification key. Install a signed release build.",
    )?;
    let bytes = base64::engine::general_purpose::STANDARD
        .decode(encoded.trim())
        .map_err(|_| "The embedded update verification key is malformed")?;
    let bytes: [u8; 32] = bytes
        .try_into()
        .map_err(|_| "The embedded update verification key has an invalid length")?;
    VerifyingKey::from_bytes(&bytes)
        .map_err(|_| "The embedded update verification key is invalid".into())
}

fn update_client() -> Result<Client, String> {
    Client::builder()
        .connect_timeout(Duration::from_secs(10))
        .timeout(Duration::from_secs(10 * 60))
        .user_agent(concat!("Metaplasia/", env!("CARGO_PKG_VERSION")))
        .redirect(reqwest::redirect::Policy::custom(|attempt| {
            if attempt.previous().len() >= 8 || !allowed_download_host(attempt.url()) {
                attempt.stop()
            } else {
                attempt.follow()
            }
        }))
        .build()
        .map_err(|error| format!("Could not initialize the HTTPS update client: {error}"))
}

fn allowed_download_host(url: &reqwest::Url) -> bool {
    if url.scheme() != "https" || url.username() != "" || url.password().is_some() {
        return false;
    }
    matches!(
        url.host_str(),
        Some("github.com")
            | Some("objects.githubusercontent.com")
            | Some("release-assets.githubusercontent.com")
    )
}

fn fetch_pending_update(key: &VerifyingKey) -> Result<Option<PendingUpdate>, String> {
    let client = update_client()?;
    let manifest_bytes = fetch_limited(&client, UPDATE_MANIFEST_URL, MAX_MANIFEST_BYTES)?;
    let signature = fetch_limited(&client, UPDATE_SIGNATURE_URL, MAX_SIGNATURE_BYTES)?;
    let manifest = verify_manifest_bytes(&manifest_bytes, &signature, key)?;

    let current = Version::parse(env!("CARGO_PKG_VERSION"))
        .map_err(|error| format!("The current application version is invalid: {error}"))?;
    let available = Version::parse(&manifest.version)
        .map_err(|error| format!("The signed release version is invalid: {error}"))?;
    if available <= current {
        return Ok(None);
    }
    Ok(Some(PendingUpdate {
        manifest,
        manifest_bytes,
        signature,
        package_path: None,
        manifest_path: None,
        signature_path: None,
    }))
}

fn fetch_limited(client: &Client, url: &str, maximum: usize) -> Result<Vec<u8>, String> {
    let response = client
        .get(url)
        .send()
        .map_err(|error| format!("Could not download update metadata: {error}"))?;
    read_limited_response(response, maximum)
}

fn read_limited_response(mut response: Response, maximum: usize) -> Result<Vec<u8>, String> {
    if !response.status().is_success() {
        return Err(format!(
            "GitHub returned HTTP {} while checking for updates",
            response.status()
        ));
    }
    if response
        .content_length()
        .is_some_and(|length| length > maximum as u64)
    {
        return Err("GitHub returned oversized update metadata".into());
    }
    let mut bytes =
        Vec::with_capacity(response.content_length().unwrap_or(0).min(maximum as u64) as usize);
    response
        .by_ref()
        .take(maximum as u64 + 1)
        .read_to_end(&mut bytes)
        .map_err(|error| format!("Could not read update metadata: {error}"))?;
    if bytes.len() > maximum {
        return Err("GitHub returned oversized update metadata".into());
    }
    Ok(bytes)
}

fn verify_manifest_bytes(
    manifest_bytes: &[u8],
    signature_bytes: &[u8],
    key: &VerifyingKey,
) -> Result<ReleaseManifest, String> {
    let signature = Signature::from_slice(signature_bytes)
        .map_err(|_| "The update manifest signature has an invalid length")?;
    key.verify_strict(manifest_bytes, &signature)
        .map_err(|_| "The update manifest signature is invalid")?;
    let manifest: ReleaseManifest = serde_json::from_slice(manifest_bytes)
        .map_err(|error| format!("The signed update manifest is invalid: {error}"))?;
    validate_manifest(&manifest)?;
    Ok(manifest)
}

fn validate_manifest(manifest: &ReleaseManifest) -> Result<(), String> {
    if manifest.schema != 1 {
        return Err("The signed update manifest uses an unsupported schema".into());
    }
    let version = Version::parse(&manifest.version)
        .map_err(|error| format!("The signed release version is invalid: {error}"))?;
    if !version.pre.is_empty()
        || !version.build.is_empty()
        || version.to_string() != manifest.version
    {
        return Err("The signed release version must be a canonical stable SemVer".into());
    }
    if manifest.published_at.is_empty() || manifest.published_at.len() > 64 {
        return Err("The signed release timestamp is invalid".into());
    }
    if manifest.notes.len() > 16 * 1024 {
        return Err("The signed release notes are too large".into());
    }
    if manifest.package.size == 0 || manifest.package.size > MAX_PACKAGE_BYTES {
        return Err("The signed portable package size is invalid".into());
    }
    if manifest.package.sha256.len() != 64
        || !manifest
            .package
            .sha256
            .bytes()
            .all(|byte| byte.is_ascii_hexdigit())
    {
        return Err("The signed portable package SHA-256 is invalid".into());
    }
    validate_package_url(&manifest.package.url, &manifest.version)
}

fn validate_package_url(url: &str, version: &str) -> Result<(), String> {
    let parsed =
        reqwest::Url::parse(url).map_err(|_| "The signed portable package URL is invalid")?;
    if parsed.scheme() != "https"
        || parsed.host_str() != Some("github.com")
        || parsed.username() != ""
        || parsed.password().is_some()
        || parsed.query().is_some()
        || parsed.fragment().is_some()
    {
        return Err("The signed portable package URL is not an approved GitHub URL".into());
    }
    let expected = format!(
        "/mkkima/Metaplasia/releases/download/v{version}/Metaplasia-{version}-windows-x64-portable.zip"
    );
    if parsed.path() != expected {
        return Err("The signed portable package URL does not match this release".into());
    }
    Ok(())
}

fn download_pending_update(pending: &mut PendingUpdate, key: &VerifyingKey) -> Result<(), String> {
    // Verify again immediately before trusting paths and download metadata.
    let verified = verify_manifest_bytes(&pending.manifest_bytes, &pending.signature, key)?;
    if verified.version != pending.manifest.version {
        return Err("The pending update manifest changed unexpectedly".into());
    }

    let directory = update_data_directory()?.join(format!("pending-{}", verified.version));
    fs::create_dir_all(&directory)
        .map_err(|error| format!("Could not create the portable update cache: {error}"))?;
    let manifest_path = directory.join("portable-update.json");
    let signature_path = directory.join("portable-update.json.sig");
    let package_path = directory.join(format!("Metaplasia-{}-portable.zip", verified.version));
    write_atomic(&manifest_path, &pending.manifest_bytes)?;
    write_atomic(&signature_path, &pending.signature)?;

    let partial_path = directory.join("package.download");
    remove_file_if_present(&partial_path)?;
    let mut output = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&partial_path)
        .map_err(|error| format!("Could not create the portable package cache: {error}"))?;
    let client = update_client()?;
    let mut response = client
        .get(&verified.package.url)
        .send()
        .map_err(|error| format!("Could not download the portable update: {error}"))?;
    if !response.status().is_success() {
        let _ = fs::remove_file(&partial_path);
        return Err(format!(
            "GitHub returned HTTP {} while downloading the portable update",
            response.status()
        ));
    }
    if response
        .content_length()
        .is_some_and(|length| length != verified.package.size)
    {
        let _ = fs::remove_file(&partial_path);
        return Err("The portable update download size does not match its signed manifest".into());
    }

    let mut hasher = Sha256::new();
    let mut received = 0_u64;
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let count = response
            .read(&mut buffer)
            .map_err(|error| format!("Could not read the portable update: {error}"))?;
        if count == 0 {
            break;
        }
        received = received
            .checked_add(count as u64)
            .ok_or("The portable update size overflowed")?;
        if received > verified.package.size || received > MAX_PACKAGE_BYTES {
            drop(output);
            let _ = fs::remove_file(&partial_path);
            return Err("The portable update exceeded its signed size".into());
        }
        output
            .write_all(&buffer[..count])
            .map_err(|error| format!("Could not store the portable update: {error}"))?;
        hasher.update(&buffer[..count]);
    }
    if received != verified.package.size {
        drop(output);
        let _ = fs::remove_file(&partial_path);
        return Err("The portable update download was incomplete".into());
    }
    output
        .sync_all()
        .map_err(|error| format!("Could not flush the portable update: {error}"))?;
    drop(output);
    let actual_hash = format!("{:x}", hasher.finalize());
    if !actual_hash.eq_ignore_ascii_case(&verified.package.sha256) {
        let _ = fs::remove_file(&partial_path);
        return Err("The portable update SHA-256 does not match its signed manifest".into());
    }
    remove_file_if_present(&package_path)?;
    fs::rename(&partial_path, &package_path)
        .map_err(|error| format!("Could not finalize the portable update download: {error}"))?;

    pending.manifest = verified;
    pending.package_path = Some(package_path);
    pending.manifest_path = Some(manifest_path);
    pending.signature_path = Some(signature_path);
    Ok(())
}

fn write_atomic(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let name = path
        .file_name()
        .and_then(OsStr::to_str)
        .ok_or("The update cache filename is invalid")?;
    let temporary = path.with_file_name(format!("{name}.tmp"));
    remove_file_if_present(&temporary)?;
    let mut file = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&temporary)
        .map_err(|error| format!("Could not create update metadata: {error}"))?;
    file.write_all(bytes)
        .map_err(|error| format!("Could not store update metadata: {error}"))?;
    file.sync_all()
        .map_err(|error| format!("Could not flush update metadata: {error}"))?;
    drop(file);
    remove_file_if_present(path)?;
    fs::rename(&temporary, path)
        .map_err(|error| format!("Could not finalize update metadata: {error}"))
}

fn update_data_directory() -> Result<PathBuf, String> {
    let local = std::env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .ok_or("LOCALAPPDATA is unavailable")?;
    if !local.is_absolute() {
        return Err("LOCALAPPDATA is not an absolute path".into());
    }
    let directory = local.join("Metaplasia").join("updates");
    fs::create_dir_all(&directory)
        .map_err(|error| format!("Could not create the portable update directory: {error}"))?;
    directory
        .canonicalize()
        .map_err(|error| format!("Could not resolve the portable update directory: {error}"))
}

fn launch_update_helper(pending: &PendingUpdate) -> Result<(), String> {
    let package = pending
        .package_path
        .as_ref()
        .ok_or("The portable package has not been downloaded")?;
    let manifest = pending
        .manifest_path
        .as_ref()
        .ok_or("The portable manifest is unavailable")?;
    let signature = pending
        .signature_path
        .as_ref()
        .ok_or("The portable manifest signature is unavailable")?;
    let current_executable = std::env::current_exe()
        .map_err(|error| format!("Could not resolve the running application path: {error}"))?
        .canonicalize()
        .map_err(|error| format!("Could not resolve the running application file: {error}"))?;
    let target_directory = current_executable
        .parent()
        .ok_or("The running application has no parent directory")?
        .to_path_buf();
    validate_portable_target(&target_directory)?;
    preflight_target_write(&target_directory)?;

    let helper = update_data_directory()?.join(format!(
        "metaplasia-update-helper-{}.exe",
        std::process::id()
    ));
    remove_file_if_present(&helper)?;
    fs::copy(&current_executable, &helper)
        .map_err(|error| format!("Could not prepare the portable update helper: {error}"))?;

    let client = PipeClient::for_current_session().map_err(|error| error.to_string())?;
    let response = client
        .transact(MessageKind::PrepareUpdateRequest, &[], crate::PIPE_TIMEOUT)
        .map_err(|error| format!("Could not prepare the native host for update: {error}"))?;
    crate::parse_command_response(response)?;

    let mut command = Command::new(&helper);
    command
        .arg("--apply-portable-update")
        .arg(std::process::id().to_string())
        .arg(&target_directory)
        .arg(manifest)
        .arg(signature)
        .arg(package)
        .creation_flags(CREATE_NO_WINDOW);
    command
        .spawn()
        .map_err(|error| format!("Could not start the portable update helper: {error}"))?;
    Ok(())
}

fn validate_portable_target(directory: &Path) -> Result<(), String> {
    if !directory.is_absolute() {
        return Err("The portable application directory is not absolute".into());
    }
    for name in EXPECTED_FILES {
        let path = directory.join(name);
        if !path.metadata().is_ok_and(|metadata| metadata.is_file()) {
            return Err(format!(
                "The portable application is incomplete: {} is missing",
                path.display()
            ));
        }
    }
    Ok(())
}

fn preflight_target_write(directory: &Path) -> Result<(), String> {
    let probe = directory.join(format!(
        ".metaplasia-update-write-test-{}.tmp",
        std::process::id()
    ));
    remove_file_if_present(&probe)?;
    let file = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&probe)
        .map_err(|error| format!("The portable application directory is not writable: {error}"))?;
    drop(file);
    fs::remove_file(&probe)
        .map_err(|error| format!("Could not remove the update write test: {error}"))
}

pub fn run_helper_from_args() -> Option<i32> {
    let arguments: Vec<OsString> = std::env::args_os().collect();
    if arguments.get(1).map(OsString::as_os_str) != Some(OsStr::new("--apply-portable-update")) {
        return None;
    }
    let result = parse_helper_arguments(&arguments).and_then(apply_portable_update_helper);
    Some(match result {
        Ok(()) => 0,
        Err(error) => {
            output_debug_error(&format!("[Metaplasia Update Helper] {error}\n"));
            40
        }
    })
}

#[derive(Debug)]
struct HelperArguments {
    parent_process_id: u32,
    target_directory: PathBuf,
    manifest_path: PathBuf,
    signature_path: PathBuf,
    package_path: PathBuf,
}

fn parse_helper_arguments(arguments: &[OsString]) -> Result<HelperArguments, String> {
    if arguments.len() != 7 {
        return Err("The portable update helper received an invalid argument count".into());
    }
    let parent_process_id = arguments[2]
        .to_str()
        .ok_or("The parent process ID is invalid")?
        .parse::<u32>()
        .map_err(|_| "The parent process ID is invalid")?;
    if parent_process_id == 0 || parent_process_id == unsafe { GetCurrentProcessId() } {
        return Err("The parent process ID is invalid".into());
    }
    Ok(HelperArguments {
        parent_process_id,
        target_directory: PathBuf::from(&arguments[3]),
        manifest_path: PathBuf::from(&arguments[4]),
        signature_path: PathBuf::from(&arguments[5]),
        package_path: PathBuf::from(&arguments[6]),
    })
}

fn apply_portable_update_helper(arguments: HelperArguments) -> Result<(), String> {
    let key = verification_key()?;
    let update_root = update_data_directory()?;
    let helper = std::env::current_exe()
        .map_err(|error| format!("Could not resolve the update helper path: {error}"))?
        .canonicalize()
        .map_err(|error| format!("Could not resolve the update helper file: {error}"))?;
    if !helper.starts_with(&update_root) {
        return Err("The update helper is not running from the protected update directory".into());
    }

    let target = canonical_directory(&arguments.target_directory, "portable target")?;
    if target.starts_with(&update_root) || update_root.starts_with(&target) {
        return Err("The portable target overlaps the update cache".into());
    }
    let manifest_path =
        canonical_file_under(&arguments.manifest_path, &update_root, "update manifest")?;
    let signature_path =
        canonical_file_under(&arguments.signature_path, &update_root, "update signature")?;
    let package_path =
        canonical_file_under(&arguments.package_path, &update_root, "portable package")?;
    if manifest_path.parent() != signature_path.parent()
        || manifest_path.parent() != package_path.parent()
    {
        return Err("The portable update files are not from one pending release".into());
    }

    let manifest_bytes = read_file_limited(&manifest_path, MAX_MANIFEST_BYTES)?;
    let signature = read_file_limited(&signature_path, MAX_SIGNATURE_BYTES)?;
    let manifest = verify_manifest_bytes(&manifest_bytes, &signature, &key)?;
    let current = Version::parse(env!("CARGO_PKG_VERSION"))
        .map_err(|error| format!("The helper version is invalid: {error}"))?;
    let update = Version::parse(&manifest.version)
        .map_err(|error| format!("The update version is invalid: {error}"))?;
    if update <= current {
        return Err("The portable update is not newer than the running application".into());
    }
    verify_package_file(&package_path, &manifest.package)?;
    wait_for_process_exit(arguments.parent_process_id, HELPER_WAIT_TIMEOUT)?;
    wait_for_metaplasia_processes(HELPER_WAIT_TIMEOUT)?;
    validate_portable_target(&target)?;

    let nonce = unique_nonce()?;
    let stage = target.join(format!(".metaplasia-update-stage-{nonce}"));
    let backup = target.join(format!(".metaplasia-update-backup-{nonce}"));
    fs::create_dir(&stage).map_err(|error| {
        format!("Could not create the portable update staging directory: {error}")
    })?;
    if let Err(error) = extract_portable_package(&package_path, &stage) {
        cleanup_known_directory(&stage);
        return Err(error);
    }

    let stop_result = crate::start_menu_policy::stop_shell_for_portable_update();
    if let Err(error) = stop_result {
        cleanup_known_directory(&stage);
        return Err(format!(
            "Could not stop the Windows shell for update: {error}"
        ));
    }

    let replacement = replace_portable_files(&target, &stage, &backup);
    let restart = crate::start_menu_policy::restart_explorer_after_portable_update();
    let operation = match replacement {
        Err(error) => {
            let mut detail = error;
            if let Err(restart_error) = restart {
                detail.push_str(&format!(" Explorer restart also failed: {restart_error}"));
            }
            Err(detail)
        }
        Ok(()) => restart.map_err(|error| {
            format!("The update installed, but Explorer could not restart: {error}")
        }),
    };
    let update_result = LastUpdateResult {
        success: operation.is_ok(),
        detail: match &operation {
            Ok(()) => format!(
                "Portable update {} installed successfully.",
                manifest.version
            ),
            Err(error) => error.clone(),
        },
    };
    if let Err(error) = write_last_update_result(&update_result) {
        output_debug_error(&format!("[Metaplasia Update Helper] {error}\n"));
    }

    let application = target.join("metaplasia.exe");
    let launch = if application
        .metadata()
        .is_ok_and(|metadata| metadata.is_file())
    {
        let mut command = Command::new(&application);
        command.creation_flags(CREATE_NO_WINDOW);
        command.spawn().map(|_| ()).map_err(|error| {
            format!("Metaplasia could not restart after the update attempt: {error}")
        })
    } else {
        Err("Metaplasia could not be restored after the update attempt".into())
    };
    if let Err(launch_error) = launch {
        let detail = match operation {
            Ok(()) => launch_error,
            Err(error) => format!("{error} Application restart also failed: {launch_error}"),
        };
        let _ = write_last_update_result(&LastUpdateResult {
            success: false,
            detail: detail.clone(),
        });
        return Err(detail);
    }
    operation
}

fn canonical_directory(path: &Path, label: &str) -> Result<PathBuf, String> {
    let canonical = path
        .canonicalize()
        .map_err(|error| format!("Could not resolve the {label}: {error}"))?;
    if !canonical.metadata().is_ok_and(|metadata| metadata.is_dir()) {
        return Err(format!("The {label} is not a directory"));
    }
    Ok(canonical)
}

fn canonical_file_under(path: &Path, root: &Path, label: &str) -> Result<PathBuf, String> {
    let canonical = path
        .canonicalize()
        .map_err(|error| format!("Could not resolve the {label}: {error}"))?;
    if !canonical.starts_with(root)
        || !canonical
            .metadata()
            .is_ok_and(|metadata| metadata.is_file())
    {
        return Err(format!(
            "The {label} is outside the update cache or is not a file"
        ));
    }
    Ok(canonical)
}

fn read_file_limited(path: &Path, maximum: usize) -> Result<Vec<u8>, String> {
    let metadata = path
        .metadata()
        .map_err(|error| format!("Could not inspect {}: {error}", path.display()))?;
    if metadata.len() > maximum as u64 {
        return Err(format!("{} is too large", path.display()));
    }
    let mut file =
        File::open(path).map_err(|error| format!("Could not open {}: {error}", path.display()))?;
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.read_to_end(&mut bytes)
        .map_err(|error| format!("Could not read {}: {error}", path.display()))?;
    Ok(bytes)
}

fn verify_package_file(path: &Path, package: &ReleasePackage) -> Result<(), String> {
    let metadata = path
        .metadata()
        .map_err(|error| format!("Could not inspect the portable update package: {error}"))?;
    if metadata.len() != package.size || metadata.len() > MAX_PACKAGE_BYTES {
        return Err("The cached portable package size does not match its signed manifest".into());
    }
    let actual = sha256_file(path)?;
    if !actual.eq_ignore_ascii_case(&package.sha256) {
        return Err(
            "The cached portable package SHA-256 does not match its signed manifest".into(),
        );
    }
    Ok(())
}

fn sha256_file(path: &Path) -> Result<String, String> {
    let mut file = File::open(path).map_err(|error| {
        format!(
            "Could not open {} for verification: {error}",
            path.display()
        )
    })?;
    let mut hasher = Sha256::new();
    let mut buffer = [0_u8; 64 * 1024];
    loop {
        let count = file
            .read(&mut buffer)
            .map_err(|error| format!("Could not verify {}: {error}", path.display()))?;
        if count == 0 {
            break;
        }
        hasher.update(&buffer[..count]);
    }
    Ok(format!("{:x}", hasher.finalize()))
}

fn wait_for_process_exit(process_id: u32, timeout: Duration) -> Result<(), String> {
    // SAFETY: no handle inheritance is requested and the PID came from the
    // trusted parent process that launched this helper.
    let handle = unsafe { OpenProcess(SYNCHRONIZE, 0, process_id) };
    if handle == 0 {
        let error = std::io::Error::last_os_error();
        if error.raw_os_error() == Some(ERROR_INVALID_PARAMETER) {
            return Ok(());
        }
        return Err(format!(
            "Could not wait for the application to exit: {error}"
        ));
    }
    let handle = OwnedHandle(handle);
    let milliseconds = timeout.as_millis().min(u32::MAX as u128) as u32;
    // SAFETY: handle remains owned and valid throughout this bounded wait.
    match unsafe { WaitForSingleObject(handle.0, milliseconds) } {
        WAIT_OBJECT_0 => Ok(()),
        WAIT_TIMEOUT => Err("The application did not exit before the update timeout".into()),
        result => Err(format!(
            "Waiting for the application failed (Windows result {result})"
        )),
    }
}

fn wait_for_metaplasia_processes(timeout: Duration) -> Result<(), String> {
    let deadline = Instant::now() + timeout;
    loop {
        let running = crate::start_menu_policy::current_session_processes_named(&[
            "metaplasia-host.exe",
            "metaplasia-watchdog.exe",
        ])?;
        if running.is_empty() {
            return Ok(());
        }
        if Instant::now() >= deadline {
            return Err(format!(
                "Metaplasia background components did not stop: {}",
                running.join(", ")
            ));
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}

fn unique_nonce() -> Result<String, String> {
    let timestamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| "The system clock is earlier than the Unix epoch")?
        .as_nanos();
    Ok(format!("{}-{timestamp}", std::process::id()))
}

fn extract_portable_package(package: &Path, stage: &Path) -> Result<(), String> {
    let file = File::open(package)
        .map_err(|error| format!("Could not open the portable package: {error}"))?;
    let mut archive = zip::ZipArchive::new(file)
        .map_err(|error| format!("The portable package is not a valid ZIP archive: {error}"))?;
    if archive.len() != EXPECTED_FILES.len() {
        return Err("The portable package does not contain the exact required file set".into());
    }
    let expected: BTreeSet<&str> = EXPECTED_FILES.into_iter().collect();
    let mut observed = BTreeSet::new();
    let mut expanded = 0_u64;
    for index in 0..archive.len() {
        let mut entry = archive
            .by_index(index)
            .map_err(|error| format!("Could not read the portable package: {error}"))?;
        let name = entry.name().to_string();
        let enclosed = entry
            .enclosed_name()
            .ok_or("The portable package contains an unsafe path")?;
        if entry.is_dir()
            || enclosed.components().count() != 1
            || !expected.contains(name.as_str())
            || !observed.insert(name.clone())
            || entry
                .unix_mode()
                .is_some_and(|mode| mode & 0o170000 == 0o120000)
        {
            return Err("The portable package contains an unexpected or unsafe entry".into());
        }
        expanded = expanded
            .checked_add(entry.size())
            .ok_or("The expanded portable package size overflowed")?;
        if expanded > MAX_EXPANDED_BYTES {
            return Err("The expanded portable package is too large".into());
        }
        let destination = stage.join(&name);
        let mut output = OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&destination)
            .map_err(|error| format!("Could not stage {name}: {error}"))?;
        let declared_size = entry.size();
        let mut actual_size = 0_u64;
        let mut buffer = [0_u8; 64 * 1024];
        loop {
            let count = entry
                .read(&mut buffer)
                .map_err(|error| format!("Could not extract {name}: {error}"))?;
            if count == 0 {
                break;
            }
            actual_size = actual_size
                .checked_add(count as u64)
                .ok_or("The expanded portable entry size overflowed")?;
            if actual_size > declared_size || actual_size > MAX_EXPANDED_BYTES {
                return Err("A portable package entry exceeded its declared size".into());
            }
            output
                .write_all(&buffer[..count])
                .map_err(|error| format!("Could not stage {name}: {error}"))?;
        }
        if actual_size != declared_size {
            return Err("A portable package entry did not match its declared size".into());
        }
        output
            .sync_all()
            .map_err(|error| format!("Could not flush staged file {name}: {error}"))?;
    }
    if observed.len() != expected.len() {
        return Err("The portable package is missing required files".into());
    }
    Ok(())
}

fn replace_portable_files(target: &Path, stage: &Path, backup: &Path) -> Result<(), String> {
    fs::create_dir(backup)
        .map_err(|error| format!("Could not create the update rollback directory: {error}"))?;
    let mut backed_up = Vec::new();
    for name in EXPECTED_FILES {
        let source = target.join(name);
        let destination = backup.join(name);
        if let Err(error) = fs::rename(&source, &destination) {
            cleanup_known_directory(stage);
            let rollback = rollback_backups(target, backup, &backed_up);
            if rollback.is_ok() {
                let _ = fs::remove_dir(backup);
            }
            return Err(match rollback {
                Ok(()) => format!("Could not back up {name} before update: {error}"),
                Err(rollback_error) => format!(
                    "Could not back up {name} before update: {error}. Rollback files were preserved in {} because restoration failed: {rollback_error}",
                    backup.display()
                ),
            });
        }
        backed_up.push(name);
    }

    for name in EXPECTED_FILES {
        let source = stage.join(name);
        let destination = target.join(name);
        if let Err(error) = fs::rename(&source, &destination) {
            cleanup_known_directory(stage);
            let rollback = rollback_backups(target, backup, &backed_up);
            if rollback.is_ok() {
                let _ = fs::remove_dir(backup);
            }
            return Err(match rollback {
                Ok(()) => format!(
                    "Could not install {name}: {error}. The previous portable version was restored."
                ),
                Err(rollback_error) => format!(
                    "Could not install {name}: {error}. Rollback files were preserved in {} because restoration failed: {rollback_error}",
                    backup.display()
                ),
            });
        }
    }

    cleanup_known_directory(stage);
    cleanup_known_directory(backup);
    Ok(())
}

fn rollback_backups(target: &Path, backup: &Path, names: &[&str]) -> Result<(), String> {
    let mut failures = Vec::new();
    for name in names.iter().rev() {
        let source = backup.join(name);
        let destination = target.join(name);
        if destination.exists()
            && let Err(error) = fs::remove_file(&destination)
        {
            failures.push(format!("could not remove replacement {name}: {error}"));
            continue;
        }
        if let Err(error) = fs::rename(&source, &destination) {
            failures.push(format!("could not restore {name}: {error}"));
        }
    }
    if failures.is_empty() {
        Ok(())
    } else {
        Err(failures.join("; "))
    }
}

fn cleanup_known_directory(directory: &Path) {
    for name in EXPECTED_FILES {
        let _ = fs::remove_file(directory.join(name));
    }
    let _ = fs::remove_dir(directory);
}

fn remove_file_if_present(path: &Path) -> Result<(), String> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(format!("Could not remove {}: {error}", path.display())),
    }
}

fn last_update_result_path() -> Result<PathBuf, String> {
    Ok(update_data_directory()?.join("last-update-result.json"))
}

fn write_last_update_result(result: &LastUpdateResult) -> Result<(), String> {
    let bytes = serde_json::to_vec(result)
        .map_err(|error| format!("Could not encode the update result: {error}"))?;
    write_atomic(&last_update_result_path()?, &bytes)
}

fn take_last_update_result() -> Option<LastUpdateResult> {
    let path = last_update_result_path().ok()?;
    let bytes = match read_file_limited(&path, 16 * 1024) {
        Ok(bytes) => bytes,
        Err(_) => {
            let _ = fs::remove_file(path);
            return None;
        }
    };
    let _ = fs::remove_file(&path);
    serde_json::from_slice(&bytes).ok()
}

fn output_debug_error(message: &str) {
    let wide: Vec<u16> = OsString::from(message)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect();
    unsafe extern "system" {
        fn OutputDebugStringW(message: *const u16);
    }
    // SAFETY: wide is a valid, null-terminated UTF-16 buffer.
    unsafe { OutputDebugStringW(wide.as_ptr()) };
}

#[cfg(test)]
mod tests {
    use super::*;
    use ed25519_dalek::{Signer, SigningKey};
    use zip::write::SimpleFileOptions;

    fn signed_manifest() -> (Vec<u8>, Vec<u8>, VerifyingKey) {
        let json = br#"{"schema":1,"version":"0.2.0","published_at":"2026-08-03T12:00:00Z","notes":"Test","package":{"url":"https://github.com/mkkima/Metaplasia/releases/download/v0.2.0/Metaplasia-0.2.0-windows-x64-portable.zip","sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","size":1234}}"#.to_vec();
        let signing = SigningKey::from_bytes(&[7_u8; 32]);
        let signature = signing.sign(&json).to_bytes().to_vec();
        (json, signature, signing.verifying_key())
    }

    #[test]
    fn accepts_valid_signed_manifest() {
        let (json, signature, key) = signed_manifest();
        let manifest = verify_manifest_bytes(&json, &signature, &key).unwrap();
        assert_eq!(manifest.version, "0.2.0");
    }

    #[test]
    fn rejects_tampered_signed_manifest() {
        let (mut json, signature, key) = signed_manifest();
        let position = json.iter().position(|byte| *byte == b'T').unwrap();
        json[position] = b'X';
        assert!(verify_manifest_bytes(&json, &signature, &key).is_err());
    }

    #[test]
    fn rejects_package_from_another_location() {
        let mut manifest = ReleaseManifest {
            schema: 1,
            version: "0.2.0".into(),
            published_at: "2026-08-03T12:00:00Z".into(),
            notes: String::new(),
            package: ReleasePackage {
                url: "https://example.com/update.zip".into(),
                sha256: "a".repeat(64),
                size: 1,
            },
        };
        assert!(validate_manifest(&manifest).is_err());
        manifest.package.url = "https://github.com/mkkima/Metaplasia/releases/download/v0.2.0/Metaplasia-0.2.0-windows-x64-portable.zip".into();
        assert!(validate_manifest(&manifest).is_ok());
    }

    #[test]
    fn helper_requires_exact_bounded_arguments() {
        let arguments = vec![
            OsString::from("helper.exe"),
            OsString::from("--apply-portable-update"),
        ];
        assert!(parse_helper_arguments(&arguments).is_err());
    }

    fn create_test_archive(path: &Path, names: &[&str]) {
        let file = File::create(path).unwrap();
        let mut archive = zip::ZipWriter::new(file);
        for name in names {
            archive
                .start_file(*name, SimpleFileOptions::default())
                .unwrap();
            archive.write_all(b"verified test component").unwrap();
        }
        archive.finish().unwrap();
    }

    #[test]
    fn extracts_only_the_exact_portable_file_set() {
        let root = std::env::temp_dir().join(format!(
            "metaplasia-update-test-{}",
            unique_nonce().unwrap()
        ));
        let stage = root.join("stage");
        fs::create_dir_all(&stage).unwrap();
        let package = root.join("valid.zip");
        create_test_archive(&package, &EXPECTED_FILES);

        extract_portable_package(&package, &stage).unwrap();
        for name in EXPECTED_FILES {
            assert!(stage.join(name).is_file());
        }

        cleanup_known_directory(&stage);
        fs::remove_file(package).unwrap();
        fs::remove_dir(root).unwrap();
    }

    #[test]
    fn rejects_zip_path_traversal_without_writing_outside_stage() {
        let root = std::env::temp_dir().join(format!(
            "metaplasia-update-test-{}",
            unique_nonce().unwrap()
        ));
        let stage = root.join("stage");
        fs::create_dir_all(&stage).unwrap();
        let package = root.join("unsafe.zip");
        let names = [
            "../metaplasia.exe",
            "metaplasia-host.exe",
            "metaplasia-watchdog.exe",
            "metaplasia-cli.exe",
            "metaplasia-agent.dll",
        ];
        create_test_archive(&package, &names);

        assert!(extract_portable_package(&package, &stage).is_err());
        assert!(!root.join("metaplasia.exe").exists());

        cleanup_known_directory(&stage);
        fs::remove_file(package).unwrap();
        fs::remove_dir(root).unwrap();
    }
}
