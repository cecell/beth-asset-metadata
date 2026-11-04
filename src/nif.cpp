// NIF header reader: version line, binary version, user versions, block
// count and the block-type table. Block bodies are never parsed.
// Version conditions follow nif.xml so older (Oblivion, Morrowind) headers
// degrade to "version only" rather than failing.
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include "common.hpp"

#include <new>

namespace asset {
using namespace detail;

namespace {

constexpr uint32_t V(uint8_t a, uint8_t b, uint8_t c, uint8_t d) noexcept {
    return (uint32_t{a} << 24) | (uint32_t{b} << 16) | (uint32_t{c} << 8) | d;
}

// Geometry block types that tell the two Skyrims apart. The BSTriShape
// family was introduced with SE; the NiTri* family is the LE style. SE loads
// NiTriShape, but not NiTriStrips (CAO's "necessary" fixes convert those).
const char* const kSeGeometry[] = {"BSTriShape", "BSDynamicTriShape", "BSMeshLODTriShape", "BSSubIndexTriShape"};
const char* const kLegacyGeometry[] = {"NiTriShape", "NiTriShapeData", "NiTriStrips", "NiTriStripsData",
                                       "bhkPackedNiTriStripsShape", "bhkPackedNiTriStripsData"};

struct Cursor {
    const uint8_t* d;
    size_t n, pos = 0;
    bool need(size_t k) const noexcept { return k <= n - pos; }
    uint8_t u8() noexcept { return d[pos++]; }
    uint16_t u16() noexcept { const uint16_t v = rd16(d + pos); pos += 2; return v; }
    uint32_t u32() noexcept { const uint32_t v = rd32(d + pos); pos += 4; return v; }
};

// Parses "a.b.c.d" after "Version " in the header line; 0 if absent.
uint32_t version_from_line(const std::string& line) noexcept {
    const size_t at = line.find("Version ");
    if (at == std::string::npos) return 0;
    uint32_t parts[4] = {0, 0, 0, 0};
    size_t i = at + 8, k = 0;
    while (i < line.size() && k < 4) {
        if (line[i] >= '0' && line[i] <= '9') {
            parts[k] = parts[k] * 10 + static_cast<uint32_t>(line[i] - '0');
            if (parts[k] > 255) return 0;
        } else if (line[i] == '.') {
            ++k;
        } else {
            break;
        }
        ++i;
    }
    return V(static_cast<uint8_t>(parts[0]), static_cast<uint8_t>(parts[1]), static_cast<uint8_t>(parts[2]), static_cast<uint8_t>(parts[3]));
}

Status parse(const uint8_t* d, size_t n, NifInfo& o) {
    // Header line, newline-terminated, short.
    size_t eol = 0;
    while (eol < n && eol < 128 && d[eol] != '\n') ++eol;
    if (eol == n) return n < 128 ? Status::Truncated : Status::NotRecognized;
    if (eol >= 128) return Status::NotRecognized;
    o.headerLine.assign(reinterpret_cast<const char*>(d), eol);
    if (o.headerLine.compare(0, 21, "Gamebryo File Format,") != 0 && o.headerLine.compare(0, 23, "NetImmerse File Format,") != 0)
        return Status::NotRecognized;
    const uint32_t lineVersion = version_from_line(o.headerLine);
    if (lineVersion == 0) return Status::Corrupt;

    Cursor c{d, n, eol + 1};
    o.version = lineVersion;
    if (lineVersion >= V(3, 3, 0, 13)) {
        if (!c.need(4)) return Status::Truncated;
        o.version = c.u32();
        if (o.version != lineVersion) return Status::Corrupt;
    }
    if (o.version >= V(20, 0, 0, 4)) {
        if (!c.need(1)) return Status::Truncated;
        o.littleEndian = c.u8() == 1;
        if (!o.littleEndian) return Status::Unsupported;
    }
    if (o.version >= V(10, 0, 1, 8)) {
        if (!c.need(4)) return Status::Truncated;
        o.userVersion = c.u32();
    }
    if (o.version >= V(3, 3, 0, 13)) {
        if (!c.need(4)) return Status::Truncated;
        o.blockCount = c.u32();
    }

    const bool bsHeader = o.userVersion >= 3 &&
                          (o.version == V(20, 2, 0, 7) || o.version == V(20, 0, 0, 5) ||
                           (o.version >= V(10, 0, 1, 2) && o.version <= V(20, 0, 0, 4) && o.userVersion <= 11));
    if (bsHeader) {
        o.hasBsHeader = true;
        if (!c.need(4)) return Status::Truncated;
        o.bsVersion = c.u32();
        auto shortString = [&]() -> Status {
            if (!c.need(1)) return Status::Truncated;
            const uint8_t len = c.u8();
            if (!c.need(len)) return Status::Truncated;
            c.pos += len;
            return Status::Ok;
        };
        Status s = shortString();  // author
        if (s != Status::Ok) return s;
        if (o.bsVersion > 130) {
            if (!c.need(4)) return Status::Truncated;
            c.u32();  // unknown int 3
        }
        if (o.bsVersion < 131) {
            s = shortString();  // process script
            if (s != Status::Ok) return s;
        }
        s = shortString();  // export script
        if (s != Status::Ok) return s;
        if (o.bsVersion >= 103) {
            s = shortString();  // max filepath
            if (s != Status::Ok) return s;
        }
    }

    // Block-type table: versions 5.0.0.1 .. 20.x. Later formats (Starfield's
    // 30.x) add metadata before it; stop with what we have.
    if (o.version < V(5, 0, 0, 1) || o.version >= V(30, 0, 0, 0)) return Status::Ok;
    if (!c.need(2)) return Status::Truncated;
    const uint16_t typeCount = c.u16();
    o.blockTypes.reserve(typeCount);
    for (uint16_t i = 0; i < typeCount; ++i) {
        if (!c.need(4)) return Status::Truncated;
        const uint32_t len = c.u32();
        if (len == 0 || len > 128) return Status::Corrupt;  // type names are short identifiers
        if (!c.need(len)) return Status::Truncated;
        o.blockTypes.emplace_back(reinterpret_cast<const char*>(d + c.pos), len);
        c.pos += len;
    }
    if (o.blockCount > (n - c.pos) / 2) return Status::Truncated;
    o.blockTypeIndex.reserve(o.blockCount);
    for (uint32_t i = 0; i < o.blockCount; ++i) {
        const uint16_t idx = c.u16() & 0x7FFF;
        if (idx >= typeCount) return Status::Corrupt;
        o.blockTypeIndex.push_back(idx);
    }
    // Block sizes (>= 20.2.0.5), strings and groups follow; not needed.

    for (const std::string& t : o.blockTypes) {
        for (const char* s : kSeGeometry) if (t == s) o.seGeometry = true;
        for (const char* s : kLegacyGeometry) if (t == s) o.legacyGeometry = true;
        if (t == "NiTriStrips" || t == "NiTriStripsData") o.stripGeometry = true;
    }
    const bool skyrimHeader = o.version == V(20, 2, 0, 7) && o.userVersion == 12 &&
                              ((o.bsVersion >= 83 && o.bsVersion <= 100) || o.bsVersion == 130);
    if (skyrimHeader) {
        if (o.seGeometry) o.engine = NifEngine::SkyrimSe;
        else if (o.stripGeometry) o.engine = NifEngine::SkyrimLe;
        else o.engine = NifEngine::SkyrimEither;
    }
    return Status::Ok;
}

}  // namespace

const char* to_string(NifEngine e) noexcept {
    switch (e) {
        case NifEngine::SkyrimLe: return "le";
        case NifEngine::SkyrimSe: return "sse";
        case NifEngine::SkyrimEither: return "either";
        case NifEngine::Unknown: break;
    }
    return "unknown";
}

std::string nif_version_string(uint32_t v) {
    return std::to_string(v >> 24) + "." + std::to_string((v >> 16) & 0xFF) + "." + std::to_string((v >> 8) & 0xFF) + "." + std::to_string(v & 0xFF);
}

Status read_nif(const uint8_t* d, size_t n, NifInfo& o) noexcept {
    o = NifInfo{};
    try {
        return parse(d, n, o);
    } catch (const std::bad_alloc&) {
        return Status::OutOfMemory;
    } catch (...) {
        return Status::Corrupt;
    }
}

}  // namespace asset
