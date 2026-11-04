// NIF block bodies: walks a Skyrim NIF's header to the block sizes, then
// reads only the blocks that name textures and the skin partitions (for
// strips). Everything else is skipped by its recorded size, so other block
// layouts never matter.
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include "common.hpp"

#include <algorithm>
#include <map>
#include <new>

namespace asset {
using namespace detail;

namespace {

// Every read checks the bytes left; a failed read sets `bad` and returns 0,
// so a parse can run on and check once.
struct Cursor {
    const uint8_t* d;
    size_t n, pos = 0;
    bool bad = false;
    bool has(size_t k) noexcept {
        if (bad || k > n - pos) bad = true;
        return !bad;
    }
    uint8_t u8() noexcept { return has(1) ? d[pos++] : 0; }
    uint16_t u16() noexcept {
        if (!has(2)) return 0;
        const uint16_t v = rd16(d + pos);
        pos += 2;
        return v;
    }
    uint32_t u32() noexcept {
        if (!has(4)) return 0;
        const uint32_t v = rd32(d + pos);
        pos += 4;
        return v;
    }
    void skip(uint64_t k) noexcept {
        if (k > n - pos) bad = true;
        if (!bad) pos += static_cast<size_t>(k);
    }
    // A u32-length string, NULs trimmed; longer than maxLen is damage.
    std::string sized(uint32_t maxLen) {
        const uint32_t len = u32();
        if (len > maxLen) bad = true;
        if (!has(len)) return {};
        const char* p = reinterpret_cast<const char*>(d + pos);
        pos += len;
        return std::string(p, std::find(p, p + len, '\0'));
    }
};

// A NiSkinPartition block: whether any partition stores strips, and (SSE)
// the shared vertex count and the triangle total, which skinned SE shapes
// keep only here (nif.xml, 20.2.0.7: BS versions above 34 add LOD level and
// Global VB, SSE (100) adds the shared vertex data up front and
// per-partition copies).
struct SkinPartition {
    bool strips = false;
    uint64_t vertices = 0, triangles = 0;
};
SkinPartition ReadSkinPartition(Cursor k, uint32_t bsVersion) {
    SkinPartition out;
    const uint32_t partitions = k.u32();
    if (bsVersion == 100) {
        const uint32_t dataSize = k.u32(), vertexSize = k.u32();
        k.skip(8);         // vertex desc
        k.skip(dataSize);  // vertex data
        if (vertexSize) out.vertices = dataSize / vertexSize;
    }
    for (uint32_t p = 0; p < partitions && p < 4096 && !k.bad; ++p) {
        const uint16_t vertices = k.u16(), triangles = k.u16(), bones = k.u16(), strips = k.u16(),
                       weights = k.u16();
        if (k.bad) return out;
        out.triangles += triangles;
        if (strips > 0) {
            out.strips = true;
            return out;
        }
        k.skip(uint64_t{bones} * 2);
        if (k.u8()) k.skip(uint64_t{vertices} * 2);            // vertex map
        if (k.u8()) k.skip(uint64_t{vertices} * weights * 4);  // vertex weights
        if (k.u8()) k.skip(uint64_t{triangles} * 6);           // faces (no strips here)
        if (k.u8()) k.skip(uint64_t{vertices} * weights);      // bone indices
        if (bsVersion > 34) k.skip(2);                         // LOD level, Global VB
        if (bsVersion == 100) k.skip(8 + uint64_t{triangles} * 6);  // vertex desc, triangles copy
    }
    return out;
}

// Vertex and triangle counts of a BSTriShape-family block (BS versions 100
// and 130: NiAVObject, bounds, refs, vertex desc, counts, data size). True
// when the stored data size agrees with the counts (or no data is inline,
// as for skinned SE shapes whose vertices live in the skin partition).
bool SeShapeCounts(Cursor k, uint32_t bsVersion, uint32_t* vertices, uint32_t* triangles, uint32_t* skinRef,
                   bool* inlineData) {
    k.skip(4);                      // name
    k.skip(uint64_t{k.u32()} * 4);  // extra data refs
    k.skip(4);                      // controller
    k.skip(4 + 12 + 36 + 4 + 4);    // flags, translation, rotation, scale, collision
    k.skip(16);                     // bounding sphere
    if (bsVersion >= 155) k.skip(24);
    *skinRef = k.u32();
    k.skip(4 + 4);                  // shader, alpha
    const uint32_t descLow = k.u32();
    k.skip(4);                      // vertex desc, high half
    const uint32_t tris = bsVersion >= 130 ? k.u32() : k.u16();
    const uint32_t verts = k.u16();
    const uint32_t dataSize = k.u32();
    if (k.bad) return false;
    const uint64_t vertexSize = uint64_t{descLow & 0xF} * 4;
    if (dataSize != 0 && dataSize != verts * vertexSize + uint64_t{tris} * 6) return false;
    *inlineData = dataSize != 0;
    *vertices = verts;
    *triangles = tris;
    return true;
}

// NiTriShapeData / NiTriStripsData (Skyrim LE style): parsed to the end; true
// only when the parse lands exactly on the block end.
bool LegacyShapeCounts(Cursor k, bool strips, uint32_t* vertices, uint32_t* triangles) {
    k.skip(4);                          // group ID
    const uint16_t verts = k.u16();
    k.skip(2);                          // keep and compress flags
    if (k.u8()) k.skip(uint64_t{verts} * 12);  // vertices
    const uint16_t vectorFlags = k.u16();
    k.skip(4);                          // material CRC (20.2.0.7)
    if (k.u8()) {                       // normals
        k.skip(uint64_t{verts} * 12);
        if (vectorFlags & 0x1000) k.skip(uint64_t{verts} * 24);  // tangents, bitangents
    }
    k.skip(16);                         // center, radius
    if (k.u8()) k.skip(uint64_t{verts} * 16);  // vertex colors
    if (vectorFlags & 1) k.skip(uint64_t{verts} * 8);  // UV set
    k.skip(2 + 4);                      // consistency flags, additional data
    const uint16_t tris = k.u16();
    if (strips) {
        const uint16_t count = k.u16();
        uint64_t points = 0;
        for (uint16_t i = 0; i < count && !k.bad; ++i) points += k.u16();
        if (k.u8()) k.skip(points * 2);
    } else {
        k.skip(4);                      // triangle points
        if (k.u8()) k.skip(uint64_t{tris} * 6);
        const uint16_t groups = k.u16();
        for (uint16_t i = 0; i < groups && !k.bad; ++i) k.skip(uint64_t{k.u16()} * 2);
    }
    if (k.bad || k.pos != k.n) return false;
    *vertices = verts;
    *triangles = tris;
    return true;
}

Status parse(const uint8_t* d, size_t n, NifBody& out) {
    const void* nl = std::memchr(d, '\n', std::min<size_t>(n, 128));
    if (!nl) return n < 128 ? Status::Truncated : Status::NotRecognized;
    Cursor c{d, n, static_cast<size_t>(static_cast<const uint8_t*>(nl) - d) + 1};
    const uint32_t version = c.u32();
    const uint8_t endian = c.u8();
    const uint32_t userVersion = c.u32();
    const uint32_t blocks = c.u32();
    if (c.bad) return Status::Truncated;
    if (version != 0x14020007 || endian != 1 || userVersion != 12) return Status::Unsupported;  // Skyrim only
    const uint32_t bsVersion = c.u32();
    auto shortString = [&] { c.skip(c.u8()); };
    shortString();                         // author
    if (bsVersion > 130) c.skip(4);
    if (bsVersion < 131) shortString();    // process script
    shortString();                         // export script
    if (bsVersion >= 103) shortString();   // max filepath
    const uint16_t typeCount = c.u16();
    std::vector<std::string> types;
    for (uint16_t i = 0; i < typeCount && !c.bad; ++i) types.push_back(c.sized(128));
    if (c.bad) return Status::Truncated;
    if (blocks > (n - c.pos) / 6) return Status::Truncated;  // each block: a type index and a size
    std::vector<uint16_t> typeOf(blocks);
    std::vector<uint32_t> sizes(blocks);
    for (auto& t : typeOf) t = c.u16() & 0x7FFF;
    for (auto& s : sizes) s = c.u32();
    const uint32_t strings = c.u32();
    c.skip(4);  // max string length
    for (uint32_t i = 0; i < strings && !c.bad; ++i) c.skip(c.u32());
    c.skip(uint64_t{c.u32()} * 4);  // groups
    if (c.bad) return Status::Truncated;

    size_t off = c.pos;
    // Skinned SE shapes without inline data take their counts from their
    // skin partition: shape -> skin instance -> partition, by block index.
    std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> skinnedShapes;  // skin ref, (verts, tris)
    std::map<uint32_t, uint32_t> partitionOf;                                      // skin instance -> partition
    std::map<uint32_t, SkinPartition> partitions;
    for (uint32_t b = 0; b < blocks; ++b) {
        if (sizes[b] > n - off) return Status::Truncated;
        if (typeOf[b] >= types.size()) return Status::Corrupt;
        const std::string& type = types[typeOf[b]];
        Cursor k{d + off, sizes[b]};
        if (type == "BSShaderTextureSet") {
            const uint32_t count = k.u32();
            for (uint32_t i = 0; i < count && i < 64 && !k.bad; ++i) {
                std::string tex = k.sized(1024);
                if (!k.bad && !tex.empty()) out.textures.push_back({type, i, std::move(tex)});
            }
        } else if (type == "BSEffectShaderProperty" && bsVersion < 130) {
            k.skip(4);                      // name
            k.skip(uint64_t{k.u32()} * 4);  // extra data refs
            k.skip(4 + 8 + 16);             // controller, shader flags 1 and 2, UV offset and scale
            std::string source = k.sized(1024);
            if (!k.bad && !source.empty()) out.textures.push_back({type, 0, std::move(source)});
            k.skip(4 + 16);                 // clamp, lighting influence, env map LOD, unused; falloff
            k.skip(16 + 4 + 4);             // emissive color and multiple, soft falloff depth
            std::string grey = k.sized(1024);
            if (!k.bad && !grey.empty()) out.textures.push_back({type, 1, std::move(grey)});
        } else if (type == "NiSkinPartition") {
            const SkinPartition sp = ReadSkinPartition(k, bsVersion);
            if (sp.strips) out.skinStrips = true;
            partitions[b] = sp;
        } else if (type == "NiSkinInstance" || type == "BSDismemberSkinInstance") {
            k.skip(4);  // skin data
            const uint32_t part = k.u32();
            if (!k.bad) partitionOf[b] = part;
        } else if (type == "BSTriShape" || type == "BSDynamicTriShape" || type == "BSMeshLODTriShape" ||
                   type == "BSSubIndexTriShape" || type == "NiTriShapeData" || type == "NiTriStripsData") {
            uint32_t v = 0, t = 0, skin = 0xFFFFFFFF;
            bool inlineData = true;
            const bool ok = type[0] == 'B' ? SeShapeCounts(k, bsVersion, &v, &t, &skin, &inlineData)
                                           : LegacyShapeCounts(k, type == "NiTriStripsData", &v, &t);
            ++out.shapes;
            if (!ok) {
                ++out.shapesUnread;
            } else if (!inlineData && skin != 0xFFFFFFFF) {
                skinnedShapes.push_back({skin, {v, t}});
            } else {
                out.vertices += v;
                out.triangles += t;
            }
        }
        off += sizes[b];
    }
    for (const auto& [skin, counts] : skinnedShapes) {
        auto pi = partitionOf.find(skin);
        auto sp = pi == partitionOf.end() ? partitions.end() : partitions.find(pi->second);
        if (sp == partitions.end()) {
            ++out.shapesUnread;
            continue;
        }
        // The shape's own counts when it has them (BSDynamicTriShape keeps
        // its vertices), the partition's otherwise.
        out.vertices += counts.first ? counts.first : sp->second.vertices;
        out.triangles += counts.second ? counts.second : sp->second.triangles;
    }
    return Status::Ok;
}

}  // namespace

Status read_nif_body(const uint8_t* data, size_t len, NifBody& out) noexcept {
    out = NifBody{};
    Status s;
    try {
        s = parse(data, len, out);
    } catch (const std::bad_alloc&) {
        s = Status::OutOfMemory;
    } catch (...) {
        s = Status::Corrupt;
    }
    if (s != Status::Ok) out = NifBody{};
    return s;
}

Status read_nif_textures(const uint8_t* data, size_t len, std::vector<NifTexture>& out) noexcept {
    NifBody body;
    const Status s = read_nif_body(data, len, body);
    out = std::move(body.textures);
    return s;
}

NifEngine nif_engine(const NifInfo& header, const NifBody& body) noexcept {
    if (header.engine == NifEngine::SkyrimEither && body.skinStrips) return NifEngine::SkyrimLe;
    return header.engine;
}

}  // namespace asset
