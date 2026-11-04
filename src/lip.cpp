// Skyrim .lip header: version, the size field and the unit count derived
// from it. The keyframe stream after the header is not decoded. Layout
// measured on 14,837 loose files and the LIP blocks inside FUZ files; no
// public specification exists.
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include "common.hpp"

namespace asset {
using namespace detail;

Status read_lip(const uint8_t* d, size_t n, LipInfo& o) noexcept {
    o = LipInfo{};
    o.bytes = n;
    if (n < 16) return Status::Truncated;
    o.version = rd32(d);
    if (o.version != 1) return Status::NotRecognized;  // no magic: version 1 is all there is
    o.sizeField = rd32(d + 4);
    o.field8 = rd32(d + 8);
    o.headerUnits = rd16(d + 12);
    // The size field is always 132 * units + 28; it, not the u16 at offset
    // 12, is authoritative (one file in 14,837 disagrees with the u16).
    if (o.sizeField < 28 || (o.sizeField - 28) % 132 != 0 || (o.sizeField - 28) / 132 > 1000000)
        return Status::Corrupt;
    o.units = (o.sizeField - 28) / 132;
    return Status::Ok;
}

}  // namespace asset
