use semver::Version;
use serde::{Deserialize, Serialize};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const HISTORY_SCHEMA: u32 = 1;
const POLICY_SCHEMA: u32 = 1;
const MAX_HISTORY_BYTES: u64 = 256 * 1024;
const MAX_POLICY_BYTES: u64 = 16 * 1024;
const MAX_HISTORY_ENTRIES: usize = 50;
const MAX_DETAIL_CHARS: usize = 1024;

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct UpdateHistoryEntry {
    pub version: String,
    pub previous_version: Option<String>,
    pub channel: String,
    pub action: String,
    pub outcome: String,
    pub timestamp_unix: u64,
    pub detail: String,
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct HistoryDocument {
    schema: u32,
    entries: Vec<UpdateHistoryEntry>,
}

impl Default for HistoryDocument {
    fn default() -> Self {
        Self {
            schema: HISTORY_SCHEMA,
            entries: Vec::new(),
        }
    }
}

#[derive(Debug, Default, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct UpdatePolicy {
    schema: u32,
    stable_skipped_version: Option<String>,
    development_skipped_version: Option<String>,
}

pub fn record(
    root: &Path,
    previous_version: &str,
    version: &str,
    channel: &str,
    action: &str,
    outcome: &str,
    detail: &str,
) -> Result<(), String> {
    validate_version(previous_version)?;
    validate_version(version)?;
    validate_channel(channel)?;
    validate_action(action)?;
    validate_outcome(outcome)?;
    let mut document = read_history(root)?;
    append_entry(
        &mut document,
        UpdateHistoryEntry {
            version: version.into(),
            previous_version: Some(previous_version.into()),
            channel: channel.into(),
            action: action.into(),
            outcome: outcome.into(),
            timestamp_unix: unix_timestamp()?,
            detail: bounded_detail(detail),
        },
    );
    write_history(root, &document)
}

pub fn skipped_version(root: &Path, channel: &str) -> Result<Option<String>, String> {
    validate_channel(channel)?;
    let policy = read_policy(root)?;
    let skipped = match channel {
        "stable" => policy.stable_skipped_version,
        "development" => policy.development_skipped_version,
        _ => unreachable!("channel was validated above"),
    };
    match skipped {
        Some(value) => {
            validate_version(&value)?;
            Ok(Some(value))
        }
        None => Ok(None),
    }
}

pub fn set_skipped_version(
    root: &Path,
    channel: &str,
    version: Option<&str>,
) -> Result<(), String> {
    validate_channel(channel)?;
    if let Some(value) = version {
        validate_version(value)?;
    }
    let mut policy = read_policy(root)?;
    match channel {
        "stable" => policy.stable_skipped_version = version.map(str::to_owned),
        "development" => policy.development_skipped_version = version.map(str::to_owned),
        _ => unreachable!("channel was validated above"),
    }
    let bytes = serde_json::to_vec(&policy)
        .map_err(|error| format!("Could not encode the portable update policy: {error}"))?;
    write_atomic(&policy_path(root), &bytes, "portable update policy")
}

fn append_entry(document: &mut HistoryDocument, entry: UpdateHistoryEntry) {
    document.entries.push(entry);
    if document.entries.len() > MAX_HISTORY_ENTRIES {
        let remove = document.entries.len() - MAX_HISTORY_ENTRIES;
        document.entries.drain(..remove);
    }
}

fn read_history(root: &Path) -> Result<HistoryDocument, String> {
    let path = history_path(root);
    recover_interrupted_write(&path, "portable update history")?;
    if !path.exists() {
        return Ok(HistoryDocument::default());
    }
    let bytes = read_bounded(&path, MAX_HISTORY_BYTES, "portable update history")?;
    let document: HistoryDocument = serde_json::from_slice(&bytes)
        .map_err(|error| format!("The portable update history is invalid: {error}"))?;
    validate_history(&document)?;
    Ok(document)
}

fn validate_history(document: &HistoryDocument) -> Result<(), String> {
    if document.schema != HISTORY_SCHEMA || document.entries.len() > MAX_HISTORY_ENTRIES {
        return Err("The portable update history uses an unsupported format".into());
    }
    for entry in &document.entries {
        validate_version(&entry.version)?;
        if let Some(previous) = &entry.previous_version {
            validate_version(previous)?;
        }
        validate_channel(&entry.channel)?;
        validate_action(&entry.action)?;
        validate_outcome(&entry.outcome)?;
        if entry.timestamp_unix == 0 || entry.detail.chars().count() > MAX_DETAIL_CHARS {
            return Err("The portable update history contains an invalid entry".into());
        }
    }
    Ok(())
}

