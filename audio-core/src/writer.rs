//! Writer thread. Drains the SPSC ring into the chosen encoding: WAV through
//! `hound`, FLAC / MP3 / M4A through Media Foundation. **Not** realtime: a slow
//! disk or encoder only back-pressures the ring; the capture thread never
//! blocks here.

use std::fs::File;
use std::io::BufWriter;
use std::path::{Path, PathBuf};
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

/// Bounds on how long the writer parks while a batch piles up. The park is
/// sized to when `MIN_DRAIN_SAMPLES` will be there (~85 ms at 48 kHz stereo,
/// ~170 ms mono), so a drain costs one or two wake-ups instead of the half
/// dozen a fixed 15 ms park spent finding the ring not ready yet. The ceiling
/// keeps a slow wake-up (timer granularity is ~15.6 ms) far inside the ring's
/// seconds of headroom; the floor stops a high-rate, many-channel stream from
/// spinning.
const PARK_MIN: Duration = Duration::from_millis(2);
const PARK_MAX: Duration = Duration::from_millis(50);

/// Largest data chunk a single WAV file is allowed to reach. RIFF sizes are
/// 32-bit and `hound` counts in a `u32` that wraps silently in release builds,
/// so a take past 4 GiB (~3.1 h of 48 kHz stereo float) used to end with a
/// corrupt header. Kept well clear of `u32::MAX` so the header fits too; at
/// the limit the take carries on in a numbered continuation file.
const WAV_DATA_LIMIT: u64 = 0xFFF0_0000;

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
            return Ok(Self::Wav(WavSink::open(cfg, depth, WAV_DATA_LIMIT)?));
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
    spec: WavSpec,
    depth: SampleDepth,
    conv: PcmConverter,
    /// The take's own path; continuation files are named after it.
    path: PathBuf,
    /// 1 for the first file, 2 for the first continuation, and so on.
    part: u32,
    /// Data bytes in the current file.
    part_bytes: u64,
    /// Where the current file stops taking frames.
    limit: u64,
    frame_bytes: u64,
    /// Position inside the current frame, so a file is only ever cut between
    /// whole frames.
    in_frame: u16,
}

impl WavSink {
    fn open(cfg: &WriterConfig, depth: SampleDepth, limit: u64) -> Result<Self, YipError> {
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
        let channels = cfg.channels.max(1);
        Ok(Self {
            writer: create_wav(&cfg.path, spec)?,
            spec,
            depth,
            conv: PcmConverter::new(depth),
            path: cfg.path.clone(),
            part: 1,
            part_bytes: 0,
            limit,
            frame_bytes: depth.bytes() as u64 * u64::from(channels),
            in_frame: 0,
        })
    }

    fn write(&mut self, a: &[f32], b: &[f32]) -> Result<(), YipError> {
        let channels = self.spec.channels.max(1);
        let sample_bytes = self.depth.bytes() as u64;
        for s in a.iter().chain(b).copied() {
            if self.in_frame == 0 && self.part_bytes + self.frame_bytes > self.limit {
                self.roll()?;
            }
            match self.depth {
                SampleDepth::Float32 => self.writer.write_sample(s)?,
                SampleDepth::Int24 => self.writer.write_sample(PcmConverter::int24(s))?,
                SampleDepth::Int16 => {
                    let v = self.conv.int16(s);
                    self.writer.write_sample(v)?;
                }
            }
            self.part_bytes += sample_bytes;
            self.in_frame = (self.in_frame + 1) % channels;
        }
        Ok(())
    }

    /// Close the full file and carry on in the next one. The new file is
    /// opened first, so a failure leaves the finished part intact.
    fn roll(&mut self) -> Result<(), YipError> {
        let next = self.part + 1;
        let fresh = create_wav(&continuation_path(&self.path, next), self.spec)?;
        let full = std::mem::replace(&mut self.writer, fresh);
        self.part = next;
        self.part_bytes = 0;
        full.finalize()?;
        Ok(())
    }
}

fn create_wav(path: &Path, spec: WavSpec) -> Result<WavWriter<BufWriter<File>>, YipError> {
    let file = File::create(path)?;
    let buf = BufWriter::with_capacity(FILE_BUFFER_BYTES, file);
    Ok(WavWriter::new(buf, spec)?)
}

