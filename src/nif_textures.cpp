// NIF texture paths: walks a Skyrim NIF's header to the block sizes, then
// reads only the blocks that name textures. Everything else is skipped by
// its recorded size, so other block layouts never matter.
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include "common.hpp"

#include <algorithm>
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

Status parse(const uint8_t* d, size_t n, std::vector<NifTexture>& out) {
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
    for (uint32_t b = 0; b < blocks; ++b) {
        if (sizes[b] > n - off) return Status::Truncated;
        if (typeOf[b] >= types.size()) return Status::Corrupt;
        const std::string& type = types[typeOf[b]];
        Cursor k{d + off, sizes[b]};
        if (type == "BSShaderTextureSet") {
            const uint32_t count = k.u32();
            for (uint32_t i = 0; i < count && i < 64 && !k.bad; ++i) {
                std::string tex = k.sized(1024);
                if (!k.bad && !tex.empty()) out.push_back({type, i, std::move(tex)});
            }
        } else if (type == "BSEffectShaderProperty" && bsVersion < 130) {
            k.skip(4);                      // name
            k.skip(uint64_t{k.u32()} * 4);  // extra data refs
            k.skip(4 + 8 + 16);             // controller, shader flags 1 and 2, UV offset and scale
            std::string source = k.sized(1024);
            if (!k.bad && !source.empty()) out.push_back({type, 0, std::move(source)});
            k.skip(4 + 16);                 // clamp, lighting influence, env map LOD, unused; falloff
            k.skip(16 + 4 + 4);             // emissive color and multiple, soft falloff depth
            std::string grey = k.sized(1024);
            if (!k.bad && !grey.empty()) out.push_back({type, 1, std::move(grey)});
        }
        off += sizes[b];
    }
    return Status::Ok;
}

}  // namespace

Status read_nif_textures(const uint8_t* data, size_t len, std::vector<NifTexture>& out) noexcept {
    out.clear();
    try {
        const Status s = parse(data, len, out);
        if (s != Status::Ok) out.clear();
        return s;
    } catch (const std::bad_alloc&) {
        out.clear();
        return Status::OutOfMemory;
    } catch (...) {
        out.clear();
        return Status::Corrupt;
    }
}

}  // namespace asset
