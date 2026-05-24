//! Event-driven WASAPI capture. Owns the capture and writer threads, hands
//! peak levels back to the FFI layer via [`SharedMeter`].
//!
//! Thread map:
//!   * **capture thread** — Rust, MMCSS *Pro Audio*. Drives WASAPI, copies
//!     into the SPSC ring, updates the peak atomic. **Zero allocs, zero
//!     locks, zero logs** inside the buffer loop.
//!   * **writer thread** — Rust, normal priority. Drains the ring into
//!     `hound`. Allowed to block on disk.
//!   * **caller** — owns `Recorder`. `start()` and `stop()` only.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, channel};
use std::thread::JoinHandle;

use windows::Win32::Foundation::{HANDLE, WAIT_OBJECT_0};
use windows::Win32::Media::Audio::{
    AUDCLNT_BUFFERFLAGS_SILENT, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, AUDCLNT_STREAMFLAGS_LOOPBACK,
    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, IAudioCaptureClient, IAudioClient, WAVEFORMATEX,
    WAVEFORMATEXTENSIBLE,
};
use windows::Win32::Media::KernelStreaming::WAVE_FORMAT_EXTENSIBLE;
use windows::Win32::Media::Multimedia::{KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, WAVE_FORMAT_IEEE_FLOAT};
use windows::Win32::System::Com::{
    CLSCTX_ALL, COINIT_MULTITHREADED, CoInitializeEx, CoTaskMemFree, CoUninitialize,
};
use windows::Win32::System::Threading::{
    AvRevertMmThreadCharacteristics, AvSetMmThreadCharacteristicsW, CreateEventW, INFINITE,
    WaitForMultipleObjects,
};
use windows::core::Interface;

use crate::devices::find_device;
use crate::error::YipError;
use crate::ffi::RecConfig;
use crate::ring::{RING_CAPACITY_SAMPLES, SharedMeter, split};
use crate::writer::{WriterConfig, run_writer};

/// Owns the running capture session.
pub struct Recorder {
    meter: Arc<SharedMeter>,
    stop: Arc<AtomicBool>,
    writer_stop: Arc<AtomicBool>,
    stop_event: SendHandle,
    capture_thread: Option<JoinHandle<Result<(), YipError>>>,
    writer_thread: Option<JoinHandle<Result<u64, YipError>>>,
    started_at: std::time::Instant,
    path: PathBuf,
}

/// `HANDLE` wrapper. Win32 handles are integers and safe to `Send` between
/// threads; the COM interfaces they refer to are not (and we never send those).
struct SendHandle(HANDLE);
// SAFETY: HANDLE is an opaque integer; ownership is tracked by the kernel.
unsafe impl Send for SendHandle {}
// SAFETY: same.
unsafe impl Sync for SendHandle {}

impl Recorder {
    pub fn start(device_id: &str, path: &Path, _cfg: RecConfig) -> Result<Self, YipError> {
        let meter = SharedMeter::new();
        let stop = Arc::new(AtomicBool::new(false));
        let writer_stop = Arc::new(AtomicBool::new(false));

        let (mut producer, consumer) = split(RING_CAPACITY_SAMPLES);

        // SAFETY: manual-reset = false (auto-reset), initial state = nonsignaled.
        let stop_event_raw = unsafe { CreateEventW(None, false, false, None)? };
        let stop_event = SendHandle(stop_event_raw);
        let stop_event_for_thread = SendHandle(stop_event_raw);

        let (ready_tx, ready_rx): (
            std::sync::mpsc::Sender<Result<WriterConfig, YipError>>,
            Receiver<Result<WriterConfig, YipError>>,
        ) = channel();

        let device_id_owned = device_id.to_string();
        let path_owned: PathBuf = path.to_path_buf();
        let meter_for_capture = meter.clone();
        let stop_for_capture = stop.clone();
        let writer_stop_for_capture = writer_stop.clone();

        let capture_thread = std::thread::Builder::new()
            .name("yip-capture".into())
            .spawn(move || -> Result<(), YipError> {
                capture_loop(
                    &device_id_owned,
                    &path_owned,
                    &mut producer,
                    &meter_for_capture,
                    &stop_for_capture,
                    &writer_stop_for_capture,
                    stop_event_for_thread.0,
                    &ready_tx,
                )
            })
            .map_err(|e| YipError::Wasapi(format!("spawn capture: {e}")))?;

        // Wait for capture thread to negotiate the format and report ready.
        let wcfg = ready_rx
            .recv()
            .map_err(|_| YipError::Wasapi("capture thread died before ready".into()))??;

        let writer_stop_for_writer = writer_stop.clone();
        let writer_thread = std::thread::Builder::new()
            .name("yip-writer".into())
            .spawn(move || run_writer(consumer, wcfg, writer_stop_for_writer))
            .map_err(|e| YipError::Io(format!("spawn writer: {e}")))?;

        Ok(Self {
            meter,
            stop,
            writer_stop,
            stop_event,
            capture_thread: Some(capture_thread),
            writer_thread: Some(writer_thread),
            started_at: std::time::Instant::now(),
            path: path.to_path_buf(),
        })
    }

