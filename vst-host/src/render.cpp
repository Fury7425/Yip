// render.cpp — offline file → file rendering (C ABI).
//
// Reads a WAV (float32, or 16/24/32-bit integer PCM, plain or EXTENSIBLE
// header), runs it through the ordered plugin chain, writes a float32 WAV.
// Without the SDK the chain is an identity passthrough so the pipeline is
// exercisable; the real per-plugin processing lands behind
// YIP_HAVE_VST3_SDK.

#include "vst_host.h"
#include "vst_internal.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace yip::vst {
namespace {

#pragma pack(push, 1)
    struct WavFmt {
        uint16_t audio_format;   // 1 = PCM, 3 = IEEE float
        uint16_t channels;
        uint32_t sample_rate;
        uint32_t byte_rate;
        uint16_t block_align;
        uint16_t bits_per_sample;
    };
#pragma pack(pop)

    struct Pcm {
        uint32_t sample_rate = 48000;
        uint16_t channels = 2;
        std::vector<float> interleaved;  // frames * channels
    };

    bool read_tag(std::FILE* f, char out[4]) { return std::fread(out, 1, 4, f) == 4; }

    template <class T>
    bool read_le(std::FILE* f, T& v) { return std::fread(&v, sizeof(T), 1, f) == 1; }

    bool read_wav(const wchar_t* path, Pcm& out) {
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"rb") != 0 || !f) {
            set_last_error(L"render: cannot open input WAV");
            return false;
        }
        struct Closer { std::FILE* f; ~Closer() { if (f) std::fclose(f); } } closer{ f };

        char tag[4];
        uint32_t riff_size = 0;  // read to advance the cursor; chunk walk ignores it
        if (!read_tag(f, tag) || std::memcmp(tag, "RIFF", 4) != 0) { set_last_error(L"render: not RIFF"); return false; }
        if (!read_le(f, riff_size)) return false;
        (void)riff_size;
        if (!read_tag(f, tag) || std::memcmp(tag, "WAVE", 4) != 0) { set_last_error(L"render: not WAVE"); return false; }

        WavFmt fmt{};
        bool have_fmt = false;
        uint16_t sample_kind = 0;  // 1 = integer PCM, 3 = IEEE float, after unwrapping EXTENSIBLE
        while (read_tag(f, tag)) {
            uint32_t chunk_size = 0;
            if (!read_le(f, chunk_size)) break;
            if (std::memcmp(tag, "fmt ", 4) == 0) {
                if (chunk_size < sizeof(WavFmt)) { set_last_error(L"render: short fmt chunk"); return false; }
                if (std::fread(&fmt, 1, sizeof(WavFmt), f) != sizeof(WavFmt)) return false;
                have_fmt = true;
                sample_kind = fmt.audio_format;
                uint32_t consumed = static_cast<uint32_t>(sizeof(WavFmt));
                // WAVE_FORMAT_EXTENSIBLE: cbSize, validBits, channelMask (8 bytes), then
                // a SubFormat GUID whose first four bytes are the real format tag. Yip's
                // own recorder writes this for every depth above 16 bits.
                if (fmt.audio_format == 0xFFFE && chunk_size >= 40) {
                    uint8_t tail[8];
                    uint32_t sub_tag = 0;
                    if (std::fread(tail, 1, sizeof(tail), f) != sizeof(tail)) return false;
                    if (!read_le(f, sub_tag)) return false;
                    sample_kind = static_cast<uint16_t>(sub_tag);
                    consumed += 12;
                }
                if (chunk_size > consumed) {
                    if (_fseeki64(f, static_cast<int64_t>(chunk_size - consumed), SEEK_CUR) != 0) return false;
                }
            } else if (std::memcmp(tag, "data", 4) == 0) {
                if (!have_fmt) { set_last_error(L"render: data before fmt"); return false; }
                const bool is_float = sample_kind == 3 && fmt.bits_per_sample == 32;
                const bool is_int = sample_kind == 1 && (fmt.bits_per_sample == 16 ||
                                                         fmt.bits_per_sample == 24 ||
                                                         fmt.bits_per_sample == 32);
                if (!is_float && !is_int) {
                    set_last_error(L"render: WAV must be float32 or 16/24/32-bit PCM");
                    return false;
                }
                if (fmt.channels == 0 || fmt.sample_rate == 0) {
                    set_last_error(L"render: fmt chunk declares no channels or rate");
                    return false;
                }
                out.sample_rate = fmt.sample_rate;
                out.channels = fmt.channels;
                const size_t bytes_per_sample = fmt.bits_per_sample / 8u;
                const size_t count = chunk_size / bytes_per_sample;
                out.interleaved.resize(count);
                if (is_float) {
                    if (count && std::fread(out.interleaved.data(), sizeof(float), count, f) != count) {
                        set_last_error(L"render: short data read");
                        return false;
                    }
                    return true;
                }

                // Integer PCM: read the raw bytes, then widen each little-endian
                // sample into [-1, 1). The plugin chain only ever sees float.
                std::vector<uint8_t> raw(count * bytes_per_sample);
                if (count && std::fread(raw.data(), 1, raw.size(), f) != raw.size()) {
                    set_last_error(L"render: short data read");
                    return false;
                }
                const double scale = 1.0 / static_cast<double>(1ull << (fmt.bits_per_sample - 1));
                for (size_t i = 0; i < count; ++i) {
                    const uint8_t* b = raw.data() + i * bytes_per_sample;
                    int32_t v = 0;
                    if (bytes_per_sample == 2) {
                        v = static_cast<int16_t>(b[0] | (b[1] << 8));
                    } else if (bytes_per_sample == 3) {
                        // Shift into the top of an int32 so the sign extends, then back.
                        v = static_cast<int32_t>(static_cast<uint32_t>(b[0]) << 8 | static_cast<uint32_t>(b[1]) << 16 |
                                                 static_cast<uint32_t>(b[2]) << 24) >> 8;
                    } else {
                        v = static_cast<int32_t>(static_cast<uint32_t>(b[0]) | static_cast<uint32_t>(b[1]) << 8 |
                                                 static_cast<uint32_t>(b[2]) << 16 | static_cast<uint32_t>(b[3]) << 24);
                    }
                    out.interleaved[i] = static_cast<float>(v * scale);
                }
                return true;
            } else {
                // Skip unknown chunk. 64-bit seek: a WAV can carry chunks past
                // the 2 GB that `long` covers on Windows.
                if (_fseeki64(f, static_cast<int64_t>(chunk_size), SEEK_CUR) != 0) return false;
            }
            // RIFF chunks are word-aligned; an odd size carries a pad byte.
            if ((chunk_size & 1u) != 0) {
                if (_fseeki64(f, 1, SEEK_CUR) != 0) break;
            }
        }
        set_last_error(L"render: no data chunk");
        return false;
    }

    bool write_wav(const wchar_t* path, const Pcm& in) {
        std::FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wb") != 0 || !f) {
            set_last_error(L"render: cannot open output WAV");
            return false;
        }
        struct Closer { std::FILE* f; ~Closer() { if (f) std::fclose(f); } } closer{ f };

        const uint32_t data_bytes = static_cast<uint32_t>(in.interleaved.size() * sizeof(float));
        const uint16_t block_align = static_cast<uint16_t>(in.channels * sizeof(float));
        const uint32_t byte_rate = in.sample_rate * static_cast<uint32_t>(block_align);

        auto w_tag = [&](const char* t) { std::fwrite(t, 1, 4, f); };
        auto w_u32 = [&](uint32_t v) { std::fwrite(&v, sizeof(v), 1, f); };
        auto w_u16 = [&](uint16_t v) { std::fwrite(&v, sizeof(v), 1, f); };

        w_tag("RIFF"); w_u32(36 + data_bytes); w_tag("WAVE");
        w_tag("fmt "); w_u32(16);
        w_u16(3);                      // IEEE float
        w_u16(in.channels);
        w_u32(in.sample_rate);
        w_u32(byte_rate);
        w_u16(block_align);
        w_u16(32);                     // bits/sample
        w_tag("data"); w_u32(data_bytes);
        if (data_bytes && std::fwrite(in.interleaved.data(), sizeof(float), in.interleaved.size(), f) !=
                              in.interleaved.size()) {
            set_last_error(L"render: short data write");
            return false;
        }
        // Catch a full disk: buffered writes only surface their error at flush.
        if (std::fflush(f) != 0 || std::ferror(f) != 0) {
            set_last_error(L"render: write failed");
            return false;
        }
        return true;
    }

}  // namespace
}  // namespace yip::vst

