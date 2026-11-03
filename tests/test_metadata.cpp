// Unit tests: synthetic headers for every reader, plus hostile-input checks.
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace asset;

static int g_failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

using Bytes = std::vector<uint8_t>;
static void put8(Bytes& v, uint8_t x) { v.push_back(x); }
static void put16(Bytes& v, uint16_t x) { v.push_back(static_cast<uint8_t>(x)); v.push_back(static_cast<uint8_t>(x >> 8)); }
static void put32(Bytes& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i))); }
static void puts4(Bytes& v, const char* s) { v.insert(v.end(), s, s + 4); }
static void patch32(Bytes& v, size_t at, uint32_t x) { for (int i = 0; i < 4; ++i) v[at + i] = static_cast<uint8_t>(x >> (8 * i)); }

// ------------------------------------------------------------------ DDS

static Bytes dds(uint32_t w, uint32_t h, uint32_t mips, const char* fourcc, uint32_t dxgi = 0, uint32_t caps2 = 0, uint32_t misc = 0) {
    Bytes b(128, 0);
    std::memcpy(b.data(), "DDS ", 4);
    patch32(b, 4, 124);
    patch32(b, 8, 0x1007u | (mips > 1 ? 0x20000u : 0u));
    patch32(b, 12, h);
    patch32(b, 16, w);
    patch32(b, 28, mips);
    patch32(b, 76, 32);
    patch32(b, 80, 0x4);  // DDPF_FOURCC
    std::memcpy(b.data() + 84, fourcc, 4);
    patch32(b, 112, caps2);
    if (std::memcmp(fourcc, "DX10", 4) == 0) {
        Bytes t(20, 0);
        patch32(t, 0, dxgi);
        patch32(t, 4, 3);
        patch32(t, 8, misc);
        patch32(t, 12, 1);
        b.insert(b.end(), t.begin(), t.end());
    }
    return b;
}

static Bytes dds_masks(uint32_t pfFlags, uint32_t bpp, uint32_t r, uint32_t g, uint32_t bl, uint32_t a) {
    Bytes b = dds(16, 16, 1, "\0\0\0\0");
    patch32(b, 80, pfFlags);
    patch32(b, 88, bpp);
    patch32(b, 92, r);
    patch32(b, 96, g);
    patch32(b, 100, bl);
    patch32(b, 104, a);
    return b;
}

static void test_dds() {
    DdsInfo i;
    auto b = dds(512, 256, 10, "DXT1");
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok);
    CHECK(i.width == 512 && i.height == 256 && i.mipmaps == 10);
    CHECK(i.format == DdsFormat::BC1 && i.compressed && i.blockBytes == 8 && !i.hasAlpha && !i.dx10);
    CHECK(std::string(i.fourcc) == "DXT1");

    b = dds(64, 64, 1, "DXT5");
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::BC3 && i.hasAlpha && i.mipmaps == 1);
    b = dds(64, 64, 1, "ATI2");
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::BC5);

    b = dds(128, 128, 8, "DX10", 99);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok);
    CHECK(i.format == DdsFormat::BC7 && i.dx10 && i.srgb && i.dxgiFormat == 99 && i.blockBytes == 16);
    b = dds(128, 128, 8, "DX10", 28);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::R8G8B8A8 && !i.compressed && i.bitsPerPixel == 32);
    b = dds(128, 128, 1, "DX10", 49);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::R8G8);
    b = dds(128, 128, 1, "DX10", 9999);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::Other);
    b = dds(128, 128, 1, "$\0\0\0");  // numeric FourCC 36 = D3DFMT_A16B16G16R16
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::R16G16B16A16 && i.bitsPerPixel == 64);

    // Cubemaps both ways.
    b = dds(64, 64, 1, "DXT1", 0, 0x200);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.cubemap);
    b = dds(64, 64, 1, "DX10", 98, 0, 0x4);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.cubemap);
    b = dds(64, 64, 1, "DX10", 98);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && !i.cubemap);

    // Uncompressed by masks.
    b = dds_masks(0x41, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::R8G8B8A8 && i.hasAlpha && i.bitsPerPixel == 32);
    b = dds_masks(0x41, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::B8G8R8A8);
    b = dds_masks(0x40, 32, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::B8G8R8X8 && !i.hasAlpha);
    b = dds_masks(0x40, 24, 0x00FF0000, 0x0000FF00, 0x000000FF, 0);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::B8G8R8);
    b = dds_masks(0x40, 16, 0xF800, 0x07E0, 0x001F, 0);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::B5G6R5);
    b = dds_masks(0x20000, 8, 0xFF, 0, 0, 0);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::L8);
    b = dds_masks(0x40, 64, 0, 0, 0, 0);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Ok && i.format == DdsFormat::Other);

    // Rejections.
    b = dds(64, 64, 1, "DXT1");
    b[0] = 'X';
    CHECK(read_dds(b.data(), b.size(), i) == Status::NotRecognized);
    b = dds(64, 64, 1, "DXT1");
    b.resize(100);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Truncated);
    b = dds(64, 64, 1, "DX10", 98);
    b.resize(140);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Truncated);
    b = dds(64, 64, 1, "DXT1");
    patch32(b, 4, 100);
    CHECK(read_dds(b.data(), b.size(), i) == Status::Corrupt);
    CHECK(read_dds(b.data(), 3, i) == Status::Truncated);
    CHECK(detect(b.data(), b.size()) == Kind::Dds);
}

