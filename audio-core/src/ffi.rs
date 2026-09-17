//! C ABI surface. The only module allowed to expose `pub extern "C"`.
//!
//! All entry points must be panic-safe (`catch_unwind`), null-safe, and never
//! allocate on the audio path. Errors return a non-zero [`RecStatus`] and
//! attach a thread-local message readable via [`rec_last_error`].

#![allow(clippy::missing_safety_doc)]

use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char, c_void};
use std::panic::catch_unwind;
use std::ptr;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

use crate::capture::Recorder;
use crate::devices::{Device, list_devices};
use crate::error::YipError;
use crate::ring::METER;

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
    Encoder = 9,
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
            YipError::Encoder(_) => Self::Encoder,
        }
    }
}

/// Container / codec for [`RecConfig::format`]. Carried across the ABI as a
/// plain `u16` so an out-of-range value from C is a validation error, not UB.
#[repr(u16)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RecFormat {
    /// RIFF/WAVE. `bit_depth` 16 or 24 (integer PCM) or 32 (float).
    Wav = 0,
    /// Lossless. `bit_depth` 16 or 24.
    Flac = 1,
    /// MPEG-1 Layer III. `bitrate_kbps`.
    Mp3 = 2,
    /// AAC in an MPEG-4 container. `bitrate_kbps`.
    M4a = 3,
}

/// C-visible config struct passed to `rec_start`.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct RecConfig {
    /// 0 = follow the device. MP3 and M4A only encode 44.1 or 48 kHz, so any
    /// other rate records at 48 kHz for them.
    pub sample_rate: u32,
    /// 0 = follow the device. Encoded formats cap this at stereo.
    pub channels: u16,
    /// A [`RecFormat`].
    pub format: u16,
    /// WAV and FLAC: 16, 24, or (WAV only) 32 for float. 0 = the format's
    /// default (32-bit float WAV, 24-bit FLAC). Ignored by lossy formats.
    pub bit_depth: u16,
    /// MP3 and M4A: kbps. 0 = 192. Ignored by lossless formats.
    pub bitrate_kbps: u16,
}

impl Default for RecConfig {
    fn default() -> Self {
        Self {
            sample_rate: 48_000,
            channels: 2,
            format: RecFormat::Wav as u16,
            bit_depth: 0,
            bitrate_kbps: 0,
        }
    }
}

/// One-call snapshot of capture health, filled by [`rec_meter`].
///
/// Every field is read from an atomic, so a UI tick costs one FFI call and no
/// lock at all — the old shape needed four calls, each taking the recorder
/// mutex. Reads are non-destructive: two windows polling at different rates
/// see the same levels instead of stealing peaks from each other.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct RecMeter {
    /// Decaying peak envelope. Float capture can exceed 1.0.
    pub peak: f32,
    /// Smoothed RMS over roughly the last 150 ms.
    pub rms: f32,
    /// Largest peak since the session started, or since [`rec_reset_clip`].
    pub session_peak: f32,
    /// Samples at or beyond full scale this session.
    pub clip_count: u32,
    /// Times the ring ran out of room.
    pub overrun_count: u32,
    /// 1 while capture is live. Stays 1 across a pause: a held take is still a
    /// take, and the indicator has to stay on screen.
    pub recording: u8,
    /// 1 once the first packet has arrived.
    pub started: u8,
    /// 1 while the session is paused. `elapsed_ms` is frozen and the levels
    /// read silence.
    pub paused: u8,
    /// Reserved; keeps the 8-byte fields below naturally aligned.
    pub reserved: [u8; 1],
    /// Frames lost to overruns.
    pub dropped_frames: u64,
    /// Frames handed to the writer.
    pub frames_captured: u64,
    /// Monotonic ms since `rec_start` succeeded. 0 when not recording.
    pub elapsed_ms: u64,
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

/// Live-session flag mirrored outside the recorder mutex, so the UI's poll
/// path never contends with `rec_start` / `rec_stop`.
static RECORDING: AtomicBool = AtomicBool::new(false);

/// `process_base()`-relative ms at which the current session started. Pushed
/// forward by `rec_resume` so paused spans fall out of the elapsed clock.
static SESSION_START_MS: AtomicU64 = AtomicU64::new(0);

/// 1 while the live session is held. Deliberately separate from [`RECORDING`]:
/// a paused take has not ended, so the state callback never fires for it.
static PAUSED: AtomicBool = AtomicBool::new(false);

/// `process_base()`-relative ms at which the current pause began. Only
/// meaningful while `PAUSED` is true.
static PAUSE_BEGAN_MS: AtomicU64 = AtomicU64::new(0);