    pub fn peak_level(&self) -> f32 {
        self.meter.take_peak()
    }

    /// Wall-clock ms since `start()` returned. Monotonic; unaffected by clock changes.
    #[must_use]
    pub fn elapsed_ms(&self) -> u64 {
        u64::try_from(self.started_at.elapsed().as_millis()).unwrap_or(u64::MAX)
    }

    /// Path the writer is targeting.
    #[must_use]
    pub fn path(&self) -> &std::path::Path {
        &self.path
    }

    pub fn stop(mut self) -> Result<(), YipError> {
        self.stop.store(true, Ordering::Release);
        // Wake the capture loop. Single SetEvent is enough on an auto-reset event.
        // SAFETY: stop_event handle was created in start() and is still alive.
        let _ = unsafe { windows::Win32::System::Threading::SetEvent(self.stop_event.0) };

        if let Some(h) = self.capture_thread.take() {
            h.join()
                .map_err(|_| YipError::Wasapi("capture thread panicked".into()))??;
        }
        // Capture thread sets writer_stop on its way out. Don't second-guess.
        self.writer_stop.store(true, Ordering::Release);
        if let Some(h) = self.writer_thread.take() {
            h.join()
                .map_err(|_| YipError::Io("writer thread panicked".into()))??;
        }
        // SAFETY: handle live, single close.
        unsafe {
            let _ = windows::Win32::Foundation::CloseHandle(self.stop_event.0);
        }
        Ok(())
    }
}

impl Drop for Recorder {
    fn drop(&mut self) {
        // Best-effort stop if user dropped without calling stop().
        if self.capture_thread.is_some() || self.writer_thread.is_some() {
            self.stop.store(true, Ordering::Release);
            // SAFETY: handle still valid.
            let _ = unsafe { windows::Win32::System::Threading::SetEvent(self.stop_event.0) };
            if let Some(h) = self.capture_thread.take() {
                let _ = h.join();
            }
            self.writer_stop.store(true, Ordering::Release);
            if let Some(h) = self.writer_thread.take() {
                let _ = h.join();
            }
            // SAFETY: single close, handle valid.
            unsafe {
                let _ = windows::Win32::Foundation::CloseHandle(self.stop_event.0);
            }
        }
    }
}

// ---------- capture thread body ----------