// ------------------------------------------------------------------ TGA

static Bytes tga(uint16_t w, uint16_t h, uint8_t depth, uint8_t type, uint8_t descriptor) {
    Bytes b(18, 0);
    b[2] = type;
    b[12] = static_cast<uint8_t>(w); b[13] = static_cast<uint8_t>(w >> 8);
    b[14] = static_cast<uint8_t>(h); b[15] = static_cast<uint8_t>(h >> 8);
    b[16] = depth;
    b[17] = descriptor;
    return b;
}

static void test_tga() {
    TgaInfo i;
    auto b = tga(256, 128, 32, 2, 0x28);
    CHECK(read_tga(b.data(), b.size(), i) == Status::Ok);
    CHECK(i.width == 256 && i.height == 128 && i.bitsPerPixel == 32 && i.alphaBits == 8 && i.topLeft && !i.rle);
    b = tga(256, 128, 24, 10, 0);
    CHECK(read_tga(b.data(), b.size(), i) == Status::Ok && i.rle && !i.topLeft && i.alphaBits == 0);
    b = tga(16, 16, 8, 3, 0);
    CHECK(read_tga(b.data(), b.size(), i) == Status::Ok && i.grayscale);
    b = tga(16, 16, 8, 1, 0);  // color-mapped without a map
    CHECK(read_tga(b.data(), b.size(), i) == Status::Corrupt);
    b = tga(16, 16, 32, 2, 0);
    b.resize(17);
    CHECK(read_tga(b.data(), b.size(), i) == Status::Truncated);
    b = tga(0, 16, 32, 2, 0);
    CHECK(read_tga(b.data(), b.size(), i) == Status::Corrupt);
    b = tga(16, 16, 12, 2, 0);
    CHECK(read_tga(b.data(), b.size(), i) == Status::Corrupt);
    b = tga(16, 16, 32, 7, 0);
    CHECK(read_tga(b.data(), b.size(), i) == Status::Corrupt);
    const uint8_t text[18] = "DDS this is text!";
    CHECK(read_tga(text, 18, i) == Status::Corrupt);
}

// ------------------------------------------------------------------ NIF

static void short_string(Bytes& b, const char* s) {
    put8(b, static_cast<uint8_t>(std::strlen(s)));
    b.insert(b.end(), s, s + std::strlen(s));
}
static void sized_string(Bytes& b, const char* s) {
    put32(b, static_cast<uint32_t>(std::strlen(s)));
    b.insert(b.end(), s, s + std::strlen(s));
}

