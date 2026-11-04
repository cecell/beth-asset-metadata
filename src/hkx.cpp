// Havok packfiles (.hkx) as Skyrim ships them (hk_2010.2.0-r1, file version
// 8, 32-bit LE / 64-bit SE): header, sections, class names and fixups, then
// a few objects at offsets measured on ~50,000 real files: animation
// duration / frames / tracks and annotations, behaviour graph string data
// and variable types, clip generators' animation names, and a character's
// animation list. XML and tagfile .hkx are Unsupported.
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include "common.hpp"

#include <algorithm>
#include <map>
#include <new>
#include <set>

namespace asset {
using namespace detail;

namespace {

struct Section {
    uint32_t start = 0, local = 0, global = 0, virt = 0, exports = 0, end = 0;
};

struct Pack {
    const uint8_t* d;
    size_t n;
    uint32_t P = 8;  // pointer size
    std::vector<Section> secs;
    // Per section: local fixups (src -> dst in the same section), global
    // fixups (src -> section, dst), objects (offset -> class name).
    std::vector<std::map<uint32_t, uint32_t>> local;
    std::vector<std::map<uint32_t, std::pair<uint32_t, uint32_t>>> global;
    std::vector<std::map<uint32_t, std::string>> objects;

    // Bytes [off, off+k) of section s's data are readable.
    bool ok(uint32_t s, uint64_t off, uint64_t k) const {
        return s < secs.size() && off + k <= secs[s].local && secs[s].start + off + k <= n;
    }
    uint32_t u32(uint32_t s, uint64_t off) const { return ok(s, off, 4) ? rd32(d + secs[s].start + off) : 0; }
    int32_t i32(uint32_t s, uint64_t off) const { return static_cast<int32_t>(u32(s, off)); }
    float f32(uint32_t s, uint64_t off) const {
        const uint32_t v = u32(s, off);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    }
    // Where the pointer at (s, off) points; false for a null pointer.
    bool ref(uint32_t s, uint32_t off, uint32_t* ts, uint32_t* to) const {
        if (s >= secs.size()) return false;
        auto l = local[s].find(off);
        if (l != local[s].end()) {
            *ts = s;
            *to = l->second;
            return true;
        }
        auto g = global[s].find(off);
        if (g != global[s].end()) {
            *ts = g->second.first;
            *to = g->second.second;
            return true;
        }
        return false;
    }
    std::string cstr(uint32_t s, uint32_t off) const {
        uint32_t ts, to;
        if (!ref(s, off, &ts, &to) || !ok(ts, to, 1)) return {};
        const char* p = reinterpret_cast<const char*>(d + secs[ts].start + to);
        const size_t room = std::min<size_t>(secs[ts].local - to, 4096);
        return std::string(p, std::find(p, p + room, '\0'));
    }
    // An hkArray at (s, off): data location and size (capped).
    bool arr(uint32_t s, uint32_t off, uint32_t* ts, uint32_t* to, uint32_t* count) const {
        const int32_t size = i32(s, uint64_t{off} + P);
        if (size <= 0 || size > 100000 || !ref(s, off, ts, to)) return false;
        *count = static_cast<uint32_t>(size);
        return true;
    }
    std::vector<std::string> strings(uint32_t s, uint32_t off) const {
        std::vector<std::string> out;
        uint32_t ts, to, count;
        if (!arr(s, off, &ts, &to, &count)) return out;
        for (uint32_t i = 0; i < count && ok(ts, uint64_t{to} + uint64_t{i} * P, P); ++i)
            out.push_back(cstr(ts, to + i * P));
        return out;
    }
};

Status load(const uint8_t* d, size_t n, Pack& p, HkxInfo& o) {
    if (n < 0x40) return Status::Truncated;
    if (rd32(d) != 0x57E0E057 || rd32(d + 4) != 0x10C0C010) {
        if (n >= 5 && (std::memcmp(d, "<?xml", 5) == 0 || std::memcmp(d, "<hkpa", 5) == 0)) return Status::Unsupported;
        if (rd32(d) == 0xCAB00D1E || rd32(d + 4) == 0xD011FACE) return Status::Unsupported;  // tagfile
        return Status::NotRecognized;
    }
    o.fileVersion = rd32(d + 12);
    o.pointerSize = d[16];
    o.littleEndian = d[17] != 0;
    const char* v = reinterpret_cast<const char*>(d + 40);
    o.version.assign(v, std::find(v, v + 16, '\0'));
    if (!o.littleEndian || (o.pointerSize != 4 && o.pointerSize != 8)) return Status::Unsupported;
    if (o.fileVersion > 8) return Status::Unsupported;  // later layouts add header fields
    p.P = o.pointerSize;
    const uint32_t sections = rd32(d + 20), classIndex = rd32(d + 32);
    if (sections == 0 || sections > 16 || classIndex >= sections) return Status::Corrupt;
    o.sectionCount = sections;
    if (0x40 + uint64_t{sections} * 0x30 > n) return Status::Truncated;
    for (uint32_t i = 0; i < sections; ++i) {
        const uint8_t* h = d + 0x40 + i * 0x30 + 20;
        Section s{rd32(h), rd32(h + 4), rd32(h + 8), rd32(h + 12), rd32(h + 16), rd32(h + 24)};
        if (s.local > s.global || s.global > s.virt || s.virt > s.exports || s.start + uint64_t{s.exports} > n)
            return Status::Corrupt;
        p.secs.push_back(s);
    }
    // Class names: u32 signature, 0x09, NUL-terminated name. The signature's
    // first byte can be 0xFF, so stop on a missing 0x09, not on 0xFF.
    std::map<uint32_t, std::string> names;
    {
        const Section& c = p.secs[classIndex];
        uint64_t q = c.start;
        const uint64_t end = std::min<uint64_t>(c.start + uint64_t{c.local}, n);
        while (q + 5 <= end && d[q + 4] == 9) {
            const uint8_t* s = d + q + 5;
            const uint8_t* z = std::find(s, d + end, '\0');
            if (z == d + end) break;
            names[static_cast<uint32_t>(q + 5 - c.start)] = std::string(reinterpret_cast<const char*>(s), z - s);
            q = static_cast<uint64_t>(z - d) + 1;
        }
    }
    p.local.resize(sections);
    p.global.resize(sections);
    p.objects.resize(sections);
    for (uint32_t i = 0; i < sections; ++i) {
        const Section& s = p.secs[i];
        for (uint64_t q = s.start + uint64_t{s.local}; q + 8 <= s.start + uint64_t{s.global}; q += 8) {
            const uint32_t a = rd32(d + q);
            if (a == 0xFFFFFFFF) break;
            p.local[i][a] = rd32(d + q + 4);
        }
        for (uint64_t q = s.start + uint64_t{s.global}; q + 12 <= s.start + uint64_t{s.virt}; q += 12) {
            const uint32_t a = rd32(d + q);
            if (a == 0xFFFFFFFF) break;
            p.global[i][a] = {rd32(d + q + 4), rd32(d + q + 8)};
        }
        for (uint64_t q = s.start + uint64_t{s.virt}; q + 12 <= s.start + uint64_t{s.exports}; q += 12) {
            const uint32_t a = rd32(d + q);
            if (a == 0xFFFFFFFF) break;
            auto it = names.find(rd32(d + q + 8));
            p.objects[i][a] = it == names.end() ? std::string("?") : it->second;
        }
    }
    return Status::Ok;
}

void readAnimation(const Pack& p, uint32_t s, uint32_t o, const std::string& cls, HkxInfo& out) {
    const uint32_t P = p.P;
    out.hasAnimation = true;
    out.animationClass = cls;
    out.animationType = p.i32(s, o + 2 * P);
    out.duration = p.f32(s, o + 2 * P + 4);
    out.transformTracks = p.i32(s, o + 2 * P + 8);
    out.floatTracks = p.i32(s, o + 2 * P + 12);
    if (cls == "hkaSplineCompressedAnimation") {
        out.frameCount = p.i32(s, o + 4 * P + 24);
        out.blockCount = p.i32(s, o + 4 * P + 28);
    }
    // Annotation tracks: {name, hkArray<{float time, hkStringPtr text}>}.
    // Skyrim gives every transform track one; empty texts are padding.
    uint32_t ts, to, tracks;
    if (!p.arr(s, o + 3 * P + 16, &ts, &to, &tracks)) return;
    const uint32_t trackSize = P == 4 ? 16 : 24, annSize = P == 4 ? 8 : 16;
    for (uint32_t t = 0; t < tracks && p.ok(ts, uint64_t{to} + uint64_t{t} * trackSize, trackSize); ++t) {
        uint32_t as, ao, count;
        if (!p.arr(ts, to + t * trackSize + P, &as, &ao, &count)) continue;
        for (uint32_t a = 0; a < count && p.ok(as, uint64_t{ao} + uint64_t{a} * annSize, annSize); ++a) {
            std::string text = p.cstr(as, ao + a * annSize + (P == 4 ? 4 : 8));
            if (text.empty()) continue;
            const float time = p.f32(as, ao + a * annSize);
            if (time > out.duration + 1e-4f) out.annotationPastEnd = true;
            out.annotations.push_back({time, std::move(text), t});
        }
    }
}

Status parse(const uint8_t* d, size_t n, HkxInfo& o) {
    Pack p{d, n};
    const Status s = load(d, n, p, o);
    if (s != Status::Ok) return s;
    const uint32_t P = p.P, A = P + 8;  // A = an hkArray's size
    std::set<std::string> classes;
    for (uint32_t si = 0; si < p.objects.size(); ++si) {
        for (const auto& [off, cls] : p.objects[si]) {
            classes.insert(cls);
            if (!o.hasAnimation && (cls == "hkaSplineCompressedAnimation" || cls == "hkaInterleavedUncompressedAnimation"))
                readAnimation(p, si, off, cls, o);
            else if (cls == "hkbBehaviorGraphStringData" && o.events.empty() && o.variables.empty()) {
                o.events = p.strings(si, off + 2 * P);
                o.variables = p.strings(si, off + 2 * P + 2 * A);
                o.characterProperties = p.strings(si, off + 2 * P + 3 * A);
            } else if (cls == "hkbBehaviorGraphData" && o.variableTypes.empty()) {
                uint32_t ts, to, count;  // hkbVariableInfo: role u16, flags u16, type u8, pad
                if (p.arr(si, off + 2 * P + A, &ts, &to, &count))
                    for (uint32_t i = 0; i < count && p.ok(ts, uint64_t{to} + uint64_t{i} * 6, 6); ++i)
                        o.variableTypes.push_back(d[p.secs[ts].start + to + i * 6 + 4]);
            } else if (cls == "hkbClipGenerator") {
                std::string anim = p.cstr(si, off + (P == 8 ? 0x48 : 0x28));
                if (!anim.empty()) o.clipAnimations.push_back(std::move(anim));
            } else if (cls == "hkbCharacterStringData" && o.animationNames.empty()) {
                o.animationNames = p.strings(si, off + 2 * P + 2 * A);
            }
        }
    }
    o.classes.assign(classes.begin(), classes.end());
    auto has = [&](const char* c) { return classes.count(c) != 0; };
    o.content = o.hasAnimation                         ? HkxContent::Animation
                : has("hkbBehaviorGraph")              ? HkxContent::Behavior
                : has("hkbCharacterData")              ? HkxContent::Character
                : has("hkbProjectData")                ? HkxContent::Project
                : has("hkaSkeleton")                   ? HkxContent::Skeleton
                                                       : HkxContent::Other;
    return Status::Ok;
}

}  // namespace

const char* to_string(HkxContent c) noexcept {
    switch (c) {
        case HkxContent::Animation: return "animation";
        case HkxContent::Behavior: return "behavior";
        case HkxContent::Character: return "character";
        case HkxContent::Project: return "project";
        case HkxContent::Skeleton: return "skeleton";
        case HkxContent::Other: return "other";
        case HkxContent::Unknown: break;
    }
    return "unknown";
}

const char* hkx_variable_type_name(uint8_t t) noexcept {
    static const char* const kNames[] = {"bool", "int8", "int16", "int32", "real", "pointer", "vector3", "vector4", "quaternion"};
    return t < 9 ? kNames[t] : "unknown";
}

Status read_hkx(const uint8_t* data, size_t len, HkxInfo& out) noexcept {
    out = HkxInfo{};
    try {
        const Status s = parse(data, len, out);
        if (s != Status::Ok) {
            HkxInfo keep;
            keep.version = out.version;
            keep.fileVersion = out.fileVersion;
            keep.pointerSize = out.pointerSize;
            keep.littleEndian = out.littleEndian;
            out = keep;
        }
        return s;
    } catch (const std::bad_alloc&) {
        out = HkxInfo{};
        return Status::OutOfMemory;
    } catch (...) {
        out = HkxInfo{};
        return Status::Corrupt;
    }
}

}  // namespace asset
