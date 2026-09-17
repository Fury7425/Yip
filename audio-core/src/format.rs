//! What the writer produces, decoded from [`RecConfig`], plus the float →
//! integer PCM conversion every non-float encoding needs.
//!
//! Pure data and arithmetic: nothing here touches a file or a COM object, so
//! all of it is unit-testable. Runs on the writer thread, never the capture
//! thread.

use crate::error::YipError;
use crate::ffi::{RecConfig, RecFormat};

/// Sample width handed to a file or an encoder.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SampleDepth {
    Int16,
    Int24,
    Float32,
}

impl SampleDepth {
    pub const fn bits(self) -> u16 {
        match self {
            Self::Int16 => 16,
            Self::Int24 => 24,
            Self::Float32 => 32,
        }
    }

    pub const fn bytes(self) -> usize {
        self.bits() as usize / 8
    }
}

/// A validated output encoding.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Encoding {
    /// Uncompressed RIFF/WAVE, written by `hound`.
    Wav(SampleDepth),
    /// Lossless, via Media Foundation. Never `Float32`.
    Flac(SampleDepth),
    /// Lossy, via Media Foundation.
    Mp3 { kbps: u32 },
    /// Lossy AAC in MPEG-4, via Media Foundation.
    M4a { kbps: u32 },
}

/// Bitrates the MPEG-1 Layer III bitstream can signal.
const MP3_KBPS: [u16; 14] = [
    32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,
];

/// Bitrates the Windows AAC encoder accepts.
const AAC_KBPS: [u16; 4] = [96, 128, 160, 192];

const DEFAULT_KBPS: u16 = 192;

impl Encoding {
    /// Decode and validate the format fields of a [`RecConfig`].
    pub fn from_config(cfg: &RecConfig) -> Result<Self, YipError> {
        let kbps = if cfg.bitrate_kbps == 0 {
            DEFAULT_KBPS
        } else {
            cfg.bitrate_kbps
        };
        match cfg.format {
            f if f == RecFormat::Wav as u16 => match cfg.bit_depth {
                16 => Ok(Self::Wav(SampleDepth::Int16)),
                24 => Ok(Self::Wav(SampleDepth::Int24)),
                0 | 32 => Ok(Self::Wav(SampleDepth::Float32)),
                _ => Err(YipError::InvalidArgument(
                    "WAV bit depth must be 16, 24 or 32",
                )),
            },
            f if f == RecFormat::Flac as u16 => match cfg.bit_depth {
                16 => Ok(Self::Flac(SampleDepth::Int16)),
                0 | 24 => Ok(Self::Flac(SampleDepth::Int24)),
                _ => Err(YipError::InvalidArgument("FLAC bit depth must be 16 or 24")),
            },
            f if f == RecFormat::Mp3 as u16 => {
                if MP3_KBPS.contains(&kbps) {
                    Ok(Self::Mp3 {
                        kbps: u32::from(kbps),
                    })
                } else {
                    Err(YipError::InvalidArgument(
                        "MP3 bitrate is not a valid MPEG-1 rate",
                    ))
                }
            }
            f if f == RecFormat::M4a as u16 => {
                if AAC_KBPS.contains(&kbps) {
                    Ok(Self::M4a {
                        kbps: u32::from(kbps),
                    })
                } else {
                    Err(YipError::InvalidArgument(
                        "M4A bitrate must be 96, 128, 160 or 192",
                    ))
                }
            }
            _ => Err(YipError::InvalidArgument("unknown recording format")),
        }
    }

    /// Integer or float width the samples leave the writer at. Lossy encoders
    /// take 16-bit PCM in.
    pub const fn depth(self) -> SampleDepth {
        match self {
            Self::Wav(d) | Self::Flac(d) => d,
            Self::Mp3 { .. } | Self::M4a { .. } => SampleDepth::Int16,
        }
    }

    /// Rate to ask WASAPI for. The AAC and MP3 encoders only take 44.1 and
    /// 48 kHz; anything else is resampled to 48 kHz by the engine on the way in
    /// rather than refused after the take.
    pub fn capture_rate(self, requested: u32) -> u32 {
        match self {
            Self::Mp3 { .. } | Self::M4a { .. } if requested != 44_100 && requested != 48_000 => {
                48_000
            }
            _ => requested,
        }
    }

    /// Channel count to ask WASAPI for. The encoders here are mono/stereo.
    pub fn capture_channels(self, requested: u16) -> u16 {
        match self {
            Self::Wav(_) => requested,
            _ => requested.clamp(1, 2),
        }
    }
}

/// Float → integer PCM.
///
/// 16-bit output gets TPDF dither: truncating a float capture to 16 bits
/// without it turns quiet passages into correlated distortion. 24 bits already
/// sits below any converter's noise floor and is rounded plainly.
pub struct PcmConverter {
    depth: SampleDepth,
    /// xorshift32 state. Never zero.
    rng: u32,
}

impl PcmConverter {
    pub const fn new(depth: SampleDepth) -> Self {
        Self {
            depth,
            rng: 0x9E37_79B9,
        }
    }