static Bytes nif(uint32_t version, uint32_t userVersion, uint32_t bsVersion, std::vector<const char*> types, std::vector<uint16_t> index) {
    Bytes b;
    const std::string line = std::string(version >= 0x0A000000 ? "Gamebryo" : "NetImmerse") + " File Format, Version " + nif_version_string(version) + "\n";
    b.insert(b.end(), line.begin(), line.end());
    if (version >= 0x0303000D) put32(b, version);
    if (version >= 0x14000004) put8(b, 1);
    if (version >= 0x0A000108) put32(b, userVersion);
    if (version >= 0x0303000D) put32(b, static_cast<uint32_t>(index.size()));
    const bool bs = userVersion >= 3 && (version == 0x14020007 || version == 0x14000005 || (version >= 0x0A000102 && version <= 0x14000004 && userVersion <= 11));
    if (bs) {
        put32(b, bsVersion);
        short_string(b, "author");
        if (bsVersion > 130) put32(b, 0);
        if (bsVersion < 131) short_string(b, "proc");
        short_string(b, "export");
        if (bsVersion >= 103) short_string(b, "C:\\max");
    }
    if (version >= 0x05000001) {
        put16(b, static_cast<uint16_t>(types.size()));
        for (const char* t : types) sized_string(b, t);
        for (uint16_t i : index) put16(b, i);
        if (version >= 0x14020005) for (size_t i = 0; i < index.size(); ++i) put32(b, 100);
        put32(b, 0);  // strings
        put32(b, 0);  // max string length
        put32(b, 0);  // groups
    }
    return b;
}

static void test_nif() {
    NifInfo i;
    auto b = nif(0x14020007, 12, 130, {"NiNode", "BSTriShape", "BSLightingShaderProperty"}, {0, 1, 2, 1});
    CHECK(read_nif(b.data(), b.size(), i) == Status::Ok);
    CHECK(i.version == 0x14020007 && i.userVersion == 12 && i.bsVersion == 130 && i.blockCount == 4);
    CHECK(i.engine == NifEngine::SkyrimSe && i.seGeometry && !i.legacyGeometry);
    CHECK(i.blockTypes.size() == 3 && i.blockTypes[1] == "BSTriShape" && i.blockTypeIndex == std::vector<uint16_t>({0, 1, 2, 1}));
    CHECK(i.headerLine == "Gamebryo File Format, Version 20.2.0.7");
    CHECK(nif_version_string(0x14020007) == "20.2.0.7");

    b = nif(0x14020007, 12, 100, {"NiNode", "NiTriShape", "NiTriStrips"}, {0, 1, 2});
    CHECK(read_nif(b.data(), b.size(), i) == Status::Ok && i.engine == NifEngine::SkyrimLe && !i.seGeometry && i.legacyGeometry);
    b = nif(0x14020007, 12, 83, {"NiNode"}, {0});
    CHECK(read_nif(b.data(), b.size(), i) == Status::Ok && i.engine == NifEngine::SkyrimEither);
    b = nif(0x14020007, 12, 100, {"NiNode", "BSTriShape"}, {0, 1});  // vanilla SSE: bs 100 but SE geometry
    CHECK(read_nif(b.data(), b.size(), i) == Status::Ok && i.engine == NifEngine::SkyrimSe);
    b = nif(0x14020007, 12, 172, {"NiNode"}, {0});  // Starfield-style bs version with the extra int
    CHECK(read_nif(b.data(), b.size(), i) == Status::Ok && i.engine == NifEngine::Unknown && i.blockTypes.size() == 1);
    b = nif(0x14000005, 11, 11, {"NiNode", "NiTriShape"}, {0, 1});  // Oblivion
    CHECK(read_nif(b.data(), b.size(), i) == Status::Ok && i.engine == NifEngine::Unknown && i.hasBsHeader && i.blockTypes.size() == 2);
    b = nif(0x04000002, 0, 0, {}, {});  // Morrowind: version line only
    CHECK(read_nif(b.data(), b.size(), i) == Status::Ok && i.version == 0x04000002 && i.blockTypes.empty() && !i.hasBsHeader);

    // Rejections.
    const char* junk = "this is not a nif\n\0\0\0\0";
    CHECK(read_nif(reinterpret_cast<const uint8_t*>(junk), 23, i) == Status::NotRecognized);
    b = nif(0x14020007, 12, 130, {"NiNode"}, {0});
    patch32(b, 39, 0x14020005);  // binary version disagrees with the line
    CHECK(read_nif(b.data(), b.size(), i) == Status::Corrupt);
    b = nif(0x14020007, 12, 130, {"NiNode"}, {0});
    b[43] = 0;  // big-endian flag
    CHECK(read_nif(b.data(), b.size(), i) == Status::Unsupported);
    b = nif(0x14020007, 12, 130, {"NiNode"}, {5});  // index out of range
    CHECK(read_nif(b.data(), b.size(), i) == Status::Corrupt);
    b = nif(0x14020007, 12, 130, {"NiNode"}, {0});
    patch32(b, 48, 0xFFFFFFFF);  // absurd block count
    CHECK(read_nif(b.data(), b.size(), i) == Status::Truncated);
    b = nif(0x14020007, 12, 130, {"NiNode", "BSTriShape"}, {0, 1});
    for (size_t n = 0; n < b.size(); ++n) {
        const Status s = read_nif(b.data(), n, i);
        CHECK(s == Status::Truncated || s == Status::Ok || s == Status::NotRecognized);
    }
    CHECK(detect(b.data(), b.size()) == Kind::Nif);
}