/// `take.wav` -> `take (part 2).wav`, beside it.
fn continuation_path(path: &Path, part: u32) -> PathBuf {
    let stem = path
        .file_stem()
        .map(|s| s.to_string_lossy().into_owned())
        .unwrap_or_default();
    let ext = path
        .extension()
        .map(|e| format!(".{}", e.to_string_lossy()))
        .unwrap_or_default();
    path.with_file_name(format!("{stem} (part {part}){ext}"))
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
///
/// A write that fails mid-take (disk full, folder gone) calls `on_fault` before
/// returning, so the host ends the take at once instead of recording into a
/// ring nobody drains.
pub fn run_writer(
    consumer: Consumer<f32>,
    cfg: WriterConfig,
    stop: Arc<AtomicBool>,
    ready: Sender<Result<(), YipError>>,
    on_fault: fn(),
) -> Result<u64, YipError> {
    let sink = match Sink::open(&cfg) {
        Ok(sink) => {
            let _ = ready.send(Ok(()));
            sink
        }
        Err(e) => {
            let _ = ready.send(Err(e.clone()));
            return Err(e);
        }
    };
    let samples_per_sec = u64::from(cfg.sample_rate) * u64::from(cfg.channels.max(1));
    let result = drain(consumer, sink, &stop, samples_per_sec);
    if result.is_err() && !stop.load(Ordering::Acquire) {
        on_fault();
    }
    result
}

/// How long until `MIN_DRAIN_SAMPLES` will have piled up, given `queued`
/// already waiting, clamped to [`PARK_MIN`, `PARK_MAX`].
fn park_for(queued: usize, samples_per_sec: u64) -> Duration {
    let missing = MIN_DRAIN_SAMPLES.saturating_sub(queued) as u64;
    let micros = missing.saturating_mul(1_000_000) / samples_per_sec.max(1);
    Duration::from_micros(micros).clamp(PARK_MIN, PARK_MAX)
}

fn drain(
    mut consumer: Consumer<f32>,
    mut sink: Sink,
    stop: &AtomicBool,
    samples_per_sec: u64,
) -> Result<u64, YipError> {
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
            std::thread::sleep(park_for(0, samples_per_sec));
            continue;
        }
        if n < MIN_DRAIN_SAMPLES && !stopping {
            // Let a few more packets pile up rather than paying the chunk
            // dance for each one. The ring has seconds of headroom.
            std::thread::sleep(park_for(n, samples_per_sec));
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

    fn no_fault() {}

    #[test]
    fn wav_rolls_over_into_a_continuation_file_between_frames() {
        let dir = tempdir().unwrap();
        let path = dir.path().join("take.wav");
        let cfg = WriterConfig {
            path: path.clone(),
            sample_rate: 48_000,
            channels: 2,
            encoding: Encoding::Wav(SampleDepth::Int16),
        };
        // Room for 3 stereo 16-bit frames (12 bytes), then a new file.
        let mut sink = WavSink::open(&cfg, SampleDepth::Int16, 13).unwrap();
        let samples = [0.0_f32; 10]; // 5 frames
        sink.write(&samples[..3], &samples[3..]).unwrap();
        sink.writer.finalize().unwrap();

        let first = hound::WavReader::open(&path).unwrap();
        assert_eq!(first.len(), 6, "first part holds three whole frames");
        let second = hound::WavReader::open(dir.path().join("take (part 2).wav")).unwrap();
        assert_eq!(second.len(), 4, "the rest carries on in part 2");
    }

    #[test]
    fn park_waits_for_the_batch_within_bounds() {
        // 48 kHz stereo: a whole batch is ~85 ms away, so the ceiling wins.
        assert_eq!(park_for(0, 96_000), PARK_MAX);
        // Most of a batch already queued: only the remainder is waited for.
        let half = park_for(MIN_DRAIN_SAMPLES - 960, 96_000);
        assert_eq!(half, Duration::from_millis(10));
        // A batch already there, or all but due, never parks below the floor.
        assert_eq!(park_for(MIN_DRAIN_SAMPLES, 96_000), PARK_MIN);
        assert_eq!(park_for(MIN_DRAIN_SAMPLES - 1, 96_000), PARK_MIN);
    }

    #[test]
    fn continuation_names_follow_the_take() {
        let p = continuation_path(Path::new("C:/rec/Yip 2026.wav"), 3);
        assert_eq!(p, Path::new("C:/rec/Yip 2026 (part 3).wav"));
    }

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
        let h = thread::spawn(move || run_writer(c, cfg, stop2, ready_tx, no_fault));
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
        let h = thread::spawn(move || run_writer(c, cfg, stop, ready_tx, no_fault));
        assert!(ready_rx.recv().unwrap().is_err());
        assert!(h.join().unwrap().is_err());
    }
}
