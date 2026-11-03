// File helpers: bounded prefix reads with share-everything opens, and the
// probe_* conveniences that retry with a bigger prefix when needed.
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include "common.hpp"

#include <cstdio>
#include <new>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace asset {

const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::Truncated: return "truncated";
        case Status::NotRecognized: return "not recognized";
        case Status::Corrupt: return "corrupt";
        case Status::Unsupported: return "unsupported";
        case Status::CannotOpen: return "cannot open";
        case Status::OutOfMemory: return "out of memory";
    }
    return "unknown status";
}

Kind detect(const uint8_t* d, size_t n) noexcept {
    if (n >= 4 && detail::tag4(d, "DDS ")) return Kind::Dds;
    if (n >= 4 && detail::tag4(d, "FUZE")) return Kind::Fuz;
    if (n >= 12 && detail::tag4(d, "RIFF")) {
        if (detail::tag4(d + 8, "WAVE")) return Kind::Wav;
        if (detail::tag4(d + 8, "XWMA")) return Kind::Xwm;
    }
    if (n >= 23 && (std::memcmp(d, "Gamebryo File Format,", 21) == 0 || std::memcmp(d, "NetImmerse File Format,", 23) == 0)) return Kind::Nif;
    return Kind::Unknown;
}

Status read_prefix(const std::filesystem::path& path, size_t maxBytes, std::vector<uint8_t>& out) noexcept {
    out.clear();
#ifdef _WIN32
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Status::CannotOpen;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart < 0) {
        CloseHandle(h);
        return Status::CannotOpen;
    }
    const size_t want = static_cast<uint64_t>(sz.QuadPart) < maxBytes ? static_cast<size_t>(sz.QuadPart) : maxBytes;
    try {
        out.resize(want);
    } catch (...) {
        CloseHandle(h);
        return Status::OutOfMemory;
    }
    size_t got = 0;
    while (got < want) {
        DWORD n = 0;
        const DWORD chunk = static_cast<DWORD>(want - got > (1u << 30) ? (1u << 30) : want - got);
        if (!ReadFile(h, out.data() + got, chunk, &n, nullptr) || n == 0) break;
        got += n;
    }
    CloseHandle(h);
    out.resize(got);
    return Status::Ok;
#else
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return Status::CannotOpen;
    try {
        out.resize(maxBytes);
    } catch (...) {
        std::fclose(f);
        return Status::OutOfMemory;
    }
    const size_t got = std::fread(out.data(), 1, maxBytes, f);
    std::fclose(f);
    out.resize(got);
    return Status::Ok;
#endif
}

namespace {

template <class Info, class Reader>
Status probe(const std::filesystem::path& path, size_t first, size_t second, Info& out, Reader reader) noexcept {
    std::vector<uint8_t> buf;
    Status s = read_prefix(path, first, buf);
    if (s != Status::Ok) return s;
    s = reader(buf.data(), buf.size(), out);
    if (s != Status::Truncated || buf.size() < first || second <= first) return s;
    s = read_prefix(path, second, buf);
    if (s != Status::Ok) return s;
    return reader(buf.data(), buf.size(), out);
}

}  // namespace

Status probe_dds(const std::filesystem::path& path, DdsInfo& out) noexcept {
    return probe(path, kDdsProbeBytes, 0, out, read_dds);
}

Status probe_tga(const std::filesystem::path& path, TgaInfo& out) noexcept {
    return probe(path, kTgaProbeBytes, 0, out, read_tga);
}

Status probe_nif(const std::filesystem::path& path, NifInfo& out) noexcept {
    return probe(path, kNifProbeBytes, size_t{4} << 20, out, read_nif);
}

Status probe_audio(const std::filesystem::path& path, AudioInfo& out) noexcept {
    // A FUZ lip block or a long XWM packet table can push the fields past
    // the first probe; retry once with a bigger one in those cases too.
    std::vector<uint8_t> buf;
    Status s = read_prefix(path, kAudioProbeBytes, buf);
    if (s != Status::Ok) return s;
    s = read_audio(buf.data(), buf.size(), out);
    const bool partial = s == Status::Truncated || (s == Status::Ok && (out.dpdsTruncated || !out.dataChunkFound));
    if (!partial || buf.size() < kAudioProbeBytes) return s;
    s = read_prefix(path, size_t{4} << 20, buf);
    if (s != Status::Ok) return s;
    return read_audio(buf.data(), buf.size(), out);
}

}  // namespace asset
