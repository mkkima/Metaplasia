use std::fmt::{Display, Formatter};
use std::fs::OpenOptions;
use std::io::{Read, Write};
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::{Duration, Instant};

const FRAME_MAGIC: u32 = 0x504D_544D;
const PROTOCOL_VERSION: u16 = 9;
const HEADER_SIZE: usize = 16;
const MAX_PAYLOAD_SIZE: usize = 64 * 1024;
const ACKNOWLEDGEMENT: u8 = 0xA5;

const ERROR_FILE_NOT_FOUND: i32 = 2;
const ERROR_SEM_TIMEOUT: i32 = 121;
const ERROR_PIPE_BUSY: i32 = 231;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn GetCurrentProcessId() -> u32;
    fn ProcessIdToSessionId(process_id: u32, session_id: *mut u32) -> i32;
    fn WaitNamedPipeW(name: *const u16, timeout: u32) -> i32;
}

#[repr(u16)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum MessageKind {
    GetSnapshotRequest = 1,
    SetEnabledRequest = 2,
    GetSettingsRequest = 3,
    SetCustomizationRequest = 4,
    GetXamlDiagnosticsRequest = 5,
    PrepareUpdateRequest = 6,
    SnapshotResponse = 100,
    CommandResponse = 101,
    ErrorResponse = 102,
    SettingsResponse = 103,
    XamlDiagnosticsResponse = 104,
}

impl TryFrom<u16> for MessageKind {
    type Error = ProtocolError;

    fn try_from(value: u16) -> Result<Self, Self::Error> {
        match value {
            1 => Ok(Self::GetSnapshotRequest),
            2 => Ok(Self::SetEnabledRequest),
            3 => Ok(Self::GetSettingsRequest),
            4 => Ok(Self::SetCustomizationRequest),
            5 => Ok(Self::GetXamlDiagnosticsRequest),
            6 => Ok(Self::PrepareUpdateRequest),
            100 => Ok(Self::SnapshotResponse),
            101 => Ok(Self::CommandResponse),
            102 => Ok(Self::ErrorResponse),
            103 => Ok(Self::SettingsResponse),
            104 => Ok(Self::XamlDiagnosticsResponse),
            _ => Err(ProtocolError::Malformed("unknown message kind")),
        }
    }
}

#[derive(Debug)]
pub enum ProtocolError {
    Io(std::io::Error),
    Malformed(&'static str),
    Host {
        code: u16,
        native_code: u32,
        detail: String,
    },
    Timeout,
}

impl Display for ProtocolError {
    fn fmt(&self, formatter: &mut Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(error) => write!(formatter, "{error}"),
            Self::Malformed(detail) => write!(formatter, "Invalid host response: {detail}"),
            Self::Host {
                code,
                native_code,
                detail,
            } => {
                if *native_code == 0 {
                    write!(formatter, "Host error {code}: {detail}")
                } else {
                    write!(
                        formatter,
                        "Host error {code} (Windows {native_code}): {detail}"
                    )
                }
            }
            Self::Timeout => formatter.write_str("Metaplasia host did not respond in time"),
        }
    }
}

impl std::error::Error for ProtocolError {}

impl From<std::io::Error> for ProtocolError {
    fn from(value: std::io::Error) -> Self {
        Self::Io(value)
    }
}

#[derive(Debug)]
pub struct Frame {
    pub kind: MessageKind,
    pub payload: Vec<u8>,
}

pub struct PipeClient {
    pipe_name: String,
    next_request_id: AtomicU32,
}

impl PipeClient {
    pub fn for_current_session() -> Result<Self, ProtocolError> {
        let mut session_id = 0_u32;
        // SAFETY: both functions are pure process queries and session_id points to
        // valid writable memory for the duration of the call.
        let success = unsafe { ProcessIdToSessionId(GetCurrentProcessId(), &mut session_id) };
        if success == 0 {
            return Err(ProtocolError::Io(std::io::Error::last_os_error()));
        }
        Ok(Self {
            pipe_name: format!(r"\\.\pipe\Metaplasia.Host.v1.{session_id}"),
            next_request_id: AtomicU32::new(1),
        })
    }

    pub fn transact(
        &self,
        kind: MessageKind,
        payload: &[u8],
        timeout: Duration,
    ) -> Result<Frame, ProtocolError> {
        if payload.len() > MAX_PAYLOAD_SIZE {
            return Err(ProtocolError::Malformed("request payload is too large"));
        }
        let request_id = self.next_request_id.fetch_add(1, Ordering::Relaxed);
        let deadline = Instant::now() + timeout;
        let mut pipe = loop {
            let now = Instant::now();
            if now >= deadline {
                return Err(ProtocolError::Timeout);
            }
            let remaining = deadline.saturating_duration_since(now);
            let wait_ms = remaining.as_millis().clamp(1, u32::MAX as u128) as u32;
            let wide_name = wide_null(&self.pipe_name);
            // SAFETY: wide_name is a valid NUL-terminated UTF-16 string.
            let available = unsafe { WaitNamedPipeW(wide_name.as_ptr(), wait_ms) };
            if available != 0 {
                match OpenOptions::new()
                    .read(true)
                    .write(true)
                    .open(&self.pipe_name)
                {
                    Ok(file) => break file,
                    Err(error) if is_retryable_pipe_error(&error) => {
                        std::thread::sleep(Duration::from_millis(10));
                        continue;
                    }
                    Err(error) => return Err(error.into()),
                }
            }
            let error = std::io::Error::last_os_error();
            if !is_retryable_pipe_error(&error) {
                return Err(error.into());
            }
            std::thread::sleep(Duration::from_millis(10));
        };

        let mut header = Vec::with_capacity(HEADER_SIZE);
        write_u32(&mut header, FRAME_MAGIC);
        write_u16(&mut header, PROTOCOL_VERSION);
        write_u16(&mut header, kind as u16);
        write_u32(&mut header, request_id);
        write_u32(&mut header, payload.len() as u32);
        pipe.write_all(&header)?;
        pipe.write_all(payload)?;
        pipe.flush()?;

        let mut response_header = [0_u8; HEADER_SIZE];
        pipe.read_exact(&mut response_header)?;
        let mut reader = Reader::new(&response_header);
        if reader.u32()? != FRAME_MAGIC {
            return Err(ProtocolError::Malformed("frame magic"));
        }
        if reader.u16()? != PROTOCOL_VERSION {
            return Err(ProtocolError::Malformed("protocol version"));
        }
        let response_kind = MessageKind::try_from(reader.u16()?)?;
        if reader.u32()? != request_id {
            return Err(ProtocolError::Malformed("request id"));
        }
        let response_size = reader.u32()? as usize;
        if response_size > MAX_PAYLOAD_SIZE {
            return Err(ProtocolError::Malformed("response payload is too large"));
        }

        let mut response_payload = vec![0_u8; response_size];
        pipe.read_exact(&mut response_payload)?;
        let _ = pipe.write_all(&[ACKNOWLEDGEMENT]);

        if response_kind == MessageKind::ErrorResponse {
            return Err(parse_host_error(&response_payload)?);
        }
        Ok(Frame {
            kind: response_kind,
            payload: response_payload,
        })
    }
}

