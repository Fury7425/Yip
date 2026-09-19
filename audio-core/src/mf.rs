//! Media Foundation encoder sink: FLAC, MP3 and M4A (AAC).
//!
//! Every codec here ships with Windows, so encoding stays offline and adds no
//! dependency. Lives on the writer thread only — the sink writer is allowed to
//! block, and does when the encoder falls behind, which back-pressures the ring
//! exactly as a slow disk does for WAV.
//!
//! The app delay-loads `mfplat.dll`, `mfreadwrite.dll` and `mf.dll`, because
//! Windows N and KN editions ship without them. [`MfSink::open`] probes for
//! them before touching any Media Foundation entry point: a missing DLL becomes
//! a readable error instead of a delay-load exception no Rust code can catch.

use std::os::windows::ffi::OsStrExt;
use std::path::Path;

use windows::Win32::Foundation::{E_POINTER, FreeLibrary, HANDLE};
use windows::Win32::Media::MediaFoundation as mf;
use windows::Win32::System::Com::{COINIT_MULTITHREADED, CoInitializeEx, CoUninitialize};
use windows::Win32::System::LibraryLoader::{LOAD_LIBRARY_SEARCH_SYSTEM32, LoadLibraryExW};
use windows::core::{Error as WinError, GUID, Interface, PCWSTR, Result as WinResult, w};

use crate::error::YipError;
use crate::format::{Encoding, PcmConverter, SampleDepth};

/// 100-ns media time per second.
const HNS_PER_SECOND: u128 = 10_000_000;

const MF_MISSING: &str = "Media Foundation is not installed. Windows N and KN editions need the \
                          Media Feature Pack for FLAC, MP3 and M4A; WAV works without it.";

fn encoder_error(what: &str, e: &WinError) -> YipError {
    YipError::Encoder(format!(
        "{what}: HRESULT 0x{:08X}: {}",
        e.code().0,
        e.message()
    ))
}

/// Tags a Media Foundation failure with the step that produced it.
trait Context<T> {
    fn context(self, what: &str) -> Result<T, YipError>;
}

impl<T> Context<T> for WinResult<T> {
    fn context(self, what: &str) -> Result<T, YipError> {
        self.map_err(|e| encoder_error(what, &e))
    }
}

/// COM (MTA) and Media Foundation, started for this thread. Declared last in
/// [`MfSink`] so it drops after every Media Foundation object; the decoder
/// side ([`crate::decode`]) holds one the same way.
pub(crate) struct Runtime;

impl Runtime {
    pub(crate) fn start() -> Result<Self, YipError> {
        media_foundation_present()?;
        // SAFETY: per-thread COM init on the writer thread, which nothing else
        // has initialised; balanced in Drop.
        let hr = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) };
        if hr.is_err() {
            return Err(YipError::Encoder(format!(
                "CoInitializeEx 0x{:08X}",
                hr.0 as u32
            )));
        }
        // SAFETY: COM is initialised on this thread; balanced in Drop.
        if let Err(e) = unsafe { mf::MFStartup(mf::MF_VERSION, mf::MFSTARTUP_LITE) } {
            // SAFETY: balances the successful CoInitializeEx above.
            unsafe { CoUninitialize() };
            return Err(encoder_error("MFStartup", &e));
        }
        Ok(Self)
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        // SAFETY: balances MFStartup and CoInitializeEx from `start`, on the
        // same thread, after every Media Foundation object has been released.
        unsafe {
            let _ = mf::MFShutdown();
            CoUninitialize();
        }
    }
}

fn media_foundation_present() -> Result<(), YipError> {
    let flags = LOAD_LIBRARY_SEARCH_SYSTEM32;
    for dll in [w!("mfplat.dll"), w!("mfreadwrite.dll"), w!("mf.dll")] {
        // SAFETY: `dll` is a static null-terminated UTF-16 literal.
        let loaded = unsafe { LoadLibraryExW(dll, HANDLE::default(), flags) };
        let Ok(module) = loaded else {
            return Err(YipError::Encoder(MF_MISSING.into()));
        };
        // SAFETY: handle from the successful load above, released once.
        let _ = unsafe { FreeLibrary(module) };
    }
    Ok(())
}

/// A running Media Foundation sink writer fed with integer PCM.
pub struct MfSink {
    writer: mf::IMFSinkWriter,
    stream: u32,
    conv: PcmConverter,
    /// Reused across batches; grows to the largest batch once.
    scratch: Vec<u8>,
    block_align: usize,
    sample_rate: u32,
    frames_written: u64,
    _runtime: Runtime,
}

