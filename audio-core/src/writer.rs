//! Writer thread. Drains the SPSC ring into the chosen encoding: WAV through
//! `hound`, FLAC / MP3 / M4A through Media Foundation. **Not** realtime: a slow
//! disk or encoder only back-pressures the ring; the capture thread never
//! blocks here.

use std::fs::File;
use std::io::BufWriter;
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Sender;
use std::time::Duration;

use hound::{SampleFormat, WavSpec, WavWriter};
use rtrb::Consumer;

use crate::error::YipError;
use crate::format::{Encoding, PcmConverter, SampleDepth};
use crate::mf::MfSink;

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
    pub encoding: Encoding,
}

/// Where drained samples go.
enum Sink {
    Wav(WavSink),
    Encoded(MfSink),
}

impl Sink {
    fn open(cfg: &WriterConfig) -> Result<Self, YipError> {
        if let Encoding::Wav(depth) = cfg.encoding {
            return Ok(Self::Wav(WavSink::open(cfg, depth)?));
        }
        let sink = MfSink::open(&cfg.path, cfg.sample_rate, cfg.channels, cfg.encoding)?;
        Ok(Self::Encoded(sink))
    }

    fn write(&mut self, a: &[f32], b: &[f32]) -> Result<(), YipError> {
        match self {
            Self::Wav(sink) => sink.write(a, b),
            Self::Encoded(sink) => sink.write(a, b),
        }
    }

    fn finish(self) -> Result<(), YipError> {
        match self {
            Self::Wav(sink) => sink.writer.finalize().map_err(YipError::from),
            Self::Encoded(sink) => sink.finish(),
        }
    }
}

struct WavSink {
    writer: WavWriter<BufWriter<File>>,
    depth: SampleDepth,
    conv: PcmConverter,
}

impl WavSink {
    fn open(cfg: &WriterConfig, depth: SampleDepth) -> Result<Self, YipError> {
        let sample_format = match depth {
            SampleDepth::Float32 => SampleFormat::Float,
            SampleDepth::Int16 | SampleDepth::Int24 => SampleFormat::Int,
        };
        let spec = WavSpec {
            channels: cfg.channels,
            sample_rate: cfg.sample_rate,
            bits_per_sample: depth.bits(),
            sample_format,
        };
        let file = File::create(&cfg.path)?;
        let buf = BufWriter::with_capacity(FILE_BUFFER_BYTES, file);
        Ok(Self {
            writer: WavWriter::new(buf, spec)?,
            depth,
            conv: PcmConverter::new(depth),
        })
    }

    fn write(&mut self, a: &[f32], b: &[f32]) -> Result<(), YipError> {
        let samples = a.iter().chain(b).copied();
        match self.depth {
            SampleDepth::Float32 => {
                for s in samples {
                    self.writer.write_sample(s)?;
                }
            }
            SampleDepth::Int24 => {
                for s in samples {
                    self.writer.write_sample(PcmConverter::int24(s))?;
                }
            }
            SampleDepth::Int16 => {
                for s in samples {
                    let v = self.conv.int16(s);
                    self.writer.write_sample(v)?;
                }
            }
        }
        Ok(())
    }
}

