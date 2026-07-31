use std::ffi::c_void;
use std::fmt::{Display, Formatter};
use std::os::windows::ffi::{OsStrExt, OsStringExt};

const HKEY_CURRENT_USER: isize = 0x8000_0001_u32 as i32 as isize;
const HKEY_LOCAL_MACHINE: isize = 0x8000_0002_u32 as i32 as isize;

const ERROR_SUCCESS: i32 = 0;
const ERROR_FILE_NOT_FOUND: i32 = 2;
const ERROR_PATH_NOT_FOUND: i32 = 3;
const KEY_QUERY_VALUE: u32 = 0x0001;
const KEY_SET_VALUE: u32 = 0x0002;
const REG_OPTION_NON_VOLATILE: u32 = 0;
const REG_DWORD: u32 = 4;
const RRF_RT_REG_SZ: u32 = 0x0000_0002;
const RRF_RT_REG_DWORD: u32 = 0x0000_0010;
const RRF_ZEROONFAILURE: u32 = 0x2000_0000;

const WM_SETTINGCHANGE: u32 = 0x001A;
const SMTO_ABORTIFHUNG: u32 = 0x0002;
const HWND_BROADCAST: isize = 0xFFFF;
const SEE_MASK_NOCLOSEPROCESS: u32 = 0x0000_0040;
const SW_HIDE: i32 = 0;
const WAIT_OBJECT_0: u32 = 0;
const WAIT_TIMEOUT: u32 = 258;
const ELEVATED_HELPER_TIMEOUT_MS: u32 = 120_000;
const ERROR_ACCESS_DENIED: i32 = 5;
const ERROR_NO_MORE_FILES: i32 = 18;
const INVALID_HANDLE_VALUE: isize = -1;
const TH32CS_SNAPPROCESS: u32 = 0x0000_0002;
const PROCESS_TERMINATE: u32 = 0x0001;
const SYNCHRONIZE: u32 = 0x0010_0000;
const CREATE_NO_WINDOW: u32 = 0x0800_0000;
const MAX_PATH: usize = 260;

const CURRENT_VERSION_KEY: &str = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
const CATEGORY_POLICY_KEY: &str = "Software\\Policies\\Microsoft\\Windows\\Explorer";
const CATEGORY_POLICY_VALUE: &str = "HideCategoryView";
const APP_LIST_POLICY_KEY: &str =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer";
const APP_LIST_POLICY_VALUE: &str = "NoStartMenuMorePrograms";
const OWNERSHIP_KEY: &str = "Software\\Metaplasia\\PolicyOwnership";
const APP_LIST_OWNERSHIP_VALUE: &str = "HideAllAppsDevicePolicy";
const LEGACY_CATEGORY_OWNERSHIP_VALUE: &str = "HideCategoryViewDevicePolicy";
const START_MENU_PROCESS: &str = "StartMenuExperienceHost.exe";

#[link(name = "advapi32")]
unsafe extern "system" {
    fn RegGetValueW(
        key: isize,
        sub_key: *const u16,
        value: *const u16,
        flags: u32,
        value_type: *mut u32,
        data: *mut c_void,
        data_size: *mut u32,
    ) -> i32;

    fn RegCreateKeyExW(
        key: isize,
        sub_key: *const u16,
        reserved: u32,
        class: *mut u16,
        options: u32,
        access: u32,
        security_attributes: *const c_void,
        result: *mut isize,
        disposition: *mut u32,
    ) -> i32;

    fn RegSetValueExW(
        key: isize,
        value_name: *const u16,
        reserved: u32,
        value_type: u32,
        data: *const u8,
        data_size: u32,
    ) -> i32;

    fn RegOpenKeyExW(
        key: isize,
        sub_key: *const u16,
        options: u32,
        access: u32,
        result: *mut isize,
    ) -> i32;

    fn RegDeleteValueW(key: isize, value_name: *const u16) -> i32;

    fn RegCloseKey(key: isize) -> i32;
}

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CloseHandle(handle: isize) -> i32;
    fn CreateToolhelp32Snapshot(flags: u32, process_id: u32) -> isize;
    fn GetCurrentProcessId() -> u32;
    fn GetWindowsDirectoryW(buffer: *mut u16, size: u32) -> u32;
    fn GetExitCodeProcess(process: isize, exit_code: *mut u32) -> i32;
    fn OpenProcess(access: u32, inherit_handle: i32, process_id: u32) -> isize;
    fn Process32FirstW(snapshot: isize, entry: *mut ProcessEntry32W) -> i32;
    fn Process32NextW(snapshot: isize, entry: *mut ProcessEntry32W) -> i32;
    fn ProcessIdToSessionId(process_id: u32, session_id: *mut u32) -> i32;
    fn TerminateProcess(process: isize, exit_code: u32) -> i32;
    fn WaitForSingleObject(handle: isize, timeout_ms: u32) -> u32;
}