impl MfSink {
    /// Create `path` and prepare the encoder. Fails here — before the take
    /// starts — if the encoder refuses the rate, channel count or quality.
    pub fn open(
        path: &Path,
        sample_rate: u32,
        channels: u16,
        encoding: Encoding,
    ) -> Result<Self, YipError> {
        let (container, subtype) = match encoding {
            Encoding::Flac(_) => (mf::MFTranscodeContainerType_FLAC, mf::MFAudioFormat_FLAC),
            Encoding::Mp3 { .. } => (mf::MFTranscodeContainerType_MP3, mf::MFAudioFormat_MP3),
            Encoding::M4a { .. } => (mf::MFTranscodeContainerType_MPEG4, mf::MFAudioFormat_AAC),
            Encoding::Wav(_) => {
                return Err(YipError::InvalidState(
                    "WAV is not a Media Foundation format",
                ));
            }
        };
        let depth = encoding.depth();
        let runtime = Runtime::start()?;

        let output = output_type(encoding, subtype, sample_rate, channels);
        let output = output.context("output type")?;
        let input = pcm_type(sample_rate, channels, depth).context("input type")?;

        let wide: Vec<u16> = path
            .as_os_str()
            .encode_wide()
            .chain(std::iter::once(0))
            .collect();
        let writer = create_writer(&wide, &container).context("sink writer")?;

        let stream = match begin_writing(&writer, &output, &input) {
            Ok(stream) => stream,
            Err(e) => {
                // The sink writer has already created the file; a refused
                // format should not leave an empty take in the list.
                drop(writer);
                let _ = std::fs::remove_file(path);
                let what = format!("{} at {sample_rate} Hz", describe(encoding));
                return Err(encoder_error(&what, &e));
            }
        };

        Ok(Self {
            writer,
            stream,
            conv: PcmConverter::new(depth),
            scratch: Vec::new(),
            block_align: usize::from(channels.max(1)) * depth.bytes(),
            sample_rate,
            frames_written: 0,
            _runtime: runtime,
        })
    }

    /// Encode one drained batch. `a` then `b` is the ring's wrap-around split.
    pub fn write(&mut self, a: &[f32], b: &[f32]) -> Result<(), YipError> {
        self.scratch.clear();
        self.conv.extend_bytes(a, &mut self.scratch);
        self.conv.extend_bytes(b, &mut self.scratch);
        if self.scratch.is_empty() {
            return Ok(());
        }
        let len = u32::try_from(self.scratch.len())
            .map_err(|_| YipError::Encoder("batch larger than 4 GiB".into()))?;
        let frames = (self.scratch.len() / self.block_align) as u64;

        // Timestamps come from the running frame count, never from summed
        // per-batch durations, so rounding cannot drift over a long take.
        let start = self.hns(self.frames_written);
        let end = self.hns(self.frames_written + frames);
        self.submit(len, start, end - start)
            .context("WriteSample")?;
        self.frames_written += frames;
        Ok(())
    }

    /// Flush the encoder and close the file.
    pub fn finish(self) -> Result<(), YipError> {
        // SAFETY: live sink writer; called once, after the last WriteSample.
        unsafe { self.writer.Finalize() }.context("Finalize")
    }

    fn hns(&self, frames: u64) -> i64 {
        (u128::from(frames) * HNS_PER_SECOND / u128::from(self.sample_rate.max(1))) as i64
    }

    fn submit(&self, len: u32, time: i64, duration: i64) -> WinResult<()> {
        let mut dst: *mut u8 = std::ptr::null_mut();
        // SAFETY: every call is on a live COM object owned by this thread.
        // Lock hands out at least `len` writable bytes until Unlock, the copy
        // is exactly `len` bytes from `scratch`, and the two never overlap.
        unsafe {
            let buffer = mf::MFCreateMemoryBuffer(len)?;
            buffer.Lock(std::ptr::addr_of_mut!(dst), None, None)?;
            std::ptr::copy_nonoverlapping(self.scratch.as_ptr(), dst, self.scratch.len());
            buffer.Unlock()?;
            buffer.SetCurrentLength(len)?;

            let sample = mf::MFCreateSample()?;
            sample.AddBuffer(&buffer)?;
            sample.SetSampleTime(time)?;
            sample.SetSampleDuration(duration)?;
            self.writer.WriteSample(self.stream, &sample)
        }
    }
}

