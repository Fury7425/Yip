//! Media Foundation decode source: WAV, FLAC, MP3 and M4A in, float PCM out.
//!
//! The mirror image of [`crate::mf`]. Every codec Yip writes, Windows can also
//! read, so playback needs a source reader and no decoder of its own. Lives on
//! the decoder thread only — `ReadSample` blocks on disk and on the codec, and
//! is allowed to.
//!
//! Windows N and KN editions ship without Media Foundation, so the runtime
//! probes for the DLLs before any entry point is touched and a missing one
//! becomes a readable error rather than a delay-load exception.

use std::os::windows::ffi::OsStrExt;
use std::path::Path;

use windows::Win32::Media::MediaFoundation as mf;
use windows::Win32::System::Variant::VT_I8;
use windows::core::{Error as WinError, GUID, PCWSTR, PROPVARIANT, Result as WinResult};

use crate::error::YipError;
use crate::mf::Runtime;

/// 100-ns media time per millisecond.
const HNS_PER_MS: u128 = 10_000;

/// Stream indices, unwrapped once. windows-rs wraps them in a newtype, while
/// every `IMFSourceReader` method that takes one takes a plain `u32`.
const FIRST_AUDIO_STREAM: u32 = mf::MF_SOURCE_READER_FIRST_AUDIO_STREAM.0;
const ALL_STREAMS: u32 = mf::MF_SOURCE_READER_ALL_STREAMS.0;
const MEDIA_SOURCE: u32 = mf::MF_SOURCE_READER_MEDIASOURCE.0;

/// `VARENUM` for an unsigned 64-bit PROPVARIANT, which is what MF_PD_DURATION
/// comes back as.
const VT_UI8_TAG: u16 = 21;