#[link(name = "shell32")]
unsafe extern "system" {
    fn ShellExecuteExW(info: *mut ShellExecuteInfoW) -> i32;
}

#[link(name = "user32")]
unsafe extern "system" {
    fn FindWindowW(class_name: *const u16, window_name: *const u16) -> isize;
    fn GetWindowThreadProcessId(window: isize, process_id: *mut u32) -> u32;
    fn SendMessageTimeoutW(
        window: isize,
        message: u32,
        wparam: usize,
        lparam: isize,
        flags: u32,
        timeout_ms: u32,
        result: *mut usize,
    ) -> isize;
}

#[derive(Debug, Clone)]
pub(crate) struct StartAppListPolicyState {
    pub hidden: bool,
    pub supported: bool,
    pub editable: bool,
    pub detail: String,
}

struct RegistryKey(isize);

#[repr(C)]
struct ProcessEntry32W {
    size: u32,
    usage: u32,
    process_id: u32,
    default_heap_id: usize,
    module_id: u32,
    thread_count: u32,
    parent_process_id: u32,
    base_priority: i32,
    flags: u32,
    executable: [u16; MAX_PATH],
}

impl Default for ProcessEntry32W {
    fn default() -> Self {
        Self {
            size: std::mem::size_of::<Self>() as u32,
            usage: 0,
            process_id: 0,
            default_heap_id: 0,
            module_id: 0,
            thread_count: 0,
            parent_process_id: 0,
            base_priority: 0,
            flags: 0,
            executable: [0; MAX_PATH],
        }
    }
}

#[repr(C)]
struct ShellExecuteInfoW {
    size: u32,
    mask: u32,
    window: isize,
    verb: *const u16,
    file: *const u16,
    parameters: *const u16,
    directory: *const u16,
    show: i32,
    instance: isize,
    id_list: *mut c_void,
    class: *const u16,
    class_key: isize,
    hot_key: u32,
    icon_or_monitor: isize,
    process: isize,
}

struct ProcessHandle(isize);

struct SnapshotHandle(isize);

impl Drop for ProcessHandle {
    fn drop(&mut self) {
        // SAFETY: this handle was returned by ShellExecuteExW with
        // SEE_MASK_NOCLOSEPROCESS and is closed exactly once here.
        let _ = unsafe { CloseHandle(self.0) };
    }
}

impl Drop for SnapshotHandle {
    fn drop(&mut self) {
        // SAFETY: the Toolhelp snapshot handle is owned by this wrapper and is
        // closed exactly once when enumeration has finished.
        let _ = unsafe { CloseHandle(self.0) };
    }
}

#[derive(Debug)]
struct RegistryWriteError {
    operation: &'static str,
    code: i32,
}

impl Display for RegistryWriteError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> std::fmt::Result {
        write!(
            formatter,
            "Could not {}: {} (Windows error {})",
            self.operation,
            std::io::Error::from_raw_os_error(self.code),
            self.code
        )
    }
}

impl Drop for RegistryKey {
    fn drop(&mut self) {
        // SAFETY: this handle was returned by RegCreateKeyExW and remains owned
        // by this RAII wrapper until the single close performed here.
        let _ = unsafe { RegCloseKey(self.0) };
    }
}

