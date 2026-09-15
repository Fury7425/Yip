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

/// Bytes buffered before the writer touches the filesystem. One second of
/// 48 kHz stereo float is ~384 KiB, so this turns a recording into a handful of
/// writes per second rather than one per ring drain.
const FILE_BUFFER_BYTES: usize = 256 * 1024;

/// Smallest batch the writer bothers to drain while capture is still running.
/// A WASAPI packet is ~480 frames; waiting for a few of them amortises the
/// chunk bookkeeping instead of paying it per packet. At 48 kHz stereo this is
/// ~85 ms of audio, against a ring that holds 5 s.
const MIN_DRAIN_SAMPLES: usize = 8192;

/// How long to park when there is nothing worth writing. Comfortably under the
/// 10 ms buffer period, so a healthy disk never lets the ring approach full.
const PARK: Duration = Duration::from_millis(2);

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
/// `Ok(samples_written)` on clean stop.
///
/// `stop` is set by the parent when the user requests stop *and* the capture
/// thread has finished pushing its last buffer. Until both conditions hold the
/// writer keeps draining — and once it is set, every remaining sample is
/// written regardless of batch size.
pub fn run_writer(
    mut consumer: Consumer<f32>,
    cfg: WriterConfig,
    stop: Arc<AtomicBool>,
) -> Result<u64, YipError> {
    let file = File::create(&cfg.path)?;
    let buf = BufWriter::with_capacity(FILE_BUFFER_BYTES, file);
    let mut writer = WavWriter::new(buf, cfg.spec())?;
    let mut total: u64 = 0;

    loop {
        // Read the flag first: anything the capture thread pushed before
        // setting it is already visible in `slots()` below, so the drain that
        // follows cannot miss the final packet.
        let stopping = stop.load(Ordering::Acquire);
        let n = consumer.slots();

        if n == 0 {
            if stopping {
                break;
            }
            std::thread::sleep(PARK);
            continue;
        }
        if n < MIN_DRAIN_SAMPLES && !stopping {
            // Let a few more packets pile up rather than paying the chunk
            // dance for each one. The ring has seconds of headroom.
            std::thread::sleep(PARK);
            continue;
        }

        // `read_chunk(n)` only fails if n > slots(), which cannot happen here.
        let chunk = consumer.read_chunk(n).map_err(|_| YipError::Overrun)?;
        let (a, b) = chunk.as_slices();
        for &s in a {
            writer.write_sample(s)?;
        }
        for &s in b {
            writer.write_sample(s)?;
        }
        total += (a.len() + b.len()) as u64;
        chunk.commit_all();
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
