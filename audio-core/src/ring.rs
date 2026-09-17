//! Lock-free SPSC ring shared between capture and writer threads, plus the
//! process-wide meter block. **No locks, no allocations on the audio path.**
//!
//! The meter is a `static` rather than an `Arc` handed out per session: every
//! reader (both UI windows, any future consumer) sees the same atomics with no
//! mutex in the way, and `rec_peak_level` costs a relaxed load.

use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use rtrb::{Consumer, Producer, RingBuffer};

/// Frames the ring can hold before back-pressure. 48 kHz stereo × 5 s ≈ 480 k
/// samples (1.9 MiB at f32). Plenty of headroom for slow disks.
pub const RING_CAPACITY_SAMPLES: usize = 480_000;

/// Frames the playback ring holds between the decoder and the render thread.
/// 48 kHz stereo x 2 s. Shorter than the capture ring on purpose: every sample
/// buffered here is one a seek has to throw away.
pub const PLAY_RING_CAPACITY_SAMPLES: usize = 192_000;

/// Meter release time: a peak decays to silence in roughly this long once the
/// source stops. Classic "fast attack, slow release" ballistics, applied on the
/// capture thread as two flops per block — no `exp()` in the audio path.
const RELEASE_SECONDS: f32 = 0.35;

/// One-pole smoothing applied to the block RMS. Blocks are ~10 ms, so this
/// averages over roughly 150 ms — steady enough to read as a number.
const RMS_SMOOTHING: f32 = 0.25;

/// Amplitude at or above which a sample counts as clipped. Float captures can
/// exceed 1.0, so this is a report, not a limit.
pub const CLIP_THRESHOLD: f32 = 0.999;

/// Per-packet statistics gathered by the capture thread in one pass over the
/// WASAPI buffer, before the data reaches the ring.
#[derive(Debug, Clone, Copy, Default)]
pub struct BlockStats {
    /// Largest absolute sample in the packet.
    pub peak: f32,
    /// Sum of squares, for the RMS readout.
    pub sum_sq: f32,
    /// Samples analysed (frames × channels).
    pub samples: u32,
    /// Samples at or beyond [`CLIP_THRESHOLD`].
    pub clips: u32,
}

/// Atomic snapshot of capture health. Written only by the capture thread, read
/// by anyone. Reads are non-destructive, so two meters polling at different
/// rates never steal peaks from each other.
#[derive(Debug)]
pub struct SharedMeter {
    /// Decaying peak envelope as `f32` bits.
    peak_bits: AtomicU32,
    /// Smoothed RMS as `f32` bits.
    rms_bits: AtomicU32,
    /// Largest peak seen since [`SharedMeter::reset`], as `f32` bits.
    session_peak_bits: AtomicU32,
    /// Samples at or beyond [`CLIP_THRESHOLD`] this session.
    clip_count: AtomicU32,
    /// Packets the ring had no room for.
    overrun_count: AtomicU32,
    /// Frames lost to those overruns.
    dropped_frames: AtomicU64,
    /// Frames handed to the ring.
    frames_captured: AtomicU64,
    /// True once capture has produced its first packet. Lets the UI tell
    /// "silent" apart from "device still warming up".
    started: AtomicBool,
}

/// The one meter. Reset at the start of every session.
pub static METER: SharedMeter = SharedMeter::new();

impl Default for SharedMeter {
    fn default() -> Self {
        Self::new()
    }
}

impl SharedMeter {
    #[must_use]
    pub const fn new() -> Self {
        Self {
            peak_bits: AtomicU32::new(0),
            rms_bits: AtomicU32::new(0),
            session_peak_bits: AtomicU32::new(0),
            clip_count: AtomicU32::new(0),
            overrun_count: AtomicU32::new(0),
            dropped_frames: AtomicU64::new(0),
            frames_captured: AtomicU64::new(0),
            started: AtomicBool::new(false),
        }
    }

    /// Clear every counter. Called before a session starts, never during one.
    pub fn reset(&self) {
        self.peak_bits.store(0, Ordering::Relaxed);
        self.rms_bits.store(0, Ordering::Relaxed);
        self.session_peak_bits.store(0, Ordering::Relaxed);
        self.clip_count.store(0, Ordering::Relaxed);
        self.overrun_count.store(0, Ordering::Relaxed);
        self.dropped_frames.store(0, Ordering::Relaxed);
        self.frames_captured.store(0, Ordering::Relaxed);
        self.started.store(false, Ordering::Release);
    }

    /// Drop the live readings to zero while keeping the session totals. Called
    /// on stop so a re-opened meter does not flash the last packet's level.
    pub fn silence(&self) {
        self.peak_bits.store(0, Ordering::Relaxed);
        self.rms_bits.store(0, Ordering::Relaxed);
    }