fn decoder_error(what: &str, e: &WinError) -> YipError {
    YipError::Decoder(format!(
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
        self.map_err(|e| decoder_error(what, &e))
    }
}

/// An open audio file, decoding to interleaved 32-bit float.
pub struct Decoder {
    // Declared before the runtime so the reader is released while Media
    // Foundation is still up.
    reader: mf::IMFSourceReader,
    sample_rate: u32,
    channels: u16,
    duration_ms: u64,
    _runtime: Runtime,
}

impl Decoder {
    /// Open `path` and read what the file says it holds. No output format is
    /// chosen yet: the endpoint picks that, and [`Decoder::set_output`]
    /// applies it.
    pub fn open(path: &Path) -> Result<Self, YipError> {
        if !path.is_file() {
            return Err(YipError::Io(format!("no such file: {}", path.display())));
        }
        let runtime = Runtime::start().map_err(rewrap)?;

        let wide: Vec<u16> = path
            .as_os_str()
            .encode_wide()
            .chain(std::iter::once(0))
            .collect();
        let attributes: Option<&mf::IMFAttributes> = None;
        // SAFETY: `wide` is null-terminated and outlives the call; Media
        // Foundation is started for this thread by `runtime`.
        let reader = unsafe { mf::MFCreateSourceReaderFromURL(PCWSTR(wide.as_ptr()), attributes) }
            .context("open file")?;

        // Audio only, and only the first audio stream: a stray video track
        // would otherwise be decoded for nothing.
        // SAFETY: live reader owned by this thread.
        unsafe {
            reader
                .SetStreamSelection(ALL_STREAMS, false)
                .context("deselect streams")?;
            reader
                .SetStreamSelection(FIRST_AUDIO_STREAM, true)
                .context("select audio stream")?;
        }

        // SAFETY: live reader; index 0 is the stream's own type.
        let native = unsafe { reader.GetNativeMediaType(FIRST_AUDIO_STREAM, 0) }
            .context("native media type")?;
        let sample_rate = get_u32(&native, &mf::MF_MT_AUDIO_SAMPLES_PER_SECOND).unwrap_or(0);
        let channels = get_u32(&native, &mf::MF_MT_AUDIO_NUM_CHANNELS).unwrap_or(0);
        if sample_rate == 0 || channels == 0 {
            return Err(YipError::Decoder(
                "file does not declare a sample rate or channel count".into(),
            ));
        }

        // Before the struct literal: the field initialisers move `reader`, and
        // the duration has to be read while it is still ours to borrow.
        let duration_ms = read_duration_ms(&reader);

        Ok(Self {
            reader,
            sample_rate,
            channels: channels as u16,
            duration_ms,
            _runtime: runtime,
        })
    }

    #[must_use]
    pub fn sample_rate(&self) -> u32 {
        self.sample_rate
    }

    #[must_use]
    pub fn channels(&self) -> u16 {
        self.channels
    }

    /// File length in ms, or 0 when the container does not declare one.
    #[must_use]
    pub fn duration_ms(&self) -> u64 {
        self.duration_ms
    }

    /// Ask for interleaved float at `sample_rate` / `channels`. The source
    /// reader inserts whatever decoder and converter that needs.
    pub fn set_output(&mut self, sample_rate: u32, channels: u16) -> Result<(), YipError> {
        let wanted = float_type(sample_rate, channels).context("float output type")?;
        // SAFETY: live reader and media type, both owned by this thread.
        unsafe {
            self.reader
                .SetCurrentMediaType(FIRST_AUDIO_STREAM, None, &wanted)
        }
        .context("set output type")?;

        // Confirm rather than assume: everything downstream reads frames out of
        // a flat buffer, and a channel count that is not the one the endpoint
        // opened with would rotate the interleave for the whole file.
        // SAFETY: live reader.
        let actual = unsafe { self.reader.GetCurrentMediaType(FIRST_AUDIO_STREAM) }
            .context("read back output type")?;
        let got_rate = get_u32(&actual, &mf::MF_MT_AUDIO_SAMPLES_PER_SECOND).unwrap_or(0);
        let got_channels = get_u32(&actual, &mf::MF_MT_AUDIO_NUM_CHANNELS).unwrap_or(0);
        if got_rate != sample_rate || got_channels != u32::from(channels) {
            return Err(YipError::Decoder(format!(
                "decoder produced {got_rate} Hz / {got_channels} ch, endpoint opened \
                 {sample_rate} Hz / {channels} ch"
            )));
        }

        self.sample_rate = sample_rate;
        self.channels = channels;
        Ok(())
    }

    /// Decode the next block into `out`. `Ok(false)` means end of file; an
    /// empty `out` with `Ok(true)` is a stream gap, not an end.
    #[allow(clippy::cast_ptr_alignment)] // SAFETY: float output type => f32-aligned buffer
    pub fn read(&mut self, out: &mut Vec<f32>) -> Result<bool, YipError> {
        out.clear();

        let mut flags: u32 = 0;
        let mut sample: Option<mf::IMFSample> = None;
        // SAFETY: live reader; both out-params are valid writable slots.
        unsafe {
            self.reader.ReadSample(
                FIRST_AUDIO_STREAM,
                0,
                None,
                Some(std::ptr::addr_of_mut!(flags)),
                None,
                Some(std::ptr::addr_of_mut!(sample)),
            )
        }
        .context("read sample")?;

        if flags & (mf::MF_SOURCE_READERF_ENDOFSTREAM.0 as u32) != 0 {
            return Ok(false);
        }
        // No sample and no end flag is a stream tick. Nothing to play, nothing
        // to report.
        let Some(sample) = sample else {
            return Ok(true);
        };

        // SAFETY: live sample owned by this thread.
        let buffer = unsafe { sample.ConvertToContiguousBuffer() }.context("contiguous buffer")?;
        let mut data: *mut u8 = std::ptr::null_mut();
        let mut len: u32 = 0;
        // SAFETY: live buffer; `data` and `len` are valid writable slots, and
        // the pointer is only valid until Unlock below.
        unsafe {
            buffer.Lock(
                std::ptr::addr_of_mut!(data),
                None,
                Some(std::ptr::addr_of_mut!(len)),
            )
        }
        .context("lock buffer")?;

        if !data.is_null() && len >= 4 {
            // SAFETY: Lock hands out `len` readable bytes until Unlock, and the
            // output type negotiated above is 32-bit float.
            let src = unsafe { std::slice::from_raw_parts(data.cast::<f32>(), len as usize / 4) };
            out.extend_from_slice(src);
        }
        // SAFETY: paired with the Lock above.
        let _ = unsafe { buffer.Unlock() };
        Ok(true)
    }

    /// Move to `ms`. The next [`Decoder::read`] comes back from there.
    pub fn seek(&mut self, ms: u64) -> Result<(), YipError> {
        let hns = i64::try_from(u128::from(ms) * HNS_PER_MS).unwrap_or(i64::MAX);
        let position = hns_propvariant(hns);
        // The null time format means 100-ns units, which is the only one every
        // source supports.
        let time_format = GUID::from_u128(0);
        // SAFETY: live reader; both pointers outlive the call.
        unsafe { self.reader.SetCurrentPosition(&time_format, &position) }.context("seek")
    }
}

/// File length in ms from the container, or 0 when it declares none — a
/// stream with no duration still plays, the scrubber just has nothing to span.
fn read_duration_ms(reader: &mf::IMFSourceReader) -> u64 {
    // SAFETY: live reader; MF_PD_DURATION is a static GUID key.
    let attribute = unsafe {
        reader.GetPresentationAttribute(MEDIA_SOURCE, &mf::MF_PD_DURATION)
    };
    let Ok(pv) = attribute else { return 0 };
    propvariant_u64(&pv).map_or(0, |hns| (u128::from(hns) / HNS_PER_MS) as u64)
}

/// Media Foundation's absence is reported by the encoder path as
/// [`YipError::Encoder`]; playback wants it phrased as a decode failure.
fn rewrap(e: YipError) -> YipError {
    match e {
        YipError::Encoder(msg) => YipError::Decoder(msg),
        other => other,
    }
}

/// Interleaved 32-bit float, the one format the render path understands.
fn float_type(sample_rate: u32, channels: u16) -> WinResult<mf::IMFMediaType> {
    // SAFETY: fresh media type owned by this thread; GUID keys are statics.
    unsafe {
        let t = mf::MFCreateMediaType()?;
        t.SetGUID(&mf::MF_MT_MAJOR_TYPE, &mf::MFMediaType_Audio)?;
        t.SetGUID(&mf::MF_MT_SUBTYPE, &mf::MFAudioFormat_Float)?;
        t.SetUINT32(&mf::MF_MT_AUDIO_SAMPLES_PER_SECOND, sample_rate)?;
        t.SetUINT32(&mf::MF_MT_AUDIO_NUM_CHANNELS, u32::from(channels))?;
        Ok(t)
    }
}

fn get_u32(t: &mf::IMFMediaType, key: &GUID) -> Option<u32> {
    // SAFETY: live media type; `key` is a valid GUID for the call.
    unsafe { t.GetUINT32(key) }.ok()
}

/// Build a `PROPVARIANT` holding a 100-ns media time.
///
/// windows-core 0.58 keeps the union opaque, so the value goes in through the
/// documented ABI layout — an 8-byte header, then the union — the same way
/// `devices::read_friendly_name` reads one back out. VT_I8 owns no memory, so
/// the `PropVariantClear` in `Drop` has nothing to free.
fn hns_propvariant(hns: i64) -> PROPVARIANT {
    // SAFETY: an all-zero PROPVARIANT is VT_EMPTY, which is a valid value and
    // safe to clear.
    let mut pv: PROPVARIANT = unsafe { std::mem::zeroed() };
    // SAFETY: the PROPVARIANT ABI is stable across Windows: `vt` at offset 0,
    // the value union at offset 8. Both writes stay inside the struct.
    unsafe {
        let base = std::ptr::addr_of_mut!(pv).cast::<u8>();
        base.cast::<u16>().write_unaligned(VT_I8.0 as u16);
        base.add(8).cast::<i64>().write_unaligned(hns);
    }
    pv
}

/// Read a VT_UI8 `PROPVARIANT` the same way, or `None` if it is something else.
fn propvariant_u64(pv: &PROPVARIANT) -> Option<u64> {
    // SAFETY: same stable layout as above, read rather than written.
    let (tag, value) = unsafe {
        let base = std::ptr::addr_of!(*pv).cast::<u8>();
        (
            base.cast::<u16>().read_unaligned(),
            base.add(8).cast::<u64>().read_unaligned(),
        )
    };
    (tag == VT_UI8_TAG).then_some(value)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hns_round_trips_through_the_union() {
        let pv = hns_propvariant(1_234_567);
        // SAFETY: the value was just written at the documented offset.
        let (tag, value) = unsafe {
            let base = std::ptr::addr_of!(pv).cast::<u8>();
            (
                base.cast::<u16>().read_unaligned(),
                base.add(8).cast::<i64>().read_unaligned(),
            )
        };
        assert_eq!(tag, VT_I8.0 as u16);
        assert_eq!(value, 1_234_567);
    }

    #[test]
    fn missing_file_is_an_io_error() {
        // `unwrap_err` would want Decoder: Debug, and a source reader has
        // nothing worth printing.
        let Err(e) = Decoder::open(Path::new("Z:/yip/does-not-exist.wav")) else {
            panic!("opened a file that is not there");
        };
        assert!(matches!(e, YipError::Io(_)), "{e}");
    }
}