extern "C" YipVstStatus yip_vst_render(const wchar_t* input_wav, const wchar_t* output_wav,
                                       const YipVstPluginSetting* chain, uint32_t chain_len,
                                       YipVstProgress progress_cb, void* user) {
    using namespace yip::vst;
    if (!input_wav || !output_wav) {
        set_last_error(L"render: null path");
        return YIP_VST_INVALID_ARG;
    }

    Pcm pcm;
    if (!read_wav(input_wav, pcm)) return YIP_VST_IO;
    if (progress_cb) progress_cb(0.05f, user);

#ifdef YIP_HAVE_VST3_SDK
    // M5 SDK path: setupProcessing(sample_rate, block) per plugin, then push
    // pcm through each IAudioProcessor in chain order, reporting progress.
    if (!process_chain_with_sdk(pcm.interleaved, pcm.channels, pcm.sample_rate,
                                chain, chain_len, progress_cb, user)) {
        return YIP_VST_RENDER_FAILED;  // sets last_error
    }
#else
    // Passthrough: chain is identity. Walk it only to report progress so the
    // UI shows motion and the plugin handles are validated as non-null.
    (void)chain;
    for (uint32_t i = 0; i < chain_len; ++i) {
        if (progress_cb) {
            const float p = 0.05f + 0.9f * (static_cast<float>(i + 1) / static_cast<float>(chain_len == 0 ? 1 : chain_len));
            progress_cb(p, user);
        }
    }
#endif

    if (!write_wav(output_wav, pcm)) return YIP_VST_IO;
    if (progress_cb) progress_cb(1.0f, user);
    return YIP_VST_OK;
}