    /// Fold one capture packet into the meter. **Capture thread only** — it is
    /// the single writer, so the read-modify-writes below are plain loads and
    /// stores rather than CAS loops.
    pub fn push_block(&self, block: BlockStats, frames: u64, sample_rate: u32) {
        // Release ballistics: walk the envelope down by the elapsed frame
        // count, then let this packet's peak pull it straight back up.
        let mut env = f32::from_bits(self.peak_bits.load(Ordering::Relaxed));
        if sample_rate > 0 {
            let per_frame = 1.0 / (RELEASE_SECONDS * sample_rate as f32);
            env -= per_frame * frames as f32;
        }
        if env < 0.0 || !env.is_finite() {
            env = 0.0;
        }
        if block.peak > env {
            env = block.peak;
        }
        self.peak_bits.store(env.to_bits(), Ordering::Relaxed);

        let session = f32::from_bits(self.session_peak_bits.load(Ordering::Relaxed));
        if block.peak > session {
            self.session_peak_bits
                .store(block.peak.to_bits(), Ordering::Relaxed);
        }

        let block_rms = if block.samples > 0 {
            (block.sum_sq / block.samples as f32).sqrt()
        } else {
            0.0
        };
        let mut rms = f32::from_bits(self.rms_bits.load(Ordering::Relaxed));
        if !rms.is_finite() {
            rms = 0.0;
        }
        rms += (block_rms - rms) * RMS_SMOOTHING;
        self.rms_bits.store(rms.to_bits(), Ordering::Relaxed);

        if block.clips > 0 {
            self.clip_count.fetch_add(block.clips, Ordering::Relaxed);
        }
        if frames > 0 {
            self.frames_captured.fetch_add(frames, Ordering::Relaxed);
            self.started.store(true, Ordering::Release);
        }
    }

    /// Record a packet the ring had no room for. `frames` is what was lost.
    pub fn note_overrun(&self, frames: u64) {
        self.overrun_count.fetch_add(1, Ordering::Relaxed);
        if frames > 0 {
            self.dropped_frames.fetch_add(frames, Ordering::Relaxed);
        }
    }

    /// Current envelope, 0.0..=1.0-ish (float capture can exceed 1.0).
    /// Non-destructive: any number of readers may poll it.
    #[must_use]
    pub fn peak(&self) -> f32 {
        f32::from_bits(self.peak_bits.load(Ordering::Relaxed))
    }

    #[must_use]
    pub fn rms(&self) -> f32 {
        f32::from_bits(self.rms_bits.load(Ordering::Relaxed))
    }

    #[must_use]
    pub fn session_peak(&self) -> f32 {
        f32::from_bits(self.session_peak_bits.load(Ordering::Relaxed))
    }

    #[must_use]
    pub fn clip_count(&self) -> u32 {
        self.clip_count.load(Ordering::Relaxed)
    }

    pub fn reset_clip(&self) {
        self.clip_count.store(0, Ordering::Relaxed);
        self.session_peak_bits.store(0, Ordering::Relaxed);
    }