/// Fixed monotonic origin for the whole process. `Instant` cannot live in an
/// atomic, so elapsed time is tracked as a ms offset from this.
fn process_base() -> std::time::Instant {
    static BASE: OnceLock<std::time::Instant> = OnceLock::new();
    *BASE.get_or_init(std::time::Instant::now)
}

fn now_ms() -> u64 {
    u64::try_from(process_base().elapsed().as_millis()).unwrap_or(u64::MAX)
}

/// Ms of *captured* audio in the current session: wall clock since the start,
/// less every span spent paused.
///
/// Paused time is taken out by moving `SESSION_START_MS` forward on resume
/// rather than by keeping a running total, so a reader needs two loads and no
/// lock. `PAUSED` is loaded first, and `rec_resume` releases it after the
/// adjustment, so nobody sees the cleared flag against the old origin.
fn session_elapsed_ms() -> u64 {
    if !RECORDING.load(Ordering::Acquire) {
        return 0;
    }
    let paused = PAUSED.load(Ordering::Acquire);
    let start = SESSION_START_MS.load(Ordering::Relaxed);
    if paused {
        // Frozen where the pause began.
        return PAUSE_BEGAN_MS.load(Ordering::Relaxed).saturating_sub(start);
    }
    now_ms().saturating_sub(start)
}

fn singleton() -> &'static parking_lot::Mutex<Option<Recorder>> {
    static REC: OnceLock<parking_lot::Mutex<Option<Recorder>>> = OnceLock::new();
    REC.get_or_init(|| parking_lot::Mutex::new(None))
}

/// Callback invoked whenever the recording state flips. `recording` is 1 while
/// a capture session is live, 0 otherwise. Delivered on the thread that called
/// [`rec_start`] / [`rec_stop`], **after** the internal lock is released, so the
/// callback may re-enter any `rec_*` getter.
pub type RecStateCallback = Option<extern "C" fn(recording: u8, user: *mut c_void)>;

/// Callback slot plus its opaque user pointer.
struct StateSink {
    cb: RecStateCallback,
    user: *mut c_void,
}

// SAFETY: `user` is an opaque token the host owns; audio-core only hands it
// back verbatim. The slot itself is guarded by a mutex, so concurrent access
// is serialised.
unsafe impl Send for StateSink {}

fn state_sink() -> &'static parking_lot::Mutex<StateSink> {
    static SINK: OnceLock<parking_lot::Mutex<StateSink>> = OnceLock::new();
    SINK.get_or_init(|| {
        parking_lot::Mutex::new(StateSink {
            cb: None,
            user: ptr::null_mut(),
        })
    })
}

/// Last state handed to the host. Lets `notify_state` stay edge-triggered so a
/// redundant `rec_stop` does not spam the UI.
static LAST_NOTIFIED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);

/// Copy the slot out, drop the guard, then call. Never invoke a host callback
/// while holding a lock the host might re-enter.
fn notify_state(recording: bool) {
    if LAST_NOTIFIED.swap(recording, std::sync::atomic::Ordering::AcqRel) == recording {
        return;
    }
    let (cb, user) = {
        let guard = state_sink().lock();
        (guard.cb, guard.user)
    };
    if let Some(f) = cb {
        f(u8::from(recording), user);
    }
}

/// Install (or clear, with a null `cb`) the recording-state callback.
///
/// Replaces any previous callback. The host must clear it before the callback
/// target is destroyed. Calling this does not fire the callback — read the
/// current state with [`rec_is_recording`].
///
/// # Safety
/// `user` is stored and handed back verbatim; it must stay valid until the
/// callback is cleared or replaced.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rec_set_state_callback(cb: RecStateCallback, user: *mut c_void) {
    let mut guard = state_sink().lock();
    guard.cb = cb;
    guard.user = user;
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
    let status = run(|| {
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
        // Clear the meter before the capture thread can push its first packet.
        METER.reset();
        let rec = Recorder::start(dev, std::path::Path::new(p), config)?;
        SESSION_START_MS.store(now_ms(), Ordering::Relaxed);
        PAUSED.store(false, Ordering::Release);
        RECORDING.store(true, Ordering::Release);
        *guard = Some(rec);
        Ok(())
    });
    if status == RecStatus::Ok {
        notify_state(true);
    }
    status
}

/// Stop the active recording. Flushes writer + closes file. Idempotent.
#[unsafe(no_mangle)]
pub extern "C" fn rec_stop() -> RecStatus {
    let status = run(|| {
        let mut guard = singleton().lock();
        if let Some(rec) = guard.take() {
            rec.stop()?;
        }
        Ok(())
    });
    // The session is gone either way — a failed flush still ends capture. The
    // session totals stay readable; only the live levels drop to zero.
    RECORDING.store(false, Ordering::Release);
    PAUSED.store(false, Ordering::Release);
    METER.silence();
    notify_state(false);
    status
}