#[allow(clippy::too_many_arguments)] // bag of refs is shorter than a struct here
fn capture_loop(
    device_id: &str,
    path: &Path,
    producer: &mut rtrb::Producer<f32>,
    meter: &Arc<SharedMeter>,
    stop: &Arc<AtomicBool>,
    writer_stop: &Arc<AtomicBool>,
    stop_event: HANDLE,
    ready_tx: &std::sync::mpsc::Sender<Result<WriterConfig, YipError>>,
) -> Result<(), YipError> {
    // SAFETY: per-thread COM init; balanced by CoUninitialize at end.
    let hr = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) };
    if hr.is_err() && hr.0 != windows::Win32::Foundation::RPC_E_CHANGED_MODE.0 {
        let e = YipError::Wasapi(format!("CoInitializeEx 0x{:08X}", hr.0 as u32));
        let _ = ready_tx.send(Err(e.clone()));
        return Err(e);
    }
    let com_guard = ComGuard;

    // Locate device, decide loopback.
    let device = find_device(device_id)?;
    // `cast` is a safe windows-rs trait method that wraps QueryInterface.
    let dataflow = device.cast::<windows::Win32::Media::Audio::IMMEndpoint>()?;
    // SAFETY: live IMMEndpoint.
    let flow = unsafe { dataflow.GetDataFlow()? };
    let is_render = flow == windows::Win32::Media::Audio::eRender;

    // SAFETY: standard activation of WASAPI client.
    let client: IAudioClient = unsafe { device.Activate::<IAudioClient>(CLSCTX_ALL, None)? };

    // Negotiate mix format (always f32 shared since Vista).
    // SAFETY: live client; returns CoTaskMem-allocated pointer.
    let fmt_ptr = unsafe { client.GetMixFormat()? };
    let (sample_rate, channels) = parse_format(fmt_ptr)?;
    let writer_cfg = WriterConfig {
        path: path.to_path_buf(),
        sample_rate,
        channels,
    };

    // Audio event (auto-reset, nonsignaled).
    // SAFETY: standard event creation.
    let audio_event = unsafe { CreateEventW(None, false, false, None)? };

    let mut stream_flags: u32 = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if is_render {
        stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }
    // Let WASAPI rate-convert if the engine differs from device — keeps us
    // glitch-free at the cost of ~0.5% CPU. Required for shared loopback.
    stream_flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    // 10 ms buffer = 480 frames at 48 kHz. Hardware aligns up.
    let buffer_100ns: i64 = 10_0000;

    // SAFETY: client live; fmt_ptr live; flags valid.
    let init_res = unsafe {
        client.Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            stream_flags,
            buffer_100ns,
            0,
            fmt_ptr,
            None,
        )
    };
    // Free the format buffer regardless of init result.
    // SAFETY: pointer returned by GetMixFormat must be freed with CoTaskMemFree.
    unsafe { CoTaskMemFree(Some(fmt_ptr.cast())) };
    init_res?;

    // SAFETY: client live; handle live.
    unsafe { client.SetEventHandle(audio_event)? };

    let capture: IAudioCaptureClient = unsafe { client.GetService::<IAudioCaptureClient>()? };

    // Promote thread to Pro Audio characteristic.
    let mut task_index: u32 = 0;
    // SAFETY: PCWSTR points at a static null-terminated UTF-16 literal.
    let mmcss = unsafe {
        AvSetMmThreadCharacteristicsW(
            windows::core::w!("Pro Audio"),
            std::ptr::addr_of_mut!(task_index),
        )
    };
    let _mmcss_guard = mmcss.ok().map(MmcssGuard);

    // SAFETY: client live; this transitions the engine to running.
    unsafe { client.Start()? };

    // Signal the parent that everything is wired and we have a writer config.
    let _ = ready_tx.send(Ok(writer_cfg));
    meter.started.store(true, Ordering::Release);

    let handles = [audio_event, stop_event];
    let frames_per_sample = u64::from(channels);

    'outer: loop {
        // SAFETY: handles array is in scope for the duration of the call.
        let wait = unsafe { WaitForMultipleObjects(&handles, false, INFINITE) };
        let idx = wait.0.wrapping_sub(WAIT_OBJECT_0.0);
        if idx == 1 || stop.load(Ordering::Acquire) {
            break;
        }
        if idx != 0 {
            return Err(YipError::Wasapi(format!("Wait failed 0x{:08X}", wait.0)));
        }

        loop {
            let mut packet_frames: u32 = 0;
            // SAFETY: capture live.
            let avail = unsafe { capture.GetNextPacketSize()? };
            if avail == 0 {
                break;
            }
            let mut data_ptr: *mut u8 = std::ptr::null_mut();
            let mut packet_flags: u32 = 0;
            // SAFETY: capture live; we provide a writable pointer for each
            // out-param. data_ptr is valid only between GetBuffer/ReleaseBuffer.
            unsafe {
                capture.GetBuffer(
                    std::ptr::addr_of_mut!(data_ptr),
                    std::ptr::addr_of_mut!(packet_frames),
                    std::ptr::addr_of_mut!(packet_flags),
                    None,
                    None,
                )?;
            }
            if packet_frames == 0 {
                // SAFETY: paired with GetBuffer.
                unsafe { capture.ReleaseBuffer(packet_frames)? };
                continue;
            }
            let n_samples = (packet_frames as usize) * (channels as usize);
            let silent = packet_flags & (AUDCLNT_BUFFERFLAGS_SILENT.0 as u32) != 0;

            // Copy into ring without allocating. Update peak in same pass.
            let copied = match producer.write_chunk_uninit(n_samples) {
                Ok(chunk) => {
                    let (slot_a, slot_b) = chunk.as_mut_slices();
                    let total_a = slot_a.len();
                    let total_b = slot_b.len();

                    if silent {
                        for s in slot_a.iter_mut() {
                            s.write(0.0);
                        }
                        for s in slot_b.iter_mut() {
                            s.write(0.0);
                        }
                    } else {
                        // SAFETY: data_ptr valid for n_samples*4 bytes; sample
                        // format negotiated to f32 above.
                        let src = unsafe {
                            std::slice::from_raw_parts(data_ptr.cast::<f32>(), n_samples)
                        };
                        let mut peak = 0.0_f32;
                        for (dst, &s) in slot_a.iter_mut().zip(src.iter()) {
                            dst.write(s);
                            let a = s.abs();
                            if a > peak {
                                peak = a;
                            }
                        }
                        for (dst, &s) in slot_b.iter_mut().zip(src[total_a..].iter()) {
                            dst.write(s);
                            let a = s.abs();
                            if a > peak {
                                peak = a;
                            }
                        }
                        if peak > 0.0 {
                            meter.fold_peak(peak);
                        }
                    }
                    chunk.commit_all();
                    total_a + total_b
                }
                Err(_) => {
                    // Ring full → writer can't keep up. Mark overrun and
                    // drop this packet (the only realtime-safe choice).
                    meter.fold_peak(1.0);
                    0
                }
            };
            meter.frames_captured.fetch_add(
                (copied as u64) / frames_per_sample.max(1),
                Ordering::Relaxed,
            );

            // SAFETY: paired with GetBuffer above.
            unsafe { capture.ReleaseBuffer(packet_frames)? };

            if stop.load(Ordering::Acquire) {
                break 'outer;
            }
        }
    }

    // SAFETY: client live; idempotent on running streams.
    let _ = unsafe { client.Stop() };
    // SAFETY: audio_event was created by us.
    unsafe {
        let _ = windows::Win32::Foundation::CloseHandle(audio_event);
    }
    // Tell writer to flush.
    writer_stop.store(true, Ordering::Release);

    drop(com_guard);
    Ok(())
}

