//! Lock-free SPSC ring shared between capture and writer threads, plus the
//! shared atomic peak level. **No locks, no allocations on the audio path.**

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

use rtrb::{Consumer, Producer, RingBuffer};

/// Frames the ring can hold before back-pressure. 48 kHz stereo × 5 s ≈ 480 k
/// samples (1.9 MiB at f32). Plenty of headroom for slow disks.
pub const RING_CAPACITY_SAMPLES: usize = 480_000;

/// Atomic envelope of the realtime peak (decays slowly in the FFI getter).
#[derive(Debug)]
pub struct SharedMeter {
    /// Peak amplitude as `f32` bits.
    bits: AtomicU32,
    /// Sample count seen by capture thread (frames * channels). Wraps; only
    /// used as a "did we move at all" sentinel by tests.
    pub frames_captured: AtomicU64,
    /// True when capture has produced its first buffer. Lets the UI hide the
    /// meter zero before the device warms up.
    pub started: AtomicBool,
}

impl SharedMeter {
    #[must_use]
    pub fn new() -> Arc<Self> {
        Arc::new(Self {
            bits: AtomicU32::new(0),
            frames_captured: AtomicU64::new(0),
            started: AtomicBool::new(false),
        })
    }

    /// Write the higher of `peak` and the currently stored value. Realtime-safe.
    pub fn fold_peak(&self, peak: f32) {
        let new = peak.abs().to_bits();
        let mut cur = self.bits.load(Ordering::Relaxed);
        loop {
            let cur_f = f32::from_bits(cur);
            if peak.abs() <= cur_f {
                return;
            }
            match self
                .bits
                .compare_exchange_weak(cur, new, Ordering::Relaxed, Ordering::Relaxed)
            {
                Ok(_) => return,
                Err(actual) => cur = actual,
            }
        }
    }

    /// Read peak and reset to zero in one operation. Called from FFI poll path.
    pub fn take_peak(&self) -> f32 {
        f32::from_bits(self.bits.swap(0, Ordering::Relaxed))
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

    #[test]
    fn meter_fold_keeps_max() {
        let m = SharedMeter::new();
        m.fold_peak(0.3);
        m.fold_peak(0.5);
        m.fold_peak(0.2);
        assert!((m.take_peak() - 0.5).abs() < f32::EPSILON);
        assert_eq!(m.take_peak().to_bits(), 0_u32, "second read drains");
    }

    #[test]
    fn meter_handles_negative_amplitudes() {
        let m = SharedMeter::new();
        m.fold_peak(-0.8);
        m.fold_peak(0.4);
        assert!((m.take_peak() - 0.8).abs() < f32::EPSILON);
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
