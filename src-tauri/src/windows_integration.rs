use serde::Serialize;
use std::ffi::OsStr;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};

const AUTOSTART_KEY: &str = "Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const AUTOSTART_VALUE: &str = "Metaplasia";
const INSTANCE_MUTEX_NAME: &str = "Local\\Metaplasia.ControlCenter.v1";
const INSTANCE_EVENT_NAME: &str = "Local\\Metaplasia.ControlCenter.Activate.v1";

const ERROR_SUCCESS: i32 = 0;
const ERROR_FILE_NOT_FOUND: i32 = 2;
const ERROR_ALREADY_EXISTS: i32 = 183;
const KEY_QUERY_VALUE: u32 = 0x0001;
const KEY_SET_VALUE: u32 = 0x0002;
const REG_OPTION_NON_VOLATILE: u32 = 0;
const REG_SZ: u32 = 1;
const MAX_REGISTRY_VALUE_BYTES: u32 = 32 * 1024;
const FALSE: i32 = 0;

type Handle = isize;
type HKey = isize;

#[link(name = "advapi32")]
unsafe extern "system" {
    fn RegCloseKey(key: HKey) -> i32;
    fn RegCreateKeyExW(
        key: HKey,
        sub_key: *const u16,
        reserved: u32,
        class: *mut u16,
        options: u32,
        access: u32,
        security_attributes: *const std::ffi::c_void,
        result: *mut HKey,
        disposition: *mut u32,
    ) -> i32;
    fn RegDeleteValueW(key: HKey, value_name: *const u16) -> i32;
    fn RegOpenKeyExW(
        key: HKey,
        sub_key: *const u16,
        options: u32,
        access: u32,
        result: *mut HKey,
    ) -> i32;
    fn RegQueryValueExW(
        key: HKey,
        value_name: *const u16,
        reserved: *mut u32,
        value_type: *mut u32,
        data: *mut u8,
        data_size: *mut u32,
    ) -> i32;
    fn RegSetValueExW(
        key: HKey,
        value_name: *const u16,
        reserved: u32,
        value_type: u32,
        data: *const u8,
        data_size: u32,
    ) -> i32;
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CloseHandle(handle: Handle) -> i32;
    fn CreateEventW(
        event_attributes: *const std::ffi::c_void,
        manual_reset: i32,
        initial_state: i32,
        name: *const u16,
    ) -> Handle;
    fn CreateMutexW(
        mutex_attributes: *const std::ffi::c_void,
        initial_owner: i32,
        name: *const u16,
    ) -> Handle;
    fn GetLastError() -> u32;
    fn SetEvent(event: Handle) -> i32;
    fn WaitForSingleObject(handle: Handle, milliseconds: u32) -> u32;
}

const INFINITE: u32 = u32::MAX;
const WAIT_OBJECT_0: u32 = 0;

const HKEY_CURRENT_USER: HKey = 0x8000_0001_u32 as i32 as isize;

#[derive(Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct StartupStatus {
    enabled: bool,
    current_executable: bool,
    detail: String,
}

pub enum InstanceDisposition {
    Primary(PrimaryInstance),
    Secondary,
}

pub struct PrimaryInstance {
    _mutex: OwnedHandle,
    activation_event: OwnedHandle,
}

impl PrimaryInstance {
    pub fn into_parts(self) -> (InstanceLifetime, ActivationEvent) {
        (
            InstanceLifetime {
                _mutex: self._mutex,
            },
            ActivationEvent(self.activation_event),
        )
    }
}

pub struct InstanceLifetime {
    _mutex: OwnedHandle,
}

// The handle is only waited on by the activation listener thread. Closing it
// during process teardown is safe and releases the kernel object automatically.
pub struct ActivationEvent(OwnedHandle);

impl ActivationEvent {
    pub fn wait(&self) -> Result<(), String> {
        // SAFETY: the event handle remains owned by self for the entire wait.
        match unsafe { WaitForSingleObject(self.0.0, INFINITE) } {
            WAIT_OBJECT_0 => Ok(()),
            _ => Err(format!(
                "Could not wait for the single-instance activation event: {}",
                std::io::Error::last_os_error()
            )),
        }
    }
}

struct OwnedHandle(Handle);

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        if self.0 != 0 {
            // SAFETY: this wrapper exclusively owns a valid kernel handle.
            let _ = unsafe { CloseHandle(self.0) };
        }
    }
}

struct RegistryKey(HKey);

impl Drop for RegistryKey {
    fn drop(&mut self) {
        // SAFETY: this wrapper exclusively owns a key returned by RegCreateKeyExW.
        let _ = unsafe { RegCloseKey(self.0) };
    }
}

pub fn is_background_launch() -> bool {
    std::env::args_os()
        .skip(1)
        .any(|argument| argument == OsStr::new("--background"))
}

