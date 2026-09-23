//! Event-driven WASAPI capture. Owns the capture and writer threads; capture
//! health lands in the process-wide [`crate::ring::METER`].
//!
//! Thread map:
//!   * **capture thread** — Rust, MMCSS *Pro Audio*. Drives WASAPI, bulk-copies
//!     each packet into the SPSC ring and folds one pass of statistics into the
//!     meter. **Zero allocs, zero locks, zero logs** inside the buffer loop.
//!   * **writer thread** — Rust, normal priority. Drains the ring into
//!     `hound` or a Media Foundation encoder. Allowed to block on disk.
//!   * **caller** — owns `Recorder`. `start()` and `stop()` only.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::channel;
use std::thread::JoinHandle;

use windows::Win32::Foundation::{HANDLE, WAIT_OBJECT_0, WAIT_TIMEOUT};
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
    AvRevertMmThreadCharacteristics, AvSetMmThreadCharacteristicsW, CreateEventW,
    WaitForMultipleObjects,
};
use windows::core::Interface;

use crate::devices::find_device;
use crate::error::YipError;
use crate::ffi::RecConfig;
use crate::format::Encoding;
use crate::ring::{METER, RING_CAPACITY_SAMPLES, analyse, split};
use crate::writer::{WriterConfig, run_writer};

/// Owns the running capture session.
pub struct Recorder {
    stop: Arc<AtomicBool>,
    /// Set by the UI thread, read by the capture loop. While it is true the
    /// stream keeps running but nothing reaches the ring.
    paused: Arc<AtomicBool>,
    writer_stop: Arc<AtomicBool>,
    stop_event: SendHandle,
    capture_thread: Option<JoinHandle<Result<(), YipError>>>,
    writer_thread: Option<JoinHandle<Result<u64, YipError>>>,
    started_at: std::time::Instant,
    path: PathBuf,
}

/// `HANDLE` wrapper. Win32 handles are integers and safe to `Send` between
/// threads; the COM interfaces they refer to are not (and we never send those).
pub(crate) struct SendHandle(pub(crate) HANDLE);
// SAFETY: HANDLE is an opaque integer; ownership is tracked by the kernel.
unsafe impl Send for SendHandle {}
// SAFETY: same.
unsafe impl Sync for SendHandle {}