/// Drives the writer thread loop. Returns the first error encountered, or
/// `Ok(samples_written)` on clean stop.
///
/// The output is opened before anything is drained, and the outcome is sent on
/// `ready` so `Recorder::start` can refuse the take up front — an encoder that
/// rejects the format, or a folder that cannot be written, is reported when the
/// user presses Record rather than when they press Stop.
///
/// `stop` is set by the parent when the user requests stop *and* the capture
/// thread has finished pushing its last buffer. Until both conditions hold the
/// writer keeps draining — and once it is set, every remaining sample is
/// written regardless of batch size.
pub fn run_writer(
    mut consumer: Consumer<f32>,
    cfg: WriterConfig,
    stop: Arc<AtomicBool>,
    ready: Sender<Result<(), YipError>>,
) -> Result<u64, YipError> {
    let mut sink = match Sink::open(&cfg) {
        Ok(sink) => {
            let _ = ready.send(Ok(()));
            sink
        }
        Err(e) => {
            let _ = ready.send(Err(e.clone()));
            return Err(e);
        }
    };
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
        sink.write(a, b)?;
        total += (a.len() + b.len()) as u64;
        chunk.commit_all();
    }

    sink.finish()?;
    Ok(total)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ring::split;
    use std::sync::Arc;
    use std::sync::mpsc::channel;
    use std::thread;
    use tempfile::tempdir;

    type Reader = hound::WavReader<std::io::BufReader<File>>;

    /// Push a ramp through the writer and reopen the result. The directory is
    /// returned too so the file outlives the call.
    fn record_wav(depth: SampleDepth) -> (tempfile::TempDir, Reader) {
        let dir = tempdir().unwrap();
        let path = dir.path().join("t.wav");
        let (mut p, c) = split(1024);

        for i in 0..512 {
            let v = (i as f32) / 512.0;
            p.push(v - 0.5).unwrap();
        }

        let stop = Arc::new(AtomicBool::new(false));
        let stop2 = stop.clone();
        let (ready_tx, ready_rx) = channel();
        let cfg = WriterConfig {
            path: path.clone(),
            sample_rate: 48_000,
            channels: 1,
            encoding: Encoding::Wav(depth),
        };
        let h = thread::spawn(move || run_writer(c, cfg, stop2, ready_tx));
        ready_rx.recv().unwrap().unwrap();
        thread::sleep(Duration::from_millis(20));
        stop.store(true, Ordering::Release);
        let n = h.join().unwrap().unwrap();
        assert_eq!(n, 512);

        let reader = hound::WavReader::open(&path).unwrap();
        (dir, reader)
    }

    #[test]
    fn writer_drains_and_finalizes() {
        let (_dir, mut reader) = record_wav(SampleDepth::Float32);
        assert_eq!(reader.spec().sample_rate, 48_000);
        assert_eq!(reader.spec().bits_per_sample, 32);
        let samples: Vec<f32> = reader.samples::<f32>().map(Result::unwrap).collect();
        assert_eq!(samples.len(), 512);
    }

    #[test]
    fn writes_integer_wav_at_16_and_24_bits() {
        let (_dir, mut reader) = record_wav(SampleDepth::Int24);
        assert_eq!(reader.spec().bits_per_sample, 24);
        assert_eq!(reader.spec().sample_format, SampleFormat::Int);
        let samples: Vec<i32> = reader.samples::<i32>().map(Result::unwrap).collect();
        assert_eq!(samples.len(), 512);
        assert_eq!(samples[0], PcmConverter::int24(-0.5));

        let (_dir16, mut reader) = record_wav(SampleDepth::Int16);
        assert_eq!(reader.spec().bits_per_sample, 16);
        let samples: Vec<i16> = reader.samples::<i16>().map(Result::unwrap).collect();
        assert_eq!(samples.len(), 512);
        // -0.5 is -16383.5 LSB; TPDF dither may land it one step either way.
        assert!((-16_385..=-16_382).contains(&samples[0]));
    }

    #[test]
    fn unwritable_path_is_reported_before_the_take() {
        let dir = tempdir().unwrap();
        let (_p, c) = split(16);
        let (ready_tx, ready_rx) = channel();
        let cfg = WriterConfig {
            path: dir.path().join("missing").join("t.wav"),
            sample_rate: 48_000,
            channels: 2,
            encoding: Encoding::Wav(SampleDepth::Float32),
        };
        let stop = Arc::new(AtomicBool::new(false));
        let h = thread::spawn(move || run_writer(c, cfg, stop, ready_tx));
        assert!(ready_rx.recv().unwrap().is_err());
        assert!(h.join().unwrap().is_err());
    }
}