pub(crate) fn query() -> Result<StartAppListPolicyState, String> {
    let build = read_registry_string(
        HKEY_LOCAL_MACHINE,
        CURRENT_VERSION_KEY,
        "CurrentBuildNumber",
    )?
    .parse::<u32>()
    .map_err(|_| "Windows reported an invalid build number".to_owned())?;
    let revision = read_registry_dword(HKEY_LOCAL_MACHINE, CURRENT_VERSION_KEY, "UBR")?
        .ok_or_else(|| "Windows did not report its update revision".to_owned())?;
    let edition = read_registry_string(HKEY_LOCAL_MACHINE, CURRENT_VERSION_KEY, "EditionID")?;

    if !supports_start_app_list_policy(build, revision, &edition) {
        return Ok(StartAppListPolicyState {
            hidden: false,
            supported: false,
            editable: false,
            detail: format!(
                "Requires Windows 11 24H2/25H2 build 26100.7019 or newer on a supported edition (current: {build}.{revision}, {edition})."
            ),
        });
    }

    let owned = read_ownership_marker(APP_LIST_OWNERSHIP_VALUE)?;
    if let Some(value) = read_app_list_policy(HKEY_LOCAL_MACHINE)? {
        return Ok(StartAppListPolicyState {
            hidden: value == 1,
            supported: true,
            editable: owned,
            detail: if owned {
                "Keeps the All apps area empty using a Metaplasia-owned device policy."
            } else {
                "The All apps list is controlled by a device policy outside Metaplasia."
            }
            .into(),
        });
    }

    Ok(StartAppListPolicyState {
        hidden: false,
        supported: true,
        editable: true,
        detail: "Removes all app entries from the All section and leaves that area empty. Requires administrator approval."
            .into(),
    })
}

pub(crate) fn set_hidden(hidden: bool) -> Result<(), String> {
    let state = query()?;
    if !state.supported {
        return Err(state.detail);
    }
    if !state.editable {
        return Err(state.detail);
    }

    run_elevated_helper(hidden)?;
    let verified = read_app_list_policy(HKEY_LOCAL_MACHINE)?;
    if (hidden && verified != Some(1)) || (!hidden && verified.is_some()) {
        return Err("The elevated helper did not persist the expected All apps policy".into());
    }
    refresh_shell_for_app_list_policy()?;
    Ok(())
}

pub(crate) fn set_hidden_from_elevated_helper(hidden: bool) -> Result<(), String> {
    let state = query()?;
    if !state.supported || !state.editable {
        return Err(state.detail);
    }
    set_hidden_direct(hidden).map_err(|error| error.to_string())
}

fn set_hidden_direct(hidden: bool) -> Result<(), RegistryWriteError> {
    remove_legacy_category_policy_if_owned()?;
    if hidden {
        write_registry_dword(
            HKEY_LOCAL_MACHINE,
            APP_LIST_POLICY_KEY,
            APP_LIST_POLICY_VALUE,
            1,
            "write the All apps device policy",
        )?;
        if let Err(error) = write_registry_dword(
            HKEY_CURRENT_USER,
            OWNERSHIP_KEY,
            APP_LIST_OWNERSHIP_VALUE,
            1,
            "record ownership of the All apps device policy",
        ) {
            let _ = delete_registry_value(
                HKEY_LOCAL_MACHINE,
                APP_LIST_POLICY_KEY,
                APP_LIST_POLICY_VALUE,
                "roll back the All apps device policy",
            );
            return Err(error);
        }
    } else {
        delete_registry_value(
            HKEY_LOCAL_MACHINE,
            APP_LIST_POLICY_KEY,
            APP_LIST_POLICY_VALUE,
            "remove the All apps device policy",
        )?;
        delete_registry_value(
            HKEY_CURRENT_USER,
            OWNERSHIP_KEY,
            APP_LIST_OWNERSHIP_VALUE,
            "clear ownership of the All apps device policy",
        )?;
    }
    broadcast_policy_change();
    Ok(())
}

fn write_registry_dword(
    root: isize,
    path: &str,
    value_name: &str,
    value: u32,
    operation: &'static str,
) -> Result<(), RegistryWriteError> {
    let key = create_registry_key(root, path, operation)?;
    let value_name = wide(value_name);
    // SAFETY: key is valid and the name plus DWORD storage remain alive for
    // the exact duration and byte count passed to RegSetValueExW.
    let status = unsafe {
        RegSetValueExW(
            key.0,
            value_name.as_ptr(),
            0,
            REG_DWORD,
            (&value as *const u32).cast::<u8>(),
            std::mem::size_of::<u32>() as u32,
        )
    };
    check_write_status(status, operation)
}

