// asset-metadata: header facts from Bethesda game assets without decoding
// them. DDS and TGA textures, NIF meshes, and WAV / XWM / FUZ audio.
//
// Every reader works on a byte buffer the caller already holds (a file
// prefix, or bytes pulled out of a BSA) and needs only a bounded prefix:
// see the k*ProbeBytes constants. Nothing throws; every call returns a
// Status, and hostile or truncated input is a status, never a crash.
// Readers that fill std::vector/std::string members (NIF) report
// OutOfMemory instead of throwing.
//
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace asset {

enum class Status : int {
    Ok = 0,
    Truncated,      // the buffer ends before the fields needed; read more and retry
    NotRecognized,  // magic does not match this reader
    Corrupt,        // a size, count or offset contradicts the format
    Unsupported,    // recognised but outside scope (big-endian NIF, unknown RIFF form)
    CannotOpen,     // file helpers only
    OutOfMemory,
};
const char* to_string(Status s) noexcept;

// What a buffer starts with. TGA has no magic and is never reported here.
enum class Kind { Unknown, Dds, Nif, Fuz, Wav, Xwm };
Kind detect(const uint8_t* data, size_t len) noexcept;

// ------------------------------------------------------------------ DDS

enum class DdsFormat {
    Unknown,
    BC1, BC2, BC3, BC4, BC5, BC6H, BC7,
    R8G8B8A8, B8G8R8A8, B8G8R8X8, R8G8B8, B8G8R8,
    R8, R8G8, A8, L8,
    B5G6R5, B5G5R5A1, B4G4R4A4,
    R16G16B16A16F, R32G32B32A32F,
    Other,  // recognised header, format not in this list; see fourcc / dxgiFormat / bitsPerPixel
};
const char* to_string(DdsFormat f) noexcept;

struct DdsInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;        // only meaningful when volume is true
    uint32_t mipmaps = 1;      // 1 when the header declares no chain
    uint32_t arraySize = 1;    // DX10 headers only
    DdsFormat format = DdsFormat::Unknown;
    uint32_t dxgiFormat = 0;   // 0 unless dx10
    char fourcc[5] = {0, 0, 0, 0, 0};  // empty unless the pixel format uses one
    uint32_t bitsPerPixel = 0; // uncompressed formats only
    uint32_t blockBytes = 0;   // 8 or 16 for block-compressed formats, else 0
    bool dx10 = false;
    bool compressed = false;
    bool srgb = false;
    bool hasAlpha = false;
    bool cubemap = false;
    bool volume = false;
};

constexpr size_t kDdsProbeBytes = 148;  // 128-byte header + 20-byte DX10 extension
Status read_dds(const uint8_t* data, size_t len, DdsInfo& out) noexcept;

// ------------------------------------------------------------------ TGA

struct TgaInfo {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t bitsPerPixel = 0;
    uint8_t alphaBits = 0;
    uint8_t imageType = 0;   // raw type byte: 1/2/3 plain, 9/10/11 RLE
    bool rle = false;
    bool colorMapped = false;
    bool grayscale = false;
    bool topLeft = false;    // origin flag; false means bottom-left (the TGA default)
    bool rightToLeft = false;
};

constexpr size_t kTgaProbeBytes = 18;
// TGA has no magic: the header is validated field by field, so random
// data usually reports Corrupt, never Ok. Pass only files named *.tga.
Status read_tga(const uint8_t* data, size_t len, TgaInfo& out) noexcept;

// ------------------------------------------------------------------ NIF

// Which Skyrim the mesh was built for. The header alone cannot tell: vanilla
// SSE meshes carry BS version 100 like LE. The geometry block types decide:
// the BSTriShape family exists only in SE, NiTriShape/NiTriStrips is the
// old style that SE still loads. A Skyrim header with neither (collision,
// nodes only) is SkyrimEither.
enum class NifEngine { Unknown, SkyrimLe, SkyrimSe, SkyrimEither };
const char* to_string(NifEngine e) noexcept;

struct NifInfo {
    std::string headerLine;      // "Gamebryo File Format, Version 20.2.0.7"
    uint32_t version = 0;        // 0x14020007 for Skyrim
    bool littleEndian = true;
    uint32_t userVersion = 0;
    bool hasBsHeader = false;
    uint32_t bsVersion = 0;      // "user version 2": 83..100 for LE and vanilla SSE, 130 for FO4 and some SSE tools
    uint32_t blockCount = 0;
    std::vector<std::string> blockTypes;   // the block-type table, in file order
    std::vector<uint16_t> blockTypeIndex;  // per block, index into blockTypes
    bool seGeometry = false;      // BSTriShape / BSDynamicTriShape / BSMeshLODTriShape / BSSubIndexTriShape present
    bool legacyGeometry = false;  // NiTriShape(Data) / NiTriStrips(Data) / bhkPackedNiTriStrips* present
    NifEngine engine = NifEngine::Unknown;
};

std::string nif_version_string(uint32_t version);  // 0x14020007 -> "20.2.0.7"
constexpr size_t kNifProbeBytes = size_t{64} << 10;
Status read_nif(const uint8_t* data, size_t len, NifInfo& out) noexcept;

// ---------------------------------------------------------------- audio

enum class AudioContainer { Unknown, Wav, Xwm, Fuz };
enum class DurationSource { None, Dpds, Fact, DataRate };
const char* to_string(AudioContainer c) noexcept;
const char* codec_name(uint16_t formatTag, AudioContainer payload) noexcept;

struct AudioInfo {
    AudioContainer container = AudioContainer::Unknown;
    AudioContainer payload = AudioContainer::Unknown;  // for FUZ: what is inside; else == container
    uint32_t fuzVersion = 0;
    bool hasLip = false;
    uint32_t lipBytes = 0;
    uint16_t formatTag = 0;      // WAVEFORMATEX wFormatTag
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    uint32_t bitsPerSample = 0;
    uint32_t avgBytesPerSec = 0;
    uint32_t bitRate = 0;        // avgBytesPerSec * 8, bits per second
    uint64_t dataBytes = 0;      // declared size of the data chunk
    bool dataChunkFound = false;
    bool dpdsTruncated = false;  // an XWM packet table was present but not fully in the buffer
    double durationSeconds = 0;  // 0 when unknown
    DurationSource durationSource = DurationSource::None;
};

constexpr size_t kAudioProbeBytes = size_t{64} << 10;
Status read_audio(const uint8_t* data, size_t len, AudioInfo& out) noexcept;

// ---------------------------------------------------------------- files

// Reads up to maxBytes from the start of the file, sharing read/write/delete.
Status read_prefix(const std::filesystem::path& path, size_t maxBytes, std::vector<uint8_t>& out) noexcept;

// Convenience probes: read the right prefix, retry with a larger one on
// Truncated (NIF, audio), and run the reader.
Status probe_dds(const std::filesystem::path& path, DdsInfo& out) noexcept;
Status probe_tga(const std::filesystem::path& path, TgaInfo& out) noexcept;
Status probe_nif(const std::filesystem::path& path, NifInfo& out) noexcept;
Status probe_audio(const std::filesystem::path& path, AudioInfo& out) noexcept;

}  // namespace asset
