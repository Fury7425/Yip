//! Playback of a finished take: Media Foundation decode → SPSC ring → WASAPI
//! render. Capture's thread map, run backwards.
//!
//! Thread map:
//!   * **render thread** — Rust, MMCSS *Pro Audio*. Drives WASAPI, copies whole
//!     frames out of the ring into the endpoint buffer and folds one peak pass
//!     for the UI. **Zero allocs, zero locks, zero logs** inside the loop.
//!   * **decoder thread** — Rust, normal priority. Pulls samples out of the
//!     source reader into the ring. Allowed to block on disk and on the codec.
//!   * **caller** — owns [`Player`]. `start()`, `pause()`, `seek()`, `stop()`.
//!
//! One decoder handles every format Yip writes, because Media Foundation ships
//! with a demuxer and decoder for all four and hands back float PCM. Nothing
//! here re-implements a codec.
//!
//! Format negotiation runs render-first: the render thread asks WASAPI for the
//! file's own rate and channel count (shared mode converts behind
//! `AUTOCONVERTPCM`), falls back to the engine mix format if the endpoint
//! refuses, and only then does the decoder learn which of the two to produce.
//! Deciding the other way round would leave a mono take with nowhere to go on
//! a device that would not take a mono stream.

use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::mpsc::{Sender, channel};
use std::thread::JoinHandle;

use windows::Win32::Foundation::{HANDLE, WAIT_OBJECT_0};
use windows::Win32::Media::Audio::{
    AUDCLNT_BUFFERFLAGS_SILENT, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
    AUDCLNT_STREAMFLAGS_EVENTCALLBACK, AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, IAudioClient,
    IAudioRenderClient,
};
use windows::Win32::System::Com::{CLSCTX_ALL, COINIT_MULTITHREADED, CoInitializeEx, CoTaskMemFree};
use windows::Win32::System::Threading::{
    AvSetMmThreadCharacteristicsW, CreateEventW, INFINITE, WaitForMultipleObjects,
};

use crate::capture::{ComGuard, MmcssGuard, SendHandle, float_wfx, parse_format};
use crate::decode::Decoder;
use crate::devices::default_render_device;
use crate::error::YipError;
use crate::ring::{PLAY_RING_CAPACITY_SAMPLES, split};

/// Endpoint buffer. 20 ms leaves the decoder room to stumble over a slow disk
/// without the endpoint running dry, and is still short enough that pause and
/// stop land on the same beat the button was pressed.
const RENDER_BUFFER_100NS: i64 = 20_0000;

/// How long the render thread waits for the decoder to fill the ring before
/// starting the endpoint. Opening an MP3 or M4A costs a decoder instantiation,
/// and starting into an empty ring turns that into an audible gap.
const PREROLL_TIMEOUT_MS: u64 = 400;

/// How long the decoder waits for the render thread to acknowledge a flush
/// before giving up on the handshake and seeking anyway. Only reached if the
/// render thread has died, in which case a stale tail is the least of it.
const FLUSH_TIMEOUT_MS: u64 = 250;

/// Peak release time, matching the capture meter's ballistics so a playback
/// level and a capture level of the same signal read the same.
const RELEASE_SECONDS: f32 = 0.35;

/// Poll interval for both of the waits above, and for the decoder's wait on
/// ring space. Never used while idle: no player, no thread, no tick.
const POLL_MS: u64 = 2;

/// Lock-free playback state. Written by the two playback threads, read by the
/// UI through `play_state`, so a poll costs no lock and cannot stall either
/// thread. Process-wide for the same reason [`crate::ring::METER`] is: there
/// is one playback at a time and every reader sees the same atomics.
#[derive(Debug)]
pub struct PlaybackState {
    /// True from the moment the endpoint starts until the file ends or the
    /// player is stopped.
    playing: AtomicBool,
    paused: AtomicBool,
    /// True once the file played through to its end on its own.
    finished: AtomicBool,
    /// Decoder → render: no more samples are coming.
    eof: AtomicBool,
    /// Decoder → render: drop everything you are holding, a seek is landing.
    flushing: AtomicBool,
    /// Render → decoder: the ring is empty and the endpoint is silent.
    drained: AtomicBool,
    /// Bumped by every seek request. The decoder acts on the latest target it
    /// sees, so dragging a scrubber collapses into one seek rather than one
    /// per pixel.
    seek_seq: AtomicU64,
    seek_target_ms: AtomicU64,
    /// Where the stream now starts. Written by the decoder before it clears
    /// `flushing`, read by the render thread as it comes out of a flush.
    seek_base_ms: AtomicU64,
    /// Written by the render thread only, so a seek cannot tear it.
    position_ms: AtomicU64,
    duration_ms: AtomicU64,
    sample_rate: AtomicU32,
    channels: AtomicU32,
    /// Decaying peak envelope of what is being rendered, as `f32` bits.
    peak_bits: AtomicU32,
}