fn create_registry_key(
    root: isize,
    path: &str,
    operation: &'static str,
) -> Result<RegistryKey, RegistryWriteError> {
    let path = wide(path);
    let mut key = 0_isize;
    let mut disposition = 0_u32;
    // SAFETY: all optional pointers are null, and output storage plus the
    // null-terminated UTF-16 path are valid for the entire call.
    let status = unsafe {
        RegCreateKeyExW(
            root,
            path.as_ptr(),
            0,
            std::ptr::null_mut(),
            REG_OPTION_NON_VOLATILE,
            KEY_QUERY_VALUE | KEY_SET_VALUE,
            std::ptr::null(),
            &mut key,
            &mut disposition,
        )
    };
    check_write_status(status, operation)?;
    if key == 0 {
        return Err(RegistryWriteError { operation, code: 6 });
    }
    Ok(RegistryKey(key))
}

fn delete_registry_value(
    root: isize,
    path: &str,
    value_name: &str,
    operation: &'static str,
) -> Result<(), RegistryWriteError> {
    let path = wide(path);
    let mut key = 0_isize;
    // SAFETY: path and output storage are valid for this registry open call.
    let status = unsafe { RegOpenKeyExW(root, path.as_ptr(), 0, KEY_SET_VALUE, &mut key) };
    if status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND {
        return Ok(());
    }
    check_write_status(status, operation)?;
    let key = RegistryKey(key);
    let value_name = wide(value_name);
    // SAFETY: key is open for KEY_SET_VALUE and value_name is null-terminated.
    let status = unsafe { RegDeleteValueW(key.0, value_name.as_ptr()) };
    if status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND {
        return Ok(());
    }
    check_write_status(status, operation)
}

fn remove_legacy_category_policy_if_owned() -> Result<(), RegistryWriteError> {
    let owned =
        read_ownership_marker(LEGACY_CATEGORY_OWNERSHIP_VALUE).map_err(|_| RegistryWriteError {
            operation: "inspect ownership of the legacy category policy",
            code: ERROR_ACCESS_DENIED,
        })?;
    if !owned {
        return Ok(());
    }
    if read_category_policy(HKEY_CURRENT_USER)
        .map_err(|_| RegistryWriteError {
            operation: "inspect the legacy Start menu user policy",
            code: ERROR_ACCESS_DENIED,
        })?
        .is_some()
    {
        write_registry_dword(
            HKEY_CURRENT_USER,
            CATEGORY_POLICY_KEY,
            CATEGORY_POLICY_VALUE,
            0,
            "neutralize the legacy Start menu user policy",
        )?;
    }
    delete_registry_value(
        HKEY_LOCAL_MACHINE,
        CATEGORY_POLICY_KEY,
        CATEGORY_POLICY_VALUE,
        "remove the legacy category device policy",
    )?;
    delete_registry_value(
        HKEY_CURRENT_USER,
        OWNERSHIP_KEY,
        LEGACY_CATEGORY_OWNERSHIP_VALUE,
        "clear ownership of the legacy category policy",
    )
}

fn read_ownership_marker(value_name: &str) -> Result<bool, String> {
    match read_registry_dword(HKEY_CURRENT_USER, OWNERSHIP_KEY, value_name)? {
        None | Some(0) => Ok(false),
        Some(1) => Ok(true),
        Some(value) => Err(format!(
            "The Metaplasia Start menu policy ownership marker is invalid: {value}"
        )),
    }
}

fn run_elevated_helper(hidden: bool) -> Result<(), String> {
    let executable = std::env::current_exe()
        .map_err(|error| format!("Could not locate the policy helper: {error}"))?;
    let verb = wide("runas");
    let file = wide_os(executable.as_os_str());
    let parameters = wide(&format!(
        "--apply-start-all-apps-policy {}",
        u8::from(hidden)
    ));
    let mut info = ShellExecuteInfoW {
        size: std::mem::size_of::<ShellExecuteInfoW>() as u32,
        mask: SEE_MASK_NOCLOSEPROCESS,
        window: 0,
        verb: verb.as_ptr(),
        file: file.as_ptr(),
        parameters: parameters.as_ptr(),
        directory: std::ptr::null(),
        show: SW_HIDE,
        instance: 0,
        id_list: std::ptr::null_mut(),
        class: std::ptr::null(),
        class_key: 0,
        hot_key: 0,
        icon_or_monitor: 0,
        process: 0,
    };
    // SAFETY: every string and the structure remain alive for the entire call.
    // The helper path is the currently running executable and its arguments
    // select one bounded policy operation.
    if unsafe { ShellExecuteExW(&mut info) } == 0 {
        return Err(format!(
            "Administrator approval was not granted: {}",
            std::io::Error::last_os_error()
        ));
    }
    if info.process == 0 {
        return Err("Windows did not return an elevated policy helper process".into());
    }
    let process = ProcessHandle(info.process);
    // SAFETY: process owns a valid process handle until this bounded wait ends.
    match unsafe { WaitForSingleObject(process.0, ELEVATED_HELPER_TIMEOUT_MS) } {
        WAIT_OBJECT_0 => {}
        WAIT_TIMEOUT => return Err("The elevated policy helper timed out".into()),
        result => {
            return Err(format!(
                "Waiting for the elevated policy helper failed (Windows wait result {result})"
            ));
        }
    }
    let mut exit_code = u32::MAX;
    // SAFETY: process is still open and exit_code points to writable storage.
    if unsafe { GetExitCodeProcess(process.0, &mut exit_code) } == 0 {
        return Err(format!(
            "Could not read the elevated policy helper result: {}",
            std::io::Error::last_os_error()
        ));
    }
    if exit_code != 0 {
        return Err(format!(
            "The elevated policy helper failed (exit code {exit_code})"
        ));
    }
    Ok(())
}

