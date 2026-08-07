use serde::{Deserialize, Serialize};
use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

const LOG_SCHEMA_VERSION: u32 = 1;
const MAXIMUM_LOG_FILES: usize = 4;
const MAXIMUM_FILE_READ_BYTES: u64 = 1024 * 1024 + 16 * 1024;
const MAXIMUM_RETURNED_ENTRIES: usize = 500;
const MAXIMUM_FIELD_BYTES: usize = 128;
const MAXIMUM_MESSAGE_BYTES: usize = 4200;

#[derive(Clone, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DiagnosticLogEntry {
    schema: u32,
    timestamp: String,
    level: String,
    component: String,
    event: String,
    target: String,
    process_id: u32,
    message: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DiagnosticLogBatch {
    entries: Vec<DiagnosticLogEntry>,
    directory: String,
    invalid_line_count: u32,
    unreadable_file_count: u32,
    truncated: bool,
}

#[tauri::command]
pub async fn get_diagnostic_logs() -> Result<DiagnosticLogBatch, String> {
    tauri::async_runtime::spawn_blocking(read_diagnostic_logs)
        .await
        .map_err(|error| format!("Diagnostics log worker failed: {error}"))?
}

fn read_diagnostic_logs() -> Result<DiagnosticLogBatch, String> {
    let directory = diagnostic_log_directory()?;
    read_diagnostic_logs_from(&directory)
}

fn diagnostic_log_directory() -> Result<PathBuf, String> {
    dirs::data_local_dir()
        .map(|directory| directory.join("Metaplasia").join("logs"))
        .ok_or_else(|| "Windows LocalAppData directory is unavailable".into())
}

fn read_diagnostic_logs_from(directory: &Path) -> Result<DiagnosticLogBatch, String> {
    let mut entries = Vec::new();
    let mut invalid_line_count = 0_u32;
    let mut unreadable_file_count = 0_u32;
    let mut truncated = false;
    for path in ordered_log_paths(directory) {
        let metadata = match path.metadata() {
            Ok(metadata) if metadata.is_file() => metadata,
            Ok(_) => continue,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => continue,
            Err(_) => {
                unreadable_file_count = unreadable_file_count.saturating_add(1);
                continue;
            }
        };
        let file = match File::open(&path) {
            Ok(file) => file,
            Err(_) => {
                unreadable_file_count = unreadable_file_count.saturating_add(1);
                continue;
            }
        };
        if metadata.len() > MAXIMUM_FILE_READ_BYTES {
            truncated = true;
        }
        let bytes = match read_file_tail(file, metadata.len()) {
            Ok(bytes) => bytes,
            Err(_) => {
                unreadable_file_count = unreadable_file_count.saturating_add(1);
                continue;
            }
        };
        let text = String::from_utf8_lossy(&bytes);
        for line in text.lines() {
            if line.is_empty() {
                continue;
            }
            match parse_diagnostic_log_line(line) {
                Some(entry) => entries.push(entry),
                None => invalid_line_count = invalid_line_count.saturating_add(1),
            }
        }
    }

    if entries.len() > MAXIMUM_RETURNED_ENTRIES {
        const KEEP: usize = MAXIMUM_RETURNED_ENTRIES;
        entries.drain(0..entries.len() - KEEP);
        truncated = true;
    }

    Ok(DiagnosticLogBatch {
        entries,
        directory: directory.to_string_lossy().into_owned(),
        invalid_line_count,
        unreadable_file_count,
        truncated,
    })
}

fn read_file_tail(mut file: File, file_length: u64) -> std::io::Result<Vec<u8>> {
    let offset = file_length.saturating_sub(MAXIMUM_FILE_READ_BYTES);
    if offset > 0 {
        file.seek(SeekFrom::Start(offset))?;
    }
    let mut bytes = Vec::new();
    file.take(MAXIMUM_FILE_READ_BYTES).read_to_end(&mut bytes)?;
    if offset > 0 {
        // The bounded tail normally starts in the middle of a JSON line.
        // Discard that fragment so it is not reported as malformed data.
        if let Some(newline) = bytes.iter().position(|byte| *byte == b'\n') {
            bytes.drain(..=newline);
        } else {
            bytes.clear();
        }
    }
    Ok(bytes)
}

