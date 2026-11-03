// DDS header reader. Layout: "DDS " + DDS_HEADER (124 bytes) + optional
// DDS_HEADER_DXT10 (20 bytes) when the pixel format's FourCC is "DX10".
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include "common.hpp"

namespace asset {
using namespace detail;

namespace {

constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000;
constexpr uint32_t DDPF_ALPHAPIXELS = 0x1;
constexpr uint32_t DDPF_ALPHA = 0x2;
constexpr uint32_t DDPF_FOURCC = 0x4;
constexpr uint32_t DDPF_RGB = 0x40;
constexpr uint32_t DDPF_LUMINANCE = 0x20000;
constexpr uint32_t DDSCAPS2_CUBEMAP = 0x200;
constexpr uint32_t DDSCAPS2_VOLUME = 0x200000;
constexpr uint32_t D3D_RESOURCE_MISC_TEXTURECUBE = 0x4;

struct DxgiDesc {
    uint32_t id;
    DdsFormat format;
    uint8_t blockBytes;  // 0 = uncompressed
    uint8_t bpp;         // uncompressed only
    bool srgb;
    bool alpha;
};

// DXGI_FORMAT values that matter for game textures.
constexpr DxgiDesc kDxgi[] = {
    {2, DdsFormat::R32G32B32A32F, 0, 128, false, true},
    {10, DdsFormat::R16G16B16A16F, 0, 64, false, true},
    {28, DdsFormat::R8G8B8A8, 0, 32, false, true},
    {29, DdsFormat::R8G8B8A8, 0, 32, true, true},
    {49, DdsFormat::R8G8, 0, 16, false, false},
    {61, DdsFormat::R8, 0, 8, false, false},
    {65, DdsFormat::A8, 0, 8, false, true},
    {71, DdsFormat::BC1, 8, 0, false, false},
    {72, DdsFormat::BC1, 8, 0, true, false},
    {74, DdsFormat::BC2, 16, 0, false, true},
    {75, DdsFormat::BC2, 16, 0, true, true},
    {77, DdsFormat::BC3, 16, 0, false, true},
    {78, DdsFormat::BC3, 16, 0, true, true},
    {80, DdsFormat::BC4, 8, 0, false, false},
    {81, DdsFormat::BC4, 8, 0, false, false},
    {83, DdsFormat::BC5, 16, 0, false, false},
    {84, DdsFormat::BC5, 16, 0, false, false},
    {85, DdsFormat::B5G6R5, 0, 16, false, false},
    {86, DdsFormat::B5G5R5A1, 0, 16, false, true},
    {87, DdsFormat::B8G8R8A8, 0, 32, false, true},
    {88, DdsFormat::B8G8R8X8, 0, 32, false, false},
    {91, DdsFormat::B8G8R8A8, 0, 32, true, true},
    {93, DdsFormat::B8G8R8X8, 0, 32, true, false},
    {95, DdsFormat::BC6H, 16, 0, false, false},
    {96, DdsFormat::BC6H, 16, 0, false, false},
    {98, DdsFormat::BC7, 16, 0, false, true},
    {99, DdsFormat::BC7, 16, 0, true, true},
    {115, DdsFormat::B4G4R4A4, 0, 16, false, true},
};

void classify_fourcc(const char* cc, DdsInfo& o) noexcept {
    struct E { const char* cc; DdsFormat f; uint8_t block; bool alpha; };
    static const E table[] = {
        {"DXT1", DdsFormat::BC1, 8, false},  {"DXT2", DdsFormat::BC2, 16, true}, {"DXT3", DdsFormat::BC2, 16, true},
        {"DXT4", DdsFormat::BC3, 16, true},  {"DXT5", DdsFormat::BC3, 16, true}, {"ATI1", DdsFormat::BC4, 8, false},
        {"BC4U", DdsFormat::BC4, 8, false},  {"BC4S", DdsFormat::BC4, 8, false}, {"ATI2", DdsFormat::BC5, 16, false},
        {"BC5U", DdsFormat::BC5, 16, false}, {"BC5S", DdsFormat::BC5, 16, false},
    };
    for (const E& e : table)
        if (std::memcmp(cc, e.cc, 4) == 0) {
            o.format = e.f;
            o.blockBytes = e.block;
            o.compressed = true;
            o.hasAlpha = e.alpha;
            return;
        }
    // D3DFMT numeric FourCCs for the float formats.
    const uint32_t n = rd32(reinterpret_cast<const uint8_t*>(cc));
    if (n == 113) { o.format = DdsFormat::R16G16B16A16F; o.bitsPerPixel = 64; o.hasAlpha = true; return; }
    if (n == 116) { o.format = DdsFormat::R32G32B32A32F; o.bitsPerPixel = 128; o.hasAlpha = true; return; }
    o.format = DdsFormat::Other;
}

void classify_masks(uint32_t pfFlags, uint32_t bpp, uint32_t r, uint32_t g, uint32_t b, uint32_t a, DdsInfo& o) noexcept {
    o.bitsPerPixel = bpp;
    const bool alphaFlag = (pfFlags & DDPF_ALPHAPIXELS) != 0;
    if (bpp == 32) {
        if (r == 0x000000FF && g == 0x0000FF00 && b == 0x00FF0000) o.format = (a == 0xFF000000 || alphaFlag) ? DdsFormat::R8G8B8A8 : DdsFormat::Other;
        else if (r == 0x00FF0000 && g == 0x0000FF00 && b == 0x000000FF) o.format = (a == 0xFF000000 || alphaFlag) ? DdsFormat::B8G8R8A8 : DdsFormat::B8G8R8X8;
        else o.format = DdsFormat::Other;
    } else if (bpp == 24) {
        if (r == 0x00FF0000 && g == 0x0000FF00 && b == 0x000000FF) o.format = DdsFormat::B8G8R8;
        else if (r == 0x000000FF && g == 0x0000FF00 && b == 0x00FF0000) o.format = DdsFormat::R8G8B8;
        else o.format = DdsFormat::Other;
    } else if (bpp == 16) {
        if (r == 0xF800 && g == 0x07E0 && b == 0x001F) o.format = DdsFormat::B5G6R5;
        else if (r == 0x7C00 && g == 0x03E0 && b == 0x001F) o.format = DdsFormat::B5G5R5A1;
        else if (r == 0x0F00 && g == 0x00F0 && b == 0x000F) o.format = DdsFormat::B4G4R4A4;
        else if ((pfFlags & DDPF_LUMINANCE) && r == 0x00FF && a == 0xFF00) o.format = DdsFormat::Other;  // A8L8
        else o.format = DdsFormat::Other;
    } else if (bpp == 8) {
        if (pfFlags & DDPF_LUMINANCE) o.format = DdsFormat::L8;
        else if ((pfFlags & DDPF_ALPHA) && !(pfFlags & DDPF_RGB)) o.format = DdsFormat::A8;
        else if (r == 0xFF) o.format = DdsFormat::R8;
        else o.format = DdsFormat::Other;
    } else {
        o.format = DdsFormat::Other;
    }
    o.hasAlpha = alphaFlag || (pfFlags & DDPF_ALPHA) != 0 || a != 0;
}

}  // namespace

const char* to_string(DdsFormat f) noexcept {
    switch (f) {
        case DdsFormat::Unknown: return "unknown";
        case DdsFormat::BC1: return "BC1 (DXT1)";
        case DdsFormat::BC2: return "BC2 (DXT3)";
        case DdsFormat::BC3: return "BC3 (DXT5)";
        case DdsFormat::BC4: return "BC4";
        case DdsFormat::BC5: return "BC5";
        case DdsFormat::BC6H: return "BC6H";
        case DdsFormat::BC7: return "BC7";
        case DdsFormat::R8G8B8A8: return "R8G8B8A8";
        case DdsFormat::B8G8R8A8: return "B8G8R8A8";
        case DdsFormat::B8G8R8X8: return "B8G8R8X8";
        case DdsFormat::R8G8B8: return "R8G8B8";
        case DdsFormat::B8G8R8: return "B8G8R8";
        case DdsFormat::R8: return "R8";
        case DdsFormat::R8G8: return "R8G8";
        case DdsFormat::A8: return "A8";
        case DdsFormat::L8: return "L8";
        case DdsFormat::B5G6R5: return "B5G6R5";
        case DdsFormat::B5G5R5A1: return "B5G5R5A1";
        case DdsFormat::B4G4R4A4: return "B4G4R4A4";
        case DdsFormat::R16G16B16A16F: return "R16G16B16A16_FLOAT";
        case DdsFormat::R32G32B32A32F: return "R32G32B32A32_FLOAT";
        case DdsFormat::Other: return "other";
    }
    return "unknown";
}

Status read_dds(const uint8_t* d, size_t n, DdsInfo& o) noexcept {
    o = DdsInfo{};
    if (n < 4) return Status::Truncated;
    if (!tag4(d, "DDS ")) return Status::NotRecognized;
    if (n < 128) return Status::Truncated;
    if (rd32(d + 4) != 124 || rd32(d + 76) != 32) return Status::Corrupt;  // dwSize, ddspf.dwSize

    const uint32_t flags = rd32(d + 8);
    o.height = rd32(d + 12);
    o.width = rd32(d + 16);
    o.depth = rd32(d + 24);
    const uint32_t mips = rd32(d + 28);
    const uint32_t pfFlags = rd32(d + 80);
    const uint32_t caps2 = rd32(d + 112);

    o.mipmaps = (flags & DDSD_MIPMAPCOUNT) ? (mips ? mips : 1) : 1;
    o.cubemap = (caps2 & DDSCAPS2_CUBEMAP) != 0;
    o.volume = (caps2 & DDSCAPS2_VOLUME) != 0;
    if (!o.volume) o.depth = 0;

    if (pfFlags & DDPF_FOURCC) {
        std::memcpy(o.fourcc, d + 84, 4);
        for (char& c : o.fourcc) if (c != 0 && (c < 0x20 || c > 0x7E)) c = '?';
        if (tag4(d + 84, "DX10")) {
            if (n < kDdsProbeBytes) return Status::Truncated;
            o.dx10 = true;
            o.dxgiFormat = rd32(d + 128);
            const uint32_t misc = rd32(d + 136);
            o.arraySize = rd32(d + 140);
            if (misc & D3D_RESOURCE_MISC_TEXTURECUBE) o.cubemap = true;
            o.format = DdsFormat::Other;
            for (const DxgiDesc& e : kDxgi)
                if (e.id == o.dxgiFormat) {
                    o.format = e.format;
                    o.blockBytes = e.blockBytes;
                    o.compressed = e.blockBytes != 0;
                    o.bitsPerPixel = e.bpp;
                    o.srgb = e.srgb;
                    o.hasAlpha = e.alpha;
                    break;
                }
        } else {
            classify_fourcc(o.fourcc, o);
        }
    } else {
        classify_masks(pfFlags, rd32(d + 88), rd32(d + 92), rd32(d + 96), rd32(d + 100), rd32(d + 104), o);
    }
    return Status::Ok;
}

}  // namespace asset