    /// Uniform in [0, 1).
    fn next_unit(&mut self) -> f32 {
        let mut x = self.rng;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.rng = x;
        // Top 24 bits: exactly representable as f32.
        (x >> 8) as f32 / 16_777_216.0
    }

    pub fn int16(&mut self, s: f32) -> i16 {
        let dither = self.next_unit() - self.next_unit();
        let v = (s.clamp(-1.0, 1.0) * 32_767.0 + dither).round();
        v.clamp(-32_768.0, 32_767.0) as i16
    }

    pub fn int24(s: f32) -> i32 {
        (s.clamp(-1.0, 1.0) * 8_388_607.0).round() as i32
    }

    /// Append `src` to `out` as little-endian PCM at this converter's depth.
    pub fn extend_bytes(&mut self, src: &[f32], out: &mut Vec<u8>) {
        out.reserve(src.len() * self.depth.bytes());
        match self.depth {
            SampleDepth::Int16 => {
                for &s in src {
                    let v = self.int16(s);
                    out.extend_from_slice(&v.to_le_bytes());
                }
            }
            SampleDepth::Int24 => {
                for &s in src {
                    out.extend_from_slice(&Self::int24(s).to_le_bytes()[..3]);
                }
            }
            SampleDepth::Float32 => {
                for &s in src {
                    out.extend_from_slice(&s.to_le_bytes());
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cfg(format: RecFormat, bit_depth: u16, bitrate_kbps: u16) -> RecConfig {
        RecConfig {
            format: format as u16,
            bit_depth,
            bitrate_kbps,
            ..RecConfig::default()
        }
    }

    #[test]
    fn defaults_keep_the_v1_float_wav() {
        let e = Encoding::from_config(&RecConfig::default()).unwrap();
        assert_eq!(e, Encoding::Wav(SampleDepth::Float32));
    }

    #[test]
    fn decodes_every_format() {
        assert_eq!(
            Encoding::from_config(&cfg(RecFormat::Wav, 16, 0)).unwrap(),
            Encoding::Wav(SampleDepth::Int16)
        );
        assert_eq!(
            Encoding::from_config(&cfg(RecFormat::Flac, 0, 0)).unwrap(),
            Encoding::Flac(SampleDepth::Int24)
        );
        assert_eq!(
            Encoding::from_config(&cfg(RecFormat::Mp3, 0, 320)).unwrap(),
            Encoding::Mp3 { kbps: 320 }
        );
        assert_eq!(
            Encoding::from_config(&cfg(RecFormat::M4a, 0, 0)).unwrap(),
            Encoding::M4a { kbps: 192 }
        );
    }

    #[test]
    fn rejects_impossible_combinations() {
        assert!(Encoding::from_config(&cfg(RecFormat::Flac, 32, 0)).is_err());
        assert!(Encoding::from_config(&cfg(RecFormat::Wav, 20, 0)).is_err());
        assert!(Encoding::from_config(&cfg(RecFormat::Mp3, 0, 100)).is_err());
        assert!(Encoding::from_config(&cfg(RecFormat::M4a, 0, 320)).is_err());
        let bad = RecConfig {
            format: 42,
            ..RecConfig::default()
        };
        assert!(Encoding::from_config(&bad).is_err());
    }

    #[test]
    fn lossy_capture_rate_snaps_to_what_the_encoder_takes() {
        let mp3 = Encoding::Mp3 { kbps: 192 };
        assert_eq!(mp3.capture_rate(44_100), 44_100);
        assert_eq!(mp3.capture_rate(96_000), 48_000);
        assert_eq!(
            Encoding::Flac(SampleDepth::Int24).capture_rate(96_000),
            96_000
        );
        assert_eq!(mp3.capture_channels(6), 2);
        assert_eq!(Encoding::Wav(SampleDepth::Float32).capture_channels(6), 6);
    }

    #[test]
    fn int24_hits_full_scale_and_clamps() {
        assert_eq!(PcmConverter::int24(1.0), 8_388_607);
        assert_eq!(PcmConverter::int24(-1.0), -8_388_607);
        assert_eq!(PcmConverter::int24(4.0), 8_388_607);
        assert_eq!(PcmConverter::int24(0.0), 0);
    }

    #[test]
    fn int16_dither_stays_within_one_lsb() {
        let mut c = PcmConverter::new(SampleDepth::Int16);
        for _ in 0..10_000 {
            assert!(c.int16(0.0).abs() <= 1);
            assert!(c.int16(1.0) >= 32_766);
            assert!(c.int16(-2.0) <= -32_766);
        }
    }

    #[test]
    fn extend_bytes_writes_little_endian_frames() {
        let mut out = Vec::new();
        PcmConverter::new(SampleDepth::Int24).extend_bytes(&[1.0, -1.0], &mut out);
        assert_eq!(out, [0xFF, 0xFF, 0x7F, 0x01, 0x00, 0x80]);

        out.clear();
        PcmConverter::new(SampleDepth::Float32).extend_bytes(&[0.5], &mut out);
        assert_eq!(out, 0.5f32.to_le_bytes());
    }
}