fn parse_format(fmt_ptr: *const WAVEFORMATEX) -> Result<(u32, u16), YipError> {
    if fmt_ptr.is_null() {
        return Err(YipError::UnsupportedFormat("null mix format".into()));
    }
    // WAVEFORMATEX is #[repr(packed)] — refs to its fields are UB without
    // explicit unaligned reads. Pull every field via addr_of! + read_unaligned.
    // SAFETY: fmt_ptr is non-null and points at a valid WAVEFORMATEX.
    let sample_rate = unsafe { std::ptr::addr_of!((*fmt_ptr).nSamplesPerSec).read_unaligned() };
    // SAFETY: same as above.
    let channels = unsafe { std::ptr::addr_of!((*fmt_ptr).nChannels).read_unaligned() };
    // SAFETY: same as above.
    let bits_per_sample = unsafe { std::ptr::addr_of!((*fmt_ptr).wBitsPerSample).read_unaligned() };
    // SAFETY: same as above.
    let format_tag = unsafe { std::ptr::addr_of!((*fmt_ptr).wFormatTag).read_unaligned() };
    // SAFETY: same as above.
    let cb_size = unsafe { std::ptr::addr_of!((*fmt_ptr).cbSize).read_unaligned() };

    if bits_per_sample != 32 {
        return Err(YipError::UnsupportedFormat(format!(
            "engine reported {bits_per_sample} bits/sample; need 32"
        )));
    }
    let is_float = if u32::from(format_tag) == WAVE_FORMAT_IEEE_FLOAT {
        true
    } else if u32::from(format_tag) == WAVE_FORMAT_EXTENSIBLE && cb_size >= 22 {
        // SAFETY: cbSize >= 22 means the trailing EXTENSIBLE fields are
        // present in the same allocation.
        let sub_format = unsafe {
            std::ptr::addr_of!((*fmt_ptr.cast::<WAVEFORMATEXTENSIBLE>()).SubFormat).read_unaligned()
        };
        sub_format == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
    } else {
        false
    };
    if !is_float {
        return Err(YipError::UnsupportedFormat(format!(
            "engine reported tag {format_tag} — only IEEE_FLOAT supported"
        )));
    }
    Ok((sample_rate, channels))
}

// ---------- RAII guards ----------

struct ComGuard;
impl Drop for ComGuard {
    fn drop(&mut self) {
        // SAFETY: balances CoInitializeEx earlier on the same thread.
        unsafe { CoUninitialize() };
    }
}

struct MmcssGuard(HANDLE);
impl Drop for MmcssGuard {
    fn drop(&mut self) {
        // SAFETY: handle obtained from AvSetMmThreadCharacteristicsW above.
        let _ = unsafe { AvRevertMmThreadCharacteristics(self.0) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_format_rejects_non_float() {
        // WAVEFORMATEX is repr(packed) — must use struct-literal init
        // rather than field assignment.
        let f = WAVEFORMATEX {
            wFormatTag: 1, // PCM integer
            nChannels: 2,
            nSamplesPerSec: 48_000,
            nAvgBytesPerSec: 0,
            nBlockAlign: 0,
            wBitsPerSample: 16,
            cbSize: 0,
        };
        assert!(parse_format(std::ptr::addr_of!(f)).is_err());
    }

    #[test]
    fn parse_format_accepts_plain_float() {
        let f = WAVEFORMATEX {
            wFormatTag: WAVE_FORMAT_IEEE_FLOAT as u16,
            nChannels: 2,
            nSamplesPerSec: 48_000,
            nAvgBytesPerSec: 0,
            nBlockAlign: 0,
            wBitsPerSample: 32,
            cbSize: 0,
        };
        let (sr, ch) = parse_format(std::ptr::addr_of!(f)).unwrap();
        assert_eq!(sr, 48_000);
        assert_eq!(ch, 2);
    }
}