fn refresh_shell_for_app_list_policy() -> Result<(), String> {
    restart_start_menu_host()?;
    restart_explorer_shell()
}

fn restart_explorer_shell() -> Result<(), String> {
    let shell_class = wide("Shell_TrayWnd");
    // SAFETY: class_name is a valid null-terminated UTF-16 string and a null
    // title requests the unique shell taskbar window.
    let taskbar = unsafe { FindWindowW(shell_class.as_ptr(), std::ptr::null()) };
    if taskbar != 0 {
        let mut process_id = 0_u32;
        // SAFETY: taskbar is a live window handle and process_id is writable.
        unsafe { GetWindowThreadProcessId(taskbar, &mut process_id) };
        if process_id == 0 {
            return Err("Windows returned an invalid Explorer shell process".into());
        }
        // SAFETY: the PID was resolved from the exact Shell_TrayWnd owner and
        // no handle inheritance is requested.
        let process = unsafe { OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, 0, process_id) };
        if process == 0 {
            return Err(format!(
                "Could not open Explorer for refresh: {}",
                std::io::Error::last_os_error()
            ));
        }
        let process = ProcessHandle(process);
        // SAFETY: process is the current shell taskbar owner and was opened
        // with PROCESS_TERMINATE.
        if unsafe { TerminateProcess(process.0, 0) } == 0 {
            return Err(format!(
                "Could not refresh Explorer: {}",
                std::io::Error::last_os_error()
            ));
        }
        // SAFETY: process remains open for this bounded wait.
        match unsafe { WaitForSingleObject(process.0, 5_000) } {
            WAIT_OBJECT_0 => {}
            WAIT_TIMEOUT => return Err("Explorer did not exit during refresh".into()),
            result => {
                return Err(format!(
                    "Waiting for Explorer refresh failed (Windows wait result {result})"
                ));
            }
        }
    }

    // Explorer may auto-restart. Give it a short chance before explicitly
    // launching the system image to avoid duplicate shell instances.
    for _ in 0..10 {
        std::thread::sleep(std::time::Duration::from_millis(100));
        // SAFETY: shell_class remains valid throughout the loop.
        if unsafe { FindWindowW(shell_class.as_ptr(), std::ptr::null()) } != 0 {
            return Ok(());
        }
    }

    let explorer = windows_explorer_path()?;
    let mut command = std::process::Command::new(&explorer);
    use std::os::windows::process::CommandExt;
    command.creation_flags(CREATE_NO_WINDOW);
    command
        .spawn()
        .map_err(|error| format!("Could not start {}: {error}", explorer.display()))?;

    for _ in 0..100 {
        std::thread::sleep(std::time::Duration::from_millis(100));
        // SAFETY: shell_class remains valid throughout the loop.
        if unsafe { FindWindowW(shell_class.as_ptr(), std::ptr::null()) } != 0 {
            return Ok(());
        }
    }
    Err("Explorer did not recreate the shell taskbar after refresh".into())
}

fn windows_explorer_path() -> Result<std::path::PathBuf, String> {
    let mut buffer = [0_u16; MAX_PATH];
    // SAFETY: buffer is writable and its capacity is passed in UTF-16 units.
    let length = unsafe { GetWindowsDirectoryW(buffer.as_mut_ptr(), buffer.len() as u32) };
    if length == 0 || length as usize >= buffer.len() {
        return Err(format!(
            "Could not resolve the Windows directory: {}",
            std::io::Error::last_os_error()
        ));
    }
    let directory = std::ffi::OsString::from_wide(&buffer[..length as usize]);
    Ok(std::path::PathBuf::from(directory).join("explorer.exe"))
}