/// The one playback state block. Reset at the start of every take.
pub static PLAYBACK: PlaybackState = PlaybackState::new();

impl Default for PlaybackState {
    fn default() -> Self {
        Self::new()
    }
}

impl PlaybackState {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            playing: AtomicBool::new(false),
            paused: AtomicBool::new(false),
            finished: AtomicBool::new(false),
            eof: AtomicBool::new(false),
            flushing: AtomicBool::new(false),
            drained: AtomicBool::new(false),
            seek_seq: AtomicU64::new(0),
            seek_target_ms: AtomicU64::new(0),
            seek_base_ms: AtomicU64::new(0),
            position_ms: AtomicU64::new(0),
            duration_ms: AtomicU64::new(0),
            sample_rate: AtomicU32::new(0),
            channels: AtomicU32::new(0),
            peak_bits: AtomicU32::new(0),
        }
    }

    /// Clear everything a previous take left behind. Called before the threads
    /// for the next one exist.
    pub fn reset(&self) {
        self.playing.store(false, Ordering::Release);
        self.paused.store(false, Ordering::Release);
        self.finished.store(false, Ordering::Release);
        self.eof.store(false, Ordering::Release);
        self.flushing.store(false, Ordering::Release);
        self.drained.store(false, Ordering::Release);
        self.seek_seq.store(0, Ordering::Relaxed);
        self.seek_target_ms.store(0, Ordering::Relaxed);
        self.seek_base_ms.store(0, Ordering::Relaxed);
        self.position_ms.store(0, Ordering::Relaxed);
        self.duration_ms.store(0, Ordering::Relaxed);
        self.sample_rate.store(0, Ordering::Relaxed);
        self.channels.store(0, Ordering::Relaxed);
        self.peak_bits.store(0, Ordering::Relaxed);
    }

    #[must_use]
    pub fn is_playing(&self) -> bool {
        self.playing.load(Ordering::Acquire)
    }

    #[must_use]
    pub fn is_paused(&self) -> bool {
        self.paused.load(Ordering::Acquire)
    }

    #[must_use]
    pub fn is_finished(&self) -> bool {
        self.finished.load(Ordering::Acquire)
    }

    #[must_use]
    pub fn position_ms(&self) -> u64 {
        self.position_ms.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn duration_ms(&self) -> u64 {
        self.duration_ms.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn sample_rate(&self) -> u32 {
        self.sample_rate.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn channels(&self) -> u32 {
        self.channels.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn peak(&self) -> f32 {
        f32::from_bits(self.peak_bits.load(Ordering::Relaxed))
    }
}

/// Owns a running playback session.
pub struct Player {
    stop: Arc<AtomicBool>,
    stop_event: SendHandle,
    render_thread: Option<JoinHandle<Result<(), YipError>>>,
    decoder_thread: Option<JoinHandle<Result<(), YipError>>>,
    path: PathBuf,
}

/// What the decoder learned about the file before anything was negotiated.
#[derive(Debug, Clone, Copy)]
struct SourceFormat {
    sample_rate: u32,
    channels: u16,
    duration_ms: u64,
}

impl Player {
    /// Open `path`, negotiate an endpoint format and start rendering.
    ///
    /// Returns once audio is flowing, so a file the decoder cannot open fails
    /// the call instead of surfacing as silence.
    pub fn start(path: &Path) -> Result<Self, YipError> {
        PLAYBACK.reset();

        let stop = Arc::new(AtomicBool::new(false));
        let (mut producer, mut consumer) = split(PLAY_RING_CAPACITY_SAMPLES);

        // SAFETY: manual-reset = false (auto-reset), initial state = nonsignaled.
        let stop_event_raw = unsafe { CreateEventW(None, false, false, None)? };
        let stop_event = SendHandle(stop_event_raw);
        let stop_event_for_render = SendHandle(stop_event_raw);

        // Two handshakes: the decoder reports the file's own format, and the
        // render thread answers with the format it managed to open.
        let (source_tx, source_rx) = channel::<Result<SourceFormat, YipError>>();
        let (negotiated_tx, negotiated_rx) = channel::<(u32, u16)>();
        let (rendering_tx, rendering_rx) = channel::<Result<(), YipError>>();
        let (decoding_tx, decoding_rx) = channel::<Result<(), YipError>>();

        let path_owned = path.to_path_buf();
        let stop_for_decoder = stop.clone();
        let decoder_thread = std::thread::Builder::new()
            .name("yip-decoder".into())
            .spawn(move || -> Result<(), YipError> {
                let result = decoder_loop(
                    &path_owned,
                    &mut producer,
                    &stop_for_decoder,
                    &source_tx,
                    &negotiated_rx,
                    &decoding_tx,
                );
                // However this ended, nothing more is coming. The render thread
                // plays out what it holds and stops, rather than waiting on a
                // producer that is already gone.
                PLAYBACK.eof.store(true, Ordering::Release);
                result
            })
            .map_err(|e| YipError::Io(format!("spawn decoder: {e}")))?;

        // Own the decoder from here on: every early return below drops `player`,
        // and Drop stops and joins whatever has been started.
        let mut player = Self {
            stop,
            stop_event,
            render_thread: None,
            decoder_thread: Some(decoder_thread),
            path: path.to_path_buf(),
        };

        let source = source_rx
            .recv()
            .map_err(|_| YipError::Decoder("decoder thread died before ready".into()))??;
        PLAYBACK
            .duration_ms
            .store(source.duration_ms, Ordering::Relaxed);

        let stop_for_render = player.stop.clone();
        let render_thread = std::thread::Builder::new()
            .name("yip-render".into())
            .spawn(move || -> Result<(), YipError> {
                render_loop(
                    source,
                    &mut consumer,
                    &stop_for_render,
                    stop_event_for_render,
                    &negotiated_tx,
                    &rendering_tx,
                )
            })
            .map_err(|e| YipError::Wasapi(format!("spawn render: {e}")))?;
        player.render_thread = Some(render_thread);

        // Wait for the endpoint to open, so a device that refuses the file
        // fails the start rather than playing nothing.
        rendering_rx
            .recv()
            .map_err(|_| YipError::Wasapi("render thread died before ready".into()))??;

        // And for the decoder to accept that format. A source reader that will
        // not produce what the endpoint opened with would otherwise play as an
        // endless silence nobody can explain.
        decoding_rx
            .recv()
            .map_err(|_| YipError::Decoder("decoder thread died before ready".into()))??;

        Ok(player)
    }

    /// File this player is reading.
    #[must_use]
    pub fn path(&self) -> &Path {
        &self.path
    }

    /// Hold or release rendering without tearing the stream down. The endpoint
    /// keeps running on silence, exactly as capture keeps its stream running
    /// across a pause, so resuming costs nothing and the position freezes.
    pub fn set_paused(&self, paused: bool) {
        PLAYBACK.paused.store(paused, Ordering::Release);
    }

    /// Ask for a new position. Cheap and coalescing: the decoder acts on the
    /// latest target it sees, so a dragged scrubber costs one seek.
    pub fn seek(&self, ms: u64) {
        let duration = PLAYBACK.duration_ms.load(Ordering::Relaxed);
        let target = if duration > 0 { ms.min(duration) } else { ms };
        // Target before sequence: a decoder that sees the new sequence must
        // already be able to see where it is going.
        PLAYBACK.seek_target_ms.store(target, Ordering::Relaxed);
        PLAYBACK.seek_seq.fetch_add(1, Ordering::Release);
        // The take ended and the threads have wound down, or it is about to:
        // report the requested position so the UI does not snap back.
        if !PLAYBACK.playing.load(Ordering::Acquire) {
            PLAYBACK.position_ms.store(target, Ordering::Relaxed);
        }
    }

    pub fn stop(mut self) -> Result<(), YipError> {
        self.shutdown();
        let render = self
            .render_thread
            .take()
            .map(|h| h.join())
            .transpose()
            .map_err(|_| YipError::Wasapi("render thread panicked".into()))?;
        let decoder = self
            .decoder_thread
            .take()
            .map(|h| h.join())
            .transpose()
            .map_err(|_| YipError::Decoder("decoder thread panicked".into()))?;
        self.close_event();

        // Report the render thread's failure first: it is the one that would
        // have been audible.
        if let Some(res) = render {
            res?;
        }
        if let Some(res) = decoder {
            res?;
        }
        Ok(())
    }

    /// Signal both threads. Split out so `Drop` and `stop` ask the same way.
    fn shutdown(&mut self) {
        self.stop.store(true, Ordering::Release);
        PLAYBACK.playing.store(false, Ordering::Release);
        // Wake the render loop. Single SetEvent is enough on an auto-reset event.
        // SAFETY: stop_event handle was created in start() and is still alive.
        let _ = unsafe { windows::Win32::System::Threading::SetEvent(self.stop_event.0) };
    }

    fn close_event(&self) {
        // SAFETY: handle live, single close.
        unsafe {
            let _ = windows::Win32::Foundation::CloseHandle(self.stop_event.0);
        }
    }
}

impl Drop for Player {
    fn drop(&mut self) {
        // Best-effort stop if the caller dropped without calling stop().
        if self.render_thread.is_some() || self.decoder_thread.is_some() {
            self.shutdown();
            if let Some(h) = self.render_thread.take() {
                let _ = h.join();
            }
            if let Some(h) = self.decoder_thread.take() {
                let _ = h.join();
            }
            self.close_event();
        }
    }
}

// ---------- decoder thread body ----------

fn decoder_loop(
    path: &Path,
    producer: &mut rtrb::Producer<f32>,
    stop: &Arc<AtomicBool>,
    source_tx: &Sender<Result<SourceFormat, YipError>>,
    negotiated_rx: &std::sync::mpsc::Receiver<(u32, u16)>,
    ready_tx: &Sender<Result<(), YipError>>,
) -> Result<(), YipError> {
    // Everything Media Foundation touches lives on this thread, so the reader
    // is never marshalled and the runtime is torn down where it was started.
    let mut decoder = match Decoder::open(path) {
        Ok(d) => d,
        Err(e) => {
            let _ = source_tx.send(Err(e.clone()));
            return Err(e);
        }
    };

    let source = SourceFormat {
        sample_rate: decoder.sample_rate(),
        channels: decoder.channels(),
        duration_ms: decoder.duration_ms(),
    };
    if source_tx.send(Ok(source)).is_err() {
        return Ok(()); // caller gave up before we were ready
    }

    // The render thread owns the choice: it is the one WASAPI can refuse.
    let Ok((rate, channels)) = negotiated_rx.recv() else {
        return Ok(());
    };
    if let Err(e) = decoder.set_output(rate, channels) {
        let _ = ready_tx.send(Err(e.clone()));
        return Err(e);
    }
    if ready_tx.send(Ok(())).is_err() {
        return Ok(()); // caller gave up while we were opening the codec
    }

    let samples_per_frame = usize::from(channels.max(1));
    let mut seen_seq = PLAYBACK.seek_seq.load(Ordering::Acquire);
    let mut scratch: Vec<f32> = Vec::new();
    // Leftover from the last sample that did not fit the ring in one go.
    let mut pending: usize = 0;

    loop {
        if stop.load(Ordering::Acquire) {
            break;
        }

        // Seeks take priority over decoding: the audio already in the ring is
        // about to be thrown away anyway.
        let seq = PLAYBACK.seek_seq.load(Ordering::Acquire);
        if seq != seen_seq {
            seen_seq = seq;
            let target = PLAYBACK.seek_target_ms.load(Ordering::Relaxed);
            flush_for_seek(producer, stop);
            scratch.clear();
            pending = 0;
            decoder.seek(target)?;
            // Base before the flag: a render thread that sees `flushing` clear
            // must already be able to read where it now is.
            PLAYBACK.seek_base_ms.store(target, Ordering::Relaxed);
            PLAYBACK.position_ms.store(target, Ordering::Relaxed);
            PLAYBACK.eof.store(false, Ordering::Release);
            PLAYBACK.flushing.store(false, Ordering::Release);
            continue;
        }

        // Push whatever is left over before asking for more.
        if pending > 0 {
            let start = scratch.len() - pending;
            let written = push_frames(producer, &scratch[start..], samples_per_frame);
            pending -= written;
            if pending > 0 {
                std::thread::sleep(std::time::Duration::from_millis(POLL_MS));
            }
            continue;
        }

        if decoder.read(&mut scratch)? {
            pending = scratch.len();
            if pending == 0 {
                continue;
            }
            let written = push_frames(producer, &scratch, samples_per_frame);
            pending -= written;
        } else {
            // End of file. The render thread plays out what is already in the
            // ring and then stops; this thread parks until it is asked to seek
            // or to quit, so scrubbing back after the end still works.
            PLAYBACK.eof.store(true, Ordering::Release);
            std::thread::sleep(std::time::Duration::from_millis(POLL_MS * 8));
        }
    }

    Ok(())
}

/// Copy as many whole frames as the ring will take. Returns samples written.
///
/// Whole frames only: a partial frame would rotate the channel interleave for
/// everything after it.
fn push_frames(producer: &mut rtrb::Producer<f32>, src: &[f32], samples_per_frame: usize) -> usize {
    let room = producer.slots().min(src.len());
    let writable = room - (room % samples_per_frame);
    if writable == 0 {
        return 0;
    }
    let Ok(mut chunk) = producer.write_chunk_uninit(writable) else {
        return 0;
    };
    let (slot_a, slot_b) = chunk.as_mut_slices();
    let len_a = slot_a.len();
    let len_b = slot_b.len();
    // SAFETY: MaybeUninit<f32> shares f32's layout, `src` and the ring slots
    // never overlap, and len_a + len_b == writable <= src.len().
    unsafe {
        std::ptr::copy_nonoverlapping(src.as_ptr(), slot_a.as_mut_ptr().cast::<f32>(), len_a);
        std::ptr::copy_nonoverlapping(
            src.as_ptr().add(len_a),
            slot_b.as_mut_ptr().cast::<f32>(),
            len_b,
        );
    }
    // SAFETY: every slot was initialised by the copies above.
    unsafe { chunk.commit_all() };
    writable
}

/// Ask the render thread to drop everything it holds, and wait for it to say
/// it has. The producer cannot discard what the consumer owns, so a seek has
/// to be agreed rather than imposed.
fn flush_for_seek(producer: &mut rtrb::Producer<f32>, stop: &Arc<AtomicBool>) {
    PLAYBACK.drained.store(false, Ordering::Release);
    PLAYBACK.flushing.store(true, Ordering::Release);

    let deadline = std::time::Instant::now() + std::time::Duration::from_millis(FLUSH_TIMEOUT_MS);
    while !PLAYBACK.drained.load(Ordering::Acquire) {
        if stop.load(Ordering::Acquire) || std::time::Instant::now() >= deadline {
            break;
        }
        // Nothing left to drain and nobody is rendering: don't wait on an
        // acknowledgement that is not coming.
        let empty = producer.slots() == producer.buffer().capacity();
        if empty && !PLAYBACK.playing.load(Ordering::Acquire) {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(POLL_MS));
    }
}

// ---------- render thread body ----------

#[allow(clippy::cast_ptr_alignment)] // SAFETY: WASAPI GetBuffer guarantees f32-aligned data
fn render_loop(
    source: SourceFormat,
    consumer: &mut rtrb::Consumer<f32>,
    stop: &Arc<AtomicBool>,
    stop_event: SendHandle,
    negotiated_tx: &Sender<(u32, u16)>,
    ready_tx: &Sender<Result<(), YipError>>,
) -> Result<(), YipError> {
    // SAFETY: per-thread COM init; balanced by the ComGuard below.
    let hr = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) };
    if hr.is_err() && hr.0 != windows::Win32::Foundation::RPC_E_CHANGED_MODE.0 {
        let e = YipError::Wasapi(format!("CoInitializeEx 0x{:08X}", hr.0 as u32));
        let _ = negotiated_tx.send((source.sample_rate, source.channels));
        let _ = ready_tx.send(Err(e.clone()));
        return Err(e);
    }
    let _com = ComGuard;

    let opened = open_endpoint(source);
    let Endpoint {
        client,
        render,
        audio_event,
        buffer_frames,
        sample_rate,
        channels,
    } = match opened {
        Ok(ep) => ep,
        Err(e) => {
            // Unblock the decoder before reporting: it is parked on the
            // negotiated format and would otherwise never see the stop.
            let _ = negotiated_tx.send((source.sample_rate, source.channels));
            let _ = ready_tx.send(Err(e.clone()));
            return Err(e);
        }
    };

    PLAYBACK.sample_rate.store(sample_rate, Ordering::Relaxed);
    PLAYBACK.channels.store(u32::from(channels), Ordering::Relaxed);
    if negotiated_tx.send((sample_rate, channels)).is_err() {
        return Ok(());
    }

    // Promote to Pro Audio: the render loop has the same deadline the capture
    // loop does, and misses it the same audible way.
    let mut task_index: u32 = 0;
    // SAFETY: PCWSTR points at a static null-terminated UTF-16 literal.
    let mmcss = unsafe {
        AvSetMmThreadCharacteristicsW(
            windows::core::w!("Pro Audio"),
            std::ptr::addr_of_mut!(task_index),
        )
    };
    let _mmcss_guard = mmcss.ok().map(MmcssGuard);

    let samples_per_frame = usize::from(channels.max(1));
    preroll(consumer, stop, buffer_frames as usize * samples_per_frame);

    // SAFETY: client live; this transitions the endpoint to running.
    if let Err(e) = unsafe { client.Start() } {
        let err = YipError::from(e);
        let _ = ready_tx.send(Err(err.clone()));
        return Err(err);
    }
    PLAYBACK.playing.store(true, Ordering::Release);
    let _ = ready_tx.send(Ok(()));

    let handles = [audio_event, stop_event.0];
    let mut base_ms = PLAYBACK.seek_base_ms.load(Ordering::Relaxed);
    let mut frames_since_base: u64 = 0;
    let mut peak: f32 = 0.0;
    // Set once the ring has run dry after EOF; from then on the loop is only
    // waiting for the endpoint to play out what it was already handed.
    let mut playing_out = false;

    let result = loop {
        // SAFETY: handles array is in scope for the duration of the call.
        let wait = unsafe { WaitForMultipleObjects(&handles, false, INFINITE) };
        let idx = wait.0.wrapping_sub(WAIT_OBJECT_0.0);
        if idx == 1 || stop.load(Ordering::Acquire) {
            break Ok(());
        }
        if idx != 0 {
            break Err(YipError::Wasapi(format!("Wait failed 0x{:08X}", wait.0)));
        }

        // SAFETY: client live.
        let padding = match unsafe { client.GetCurrentPadding() } {
            Ok(p) => p,
            Err(e) => break Err(YipError::from(e)),
        };
        // Playing out the tail: writing more silence would keep the buffer
        // topped up and padding would never reach zero.
        if playing_out {
            if padding == 0 {
                PLAYBACK.finished.store(true, Ordering::Release);
                break Ok(());
            }
            continue;
        }
        let frames = buffer_frames.saturating_sub(padding);
        if frames == 0 {
            continue;
        }

        // SAFETY: client live; `frames` is at most the free space reported
        // above, which is what GetBuffer requires.
        let data = match unsafe { render.GetBuffer(frames) } {
            Ok(p) => p,
            Err(e) => break Err(YipError::from(e)),
        };
        let len = frames as usize * samples_per_frame;
        // SAFETY: between GetBuffer and ReleaseBuffer the endpoint buffer is
        // ours for `frames` frames, and the stream format was negotiated to
        // f32 above.
        let dst: &mut [f32] = unsafe { std::slice::from_raw_parts_mut(data.cast::<f32>(), len) };

        let flushing = PLAYBACK.flushing.load(Ordering::Acquire);
        let paused = PLAYBACK.paused.load(Ordering::Acquire);
        let mut written = 0usize;

        if flushing {
            // Drop everything the decoder is about to make stale, then say so.
            let held = consumer.slots();
            if held > 0 {
                if let Ok(chunk) = consumer.read_chunk(held) {
                    chunk.commit_all();
                }
            }
            PLAYBACK.drained.store(true, Ordering::Release);
        } else if !paused {
            written = pop_frames(consumer, dst, samples_per_frame);
        }

        // Anything not filled is silence: a pause, a flush, or the ring
        // running dry all sound better empty than repeated.
        dst[written..].fill(0.0);
        // Peak ballistics while the buffer is still ours: instant attack so a
        // transient registers, slow release so it stays readable.
        peak = envelope(peak, &dst[..written], frames, sample_rate);
        let silent = written == 0;
        let flags = if silent {
            AUDCLNT_BUFFERFLAGS_SILENT.0 as u32
        } else {
            0
        };
        // SAFETY: paired with GetBuffer above; `frames` frames were handed out
        // and every sample of them is initialised.
        if let Err(e) = unsafe { render.ReleaseBuffer(frames, flags) } {
            break Err(YipError::from(e));
        }

        // Coming out of a flush the position is the decoder's, not ours.
        if flushing {
            base_ms = PLAYBACK.seek_base_ms.load(Ordering::Relaxed);
            frames_since_base = 0;
            peak = 0.0;
            PLAYBACK.peak_bits.store(0, Ordering::Relaxed);
            continue;
        }

        if !paused && written > 0 {
            frames_since_base += (written / samples_per_frame) as u64;
            let played = frames_since_base * 1000 / u64::from(sample_rate.max(1));
            PLAYBACK
                .position_ms
                .store(base_ms.saturating_add(played), Ordering::Relaxed);
        }

        PLAYBACK.peak_bits.store(peak.to_bits(), Ordering::Relaxed);

        if silent && !paused && PLAYBACK.eof.load(Ordering::Acquire) && consumer.slots() == 0 {
            playing_out = true;
        }
    };

    PLAYBACK.playing.store(false, Ordering::Release);
    PLAYBACK.peak_bits.store(0, Ordering::Relaxed);
    // SAFETY: client live; idempotent on a running endpoint.
    let _ = unsafe { client.Stop() };
    // SAFETY: audio_event was created by open_endpoint.
    unsafe {
        let _ = windows::Win32::Foundation::CloseHandle(audio_event);
    }
    result
}

/// Copy whole frames out of the ring. Returns samples written.
fn pop_frames(
    consumer: &mut rtrb::Consumer<f32>,
    dst: &mut [f32],
    samples_per_frame: usize,
) -> usize {
    let held = consumer.slots().min(dst.len());
    let readable = held - (held % samples_per_frame);
    if readable == 0 {
        return 0;
    }
    let Ok(chunk) = consumer.read_chunk(readable) else {
        return 0;
    };
    let (src_a, src_b) = chunk.as_slices();
    dst[..src_a.len()].copy_from_slice(src_a);
    dst[src_a.len()..src_a.len() + src_b.len()].copy_from_slice(src_b);
    chunk.commit_all();
    readable
}

/// One pass of peak ballistics over the block just rendered.
fn envelope(previous: f32, block: &[f32], frames: u32, sample_rate: u32) -> f32 {
    let mut peak = 0.0f32;
    for s in block {
        let a = s.abs();
        if a > peak {
            peak = a;
        }
    }
    if peak >= previous {
        return peak;
    }
    // Same 0.35 s release the capture meter uses, applied per block so there
    // is no exp() on the audio path.
    let seconds = frames as f32 / sample_rate.max(1) as f32;
    let decay = 1.0 - (seconds / RELEASE_SECONDS).min(1.0);
    (previous * decay).max(peak)
}

/// Wait for the decoder to get ahead before the endpoint starts, so playback
/// does not open on a gap. Gives up quietly: a short file that is already at
/// EOF, or a slow codec, both start anyway.
fn preroll(consumer: &rtrb::Consumer<f32>, stop: &Arc<AtomicBool>, want: usize) {
    let deadline = std::time::Instant::now() + std::time::Duration::from_millis(PREROLL_TIMEOUT_MS);
    while consumer.slots() < want {
        if stop.load(Ordering::Acquire)
            || PLAYBACK.eof.load(Ordering::Acquire)
            || std::time::Instant::now() >= deadline
        {
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(POLL_MS));
    }
}

/// An opened render endpoint and everything the loop needs to drive it.
struct Endpoint {
    client: IAudioClient,
    render: IAudioRenderClient,
    audio_event: HANDLE,
    buffer_frames: u32,
    sample_rate: u32,
    channels: u16,
}

/// Open the default render endpoint at the file's own format.
///
/// Shared mode converts rate and channel count behind `AUTOCONVERTPCM`, which
/// is what lets a 96 kHz mono take play on a 48 kHz stereo endpoint without a
/// resampler of our own. A device that refuses it falls back to the engine mix
/// format, and the decoder is told to produce that instead.
fn open_endpoint(source: SourceFormat) -> Result<Endpoint, YipError> {
    let device = default_render_device()?;
    // SAFETY: standard activation of WASAPI client.
    let client: IAudioClient = unsafe { device.Activate::<IAudioClient>(CLSCTX_ALL, None)? };

    // SAFETY: live client; returns a CoTaskMem-allocated pointer.
    let fmt_ptr = unsafe { client.GetMixFormat()? };
    let mix = parse_format(fmt_ptr);

    // Audio event (auto-reset, nonsignaled).
    // SAFETY: standard event creation.
    let audio_event = unsafe { CreateEventW(None, false, false, None)? };

    let flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
        | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
        | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    let mut client = client;
    let mut sample_rate = source.sample_rate;
    let mut channels = source.channels;
    let want = float_wfx(sample_rate, channels);
    // SAFETY: client live; `want` outlives the call; flags valid.
    let first = unsafe {
        client.Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            flags,
            RENDER_BUFFER_100NS,
            0,
            std::ptr::addr_of!(want),
            None,
        )
    };

    if first.is_err() {
        // Initialize consumed this client even on failure. Get a new one and
        // take the engine's own format; the decoder resamples to match.
        let (mix_rate, mix_channels) = match mix {
            Ok(m) => m,
            Err(e) => {
                // SAFETY: pointer from GetMixFormat, freed exactly once.
                unsafe { CoTaskMemFree(Some(fmt_ptr.cast())) };
                return Err(e);
            }
        };
        // SAFETY: device is still live.
        client = unsafe { device.Activate::<IAudioClient>(CLSCTX_ALL, None)? };
        // SAFETY: client live; fmt_ptr live; flags valid.
        let second = unsafe {
            client.Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                RENDER_BUFFER_100NS,
                0,
                fmt_ptr,
                None,
            )
        };
        // SAFETY: pointer from GetMixFormat, freed exactly once, after the
        // last Initialize that reads it.
        unsafe { CoTaskMemFree(Some(fmt_ptr.cast())) };
        second?;
        sample_rate = mix_rate;
        channels = mix_channels;
    } else {
        // SAFETY: pointer from GetMixFormat, freed exactly once.
        unsafe { CoTaskMemFree(Some(fmt_ptr.cast())) };
    }

    // SAFETY: client live; handle live.
    unsafe { client.SetEventHandle(audio_event)? };
    // SAFETY: client live and initialised.
    let buffer_frames = unsafe { client.GetBufferSize()? };
    // SAFETY: client live and initialised.
    let render: IAudioRenderClient = unsafe { client.GetService::<IAudioRenderClient>()? };

    Ok(Endpoint {
        client,
        render,
        audio_event,
        buffer_frames,
        sample_rate,
        channels,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn envelope_attacks_instantly() {
        assert!((envelope(0.0, &[0.5, -0.8], 480, 48_000) - 0.8).abs() < 1e-6);
    }

    #[test]
    fn envelope_releases_gradually() {
        // 10 ms of silence out of a 350 ms release: most of the level stands.
        let after = envelope(1.0, &[0.0; 960], 480, 48_000);
        assert!(after < 1.0, "must fall");
        assert!(after > 0.9, "must not fall off a cliff: {after}");
    }

    #[test]
    fn state_resets_between_takes() {
        let s = PlaybackState::new();
        s.playing.store(true, Ordering::Release);
        s.position_ms.store(1234, Ordering::Relaxed);
        s.reset();
        assert!(!s.is_playing());
        assert_eq!(s.position_ms(), 0);
    }

    #[test]
    fn pop_frames_keeps_the_interleave() {
        let (mut p, mut c) = split(16);
        for i in 0..6 {
            p.push(i as f32).unwrap();
        }
        let mut dst = [0.0f32; 5];
        // Five slots, stereo: four samples move, the odd one waits for its pair.
        assert_eq!(pop_frames(&mut c, &mut dst, 2), 4);
        assert_eq!(dst[..4], [0.0, 1.0, 2.0, 3.0]);
    }
}