fn describe(encoding: Encoding) -> String {
    match encoding {
        Encoding::Wav(d) => format!("WAV {}-bit", d.bits()),
        Encoding::Flac(d) => format!("FLAC {}-bit", d.bits()),
        Encoding::Mp3 { kbps } => format!("MP3 {kbps} kbps"),
        Encoding::M4a { kbps } => format!("M4A {kbps} kbps"),
    }
}

fn create_writer(path_wide: &[u16], container: &GUID) -> WinResult<mf::IMFSinkWriter> {
    let mut attrs: Option<mf::IMFAttributes> = None;
    let byte_stream: Option<&mf::IMFByteStream> = None;
    // SAFETY: `attrs` is a valid out-slot; `path_wide` is null-terminated and
    // outlives the call.
    unsafe {
        mf::MFCreateAttributes(std::ptr::addr_of_mut!(attrs), 1)?;
        let attrs = attrs.ok_or_else(|| WinError::from(E_POINTER))?;
        // Name the container outright: the sink writer's extension lookup
        // does not know `.m4a` or `.flac`.
        attrs.SetGUID(&mf::MF_TRANSCODE_CONTAINERTYPE, container)?;
        mf::MFCreateSinkWriterFromURL(PCWSTR(path_wide.as_ptr()), byte_stream, &attrs)
    }
}

fn begin_writing(
    writer: &mf::IMFSinkWriter,
    output: &mf::IMFMediaType,
    input: &mf::IMFMediaType,
) -> WinResult<u32> {
    let encoding_params: Option<&mf::IMFAttributes> = None;
    // SAFETY: live COM objects owned by this thread.
    unsafe {
        let stream = writer.AddStream(output)?;
        writer.SetInputMediaType(stream, input, encoding_params)?;
        writer.BeginWriting()?;
        Ok(stream)
    }
}

/// Interleaved little-endian integer PCM, the input every encoder here takes.
fn pcm_type(sample_rate: u32, channels: u16, depth: SampleDepth) -> WinResult<mf::IMFMediaType> {
    let bits = u32::from(depth.bits());
    let block = u32::from(channels) * bits / 8;
    // SAFETY: fresh media type owned by this thread; GUID keys are statics.
    unsafe {
        let t = mf::MFCreateMediaType()?;
        t.SetGUID(&mf::MF_MT_MAJOR_TYPE, &mf::MFMediaType_Audio)?;
        t.SetGUID(&mf::MF_MT_SUBTYPE, &mf::MFAudioFormat_PCM)?;
        t.SetUINT32(&mf::MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate)?;
        t.SetUINT32(&mf::MF_MT_AUDIO_NUM_CHANNELS, u32::from(channels))?;
        t.SetUINT32(&mf::MF_MT_AUDIO_BITS_PER_SAMPLE, bits)?;
        t.SetUINT32(&mf::MF_MT_AUDIO_BLOCK_ALIGNMENT, block)?;
        t.SetUINT32(&mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sample_rate * block)?;
        Ok(t)
    }
}

/// The encoded type to ask for.
///
/// Prefers one the installed encoder advertises for this rate and channel
/// count — closest bitrate for the lossy codecs, matching bit depth for FLAC —
/// because an encoder may want private attributes (AAC payload, MP3 user data)
/// a hand-built type would lack. Falls back to building the type by hand when
/// the encoder advertises nothing, which some only do once an input is set.
fn output_type(
    encoding: Encoding,
    subtype: GUID,
    sample_rate: u32,
    channels: u16,
) -> WinResult<mf::IMFMediaType> {
    let target_bytes = match encoding {
        Encoding::Mp3 { kbps } | Encoding::M4a { kbps } => Some(kbps * 1000 / 8),
        Encoding::Wav(_) | Encoding::Flac(_) => None,
    };
    let bits = u32::from(encoding.depth().bits());

    let mut best: Option<(u32, mf::IMFMediaType)> = None;
    for t in list_types(&subtype).unwrap_or_default() {
        if get_u32(&t, &mf::MF_MT_AUDIO_SAMPLES_PER_SECOND) != Some(sample_rate) {
            continue;
        }
        if get_u32(&t, &mf::MF_MT_AUDIO_NUM_CHANNELS) != Some(u32::from(channels)) {
            continue;
        }
        let score = match target_bytes {
            Some(want) => match get_u32(&t, &mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND) {
                Some(have) => have.abs_diff(want),
                None => continue,
            },
            None => match get_u32(&t, &mf::MF_MT_AUDIO_BITS_PER_SAMPLE) {
                Some(have) if have == bits => 0,
                Some(_) => continue,
                None => 1,
            },
        };
        if best.as_ref().is_none_or(|(s, _)| score < *s) {
            best = Some((score, t));
        }
    }
    if let Some((_, t)) = best {
        return Ok(t);
    }

    // SAFETY: fresh media type owned by this thread; GUID keys are statics.
    unsafe {
        let t = mf::MFCreateMediaType()?;
        t.SetGUID(&mf::MF_MT_MAJOR_TYPE, &mf::MFMediaType_Audio)?;
        t.SetGUID(&mf::MF_MT_SUBTYPE, &raw const subtype)?;
        t.SetUINT32(&mf::MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate)?;
        t.SetUINT32(&mf::MF_MT_AUDIO_NUM_CHANNELS, u32::from(channels))?;
        match target_bytes {
            Some(bytes) if subtype == mf::MFAudioFormat_MP3 => {
                t.SetUINT32(&mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes)?;
                t.SetUINT32(&mf::MF_MT_AUDIO_BLOCK_ALIGNMENT, 1)?;
            }
            Some(bytes) => {
                t.SetUINT32(&mf::MF_MT_AUDIO_AVG_BYTES_PER_SECOND, bytes)?;
                t.SetUINT32(&mf::MF_MT_AUDIO_BITS_PER_SAMPLE, 16)?;
            }
            None => t.SetUINT32(&mf::MF_MT_AUDIO_BITS_PER_SAMPLE, bits)?,
        }
        Ok(t)
    }
}