fn write_history(root: &Path, document: &HistoryDocument) -> Result<(), String> {
    let bytes = serde_json::to_vec(document)
        .map_err(|error| format!("Could not encode the portable update history: {error}"))?;
    if bytes.len() as u64 > MAX_HISTORY_BYTES {
        return Err("The portable update history exceeded its size limit".into());
    }
    write_atomic(&history_path(root), &bytes, "portable update history")
}

fn read_policy(root: &Path) -> Result<UpdatePolicy, String> {
    let path = policy_path(root);
    recover_interrupted_write(&path, "portable update policy")?;
    if !path.exists() {
        return Ok(UpdatePolicy {
            schema: POLICY_SCHEMA,
            stable_skipped_version: None,
            development_skipped_version: None,
        });
    }
    let bytes = read_bounded(&path, MAX_POLICY_BYTES, "portable update policy")?;
    let policy: UpdatePolicy = serde_json::from_slice(&bytes)
        .map_err(|error| format!("The portable update policy is invalid: {error}"))?;
    if policy.schema != POLICY_SCHEMA {
        return Err("The portable update policy uses an unsupported format".into());
    }
    if let Some(version) = policy.stable_skipped_version.as_deref() {
        validate_version(version)?;
    }
    if let Some(version) = policy.development_skipped_version.as_deref() {
        validate_version(version)?;
    }
    Ok(policy)
}

fn read_bounded(path: &Path, maximum: u64, label: &str) -> Result<Vec<u8>, String> {
    let metadata = path
        .metadata()
        .map_err(|error| format!("Could not inspect the {label}: {error}"))?;
    if !metadata.is_file() || metadata.len() > maximum {
        return Err(format!("The {label} is not a valid bounded file"));
    }
    let mut file =
        File::open(path).map_err(|error| format!("Could not open the {label}: {error}"))?;
    let mut bytes = Vec::with_capacity(metadata.len() as usize);
    file.read_to_end(&mut bytes)
        .map_err(|error| format!("Could not read the {label}: {error}"))?;
    Ok(bytes)
}

fn write_atomic(path: &Path, bytes: &[u8], label: &str) -> Result<(), String> {
    let temporary = temporary_path(path)?;
    let backup = backup_path(path)?;
    remove_file_if_present(&temporary)?;
    let mut file = OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&temporary)
        .map_err(|error| format!("Could not create the {label}: {error}"))?;
    file.write_all(bytes)
        .map_err(|error| format!("Could not store the {label}: {error}"))?;
    file.sync_all()
        .map_err(|error| format!("Could not flush the {label}: {error}"))?;
    drop(file);

    let replace_existing = path.exists();
    if replace_existing {
        remove_file_if_present(&backup)?;
        fs::rename(path, &backup)
            .map_err(|error| format!("Could not preserve the previous {label}: {error}"))?;
    }
    if let Err(error) = fs::rename(&temporary, path) {
        let _ = fs::remove_file(&temporary);
        if replace_existing {
            let _ = fs::rename(&backup, path);
        }
        return Err(format!("Could not finalize the {label}: {error}"));
    }
    if replace_existing {
        let _ = remove_file_if_present(&backup);
    }
    Ok(())
}

fn temporary_path(path: &Path) -> Result<PathBuf, String> {
    let name = path
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or("The portable update metadata filename is invalid")?;
    Ok(path.with_file_name(format!("{name}.tmp")))
}

fn backup_path(path: &Path) -> Result<PathBuf, String> {
    let name = path
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or("The portable update metadata filename is invalid")?;
    Ok(path.with_file_name(format!("{name}.bak")))
}

fn recover_interrupted_write(path: &Path, label: &str) -> Result<(), String> {
    if path.exists() {
        return Ok(());
    }
    let backup = backup_path(path)?;
    if !backup.exists() {
        return Ok(());
    }
    fs::rename(&backup, path)
        .map_err(|error| format!("Could not recover the previous {label}: {error}"))
}

fn history_path(root: &Path) -> PathBuf {
    root.join("update-history.json")
}

fn policy_path(root: &Path) -> PathBuf {
    root.join("update-policy.json")
}

fn remove_file_if_present(path: &Path) -> Result<(), String> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(error) => Err(format!("Could not remove {}: {error}", path.display())),
    }
}