    #[must_use]
    pub fn overrun_count(&self) -> u32 {
        self.overrun_count.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn dropped_frames(&self) -> u64 {
        self.dropped_frames.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn frames_captured(&self) -> u64 {
        self.frames_captured.load(Ordering::Relaxed)
    }

    #[must_use]
    pub fn started(&self) -> bool {
        self.started.load(Ordering::Acquire)
    }
}

/// Analyse one WASAPI packet: peak, energy and clip count in a single pass the
/// optimiser can vectorise. Runs on the capture thread — no allocation, no
/// branching beyond the compare-selects below.
#[must_use]
pub fn analyse(src: &[f32]) -> BlockStats {
    let mut peak = 0.0_f32;
    let mut sum_sq = 0.0_f32;
    let mut clips = 0_u32;
    for &s in src {
        let a = s.abs();
        if a > peak {
            peak = a;
        }
        sum_sq += s * s;
        clips += u32::from(a >= CLIP_THRESHOLD);
    }
    BlockStats {
        peak,
        sum_sq,
        samples: src.len() as u32,
        clips,
    }
}

/// SPSC ring of interleaved `f32` samples. Producer = capture thread,
/// Consumer = writer thread.
pub struct AudioRing {
    pub producer: Producer<f32>,
    pub consumer: Consumer<f32>,
}

impl AudioRing {
    #[must_use]
    pub fn new(capacity: usize) -> Self {
        let (producer, consumer) = RingBuffer::<f32>::new(capacity);
        Self { producer, consumer }
    }
}

impl Default for AudioRing {
    fn default() -> Self {
        Self::new(RING_CAPACITY_SAMPLES)
    }
}

/// Split the ring into the two halves that travel to different threads.
#[must_use]
pub fn split(capacity: usize) -> (Producer<f32>, Consumer<f32>) {
    RingBuffer::<f32>::new(capacity)
}

#[cfg(test)]
mod tests {
    use super::*;

    const SR: u32 = 48_000;

    #[test]
    fn envelope_attacks_instantly() {
        let m = SharedMeter::new();
        m.push_block(
            BlockStats {
                peak: 0.5,
                sum_sq: 0.0,
                samples: 0,
                clips: 0,
            },
            480,
            SR,
        );
        assert!((m.peak() - 0.5).abs() < 1e-6, "peak was {}", m.peak());
    }

    #[test]
    fn envelope_reads_are_not_destructive() {
        let m = SharedMeter::new();
        m.push_block(
            BlockStats {
                peak: 0.5,
                ..BlockStats::default()
            },
            480,
            SR,
        );
        let first = m.peak();
        assert!(
            (m.peak() - first).abs() < f32::EPSILON,
            "second read drained"
        );
    }

    #[test]
    fn envelope_releases_towards_silence() {
        let m = SharedMeter::new();
        m.push_block(
            BlockStats {
                peak: 1.0,
                ..BlockStats::default()
            },
            480,
            SR,
        );
        // Silence for a full release window must land on zero.
        for _ in 0..40 {
            m.push_block(BlockStats::default(), 480, SR);
        }
        assert!(m.peak() <= 0.0, "envelope never reached silence");
    }

    #[test]
    fn session_peak_survives_release() {
        let m = SharedMeter::new();
        m.push_block(
            BlockStats {
                peak: 0.9,
                ..BlockStats::default()
            },
            480,
            SR,
        );
        for _ in 0..40 {
            m.push_block(BlockStats::default(), 480, SR);
        }
        assert!(m.peak() <= 0.0);
        assert!((m.session_peak() - 0.9).abs() < 1e-6);
    }

    #[test]
    fn analyse_reports_peak_energy_and_clips() {
        let src = [0.0_f32, -0.5, 1.0, 0.25];
        let s = analyse(&src);
        assert!((s.peak - 1.0).abs() < f32::EPSILON);
        assert_eq!(s.samples, 4);
        assert_eq!(s.clips, 1, "1.0 is at full scale");
        assert!((s.sum_sq - (0.25 + 1.0 + 0.0625)).abs() < 1e-6);
    }

    #[test]
    fn analyse_ignores_sign() {
        let s = analyse(&[-0.8, 0.4]);
        assert!((s.peak - 0.8).abs() < f32::EPSILON);
        assert_eq!(s.clips, 0);
    }

    #[test]
    fn rms_tracks_a_steady_tone() {
        let m = SharedMeter::new();
        // A constant 0.5 amplitude block has RMS 0.5; the one-pole should
        // converge onto it.
        for _ in 0..100 {
            m.push_block(
                BlockStats {
                    peak: 0.5,
                    sum_sq: 0.25 * 960.0,
                    samples: 960,
                    clips: 0,
                },
                480,
                SR,
            );
        }
        assert!((m.rms() - 0.5).abs() < 0.01, "rms was {}", m.rms());
    }

    #[test]
    fn overruns_do_not_touch_the_level() {
        let m = SharedMeter::new();
        m.push_block(
            BlockStats {
                peak: 0.2,
                ..BlockStats::default()
            },
            480,
            SR,
        );
        m.note_overrun(480);
        assert!(m.peak() < 0.25, "an overrun must not read as a clip");
        assert_eq!(m.overrun_count(), 1);
        assert_eq!(m.dropped_frames(), 480);
    }

    #[test]
    fn reset_clears_everything() {
        let m = SharedMeter::new();
        m.push_block(
            BlockStats {
                peak: 1.0,
                sum_sq: 1.0,
                samples: 1,
                clips: 1,
            },
            480,
            SR,
        );
        m.note_overrun(10);
        m.reset();
        assert!(m.peak() <= 0.0);
        assert!(m.rms() <= 0.0);
        assert!(m.session_peak() <= 0.0);
        assert_eq!(m.clip_count(), 0);
        assert_eq!(m.overrun_count(), 0);
        assert_eq!(m.dropped_frames(), 0);
        assert_eq!(m.frames_captured(), 0);
        assert!(!m.started());
    }

    #[test]
    fn ring_capacity_round_trip() {
        let (mut p, mut c) = split(8);
        for i in 0..6 {
            p.push(i as f32).unwrap();
        }
        let mut out = Vec::new();
        while let Ok(v) = c.pop() {
            out.push(v);
        }
        assert_eq!(out, vec![0.0, 1.0, 2.0, 3.0, 4.0, 5.0]);
    }

    #[test]
    fn ring_blocks_at_capacity() {
        let (mut p, mut _c) = split(2);
        assert!(p.push(0.0).is_ok());
        assert!(p.push(1.0).is_ok());
        assert!(p.push(2.0).is_err(), "third push must fail");
    }
}