impl Recorder {
    /// `on_fault` runs on the capture or writer thread when the take ends by
    /// itself — the device removed, the disk full. It must not block and must
    /// not call back into `Recorder`; the owner stops the session afterwards.
    pub fn start(
        device_id: &str,
        path: &Path,
        cfg: RecConfig,
        on_fault: fn(),
    ) -> Result<Self, YipError> {
        // Refuse a bad format before any thread or device is touched.
        let encoding = Encoding::from_config(&cfg)?;

        let stop = Arc::new(AtomicBool::new(false));
        let paused = Arc::new(AtomicBool::new(false));
        let writer_stop = Arc::new(AtomicBool::new(false));

        let (mut producer, consumer) = split(RING_CAPACITY_SAMPLES);

        // SAFETY: manual-reset = false (auto-reset), initial state = nonsignaled.
        let stop_event_raw = unsafe { CreateEventW(None, false, false, None)? };
        let stop_event = SendHandle(stop_event_raw);
        let stop_event_for_thread = SendHandle(stop_event_raw);

        let (ready_tx, ready_rx) = channel::<Result<WriterConfig, YipError>>();

        let device_id_owned = device_id.to_string();
        let path_owned: PathBuf = path.to_path_buf();
        let stop_for_capture = stop.clone();
        let paused_for_capture = paused.clone();
        let writer_stop_for_capture = writer_stop.clone();

        let capture_thread = std::thread::Builder::new()
            .name("yip-capture".into())
            .spawn(move || -> Result<(), YipError> {
                // However capture ends, the writer is told to flush what it has.
                let _flush = FlagOnDrop(writer_stop_for_capture);
                capture_loop(
                    &device_id_owned,
                    &path_owned,
                    cfg,
                    encoding,
                    &mut producer,
                    &stop_for_capture,
                    &paused_for_capture,
                    &stop_event_for_thread,
                    &ready_tx,
                    on_fault,
                )
            })
            .map_err(|e| YipError::Wasapi(format!("spawn capture: {e}")))?;

        // Own the capture thread from here on: every early return below drops
        // `rec`, and Drop stops and joins whatever has been started.
        let mut rec = Self {
            stop,
            paused,
            writer_stop,
            stop_event,
            capture_thread: Some(capture_thread),
            writer_thread: None,
            started_at: std::time::Instant::now(),
            path: path.to_path_buf(),
        };

        // Wait for capture thread to negotiate the format and report ready.
        let wcfg = ready_rx
            .recv()
            .map_err(|_| YipError::Wasapi("capture thread died before ready".into()))??;

        let writer_stop_for_writer = rec.writer_stop.clone();
        let (writer_ready_tx, writer_ready_rx) = channel::<Result<(), YipError>>();
        let writer_thread = std::thread::Builder::new()
            .name("yip-writer".into())
            .spawn(move || {
                run_writer(
                    consumer,
                    wcfg,
                    writer_stop_for_writer,
                    writer_ready_tx,
                    on_fault,
                )
            })
            .map_err(|e| YipError::Io(format!("spawn writer: {e}")))?;
        rec.writer_thread = Some(writer_thread);

        // Wait for the file and encoder to open, so a refused format fails the
        // start instead of surfacing at stop with nothing written.
        writer_ready_rx
            .recv()
            .map_err(|_| YipError::Io("writer thread died before ready".into()))??;

        Ok(rec)
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

    /// Hold or release capture without tearing the stream down.
    ///
    /// The WASAPI client keeps running either way: stopping it would drop the
    /// endpoint's position and make resuming cost a re-initialise, and leaving
    /// it running is what keeps the device from backing up into an overrun the
    /// moment capture comes back. Paused packets are drained and discarded.
    pub fn set_paused(&self, paused: bool) {
        self.paused.store(paused, Ordering::Release);
    }

    /// Stop both threads and finalise the file. Both threads are always
    /// joined — a capture error must not skip the writer's flush — and the
    /// first failure is what is reported: a device that went away is the
    /// cause, a writer complaining about it afterwards is not.
    pub fn stop(mut self) -> Result<(), YipError> {
        self.stop.store(true, Ordering::Release);
        // Wake the capture loop. Single SetEvent is enough on an auto-reset event.
        // SAFETY: stop_event handle was created in start() and is still alive.
        let _ = unsafe { windows::Win32::System::Threading::SetEvent(self.stop_event.0) };

        let captured = match self.capture_thread.take() {
            Some(h) => h
                .join()
                .unwrap_or_else(|_| Err(YipError::Wasapi("capture thread panicked".into()))),
            None => Ok(()),
        };
        // Capture thread sets writer_stop on its way out. Don't second-guess.
        self.writer_stop.store(true, Ordering::Release);
        let written = match self.writer_thread.take() {
            Some(h) => {
                // Cut the writer's park short: it may be sleeping out a whole
                // batch, and the file is not finalised until it wakes.
                h.thread().unpark();
                h.join()
                    .unwrap_or_else(|_| Err(YipError::Io("writer thread panicked".into())))
                    .map(|_| ())
            }
            None => Ok(()),
        };
        // SAFETY: handle live, single close. Drop skips it: both threads are
        // already taken.
        unsafe {
            let _ = windows::Win32::Foundation::CloseHandle(self.stop_event.0);
        }
        captured.and(written)
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
                h.thread().unpark();
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

/// How long the capture loop waits for a buffer event before it checks on the
/// device itself. A removed endpoint can simply stop signalling, and an
/// `INFINITE` wait then parked the thread for good while the UI went on
/// reporting a live take. Loopback capture legitimately goes quiet while
/// nothing is playing, so a timeout is a reason to ask the device, never a
/// failure on its own.
pub(crate) const DEVICE_PROBE_MS: u32 = 500;

/// A Win32 event this module created. Closed on every exit path, including
/// the early `?` returns that used to leak it.
struct OwnedEvent(HANDLE);

impl OwnedEvent {
    fn new() -> Result<Self, YipError> {
        // SAFETY: auto-reset, initially nonsignaled, unnamed.
        let handle = unsafe { CreateEventW(None, false, false, None)? };
        Ok(Self(handle))
    }
}

impl Drop for OwnedEvent {
    fn drop(&mut self) {
        // SAFETY: created by `new` and closed exactly once, here.
        unsafe {
            let _ = windows::Win32::Foundation::CloseHandle(self.0);
        }
    }
}

/// The engine mix format from `GetMixFormat`, freed with `CoTaskMemFree` on
/// every path rather than only the one that reaches `Initialize`.
struct MixFormat(*mut WAVEFORMATEX);

impl Drop for MixFormat {
    fn drop(&mut self) {
        // SAFETY: CoTaskMem allocation handed out by GetMixFormat, freed once.
        unsafe { CoTaskMemFree(Some(self.0.cast())) };
    }
}

/// Raises a flag when dropped. The capture thread uses it so the writer is
/// told to flush however capture ends — a device error included, which used
/// to leave the writer draining an empty ring until `stop`.
pub(crate) struct FlagOnDrop(pub(crate) Arc<AtomicBool>);

impl Drop for FlagOnDrop {
    fn drop(&mut self) {
        self.0.store(true, Ordering::Release);
    }
}

/// A started shared-mode capture stream and the format it delivers.
struct Stream {
    client: IAudioClient,
    capture: IAudioCaptureClient,
    audio_event: OwnedEvent,
    sample_rate: u32,
    channels: u16,
}

#[allow(clippy::too_many_arguments)] // bag of refs is shorter than a struct here
fn capture_loop(
    device_id: &str,
    path: &Path,
    cfg: RecConfig,
    encoding: Encoding,
    producer: &mut rtrb::Producer<f32>,
    stop: &Arc<AtomicBool>,
    paused: &Arc<AtomicBool>,
    stop_event: &SendHandle,
    ready_tx: &std::sync::mpsc::Sender<Result<WriterConfig, YipError>>,
    on_fault: fn(),
) -> Result<(), YipError> {
    // SAFETY: per-thread COM init; balanced by the ComGuard below.
    let hr = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) };
    if hr.is_err() && hr.0 != windows::Win32::Foundation::RPC_E_CHANGED_MODE.0 {
        let e = YipError::Wasapi(format!("CoInitializeEx 0x{:08X}", hr.0 as u32));
        let _ = ready_tx.send(Err(e.clone()));
        return Err(e);
    }
    // Declared before the stream, so it drops after it: every COM object is
    // released before the apartment goes away.
    let _com = ComGuard;

    // Any failure while opening is the caller's answer, not a lost thread: it
    // goes back over `ready_tx`, so Record reports why rather than "capture
    // thread died before ready".
    let stream = match open_stream(device_id, cfg, encoding) {
        Ok(stream) => stream,
        Err(e) => {
            let _ = ready_tx.send(Err(e.clone()));
            return Err(e);
        }
    };

    let _ = ready_tx.send(Ok(WriterConfig {
        path: path.to_path_buf(),
        sample_rate: stream.sample_rate,
        channels: stream.channels,
        encoding,
    }));

    let result = run_stream(&stream, producer, stop, paused, stop_event);
    // SAFETY: client live; idempotent on a stopped or invalidated stream.
    let _ = unsafe { stream.client.Stop() };

    // A failure nobody asked for — the device pulled, the driver gone — ends
    // the take on its own. Tell the host now; `rec_stop` collects the error.
    if result.is_err() && !stop.load(Ordering::Acquire) {
        on_fault();
    }
    result
}

/// Locate the endpoint, negotiate a float format and start the stream.
fn open_stream(device_id: &str, cfg: RecConfig, encoding: Encoding) -> Result<Stream, YipError> {
    // Locate device, decide loopback.
    let device = find_device(device_id)?;
    // `cast` is a safe windows-rs trait method that wraps QueryInterface.
    let dataflow = device.cast::<windows::Win32::Media::Audio::IMMEndpoint>()?;
    // SAFETY: live IMMEndpoint.
    let flow = unsafe { dataflow.GetDataFlow()? };
    let is_render = flow == windows::Win32::Media::Audio::eRender;

    // SAFETY: standard activation of WASAPI client.
    let mut client: IAudioClient = unsafe { device.Activate::<IAudioClient>(CLSCTX_ALL, None)? };

    // Negotiate mix format (always f32 shared since Vista).
    // SAFETY: live client; returns CoTaskMem-allocated pointer, owned below.
    let mix = MixFormat(unsafe { client.GetMixFormat()? });
    let (mix_rate, mix_channels) = parse_format(mix.0)?;

    let audio_event = OwnedEvent::new()?;

    let mut stream_flags: u32 = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if is_render {
        stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }
    // Let WASAPI rate-convert if the requested format differs from the engine
    // mix format — costs ~0.5% CPU and is what makes the settings dialog's
    // sample-rate / channel-count picker mean anything in shared mode.
    stream_flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    // 10 ms buffer = 480 frames at 48 kHz. Hardware aligns up.
    let buffer_100ns: i64 = 10_0000;

    // A 0 in either field means "follow the device".
    let want_rate = if cfg.sample_rate == 0 {
        mix_rate
    } else {
        cfg.sample_rate
    };
    let want_channels = if cfg.channels == 0 {
        mix_channels
    } else {
        cfg.channels
    };
    // Encoders take fewer rates and channel counts than WAV. Asking WASAPI to
    // convert on the way in beats an encoder refusing the take.
    let want_rate = encoding.capture_rate(want_rate);
    let want_channels = encoding.capture_channels(want_channels);

    // First choice: the caller's format. WASAPI resamples/remixes behind
    // AUTOCONVERTPCM. A device that refuses it leaves the client unusable, so
    // fall back on a *fresh* client at the engine mix format.
    let mut sample_rate = mix_rate;
    let mut channels = mix_channels;
    let mut initialized = false;

    if want_rate != mix_rate || want_channels != mix_channels {
        let want = float_wfx(want_rate, want_channels);
        // SAFETY: client live; `want` outlives the call; flags valid.
        let res = unsafe {
            client.Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                stream_flags,
                buffer_100ns,
                0,
                std::ptr::addr_of!(want),
                None,
            )
        };
        if res.is_ok() {
            sample_rate = want_rate;
            channels = want_channels;
            initialized = true;
        } else {
            // Initialize consumed this client even on failure. Get a new one.
            // SAFETY: device is still live.
            client = unsafe { device.Activate::<IAudioClient>(CLSCTX_ALL, None)? };
        }
    }

