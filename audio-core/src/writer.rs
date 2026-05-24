//! WAV writer thread. Drains the SPSC ring, writes interleaved `f32` PCM via
//! `hound`. **Not** realtime: a slow disk only back-pressures the ring; the
//! capture thread never blocks here.

use std::fs::File;
use std::io::BufWriter;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::Duration;

use hound::{SampleFormat, WavSpec, WavWriter};
use rtrb::Consumer;

use crate::error::YipError;

pub struct WriterConfig {
    pub path: PathBuf,
    pub sample_rate: u32,
    pub channels: u16,
}

impl WriterConfig {
    fn spec(&self) -> WavSpec {
        WavSpec {
            channels: self.channels,
            sample_rate: self.sample_rate,
            bits_per_sample: 32,
            sample_format: SampleFormat::Float,
        }
    }
}

/// Drives the writer thread loop. Returns the first error encountered, or
/// `Ok(frames_written)` on clean stop.
///
/// `stop` is set by the parent when the user requests stop *and* the capture
/// thread has finished pushing its last buffer. Until both conditions hold the
/// writer keeps draining.
pub fn run_writer(
    mut consumer: Consumer<f32>,
    cfg: WriterConfig,
    stop: Arc<AtomicBool>,
) -> Result<u64, YipError> {
    let file = File::create(&cfg.path)?;
    let buf = BufWriter::with_capacity(64 * 1024, file);
    let mut writer = WavWriter::new(buf, cfg.spec())?;
    let mut total: u64 = 0;

    loop {
        let n = consumer.slots();
        if n > 0 {
            // `read_chunk(n)` only fails if n > slots(), which cannot happen here.
            let chunk = consumer.read_chunk(n).map_err(|_| YipError::Overrun)?;
            let (a, b) = chunk.as_slices();
            for &s in a {
                writer.write_sample(s)?;
                total += 1;
            }
            for &s in b {
                writer.write_sample(s)?;
                total += 1;
            }
            chunk.commit_all();
        } else if stop.load(Ordering::Acquire) {
            break;
        } else {
            // Ring is empty and capture is still running. Park briefly —
            // not a hot loop: we wake on every audio buffer (~10 ms).
            std::thread::sleep(Duration::from_millis(2));
        }
    }

    writer.finalize()?;
    Ok(total)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ring::split;
    use std::sync::Arc;
    use std::thread;
    use tempfile::tempdir;

    #[test]
    fn writer_drains_and_finalizes() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("t.wav");
        let (mut p, c) = split(1024);

        for i in 0..512 {
            let v = (i as f32) / 512.0;
            p.push(v - 0.5).unwrap();
        }

        let stop = Arc::new(AtomicBool::new(false));
        let stop2 = stop.clone();
        let cfg = WriterConfig {
            path: path.clone(),
            sample_rate: 48_000,
            channels: 1,
        };
        let h = thread::spawn(move || run_writer(c, cfg, stop2));
        thread::sleep(Duration::from_millis(20));
        stop.store(true, Ordering::Release);
        let n = h.join().unwrap().unwrap();
        assert_eq!(n, 512);

        let mut reader = hound::WavReader::open(&path).unwrap();
        assert_eq!(reader.spec().sample_rate, 48_000);
        assert_eq!(reader.spec().bits_per_sample, 32);
        let samples: Vec<f32> = reader.samples::<f32>().map(Result::unwrap).collect();
        assert_eq!(samples.len(), 512);
    }
}
