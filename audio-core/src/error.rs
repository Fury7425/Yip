use std::fmt;

/// Public error type for the audio-core crate.
/// Maps 1:1 to integer codes in the FFI layer (see `ffi::RecStatus`).
#[derive(Debug, Clone)]
pub enum YipError {
    /// Recorder is not running but a stop was requested, or vice-versa.
    InvalidState(&'static str),
    /// Device id not present in current enumeration.
    DeviceNotFound(String),
    /// COM/WASAPI initialization or capture failure.
    Wasapi(String),
    /// Filesystem / WAV write error.
    Io(String),
    /// Unsupported wave format reported by device.
    UnsupportedFormat(String),
    /// Ring buffer overrun — writer thread couldn't keep up.
    Overrun,
    /// FFI input pointer was null or invalid UTF-8.
    InvalidArgument(&'static str),
}

impl fmt::Display for YipError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidState(s) => write!(f, "invalid state: {s}"),
            Self::DeviceNotFound(d) => write!(f, "device not found: {d}"),
            Self::Wasapi(s) => write!(f, "wasapi: {s}"),
            Self::Io(s) => write!(f, "io: {s}"),
            Self::UnsupportedFormat(s) => write!(f, "unsupported format: {s}"),
            Self::Overrun => write!(f, "ring overrun"),
            Self::InvalidArgument(s) => write!(f, "invalid argument: {s}"),
        }
    }
}

impl std::error::Error for YipError {}

impl From<std::io::Error> for YipError {
    fn from(e: std::io::Error) -> Self {
        Self::Io(e.to_string())
    }
}

impl From<hound::Error> for YipError {
    fn from(e: hound::Error) -> Self {
        Self::Io(e.to_string())
    }
}

impl From<windows::core::Error> for YipError {
    fn from(e: windows::core::Error) -> Self {
        Self::Wasapi(format!("HRESULT 0x{:08X}: {}", e.code().0, e.message()))
    }
}