fn ordered_log_paths(directory: &Path) -> [PathBuf; MAXIMUM_LOG_FILES] {
    [
        directory.join("host.log.3"),
        directory.join("host.log.2"),
        directory.join("host.log.1"),
        directory.join("host.log"),
    ]
}

fn parse_diagnostic_log_line(line: &str) -> Option<DiagnosticLogEntry> {
    if line.len() > MAXIMUM_MESSAGE_BYTES + 1024 {
        return None;
    }
    let entry: DiagnosticLogEntry = serde_json::from_str(line).ok()?;
    if entry.schema != LOG_SCHEMA_VERSION
        || !matches!(entry.level.as_str(), "info" | "warning" | "error")
        || !valid_field(&entry.timestamp, 40)
        || !entry.timestamp.ends_with('Z')
        || !valid_field(&entry.component, MAXIMUM_FIELD_BYTES)
        || !valid_field(&entry.event, MAXIMUM_FIELD_BYTES)
        || !valid_field(&entry.target, MAXIMUM_FIELD_BYTES)
        || entry.message.len() > MAXIMUM_MESSAGE_BYTES
    {
        return None;
    }
    Some(entry)
}

fn valid_field(value: &str, maximum_bytes: usize) -> bool {
    !value.is_empty() && value.len() <= maximum_bytes && !value.chars().any(char::is_control)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_a_valid_structured_log_entry() {
        let entry = parse_diagnostic_log_line(
            r#"{"schema":1,"timestamp":"2026-08-07T12:34:56.789Z","level":"error","component":"engine","event":"configure-failed","target":"taskbar","processId":42,"message":"native=0x80004005"}"#,
        )
        .expect("entry should be valid");
        assert_eq!(entry.level, "error");
        assert_eq!(entry.process_id, 42);
        assert_eq!(entry.target, "taskbar");
    }

    #[test]
    fn rejects_unknown_schema_and_oversized_fields() {
        assert!(
            parse_diagnostic_log_line(
                r#"{"schema":2,"timestamp":"2026-08-07T12:34:56.789Z","level":"info","component":"host","event":"startup","target":"host","processId":1,"message":"x"}"#,
            )
            .is_none()
        );
        let component = "x".repeat(MAXIMUM_FIELD_BYTES + 1);
        let line = format!(
            r#"{{"schema":1,"timestamp":"2026-08-07T12:34:56.789Z","level":"info","component":"{component}","event":"startup","target":"host","processId":1,"message":"x"}}"#
        );
        assert!(parse_diagnostic_log_line(&line).is_none());
    }

    #[test]
    fn oversized_log_reads_the_newest_complete_entries() {
        let directory = std::env::temp_dir().join(format!(
            "MetaplasiaDiagnostics-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("system time should be after the Unix epoch")
                .as_nanos()
        ));
        std::fs::create_dir_all(&directory).expect("create test log directory");
        let newest = r#"{"schema":1,"timestamp":"2026-08-07T12:34:56.789Z","level":"info","component":"host","event":"newest","target":"host","processId":1,"message":"newest complete entry"}"#;
        let mut contents = "x".repeat(MAXIMUM_FILE_READ_BYTES as usize + 128);
        contents.push('\n');
        contents.push_str(newest);
        contents.push('\n');
        std::fs::write(directory.join("host.log"), contents).expect("write oversized test log");

        let batch = read_diagnostic_logs_from(&directory).expect("read bounded log tail");
        assert!(batch.truncated);
        assert_eq!(batch.invalid_line_count, 0);
        assert_eq!(batch.unreadable_file_count, 0);
        assert_eq!(batch.entries.len(), 1);
        assert_eq!(batch.entries[0].event, "newest");
        assert_eq!(batch.entries[0].message, "newest complete entry");

        std::fs::remove_dir_all(directory).expect("remove test log directory");
    }
}
