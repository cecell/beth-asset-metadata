// WAV / XWM (RIFF) and FUZ readers.
//
// FUZ: "FUZE", u32 version, u32 lipSize, lip block, then a RIFF file
// (WAVE or XWMA). RIFF: 12-byte header, then chunks (id, u32 size, body,
// pad to even). "fmt " carries WAVEFORMATEX; "data" is the payload. xWMA
// adds "dpds": one u32 per packet with the cumulative decoded PCM byte
// count, so the last entry gives the exact duration.
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include "common.hpp"

namespace asset {
using namespace detail;

namespace {

Status parse_riff(const uint8_t* d, size_t n, AudioInfo& o) noexcept {
    if (n < 12) return Status::Truncated;
    if (!tag4(d, "RIFF")) return Status::NotRecognized;
    if (tag4(d + 8, "WAVE")) o.payload = AudioContainer::Wav;
    else if (tag4(d + 8, "XWMA")) o.payload = AudioContainer::Xwm;
    else return Status::Unsupported;
    if (o.container == AudioContainer::Unknown) o.container = o.payload;

    bool fmt = false;
    double durDpds = 0, durFact = 0;
    uint64_t off = 12;
    while (off + 8 <= n) {
        const uint8_t* ch = d + off;
        const uint32_t size = rd32(ch + 4);
        const uint64_t body = off + 8;
        const uint64_t avail = size < n - body ? size : n - body;
        if (tag4(ch, "fmt ")) {
            if (avail < 16) return size < 16 ? Status::Corrupt : Status::Truncated;
            const uint8_t* f = d + body;
            o.formatTag = rd16(f);
            o.channels = rd16(f + 2);
            o.sampleRate = rd32(f + 4);
            o.avgBytesPerSec = rd32(f + 8);
            o.bitsPerSample = rd16(f + 14);
            o.bitRate = o.avgBytesPerSec > 0x1FFFFFFF ? 0xFFFFFFFFu : o.avgBytesPerSec * 8;
            fmt = true;
        } else if (tag4(ch, "fact")) {
            if (avail >= 4 && o.sampleRate) durFact = static_cast<double>(rd32(d + body)) / o.sampleRate;
        } else if (tag4(ch, "dpds")) {
            if (avail < size || size < 4) {
                o.dpdsTruncated = true;
            } else if (o.sampleRate && o.channels) {
                const uint32_t bytesPerSample = o.bitsPerSample ? o.bitsPerSample / 8 : 2;
                const double denom = static_cast<double>(o.sampleRate) * o.channels * (bytesPerSample ? bytesPerSample : 2);
                durDpds = static_cast<double>(rd32(d + body + size - 4)) / denom;
            }
        } else if (tag4(ch, "data")) {
            o.dataBytes = size;
            o.dataChunkFound = true;
            break;
        }
        off = body + size + (size & 1);
    }
    if (!fmt) return o.dataChunkFound ? Status::Corrupt : Status::Truncated;

    if (durDpds > 0) {
        o.durationSeconds = durDpds;
        o.durationSource = DurationSource::Dpds;
    } else if (durFact > 0) {
        o.durationSeconds = durFact;
        o.durationSource = DurationSource::Fact;
    } else if (o.dataChunkFound && o.avgBytesPerSec > 0) {
        o.durationSeconds = static_cast<double>(o.dataBytes) / o.avgBytesPerSec;
        o.durationSource = DurationSource::DataRate;
    }
    return Status::Ok;
}

}  // namespace

const char* to_string(AudioContainer c) noexcept {
    switch (c) {
        case AudioContainer::Wav: return "wav";
        case AudioContainer::Xwm: return "xwm";
        case AudioContainer::Fuz: return "fuz";
        case AudioContainer::Unknown: break;
    }
    return "unknown";
}

const char* codec_name(uint16_t tag, AudioContainer payload) noexcept {
    switch (tag) {
        case 0x0001: return "PCM";
        case 0x0002: return "ADPCM";
        case 0x0003: return "IEEE float";
        case 0x0006: return "A-law";
        case 0x0007: return "mu-law";
        case 0x0011: return "IMA ADPCM";
        case 0x0055: return "MP3";
        case 0x0161: return payload == AudioContainer::Xwm ? "xWMA (WMA v2)" : "WMA v2";
        case 0x0162: return payload == AudioContainer::Xwm ? "xWMA (WMA Pro)" : "WMA Pro";
        case 0x0163: return "WMA Lossless";
        case 0xFFFE: return "WAVE_FORMAT_EXTENSIBLE";
        default: return "unknown";
    }
}

Status read_audio(const uint8_t* d, size_t n, AudioInfo& o) noexcept {
    o = AudioInfo{};
    if (n < 4) return Status::Truncated;
    if (tag4(d, "FUZE")) {
        if (n < 12) return Status::Truncated;
        o.container = AudioContainer::Fuz;
        o.fuzVersion = rd32(d + 4);
        o.lipBytes = rd32(d + 8);
        o.hasLip = o.lipBytes != 0;
        const uint64_t payload = 12 + uint64_t{o.lipBytes};
        if (payload > n) return Status::Truncated;
        const Status s = parse_riff(d + payload, n - static_cast<size_t>(payload), o);
        return s == Status::NotRecognized ? Status::Corrupt : s;
    }
    if (tag4(d, "RIFF")) return parse_riff(d, n, o);
    return Status::NotRecognized;
}

}  // namespace asset