fn restart_start_menu_host() -> Result<(), String> {
    // SAFETY: no process-specific pointer data is passed to snapshot creation.
    let snapshot = unsafe { CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0) };
    if snapshot == INVALID_HANDLE_VALUE {
        return Err(format!(
            "Could not enumerate the Start menu process: {}",
            std::io::Error::last_os_error()
        ));
    }
    let snapshot = SnapshotHandle(snapshot);

    let mut current_session = 0_u32;
    // SAFETY: current_session is writable and the current PID is always valid.
    if unsafe { ProcessIdToSessionId(GetCurrentProcessId(), &mut current_session) } == 0 {
        return Err(format!(
            "Could not resolve the current Windows session: {}",
            std::io::Error::last_os_error()
        ));
    }

    let mut entry = ProcessEntry32W::default();
    // SAFETY: entry has the required size field and writable fixed storage.
    let mut has_entry = unsafe { Process32FirstW(snapshot.0, &mut entry) } != 0;
    while has_entry {
        let length = entry
            .executable
            .iter()
            .position(|unit| *unit == 0)
            .unwrap_or(entry.executable.len());
        let executable = String::from_utf16_lossy(&entry.executable[..length]);
        if executable.eq_ignore_ascii_case(START_MENU_PROCESS) {
            let mut process_session = u32::MAX;
            // SAFETY: process_session points to writable storage.
            let session_ok =
                unsafe { ProcessIdToSessionId(entry.process_id, &mut process_session) } != 0;
            if session_ok && process_session == current_session {
                // SAFETY: the PID came from the live Toolhelp snapshot and no
                // handle inheritance is requested.
                let process =
                    unsafe { OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, 0, entry.process_id) };
                if process == 0 {
                    return Err(format!(
                        "Could not open the Start menu process for refresh: {}",
                        std::io::Error::last_os_error()
                    ));
                }
                let process = ProcessHandle(process);
                // SAFETY: process is an exact-name, current-session Start host
                // opened with PROCESS_TERMINATE.
                if unsafe { TerminateProcess(process.0, 0) } == 0 {
                    return Err(format!(
                        "Could not refresh the Start menu process: {}",
                        std::io::Error::last_os_error()
                    ));
                }
                // SAFETY: the process handle remains open during this bounded
                // wait, ensuring the old instance has actually exited.
                match unsafe { WaitForSingleObject(process.0, 5_000) } {
                    WAIT_OBJECT_0 => return Ok(()),
                    WAIT_TIMEOUT => {
                        return Err("The Start menu process did not exit during refresh".into());
                    }
                    result => {
                        return Err(format!(
                            "Waiting for the Start menu refresh failed (Windows wait result {result})"
                        ));
                    }
                }
            }
        }

        entry = ProcessEntry32W::default();
        // SAFETY: entry is reset with the required structure size before the
        // next Toolhelp enumeration call.
        has_entry = unsafe { Process32NextW(snapshot.0, &mut entry) } != 0;
    }
    if std::io::Error::last_os_error().raw_os_error() != Some(ERROR_NO_MORE_FILES) {
        return Err(format!(
            "Start menu process enumeration failed: {}",
            std::io::Error::last_os_error()
        ));
    }
    // If Start has not been launched in this session, the next activation will
    // read the new policy and no process needs to be terminated.
    Ok(())
}

fn read_category_policy(root: isize) -> Result<Option<u32>, String> {
    read_registry_dword(root, CATEGORY_POLICY_KEY, CATEGORY_POLICY_VALUE)
}

fn read_app_list_policy(root: isize) -> Result<Option<u32>, String> {
    read_registry_dword(root, APP_LIST_POLICY_KEY, APP_LIST_POLICY_VALUE)
}

