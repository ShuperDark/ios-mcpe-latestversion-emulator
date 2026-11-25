// MP3 (minimp3), Ogg Vorbis (stb_vorbis) and WAV decoding for the mods' sounds (emu_sound_load in hle_media.cpp).
// Kept in its own file: stb_vorbis defines short macro names.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#pragma warning(push)
#pragma warning(disable : 4244 4245 4267 4456 4457 4701 4996)
#define MINIMP3_IMPLEMENTATION
#include "../external/minimp3/minimp3_ex.h"

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../external/stb_vorbis/stb_vorbis.c"
#pragma warning(pop)

namespace {
uint32_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// WAV (RIFF WAVE): integer PCM of 8 / 16 / 24 / 32 bits or IEEE float of 32 / 64 bits, also in
// WAVE_FORMAT_EXTENSIBLE
bool decode_wav(const uint8_t* data, size_t len, std::vector<int16_t>& out, int& channels, int& rate) {
    if (len < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) return false;
    const uint8_t* fmt = nullptr;
    const uint8_t* pcm = nullptr;
    size_t fmtLen = 0, pcmLen = 0;
    for (size_t p = 12; p + 8 <= len;) {
        size_t n = le32(data + p + 4), body = p + 8;
        size_t avail = len - body < n ? len - body : n;  // (a cut-off last chunk: what is there)
        if (memcmp(data + p, "fmt ", 4) == 0) fmt = data + body, fmtLen = avail;
        else if (memcmp(data + p, "data", 4) == 0) pcm = data + body, pcmLen = avail;
        if (n > len - body) break;
        p = body + n + (n & 1);  // (chunks are padded to even sizes)
    }
    if (!fmt || fmtLen < 16 || !pcm) return false;
    uint32_t tag = le16(fmt), ch = le16(fmt + 2), r = le32(fmt + 4), bits = le16(fmt + 14);
    if (tag == 0xFFFE && fmtLen >= 26) tag = le16(fmt + 24);  // extensible: the sub-format's tag
    bool isFloat = tag == 3;
    if ((tag != 1 && !isFloat) || ch == 0 || r == 0) return false;
    if (isFloat ? bits != 32 && bits != 64 : bits != 8 && bits != 16 && bits != 24 && bits != 32) return false;
    size_t bytes = bits / 8, frames = pcmLen / (bytes * ch);
    if (frames == 0) return false;
    int keep = ch > 2 ? 2 : (int)ch;
    out.resize(frames * keep);
    for (size_t f = 0; f < frames; f++) {
        for (int c = 0; c < keep; c++) {
            const uint8_t* s = pcm + (f * ch + c) * bytes;
            int v;
            if (isFloat) {
                double x;
                if (bits == 32) {
                    uint32_t u = le32(s);
                    float fl;
                    memcpy(&fl, &u, 4);
                    x = fl;
                } else {
                    uint64_t u = le32(s) | ((uint64_t)le32(s + 4) << 32);
                    memcpy(&x, &u, 8);
                }
                if (!(x == x)) x = 0;  // NaN
                v = (int)(x * 32767.0 + (x >= 0 ? 0.5 : -0.5));
            } else if (bits == 8) {
                v = ((int)s[0] - 128) << 8;  // (8-bit WAV is unsigned)
            } else if (bits == 16) {
                v = (int16_t)le16(s);
            } else if (bits == 24) {
                v = (int16_t)le16(s + 1);  // the top 16 of the 24 bits
            } else {
                v = (int16_t)le16(s + 2);  // the top 16 of the 32 bits
            }
            out[f * keep + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    channels = keep;
    rate = (int)r;
    return true;
}
}  // namespace

// Decodes a whole MP3, Ogg Vorbis or WAV file to interleaved 16-bit samples (1 or 2 channels).
bool decode_audio(const uint8_t* data, size_t len, std::vector<int16_t>& out, int& channels, int& rate) {
    out.clear();
    if (len >= 4 && memcmp(data, "RIFF", 4) == 0) return decode_wav(data, len, out, channels, rate);
    if (len >= 4 && memcmp(data, "OggS", 4) == 0) {
        int ch = 0, r = 0;
        short* pcm = nullptr;
        int frames = stb_vorbis_decode_memory(data, (int)len, &ch, &r, &pcm);
        if (frames <= 0 || !pcm || ch <= 0) {
            free(pcm);
            return false;
        }
        // down to stereo
        int keep = ch > 2 ? 2 : ch;
        out.resize((size_t)frames * keep);
        for (int f = 0; f < frames; f++)
            for (int c = 0; c < keep; c++) out[(size_t)f * keep + c] = pcm[(size_t)f * ch + c];
        free(pcm);
        channels = keep;
        rate = r;
        return true;
    }
    mp3dec_t dec;
    mp3dec_file_info_t info{};
    if (mp3dec_load_buf(&dec, data, len, &info, nullptr, nullptr) != 0 || !info.buffer || info.samples == 0 ||
        info.channels <= 0) {
        free(info.buffer);
        return false;
    }
    int ch = info.channels, keep = ch > 2 ? 2 : ch;
    size_t frames = info.samples / ch;
    out.resize(frames * keep);
    for (size_t f = 0; f < frames; f++)
        for (int c = 0; c < keep; c++) out[f * keep + c] = info.buffer[f * ch + c];
    free(info.buffer);
    channels = keep;
    rate = info.hz;
    return true;
}