/// Hold the active recording without closing the file. Idempotent; returns
/// `InvalidState` when nothing is recording.
///
/// The WASAPI stream keeps running underneath, so resuming is instant and the
/// device never backs up into an overrun. Captured audio stops reaching the
/// file, the meter reads silence, and the elapsed clock freezes. The state
/// callback does **not** fire: the session is held, not over.
#[unsafe(no_mangle)]
pub extern "C" fn rec_pause() -> RecStatus {
    run(|| {
        let guard = singleton().lock();
        let Some(rec) = guard.as_ref() else {
            return Err(YipError::InvalidState("not recording"));
        };
        if PAUSED.load(Ordering::Acquire) {
            return Ok(());
        }
        // Stamp before the flag: a lock-free reader that sees PAUSED set must
        // already be able to see where the pause began.
        PAUSE_BEGAN_MS.store(now_ms(), Ordering::Relaxed);
        PAUSED.store(true, Ordering::Release);
        rec.set_paused(true);
        METER.silence();
        Ok(())
    })
}

/// Resume a paused recording. Idempotent; returns `InvalidState` when nothing
/// is recording.
#[unsafe(no_mangle)]
pub extern "C" fn rec_resume() -> RecStatus {
    run(|| {
        let guard = singleton().lock();
        let Some(rec) = guard.as_ref() else {
            return Err(YipError::InvalidState("not recording"));
        };
        if !PAUSED.load(Ordering::Acquire) {
            return Ok(());
        }
        rec.set_paused(false);
        // Slide the session origin past the held span, then clear the flag.
        let held = now_ms().saturating_sub(PAUSE_BEGAN_MS.load(Ordering::Relaxed));
        SESSION_START_MS.fetch_add(held, Ordering::Relaxed);
        PAUSED.store(false, Ordering::Release);
        Ok(())
    })
}

/// 1 while the active recording is paused, 0 otherwise. Lock-free.
#[unsafe(no_mangle)]
pub extern "C" fn rec_is_paused() -> u8 {
    u8::from(RECORDING.load(Ordering::Acquire) && PAUSED.load(Ordering::Acquire))
}

/// Current peak envelope, roughly 0.0..=1.0. Returns 0.0 when not recording.
///
/// Lock-free and non-destructive — poll it from as many places as you like.
#[unsafe(no_mangle)]
pub extern "C" fn rec_peak_level() -> f32 {
    METER.peak()
}

/// 1 if a recording is active, 0 otherwise. Lock-free.
#[unsafe(no_mangle)]
pub extern "C" fn rec_is_recording() -> u8 {
    u8::from(RECORDING.load(Ordering::Acquire))
}

/// Monotonic ms of captured audio this session, paused spans excluded. 0 if
/// not recording. Lock-free.
#[unsafe(no_mangle)]
pub extern "C" fn rec_elapsed_ms() -> u64 {
    session_elapsed_ms()
}

/// Fill `out` with the whole meter in one lock-free call.
///
/// # Safety
/// `out` must point at a writable [`RecMeter`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rec_meter(out: *mut RecMeter) -> RecStatus {
    run(|| {
        if out.is_null() {
            return Err(YipError::InvalidArgument("out was null"));
        }
        let recording = RECORDING.load(Ordering::Acquire);
        let snapshot = RecMeter {
            peak: METER.peak(),
            rms: METER.rms(),
            session_peak: METER.session_peak(),
            clip_count: METER.clip_count(),
            overrun_count: METER.overrun_count(),
            recording: u8::from(recording),
            started: u8::from(METER.started()),
            paused: u8::from(recording && PAUSED.load(Ordering::Acquire)),
            reserved: [0; 1],
            dropped_frames: METER.dropped_frames(),
            frames_captured: METER.frames_captured(),
            elapsed_ms: session_elapsed_ms(),
        };
        // SAFETY: caller guarantees `out` is a valid writable RecMeter.
        unsafe { ptr::write(out, snapshot) };
        Ok(())
    })
}

/// Clear the clip counter and the session peak. Lets the UI's clip indicator
/// be acknowledged without interrupting the recording.
#[unsafe(no_mangle)]
pub extern "C" fn rec_reset_clip() {
    METER.reset_clip();
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
        // `into_boxed_slice` guarantees capacity == len, so `rec_free_devices`
        // can reconstruct the exact same allocation. A bare `Vec` + `forget`
        // would leave the capacity unknown and the free size wrong.
        let slice: Box<[DeviceInfo]> = devs.iter().map(device_to_info).collect();
        let len = slice.len();
        let ptr = Box::into_raw(slice).cast::<DeviceInfo>();

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