/// Output types the installed encoders advertise for `subtype`.
fn list_types(subtype: &GUID) -> WinResult<Vec<mf::IMFMediaType>> {
    let flags = mf::MFT_ENUM_FLAG_ALL.0 as u32;
    let config: Option<&mf::IMFAttributes> = None;
    // SAFETY: `subtype` is a valid GUID for the call; the collection and its
    // elements are live COM objects owned by this thread.
    unsafe {
        let list = mf::MFTranscodeGetAudioOutputAvailableTypes(subtype, flags, config)?;
        let count = list.GetElementCount()?;
        let types = (0..count)
            .filter_map(|i| list.GetElement(i).ok()?.cast::<mf::IMFMediaType>().ok())
            .collect();
        Ok(types)
    }
}

fn get_u32(t: &mf::IMFMediaType, key: &GUID) -> Option<u32> {
    // SAFETY: live media type; `key` is a valid GUID for the call.
    unsafe { t.GetUINT32(key) }.ok()
}

#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::tempdir;

    /// True when the machine has no Media Foundation or no encoder for the
    /// codec at all (Windows N, Server Core), as opposed to an encoder that
    /// exists and refused what it was given — which must still fail.
    fn unavailable(e: &YipError) -> bool {
        let YipError::Encoder(msg) = e else {
            return false;
        };
        // MF_E_TOPO_CODEC_NOT_FOUND, REGDB_E_CLASSNOTREG.
        msg == MF_MISSING || msg.contains("0xC00D5212") || msg.contains("0x80040154")
    }

    /// One second of a 440 Hz stereo tone through an encoder.
    fn encode(encoding: Encoding, ext: &str, sample_rate: u32) {
        let dir = tempdir().unwrap();
        let path = dir.path().join(format!("t.{ext}"));
        let mut sink = match MfSink::open(&path, sample_rate, 2, encoding) {
            Ok(sink) => sink,
            Err(e) if unavailable(&e) => {
                eprintln!("skipping {ext}: {e}");
                return;
            }
            Err(e) => panic!("open {ext}: {e}"),
        };

        let step = 440.0 * std::f32::consts::TAU / sample_rate as f32;
        let mut tone = Vec::with_capacity(sample_rate as usize * 2);
        for i in 0..sample_rate {
            let s = (i as f32 * step).sin() * 0.5;
            tone.extend_from_slice(&[s, s]);
        }
        for block in tone.chunks(8192) {
            sink.write(block, &[]).unwrap();
        }
        sink.finish().unwrap();

        let size = std::fs::metadata(&path).unwrap().len();
        assert!(size > 1000, "{ext} is only {size} bytes");
    }

    #[test]
    fn encodes_flac_16_and_24() {
        encode(Encoding::Flac(SampleDepth::Int16), "flac", 48_000);
        encode(Encoding::Flac(SampleDepth::Int24), "flac", 96_000);
    }

    #[test]
    fn encodes_mp3() {
        encode(Encoding::Mp3 { kbps: 192 }, "mp3", 44_100);
        encode(Encoding::Mp3 { kbps: 320 }, "mp3", 48_000);
    }

    #[test]
    fn encodes_m4a() {
        encode(Encoding::M4a { kbps: 160 }, "m4a", 48_000);
    }
}