// ---------------------------------------------------------------- audio

static Bytes riff(const char* form, uint16_t tag, uint16_t ch, uint32_t rate, uint32_t avg, uint16_t bits, uint32_t dataSize,
                  const std::vector<uint32_t>& dpds = {}, uint32_t factSamples = 0) {
    Bytes b;
    puts4(b, "RIFF");
    put32(b, 0);
    puts4(b, form);
    puts4(b, "fmt ");
    put32(b, 18);
    put16(b, tag); put16(b, ch); put32(b, rate); put32(b, avg); put16(b, static_cast<uint16_t>(ch * bits / 8)); put16(b, bits); put16(b, 0);
    if (!dpds.empty()) {
        puts4(b, "dpds");
        put32(b, static_cast<uint32_t>(dpds.size() * 4));
        for (uint32_t v : dpds) put32(b, v);
    }
    if (factSamples) {
        puts4(b, "fact");
        put32(b, 4);
        put32(b, factSamples);
    }
    puts4(b, "LIST");
    put32(b, 3);
    b.insert(b.end(), {1, 2, 3, 0});  // odd chunk + pad byte
    puts4(b, "data");
    put32(b, dataSize);
    b.insert(b.end(), 16, 0);  // only the start of the data is in the probe
    patch32(b, 4, static_cast<uint32_t>(b.size() - 8));
    return b;
}