pub fn acquire_single_instance(notify_existing: bool) -> Result<InstanceDisposition, String> {
    let event_name = wide_null(INSTANCE_EVENT_NAME);
    // CreateEventW returns the existing object when another instance owns it.
    // Creating it before the mutex removes the startup race where a secondary
    // process observes the mutex before the primary has published its event.
    // SAFETY: all pointers are valid null-terminated UTF-16 strings.
    let event = unsafe { CreateEventW(std::ptr::null(), FALSE, FALSE, event_name.as_ptr()) };
    if event == 0 {
        return Err(format!(
            "Could not create the single-instance activation event: {}",
            std::io::Error::last_os_error()
        ));
    }
    let event = OwnedHandle(event);

    let mutex_name = wide_null(INSTANCE_MUTEX_NAME);
    // SAFETY: all pointers are valid null-terminated UTF-16 strings.
    let mutex = unsafe { CreateMutexW(std::ptr::null(), FALSE, mutex_name.as_ptr()) };
    if mutex == 0 {
        return Err(format!(
            "Could not create the single-instance mutex: {}",
            std::io::Error::last_os_error()
        ));
    }
    // GetLastError must be read immediately after CreateMutexW.
    let already_exists = unsafe { GetLastError() } == ERROR_ALREADY_EXISTS as u32;
    let mutex = OwnedHandle(mutex);
    if already_exists {
        if notify_existing {
            // SAFETY: CreateEventW returned a valid handle with EVENT_MODIFY_STATE.
            if unsafe { SetEvent(event.0) } == 0 {
                return Err(format!(
                    "Could not activate the running Metaplasia instance: {}",
                    std::io::Error::last_os_error()
                ));
            }
        }
        return Ok(InstanceDisposition::Secondary);
    }

    Ok(InstanceDisposition::Primary(PrimaryInstance {
        _mutex: mutex,
        activation_event: event,
    }))
}

pub fn startup_status() -> Result<StartupStatus, String> {
    let expected = startup_command(&current_executable()?);
    Ok(status_from_registered_command(
        &expected,
        read_autostart_value()?.as_deref(),
    ))
}

fn status_from_registered_command(expected: &str, registered: Option<&str>) -> StartupStatus {
    match registered {
        Some(command) if command == expected => StartupStatus {
            enabled: true,
            current_executable: true,
            detail: "Starts in the system tray at sign-in using the saved shell settings.".into(),
        },
        Some(_) => StartupStatus {
            enabled: true,
            current_executable: false,
            detail: "Autostart points to another portable copy. Turn it off and on to use this executable."
                .into(),
        },
        None => StartupStatus {
            enabled: false,
            current_executable: false,
            detail: "Autostart is disabled. Metaplasia will not run at sign-in.".into(),
        },
    }
}

pub fn set_startup_enabled(enabled: bool) -> Result<StartupStatus, String> {
    let key = open_autostart_key()?;
    let value_name = wide_null(AUTOSTART_VALUE);
    if enabled {
        let command = startup_command(&current_executable()?);
        let command_wide = wide_null(&command);
        let byte_count = command_wide
            .len()
            .checked_mul(std::mem::size_of::<u16>())
            .and_then(|size| u32::try_from(size).ok())
            .ok_or("The autostart command is too long")?;
        // SAFETY: key and UTF-16 buffers are valid for the specified sizes.
        let result = unsafe {
            RegSetValueExW(
                key.0,
                value_name.as_ptr(),
                0,
                REG_SZ,
                command_wide.as_ptr().cast::<u8>(),
                byte_count,
            )
        };
        registry_result(result, "write the autostart value")?;
    } else {
        // SAFETY: key and value name are valid for this call.
        let result = unsafe { RegDeleteValueW(key.0, value_name.as_ptr()) };
        if result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND {
            registry_result(result, "remove the autostart value")?;
        }
    }
    drop(key);

    let status = startup_status()?;
    if status.enabled != enabled || (enabled && !status.current_executable) {
        return Err("Windows did not persist the requested autostart state".into());
    }
    Ok(status)
}

fn current_executable() -> Result<PathBuf, String> {
    let executable = std::env::current_exe()
        .map_err(|error| format!("Could not resolve the portable executable path: {error}"))?;
    if !executable.is_absolute()
        || !executable
            .metadata()
            .is_ok_and(|metadata| metadata.is_file())
    {
        return Err("The portable executable path is not a valid absolute file".into());
    }
    Ok(executable)
}

fn startup_command(executable: &Path) -> String {
    format!("\"{}\" --background", executable.display())
}