fn unix_timestamp() -> Result<u64, String> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .map_err(|_| "The system clock is earlier than the Unix epoch".into())
}

fn bounded_detail(value: &str) -> String {
    value
        .chars()
        .map(|character| {
            if character.is_control() && !matches!(character, '\n' | '\r' | '\t') {
                '\u{fffd}'
            } else {
                character
            }
        })
        .take(MAX_DETAIL_CHARS)
        .collect()
}

fn validate_version(value: &str) -> Result<(), String> {
    let version = Version::parse(value)
        .map_err(|error| format!("The portable update history version is invalid: {error}"))?;
    if !version.pre.is_empty() || !version.build.is_empty() || version.to_string() != value {
        return Err("Portable update history requires canonical stable SemVer versions".into());
    }
    Ok(())
}

fn validate_channel(value: &str) -> Result<(), String> {
    if matches!(value, "stable" | "development") {
        Ok(())
    } else {
        Err("The portable update history channel is invalid".into())
    }
}

fn validate_action(value: &str) -> Result<(), String> {
    if matches!(value, "detected" | "update" | "rollback") {
        Ok(())
    } else {
        Err("The portable update history action is invalid".into())
    }
}

fn validate_outcome(value: &str) -> Result<(), String> {
    if matches!(value, "success" | "failed") {
        Ok(())
    } else {
        Err("The portable update history outcome is invalid".into())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temporary_root() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let root = std::env::temp_dir().join(format!(
            "metaplasia-history-test-{}-{}",
            std::process::id(),
            nonce
        ));
        fs::create_dir_all(&root).unwrap();
        root
    }

    fn cleanup(root: &Path) {
        for name in [
            "update-history.json",
            "update-history.json.tmp",
            "update-history.json.bak",
            "update-policy.json",
            "update-policy.json.tmp",
            "update-policy.json.bak",
        ] {
            let _ = fs::remove_file(root.join(name));
        }
        let _ = fs::remove_dir(root);
    }

    #[test]
    fn records_version_transitions() {
        let root = temporary_root();
        record(
            &root,
            "0.1.1",
            "0.1.2",
            "development",
            "update",
            "success",
            "Installed.",
        )
        .unwrap();
        let document = read_history(&root).unwrap();
        assert_eq!(document.entries.len(), 1);
        assert_eq!(document.entries[0].version, "0.1.2");
        assert_eq!(
            document.entries[0].previous_version.as_deref(),
            Some("0.1.1")
        );
        cleanup(&root);
    }

    #[test]
    fn persists_and_clears_skipped_version() {
        let root = temporary_root();
        set_skipped_version(&root, "development", Some("0.1.2")).unwrap();
        assert_eq!(
            skipped_version(&root, "development").unwrap().as_deref(),
            Some("0.1.2")
        );
        assert_eq!(skipped_version(&root, "stable").unwrap(), None);
        set_skipped_version(&root, "stable", Some("1.0.0")).unwrap();
        assert_eq!(
            skipped_version(&root, "development").unwrap().as_deref(),
            Some("0.1.2")
        );
        set_skipped_version(&root, "development", None).unwrap();
        assert_eq!(skipped_version(&root, "development").unwrap(), None);
        assert_eq!(
            skipped_version(&root, "stable").unwrap().as_deref(),
            Some("1.0.0")
        );
        cleanup(&root);
    }

    #[test]
    fn rejects_malformed_history_without_overwriting_it() {
        let root = temporary_root();
        fs::write(history_path(&root), b"not-json").unwrap();
        assert!(
            record(
                &root,
                "0.1.0",
                "0.1.1",
                "development",
                "update",
                "success",
                "Installed.",
            )
            .is_err()
        );
        assert_eq!(fs::read(history_path(&root)).unwrap(), b"not-json");
        cleanup(&root);
    }

    #[test]
    fn recovers_history_after_an_interrupted_replacement() {
        let root = temporary_root();
        record(
            &root,
            "0.1.0",
            "0.1.1",
            "development",
            "update",
            "success",
            "Installed.",
        )
        .unwrap();
        fs::rename(
            history_path(&root),
            backup_path(&history_path(&root)).unwrap(),
        )
        .unwrap();

        let document = read_history(&root).unwrap();
        assert_eq!(document.entries.len(), 1);
        assert!(history_path(&root).is_file());
        assert!(!backup_path(&history_path(&root)).unwrap().exists());
        cleanup(&root);
    }
}
