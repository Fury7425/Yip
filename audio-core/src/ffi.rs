//! C ABI surface. The only module allowed to expose `pub extern "C"`.
//!
//! All entry points must be panic-safe (`catch_unwind`), null-safe, and never
//! allocate on the audio path. Errors return a non-zero [`RecStatus`] and
//! attach a thread-local message readable via [`rec_last_error`].

#![allow(clippy::missing_safety_doc)]

use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char};
use std::panic::catch_unwind;
use std::ptr;
use std::sync::OnceLock;

use crate::capture::Recorder;
use crate::devices::{Device, list_devices};
use crate::error::YipError;

/// Integer status returned across FFI. 0 == success.
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecStatus {
    Ok = 0,
    InvalidArgument = 1,
    InvalidState = 2,
    DeviceNotFound = 3,
    Wasapi = 4,
    Io = 5,
    UnsupportedFormat = 6,
    Overrun = 7,
    Panic = 8,
    Unknown = 99,
}

impl From<&YipError> for RecStatus {
    fn from(e: &YipError) -> Self {
        match e {
            YipError::InvalidArgument(_) => Self::InvalidArgument,
            YipError::InvalidState(_) => Self::InvalidState,
            YipError::DeviceNotFound(_) => Self::DeviceNotFound,
            YipError::Wasapi(_) => Self::Wasapi,
            YipError::Io(_) => Self::Io,
            YipError::UnsupportedFormat(_) => Self::UnsupportedFormat,
            YipError::Overrun => Self::Overrun,
        }
    }
}

/// C-visible config struct passed to `rec_start`.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct RecConfig {
    pub sample_rate: u32,
    pub channels: u16,
    /// 0 = PCM float32 (only format in v1).
    pub format: u16,
}

impl Default for RecConfig {
    fn default() -> Self {
        Self {
            sample_rate: 48_000,
            channels: 2,
            format: 0,
        }
    }
}

/// C-visible device record. Strings are UTF-8, null-terminated, owned by the
/// returned array and freed via [`rec_free_devices`].
#[repr(C)]
pub struct DeviceInfo {
    pub id: *mut c_char,
    pub name: *mut c_char,
    /// 0 = render (loopback), 1 = capture.
    pub kind: u8,
    pub is_default: u8,
}

thread_local! {
    static LAST_ERROR: RefCell<Option<CString>> = const { RefCell::new(None) };
}

fn set_last_error(e: &YipError) {
    let msg = CString::new(e.to_string()).unwrap_or_else(|_| CString::new("error").unwrap());
    LAST_ERROR.with(|cell| *cell.borrow_mut() = Some(msg));
}

fn clear_last_error() {
    LAST_ERROR.with(|cell| *cell.borrow_mut() = None);
}

fn singleton() -> &'static parking_lot::Mutex<Option<Recorder>> {
    static REC: OnceLock<parking_lot::Mutex<Option<Recorder>>> = OnceLock::new();
    REC.get_or_init(|| parking_lot::Mutex::new(None))
}

fn run<F>(f: F) -> RecStatus
where
    F: FnOnce() -> Result<(), YipError> + std::panic::UnwindSafe,
{
    clear_last_error();
    match catch_unwind(f) {
        Ok(Ok(())) => RecStatus::Ok,
        Ok(Err(e)) => {
            let s = RecStatus::from(&e);
            set_last_error(&e);
            s
        }
        Err(_) => {
            let e = YipError::InvalidState("panic in ffi");
            set_last_error(&e);
            RecStatus::Panic
        }
    }
}

// ----------------------------------------------------------------------------
// M1 dummy entry point — kept after M2 so the smoke test in yip-app keeps
// linking. Returns 42.
// ----------------------------------------------------------------------------

/// Smoke-test FFI symbol. Returns 42. Removed once UI no longer references it.
#[unsafe(no_mangle)]
pub extern "C" fn rec_dummy() -> i32 {
    42
}

// ----------------------------------------------------------------------------
// Recording control
// ----------------------------------------------------------------------------

/// Start recording from `device_id` into `path` using `config`.
/// `device_id` and `path` are null-terminated UTF-8 strings.
///
/// # Safety
/// `device_id` and `path` must point to valid, null-terminated UTF-8.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rec_start(
    device_id: *const c_char,
    path: *const c_char,
    config: RecConfig,
) -> RecStatus {
    run(|| {
        if device_id.is_null() || path.is_null() {
            return Err(YipError::InvalidArgument("device_id or path was null"));
        }
        // SAFETY: caller contract: pointers are valid null-terminated UTF-8.
        let dev = unsafe { CStr::from_ptr(device_id) }
            .to_str()
            .map_err(|_| YipError::InvalidArgument("device_id not utf-8"))?;
        // SAFETY: same as above.
        let p = unsafe { CStr::from_ptr(path) }
            .to_str()
            .map_err(|_| YipError::InvalidArgument("path not utf-8"))?;

        let mut guard = singleton().lock();
        if guard.is_some() {
            return Err(YipError::InvalidState("already recording"));
        }
        let rec = Recorder::start(dev, std::path::Path::new(p), config)?;
        *guard = Some(rec);
        Ok(())
    })
}