fn is_retryable_pipe_error(error: &std::io::Error) -> bool {
    matches!(
        error.raw_os_error(),
        Some(ERROR_FILE_NOT_FOUND | ERROR_SEM_TIMEOUT | ERROR_PIPE_BUSY)
    )
}

fn wide_null(value: &str) -> Vec<u16> {
    value.encode_utf16().chain(std::iter::once(0)).collect()
}

fn parse_host_error(payload: &[u8]) -> Result<ProtocolError, ProtocolError> {
    let mut reader = Reader::new(payload);
    let code = reader.u16()?;
    let native_code = reader.u32()?;
    let detail = reader.string()?;
    reader.finish()?;
    Ok(ProtocolError::Host {
        code,
        native_code,
        detail,
    })
}

pub fn expect_kind(frame: &Frame, expected: MessageKind) -> Result<(), ProtocolError> {
    if frame.kind != expected {
        return Err(ProtocolError::Malformed("unexpected message kind"));
    }
    Ok(())
}

pub fn write_u16(output: &mut Vec<u8>, value: u16) {
    output.extend_from_slice(&value.to_le_bytes());
}

pub fn write_u32(output: &mut Vec<u8>, value: u32) {
    output.extend_from_slice(&value.to_le_bytes());
}

pub fn write_string(output: &mut Vec<u8>, value: &str) -> Result<(), ProtocolError> {
    let length =
        u16::try_from(value.len()).map_err(|_| ProtocolError::Malformed("string is too large"))?;
    write_u16(output, length);
    output.extend_from_slice(value.as_bytes());
    Ok(())
}

pub struct Reader<'a> {
    bytes: &'a [u8],
    offset: usize,
}

impl<'a> Reader<'a> {
    pub fn new(bytes: &'a [u8]) -> Self {
        Self { bytes, offset: 0 }
    }

    pub fn u8(&mut self) -> Result<u8, ProtocolError> {
        Ok(self.take(1)?[0])
    }

    pub fn u16(&mut self) -> Result<u16, ProtocolError> {
        let bytes: [u8; 2] = self.take(2)?.try_into().expect("fixed-size slice");
        Ok(u16::from_le_bytes(bytes))
    }

    pub fn u32(&mut self) -> Result<u32, ProtocolError> {
        let bytes: [u8; 4] = self.take(4)?.try_into().expect("fixed-size slice");
        Ok(u32::from_le_bytes(bytes))
    }

    pub fn u64(&mut self) -> Result<u64, ProtocolError> {
        let bytes: [u8; 8] = self.take(8)?.try_into().expect("fixed-size slice");
        Ok(u64::from_le_bytes(bytes))
    }

    pub fn string(&mut self) -> Result<String, ProtocolError> {
        let length = self.u16()? as usize;
        let value = self.take(length)?;
        String::from_utf8(value.to_vec())
            .map_err(|_| ProtocolError::Malformed("invalid UTF-8 string"))
    }

    pub fn finish(&self) -> Result<(), ProtocolError> {
        if self.offset == self.bytes.len() {
            Ok(())
        } else {
            Err(ProtocolError::Malformed("trailing payload bytes"))
        }
    }

    fn take(&mut self, length: usize) -> Result<&'a [u8], ProtocolError> {
        let end = self
            .offset
            .checked_add(length)
            .ok_or(ProtocolError::Malformed("payload size overflow"))?;
        let value = self
            .bytes
            .get(self.offset..end)
            .ok_or(ProtocolError::Malformed("truncated payload"))?;
        self.offset = end;
        Ok(value)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reader_rejects_truncated_string() {
        let mut reader = Reader::new(&[5, 0, b'a']);
        assert!(matches!(reader.string(), Err(ProtocolError::Malformed(_))));
    }

    #[test]
    fn utf8_string_round_trips() {
        let mut encoded = Vec::new();
        write_string(&mut encoded, "Meta · ").unwrap();
        let mut reader = Reader::new(&encoded);
        assert_eq!(reader.string().unwrap(), "Meta · ");
        reader.finish().unwrap();
    }

    #[test]
    fn portable_update_shutdown_message_is_versioned() {
        assert_eq!(PROTOCOL_VERSION, 9);
        assert_eq!(
            MessageKind::try_from(6).unwrap(),
            MessageKind::PrepareUpdateRequest
        );
    }
}