fn open_autostart_key() -> Result<RegistryKey, String> {
    let sub_key = wide_null(AUTOSTART_KEY);
    let mut key = 0;
    let mut disposition = 0;
    // SAFETY: all output pointers and the null-terminated key path are valid.
    let result = unsafe {
        RegCreateKeyExW(
            HKEY_CURRENT_USER,
            sub_key.as_ptr(),
            0,
            std::ptr::null_mut(),
            REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE,
            std::ptr::null(),
            &mut key,
            &mut disposition,
        )
    };
    registry_result(result, "open the current-user autostart key")?;
    Ok(RegistryKey(key))
}

fn read_autostart_value() -> Result<Option<String>, String> {
    let sub_key = wide_null(AUTOSTART_KEY);
    let mut key = 0;
    // SAFETY: the key path and output pointer are valid for this call.
    let result = unsafe {
        RegOpenKeyExW(
            HKEY_CURRENT_USER,
            sub_key.as_ptr(),
            0,
            KEY_QUERY_VALUE,
            &mut key,
        )
    };
    if result == ERROR_FILE_NOT_FOUND {
        return Ok(None);
    }
    registry_result(result, "open the current-user autostart key")?;
    let key = RegistryKey(key);
    let value_name = wide_null(AUTOSTART_VALUE);
    let mut value_type = 0;
    let mut byte_count = 0;
    // SAFETY: key and output pointers are valid; a null data pointer queries size.
    let result = unsafe {
        RegQueryValueExW(
            key.0,
            value_name.as_ptr(),
            std::ptr::null_mut(),
            &mut value_type,
            std::ptr::null_mut(),
            &mut byte_count,
        )
    };
    if result == ERROR_FILE_NOT_FOUND {
        return Ok(None);
    }
    registry_result(result, "read the autostart value size")?;
    if value_type != REG_SZ || byte_count == 0 || byte_count > MAX_REGISTRY_VALUE_BYTES {
        return Err("The Metaplasia autostart value has an invalid type or size".into());
    }

    let code_units = usize::try_from(byte_count)
        .ok()
        .and_then(|bytes| bytes.checked_add(1))
        .map(|bytes| bytes / 2)
        .ok_or("The Metaplasia autostart value size is invalid")?;
    let mut buffer = vec![0_u16; code_units];
    // SAFETY: buffer has at least byte_count writable bytes and pointers are valid.
    let result = unsafe {
        RegQueryValueExW(
            key.0,
            value_name.as_ptr(),
            std::ptr::null_mut(),
            &mut value_type,
            buffer.as_mut_ptr().cast::<u8>(),
            &mut byte_count,
        )
    };
    registry_result(result, "read the autostart value")?;
    if value_type != REG_SZ || byte_count % 2 != 0 {
        return Err("The Metaplasia autostart value is not a valid UTF-16 string".into());
    }
    let returned_units = usize::try_from(byte_count / 2)
        .map_err(|_| "The Metaplasia autostart value is too large")?;
    buffer.truncate(returned_units);
    while buffer.last() == Some(&0) {
        buffer.pop();
    }
    String::from_utf16(&buffer)
        .map(Some)
        .map_err(|_| "The Metaplasia autostart value contains invalid UTF-16".into())
}

fn registry_result(result: i32, operation: &str) -> Result<(), String> {
    if result == ERROR_SUCCESS {
        Ok(())
    } else {
        Err(format!(
            "Could not {operation}: {}",
            std::io::Error::from_raw_os_error(result)
        ))
    }
}

fn wide_null(value: &str) -> Vec<u16> {
    OsStr::new(value)
        .encode_wide()
        .chain(std::iter::once(0))
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::os::windows::ffi::OsStringExt;

    #[test]
    fn startup_command_quotes_a_portable_path_and_uses_background_mode() {
        assert_eq!(
            startup_command(Path::new("D:\\Portable Apps\\Metaplasia\\metaplasia.exe")),
            "\"D:\\Portable Apps\\Metaplasia\\metaplasia.exe\" --background"
        );
    }

    #[test]
    fn wide_null_adds_exactly_one_terminator() {
        let encoded = wide_null("Metaplasia");
        assert_eq!(encoded.last(), Some(&0));
        assert!(!encoded[..encoded.len() - 1].contains(&0));
        assert_eq!(
            std::ffi::OsString::from_wide(&encoded[..encoded.len() - 1]),
            "Metaplasia"
        );
    }

    #[test]
    fn startup_status_distinguishes_current_stale_and_missing_registrations() {
        let expected = "\"D:\\Metaplasia\\metaplasia.exe\" --background";

        let current = status_from_registered_command(expected, Some(expected));
        assert!(current.enabled);
        assert!(current.current_executable);

        let stale = status_from_registered_command(
            expected,
            Some("\"C:\\Old\\metaplasia.exe\" --background"),
        );
        assert!(stale.enabled);
        assert!(!stale.current_executable);
        assert!(stale.detail.contains("another portable copy"));

        let missing = status_from_registered_command(expected, None);
        assert!(!missing.enabled);
        assert!(!missing.current_executable);
    }
}