/// Stop the active recording. Flushes writer + closes file. Idempotent.
#[unsafe(no_mangle)]
pub extern "C" fn rec_stop() -> RecStatus {
    run(|| {
        let mut guard = singleton().lock();
        if let Some(rec) = guard.take() {
            rec.stop()?;
        }
        Ok(())
    })
}

/// 0.0..=1.0 peak level since last call. Returns 0.0 if not recording.
/// Realtime-safe: single atomic load.
#[unsafe(no_mangle)]
pub extern "C" fn rec_peak_level() -> f32 {
    let guard = singleton().lock();
    guard.as_ref().map_or(0.0, Recorder::peak_level)
}

/// 1 if a recording is active, 0 otherwise.
#[unsafe(no_mangle)]
pub extern "C" fn rec_is_recording() -> u8 {
    let guard = singleton().lock();
    u8::from(guard.is_some())
}

/// Monotonic ms since `rec_start` succeeded. 0 if not recording.
#[unsafe(no_mangle)]
pub extern "C" fn rec_elapsed_ms() -> u64 {
    let guard = singleton().lock();
    guard.as_ref().map_or(0, Recorder::elapsed_ms)
}

/// Thread-local copy of the active recording path. Pointer valid until next
/// FFI call on this thread. Returns null when not recording.
#[unsafe(no_mangle)]
pub extern "C" fn rec_current_path() -> *const c_char {
    thread_local! {
        static PATH: RefCell<Option<CString>> = const { RefCell::new(None) };
    }
    let guard = singleton().lock();
    let Some(rec) = guard.as_ref() else {
        PATH.with(|cell| *cell.borrow_mut() = None);
        return ptr::null();
    };
    let s = rec.path().to_string_lossy().into_owned();
    PATH.with(|cell| {
        let owned = CString::new(s).unwrap_or_default();
        let ptr = owned.as_ptr();
        *cell.borrow_mut() = Some(owned);
        ptr
    })
}

/// Thread-local last error message. Pointer valid until next FFI call on this
/// thread. Returns null if no error.
#[unsafe(no_mangle)]
pub extern "C" fn rec_last_error() -> *const c_char {
    LAST_ERROR.with(|cell| cell.borrow().as_ref().map_or(ptr::null(), |s| s.as_ptr()))
}

// ----------------------------------------------------------------------------
// Device enumeration
// ----------------------------------------------------------------------------

/// Enumerate audio endpoints. Writes a heap-allocated array into `*out` and
/// the count into `*out_len`. Free via [`rec_free_devices`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rec_list_devices(
    out: *mut *mut DeviceInfo,
    out_len: *mut usize,
) -> RecStatus {
    run(|| {
        if out.is_null() || out_len.is_null() {
            return Err(YipError::InvalidArgument("out or out_len null"));
        }

        let devs = list_devices()?;
        let mut boxed: Vec<DeviceInfo> = devs.iter().map(device_to_info).collect();
        boxed.shrink_to_fit();

        let len = boxed.len();
        let ptr = boxed.as_mut_ptr();
        std::mem::forget(boxed);

        // SAFETY: caller guarantees out + out_len are valid writable pointers.
        unsafe {
            *out = ptr;
            *out_len = len;
        }
        Ok(())
    })
}

/// Free an array previously returned by [`rec_list_devices`].
///
/// # Safety
/// `ptr` must be a pointer previously returned by [`rec_list_devices`] with
/// the matching `len`. Calling with any other pointer is UB.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rec_free_devices(ptr: *mut DeviceInfo, len: usize) {
    if ptr.is_null() {
        return;
    }
    // SAFETY: caller contract — pointer and length match a prior `rec_list_devices`.
    let vec: Vec<DeviceInfo> =
        unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(ptr, len)) }.into();
    for info in vec {
        if !info.id.is_null() {
            // SAFETY: id was CString::into_raw'd.
            unsafe {
                let _ = CString::from_raw(info.id);
            }
        }
        if !info.name.is_null() {
            // SAFETY: name was CString::into_raw'd.
            unsafe {
                let _ = CString::from_raw(info.name);
            }
        }
    }
}

fn device_to_info(d: &Device) -> DeviceInfo {
    let id = CString::new(d.id.as_str()).unwrap_or_default().into_raw();
    let name = CString::new(d.name.as_str()).unwrap_or_default().into_raw();
    DeviceInfo {
        id,
        name,
        kind: u8::from(d.is_capture),
        is_default: u8::from(d.is_default),
    }
}