static Bytes fuz(const Bytes& payload, uint32_t lipSize) {
    Bytes b;
    puts4(b, "FUZE");
    put32(b, 1);
    put32(b, lipSize);
    b.insert(b.end(), lipSize, 0x5A);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

static void test_audio() {
    AudioInfo i;
    auto w = riff("WAVE", 1, 1, 44100, 88200, 16, 88200 * 3);
    CHECK(read_audio(w.data(), w.size(), i) == Status::Ok);
    CHECK(i.container == AudioContainer::Wav && i.payload == AudioContainer::Wav && i.formatTag == 1);
    CHECK(i.sampleRate == 44100 && i.channels == 1 && i.bitsPerSample == 16 && i.bitRate == 88200 * 8);
    CHECK(i.durationSource == DurationSource::DataRate && i.durationSeconds > 2.99 && i.durationSeconds < 3.01);
    CHECK(i.dataChunkFound && i.dataBytes == 88200 * 3 && !i.hasLip);
    CHECK(std::string(codec_name(i.formatTag, i.payload)) == "PCM");

    // xWMA: dpds last entry = 2 s of 16-bit stereo at 44.1 kHz.
    auto x = riff("XWMA", 0x161, 2, 44100, 8000, 16, 16000, {40000, 176400 * 2});
    CHECK(read_audio(x.data(), x.size(), i) == Status::Ok);
    CHECK(i.container == AudioContainer::Xwm && i.durationSource == DurationSource::Dpds);
    CHECK(i.durationSeconds > 1.99 && i.durationSeconds < 2.01);
    CHECK(std::string(codec_name(i.formatTag, i.payload)) == "xWMA (WMA v2)");

    // fact beats data/rate; dpds beats fact.
    auto f = riff("WAVE", 2, 1, 22050, 11025, 4, 11025, {}, 22050 * 5);
    CHECK(read_audio(f.data(), f.size(), i) == Status::Ok && i.durationSource == DurationSource::Fact && i.durationSeconds > 4.99 && i.durationSeconds < 5.01);

    // FUZ around XWM and around WAV.
    auto z = fuz(x, 777);
    CHECK(read_audio(z.data(), z.size(), i) == Status::Ok);
    CHECK(i.container == AudioContainer::Fuz && i.payload == AudioContainer::Xwm && i.hasLip && i.lipBytes == 777 && i.fuzVersion == 1);
    CHECK(i.durationSource == DurationSource::Dpds && i.sampleRate == 44100);
    z = fuz(w, 0);
    CHECK(read_audio(z.data(), z.size(), i) == Status::Ok && i.payload == AudioContainer::Wav && !i.hasLip);

    // Partial buffers.
    CHECK(read_audio(z.data(), 20, i) == Status::Truncated);  // lip + payload beyond
    auto zt = fuz(x, 777);
    CHECK(read_audio(zt.data(), 12 + 777 + 30, i) == Status::Truncated);  // fmt not complete
    auto xt = x;
    xt.resize(12 + 8 + 18 + 8 + 4);  // dpds header + first entry only, no data chunk
    CHECK(read_audio(xt.data(), xt.size(), i) == Status::Ok && i.dpdsTruncated && !i.dataChunkFound && i.durationSource == DurationSource::None);

    // Rejections.
    const uint8_t junk[17] = "RIFF....AVI ....";
    CHECK(read_audio(junk, 16, i) == Status::Unsupported);
    const uint8_t other[8] = "OggS\0\0\0";
    CHECK(read_audio(other, 8, i) == Status::NotRecognized);
    auto nofmt = fuz(Bytes{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'd', 'a', 't', 'a', 4, 0, 0, 0, 1, 2, 3, 4}, 0);
    CHECK(read_audio(nofmt.data(), nofmt.size(), i) == Status::Corrupt);
    CHECK(detect(w.data(), w.size()) == Kind::Wav && detect(x.data(), x.size()) == Kind::Xwm && detect(z.data(), z.size()) == Kind::Fuz);
}

// ---------------------------------------------------------------- hostile

static void test_hostile() {
    Bytes samples[] = {dds(64, 64, 4, "DX10", 98, 0, 4), tga(32, 32, 32, 10, 0x28),
                       nif(0x14020007, 12, 130, {"NiNode", "BSTriShape"}, {0, 1}),
                       fuz(riff("XWMA", 0x161, 2, 44100, 8000, 16, 16000, {1, 2, 3}), 9)};
    uint32_t seed = 42;
    auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed; };
    for (const Bytes& s : samples) {
        for (size_t n = 0; n <= s.size(); ++n) {
            DdsInfo d; TgaInfo t; NifInfo f; AudioInfo a;
            read_dds(s.data(), n, d);
            read_tga(s.data(), n, t);
            read_nif(s.data(), n, f);
            read_audio(s.data(), n, a);
        }
        for (int iter = 0; iter < 3000; ++iter) {
            Bytes b = s;
            const int flips = 1 + static_cast<int>(rnd() % 4);
            for (int k = 0; k < flips; ++k) b[rnd() % b.size()] = static_cast<uint8_t>(rnd());
            DdsInfo d; TgaInfo t; NifInfo f; AudioInfo a;
            read_dds(b.data(), b.size(), d);
            read_tga(b.data(), b.size(), t);
            read_nif(b.data(), b.size(), f);
            read_audio(b.data(), b.size(), a);
        }
    }
}

int main() {
    test_dds();
    test_tga();
    test_nif();
    test_audio();
    test_hostile();
    if (g_failures) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("all tests passed");
    return 0;
}