    // Either the caller's format was refused or it matched the mix format
    // anyway: fall back to initialising at the engine format.
    if !initialized {
        // SAFETY: client live; the mix format stays allocated until `mix`
        // drops at the end of this function; flags valid.
        unsafe {
            client.Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                stream_flags,
                buffer_100ns,
                0,
                mix.0,
                None,
            )?;
        }
    }

    // SAFETY: client live; handle live for as long as the returned Stream.
    unsafe { client.SetEventHandle(audio_event.0)? };

    // SAFETY: client initialised above.
    let capture: IAudioCaptureClient = unsafe { client.GetService::<IAudioCaptureClient>()? };

    // SAFETY: client live; this transitions the engine to running.
    unsafe { client.Start()? };

    Ok(Stream {
        client,
        capture,
        audio_event,
        sample_rate,
        channels,
    })
}

/// The realtime part: drain packets into the ring until asked to stop or the
/// device fails.
#[allow(clippy::cast_ptr_alignment)] // SAFETY: WASAPI GetBuffer guarantees f32-aligned data
fn run_stream(
    stream: &Stream,
    producer: &mut rtrb::Producer<f32>,
    stop: &Arc<AtomicBool>,
    paused: &Arc<AtomicBool>,
    stop_event: &SendHandle,
) -> Result<(), YipError> {
    let capture = &stream.capture;
    let sample_rate = stream.sample_rate;

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

    let handles = [stream.audio_event.0, stop_event.0];
    let samples_per_frame = usize::from(stream.channels).max(1);

    'outer: loop {
        // SAFETY: handles array is in scope for the duration of the call.
        let wait = unsafe { WaitForMultipleObjects(&handles, false, DEVICE_PROBE_MS) };
        let idx = wait.0.wrapping_sub(WAIT_OBJECT_0.0);
        if idx == 1 || stop.load(Ordering::Acquire) {
            break;
        }
        // A timeout falls through to the drain below: GetNextPacketSize is
        // what reports AUDCLNT_E_DEVICE_INVALIDATED once the endpoint is gone.
        if idx != 0 && wait != WAIT_TIMEOUT {
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
            if paused.load(Ordering::Acquire) {
                // Paused. Drain the packet so the endpoint keeps cycling, but
                // it reaches neither the ring nor the statistics pass: a
                // paused take must not grow the file or move the meter.
                // SAFETY: paired with GetBuffer above.
                unsafe { capture.ReleaseBuffer(packet_frames)? };
                METER.silence();
                continue;
            }

            let n_samples = (packet_frames as usize) * samples_per_frame;
            let silent = packet_flags & (AUDCLNT_BUFFERFLAGS_SILENT.0 as u32) != 0;

            // A SILENT packet may point at a stale or unmapped buffer, so that
            // case never dereferences `data_ptr`.
            let src: &[f32] = if silent {
                &[]
            } else {
                // SAFETY: between GetBuffer and ReleaseBuffer, data_ptr is
                // valid for n_samples * 4 bytes, and the stream format was
                // negotiated to f32 above.
                unsafe { std::slice::from_raw_parts(data_ptr.cast::<f32>(), n_samples) }
            };

            // Analyse before touching the ring: the meter reports what the
            // device delivered, whether or not there was room for all of it.
            let stats = analyse(src);

            // Take whatever the ring can hold, rounded down to whole frames —
            // a partial frame would rotate the channel interleave for the rest
            // of the file. A short write loses the tail of one packet instead
            // of the packet entirely.
            let room = producer.slots().min(n_samples);
            let writable = room - (room % samples_per_frame);

            if let Ok(mut chunk) = producer.write_chunk_uninit(writable) {
                let (slot_a, slot_b) = chunk.as_mut_slices();
                let len_a = slot_a.len();
                let len_b = slot_b.len();
                if silent {
                    // SAFETY: the all-zero bit pattern is the valid f32 0.0,
                    // and the slots are ours to initialise until commit_all.
                    unsafe {
                        std::ptr::write_bytes(slot_a.as_mut_ptr(), 0, len_a);
                        std::ptr::write_bytes(slot_b.as_mut_ptr(), 0, len_b);
                    }
                } else {
                    // SAFETY: MaybeUninit<f32> shares f32's layout, the WASAPI
                    // buffer and the ring slots never overlap, and
                    // len_a + len_b == writable <= src.len().
                    unsafe {
                        std::ptr::copy_nonoverlapping(
                            src.as_ptr(),
                            slot_a.as_mut_ptr().cast::<f32>(),
                            len_a,
                        );
                        std::ptr::copy_nonoverlapping(
                            src.as_ptr().add(len_a),
                            slot_b.as_mut_ptr().cast::<f32>(),
                            len_b,
                        );
                    }
                }
                // SAFETY: every slot was initialised by the branch above.
                unsafe { chunk.commit_all() };
            }

            let dropped = (n_samples - writable) / samples_per_frame;
            if dropped > 0 {
                // Writer can't keep up. Report it as lost frames, never as a
                // full-scale peak — that used to light the UI up as a clip.
                METER.note_overrun(dropped as u64);
            }
            METER.push_block(stats, (writable / samples_per_frame) as u64, sample_rate);

            // SAFETY: paired with GetBuffer above.
            unsafe { capture.ReleaseBuffer(packet_frames)? };

            if stop.load(Ordering::Acquire) {
                break 'outer;
            }
        }
    }
    Ok(())
}

/// Build a plain float32 `WAVEFORMATEX`. Valid without the EXTENSIBLE tail for
/// mono and stereo, which is all the settings dialog offers.
pub(crate) fn float_wfx(sample_rate: u32, channels: u16) -> WAVEFORMATEX {
    let block_align = channels.saturating_mul(4);
    WAVEFORMATEX {
        wFormatTag: WAVE_FORMAT_IEEE_FLOAT as u16,
        nChannels: channels,
        nSamplesPerSec: sample_rate,
        nAvgBytesPerSec: sample_rate.saturating_mul(u32::from(block_align)),
        nBlockAlign: block_align,
        wBitsPerSample: 32,
        cbSize: 0,
    }
}

pub(crate) fn parse_format(fmt_ptr: *const WAVEFORMATEX) -> Result<(u32, u16), YipError> {
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

pub(crate) struct ComGuard;
impl Drop for ComGuard {
    fn drop(&mut self) {
        // SAFETY: balances CoInitializeEx earlier on the same thread.
        unsafe { CoUninitialize() };
    }
}

pub(crate) struct MmcssGuard(pub(crate) HANDLE);
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