fn read_registry_dword(
    root: isize,
    sub_key: &str,
    value_name: &str,
) -> Result<Option<u32>, String> {
    let sub_key = wide(sub_key);
    let value_name = wide(value_name);
    let mut value = 0_u32;
    let mut size = std::mem::size_of::<u32>() as u32;
    // SAFETY: the input strings are null-terminated and the output buffer is
    // exactly one DWORD, with its byte size supplied to RegGetValueW.
    let status = unsafe {
        RegGetValueW(
            root,
            sub_key.as_ptr(),
            value_name.as_ptr(),
            RRF_RT_REG_DWORD | RRF_ZEROONFAILURE,
            std::ptr::null_mut(),
            (&mut value as *mut u32).cast::<c_void>(),
            &mut size,
        )
    };
    if status == ERROR_FILE_NOT_FOUND {
        return Ok(None);
    }
    check_status(status, "read a Windows policy value")?;
    if size != std::mem::size_of::<u32>() as u32 {
        return Err("Windows returned a malformed DWORD policy value".into());
    }
    Ok(Some(value))
}

fn read_registry_string(root: isize, sub_key: &str, value_name: &str) -> Result<String, String> {
    let sub_key = wide(sub_key);
    let value_name = wide(value_name);
    let mut size = 0_u32;
    // SAFETY: this sizing call uses valid null-terminated input strings and a
    // null data pointer, as required by RegGetValueW.
    let status = unsafe {
        RegGetValueW(
            root,
            sub_key.as_ptr(),
            value_name.as_ptr(),
            RRF_RT_REG_SZ,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut size,
        )
    };
    check_status(status, "read Windows version metadata")?;
    if !(2..=512).contains(&size) || !size.is_multiple_of(2) {
        return Err("Windows returned malformed version metadata".into());
    }

    let mut buffer = vec![0_u16; size as usize / 2];
    // SAFETY: the buffer is sized from the successful registry sizing call;
    // the byte count accurately describes its writable capacity.
    let status = unsafe {
        RegGetValueW(
            root,
            sub_key.as_ptr(),
            value_name.as_ptr(),
            RRF_RT_REG_SZ | RRF_ZEROONFAILURE,
            std::ptr::null_mut(),
            buffer.as_mut_ptr().cast::<c_void>(),
            &mut size,
        )
    };
    check_status(status, "read Windows version metadata")?;
    let length = buffer
        .iter()
        .position(|unit| *unit == 0)
        .unwrap_or(buffer.len());
    String::from_utf16(&buffer[..length])
        .map_err(|_| "Windows returned invalid UTF-16 version metadata".into())
}

fn supports_start_app_list_policy(build: u32, revision: u32, edition: &str) -> bool {
    let supported_build = build > 26200 || ((build == 26100 || build == 26200) && revision >= 7019);
    supported_build && is_supported_edition(edition)
}

fn is_supported_edition(edition: &str) -> bool {
    ["Professional", "Enterprise", "Education", "IoTEnterprise"]
        .iter()
        .any(|prefix| edition.starts_with(prefix))
}

fn broadcast_policy_change() {
    let parameter = wide("Policy");
    let mut result = 0_usize;
    // SAFETY: HWND_BROADCAST is the documented broadcast pseudo-handle and
    // the UTF-16 parameter remains valid until this bounded call returns.
    let _ = unsafe {
        SendMessageTimeoutW(
            HWND_BROADCAST,
            WM_SETTINGCHANGE,
            0,
            parameter.as_ptr() as isize,
            SMTO_ABORTIFHUNG,
            1_000,
            &mut result,
        )
    };
}

fn wide(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(std::iter::once(0)).collect()
}

fn wide_os(value: &std::ffi::OsStr) -> Vec<u16> {
    value.encode_wide().chain(std::iter::once(0)).collect()
}

fn check_status(status: i32, operation: &str) -> Result<(), String> {
    if status == ERROR_SUCCESS {
        return Ok(());
    }
    Err(format!(
        "Could not {operation}: {} (Windows error {status})",
        std::io::Error::from_raw_os_error(status)
    ))
}

fn check_write_status(status: i32, operation: &'static str) -> Result<(), RegistryWriteError> {
    if status == ERROR_SUCCESS {
        return Ok(());
    }
    Err(RegistryWriteError {
        operation,
        code: status,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn enforces_the_documented_minimum_build() {
        assert!(!supports_start_app_list_policy(26100, 7018, "Professional"));
        assert!(supports_start_app_list_policy(26100, 7019, "Professional"));
        assert!(supports_start_app_list_policy(
            26200,
            7019,
            "ProfessionalWorkstation"
        ));
        assert!(supports_start_app_list_policy(27000, 1, "Enterprise"));
    }

    #[test]
    fn rejects_unsupported_editions() {
        assert!(!supports_start_app_list_policy(26200, 9000, "Core"));
    }
}
