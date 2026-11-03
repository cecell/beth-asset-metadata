// TGA header reader (18-byte Truevision header, no magic).
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include "common.hpp"

namespace asset {
using namespace detail;

Status read_tga(const uint8_t* d, size_t n, TgaInfo& o) noexcept {
    o = TgaInfo{};
    if (n < kTgaProbeBytes) return Status::Truncated;
    const uint8_t colorMapType = d[1], imageType = d[2];
    const uint16_t cmapLength = rd16(d + 5);
    const uint8_t cmapDepth = d[7];
    const uint16_t width = rd16(d + 12), height = rd16(d + 14);
    const uint8_t depth = d[16], descriptor = d[17];

    if (colorMapType > 1) return Status::Corrupt;
    const uint8_t base = imageType & 0x07;
    const bool rle = (imageType & 0x08) != 0;
    if (base < 1 || base > 3 || (imageType & 0xF0) != 0) return Status::Corrupt;
    if (depth != 8 && depth != 15 && depth != 16 && depth != 24 && depth != 32) return Status::Corrupt;
    if (width == 0 || height == 0) return Status::Corrupt;
    if (base == 1 && (colorMapType != 1 || cmapLength == 0 || (cmapDepth != 15 && cmapDepth != 16 && cmapDepth != 24 && cmapDepth != 32)))
        return Status::Corrupt;
    if (base != 1 && colorMapType == 1 && cmapLength == 0) return Status::Corrupt;
    if ((base == 1 || base == 3) && depth != 8 && depth != 16) return Status::Corrupt;
    if (descriptor & 0xC0) return Status::Corrupt;  // reserved interleave bits

    o.width = width;
    o.height = height;
    o.bitsPerPixel = depth;
    o.alphaBits = descriptor & 0x0F;
    o.imageType = imageType;
    o.rle = rle;
    o.colorMapped = base == 1;
    o.grayscale = base == 3;
    o.topLeft = (descriptor & 0x20) != 0;
    o.rightToLeft = (descriptor & 0x10) != 0;
    return Status::Ok;
}

}  // namespace asset
