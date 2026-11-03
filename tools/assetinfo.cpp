// assetinfo: print the facts for one file, or sweep a folder (and, with
// bsa-reader, the archives in it) and tally what every reader says.
// SPDX-License-Identifier: GPL-3.0-only
#include "asset/metadata.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#ifdef ASSETINFO_HAS_BSA
#include "bsa/reader.hpp"
#endif

namespace fs = std::filesystem;
using namespace asset;

namespace {

std::string lower_ext(const std::string& name) {
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return "";
    std::string e = name.substr(dot);
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

bool is_asset_ext(const std::string& e) {
    return e == ".dds" || e == ".tga" || e == ".nif" || e == ".wav" || e == ".xwm" || e == ".fuz";
}

void print_dds(const DdsInfo& i) {
    std::printf("dds  %ux%u  mips %u  %s%s  fourcc '%s'  dxgi %u  bpp %u  block %u%s%s%s%s  array %u\n", i.width, i.height,
                i.mipmaps, to_string(i.format), i.srgb ? " sRGB" : "", i.fourcc, i.dxgiFormat, i.bitsPerPixel, i.blockBytes,
                i.compressed ? "  compressed" : "", i.hasAlpha ? "  alpha" : "", i.cubemap ? "  cubemap" : "",
                i.volume ? "  volume" : "", i.arraySize);
}
void print_tga(const TgaInfo& i) {
    std::printf("tga  %ux%u  %u bpp  alpha bits %u  type %u%s%s%s  origin %s\n", i.width, i.height, i.bitsPerPixel, i.alphaBits,
                i.imageType, i.rle ? "  rle" : "", i.colorMapped ? "  color-mapped" : "", i.grayscale ? "  grayscale" : "",
                i.topLeft ? "top-left" : "bottom-left");
}
void print_nif(const NifInfo& i) {
    std::printf("nif  %s  user %u  bs %u  blocks %u  engine %s%s%s  types:", nif_version_string(i.version).c_str(), i.userVersion,
                i.bsVersion, i.blockCount, to_string(i.engine), i.seGeometry ? "  se-geometry" : "",
                i.legacyGeometry ? "  legacy-geometry" : "");
    for (const auto& t : i.blockTypes) std::printf(" %s", t.c_str());
    std::printf("\n");
}
void print_audio(const AudioInfo& i) {
    std::printf("%s  payload %s  %s  %u Hz  %u ch  %u bit  %u bps  duration %.3f s (%s)  data %llu%s%s\n", to_string(i.container),
                to_string(i.payload), codec_name(i.formatTag, i.payload), i.sampleRate, i.channels, i.bitsPerSample, i.bitRate,
                i.durationSeconds,
                i.durationSource == DurationSource::Dpds ? "dpds" : i.durationSource == DurationSource::Fact ? "fact"
                : i.durationSource == DurationSource::DataRate ? "data/rate" : "unknown",
                static_cast<unsigned long long>(i.dataBytes), i.hasLip ? "  lip" : "", i.dpdsTruncated ? "  dpds-truncated" : "");
}

int cmd_file(const fs::path& p) {
    const std::string ext = lower_ext(p.filename().u8string());
    Status s;
    std::vector<uint8_t> head;
    if (read_prefix(p, 32, head) != Status::Ok) {
        std::fprintf(stderr, "%s: cannot open\n", p.u8string().c_str());
        return 1;
    }
    if (ext == ".tga" && detect(head.data(), head.size()) != Kind::Dds) {
        TgaInfo i;
        s = probe_tga(p, i);
        if (s == Status::Ok) print_tga(i);
    } else {
        switch (detect(head.data(), head.size())) {
            case Kind::Dds: { DdsInfo i; s = probe_dds(p, i); if (s == Status::Ok) print_dds(i); break; }
            case Kind::Nif: { NifInfo i; s = probe_nif(p, i); if (s == Status::Ok) print_nif(i); break; }
            case Kind::Fuz: case Kind::Wav: case Kind::Xwm: { AudioInfo i; s = probe_audio(p, i); if (s == Status::Ok) print_audio(i); break; }
            default: s = Status::NotRecognized;
        }
    }
    if (s != Status::Ok) std::fprintf(stderr, "%s: %s\n", p.u8string().c_str(), to_string(s));
    return s == Status::Ok ? 0 : 1;
}

// ------------------------------------------------------------------ check

struct Tally {
    std::map<Status, size_t> status;
    size_t anomalies = 0;
    std::map<std::string, size_t> notes;  // free-form counters per ext
    std::vector<std::string> samples;     // first few anomalies
};

std::map<std::string, Tally> g_tally;
size_t g_files = 0;

void note(Tally& t, const std::string& key, const std::string& where) {
    ++t.notes[key];
    ++t.anomalies;
    if (t.samples.size() < 5) t.samples.push_back(key + ": " + where);
}

void check_buffer(const std::string& ext, const uint8_t* d, size_t n, const std::string& where, bool bounded) {
    Tally& t = g_tally[ext];
    ++g_files;
    Status s;
    if (ext == ".dds") {
        DdsInfo i;
        s = read_dds(d, n, i);
        if (s == Status::Ok) {
            if (i.width == 0 || i.height == 0) note(t, "zero dimension", where);
            if (i.format == DdsFormat::Other || i.format == DdsFormat::Unknown) note(t, std::string("format other fourcc=") + i.fourcc + " dxgi=" + std::to_string(i.dxgiFormat), where);
            if (i.compressed && ((i.width & 3) || (i.height & 3))) ++t.notes["compressed, size not multiple of 4"];
            ++t.notes[std::string("fmt ") + to_string(i.format)];
        }
    } else if (ext == ".tga") {
        if (detect(d, n) == Kind::Dds) {  // the CK writes FaceTint DDS data under .tga names
            DdsInfo i;
            s = read_dds(d, n, i);
            ++t.notes["DDS content under .tga name"];
        } else {
            TgaInfo i;
            s = read_tga(d, n, i);
            if (s == Status::Ok) ++t.notes[std::to_string(i.bitsPerPixel) + " bpp" + (i.rle ? " rle" : "")];
        }
    } else if (ext == ".nif") {
        NifInfo i;
        s = read_nif(d, n, i);
        if (s == Status::Ok) {
            ++t.notes["version " + nif_version_string(i.version) + " engine " + to_string(i.engine)];
            ++t.notes[std::string("bs version ") + std::to_string(i.bsVersion)];
            if (i.seGeometry) ++t.notes["se geometry"];
            if (i.legacyGeometry) ++t.notes["legacy geometry"];
            if (i.seGeometry && i.legacyGeometry) ++t.notes["both geometry styles"];
        }
    } else {
        AudioInfo i;
        s = read_audio(d, n, i);
        if (s == Status::Ok) {
            ++t.notes[std::string("payload ") + to_string(i.payload) + " " + codec_name(i.formatTag, i.payload)];
            if (i.sampleRate < 8000 || i.sampleRate > 192000) note(t, "odd sample rate " + std::to_string(i.sampleRate), where);
            if (i.channels == 0 || i.channels > 8) note(t, "odd channels " + std::to_string(i.channels), where);
            if (i.durationSource == DurationSource::None) note(t, "no duration", where);
            if (i.dpdsTruncated) ++t.notes["dpds beyond buffer"];
        }
    }
    if (s == Status::Truncated && !bounded) s = Status::Truncated;  // whole file given: a real truncation
    ++t.status[s];
    if (s != Status::Ok) note(t, std::string("status ") + to_string(s), where);
}

void check_loose(const fs::path& p, const std::string& ext) {
    std::vector<uint8_t> buf;
    const size_t cap = ext == ".dds" ? kDdsProbeBytes : ext == ".tga" ? kDdsProbeBytes : (size_t{4} << 20);
    if (read_prefix(p, cap, buf) != Status::Ok) {
        ++g_tally[ext].status[Status::CannotOpen];
        return;
    }
    check_buffer(ext, buf.data(), buf.size(), p.u8string(), true);
}

#ifdef ASSETINFO_HAS_BSA
void check_archive(const fs::path& p) {
    bsa::Reader r;
    if (r.open(p) != bsa::Status::Ok) {
        std::fprintf(stderr, "%s: cannot open archive\n", p.u8string().c_str());
        return;
    }
    std::vector<uint8_t> buf;
    std::string path;
    for (uint32_t i = 0; i < r.header().fileCount; ++i) {
        bsa::Entry e;
        std::string_view folder, name;
        if (r.entry(i, e) != bsa::Status::Ok || r.file_name(e, name) != bsa::Status::Ok) continue;
        const std::string ext = lower_ext(std::string(name));
        if (!is_asset_ext(ext)) continue;
        r.folder_name(e.folderIndex, folder);
        path = p.filename().u8string() + ":" + std::string(folder) + "\\" + std::string(name);
        if (r.read(e, buf, size_t{256} << 20) != bsa::Status::Ok) {
            ++g_tally[ext].status[Status::CannotOpen];
            continue;
        }
        check_buffer(ext, buf.data(), buf.size(), path, false);
    }
}
#endif

int cmd_check(const fs::path& root, bool archives) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<fs::path> bsas;
    if (fs::is_directory(root)) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        while (!ec && it != end) {
            const fs::path p = it->path();
            if (it->is_regular_file(ec)) {
                const std::string ext = lower_ext(p.filename().u8string());
                if (is_asset_ext(ext)) check_loose(p, ext);
                else if (archives && ext == ".bsa") bsas.push_back(p);
            }
            it.increment(ec);
        }
        if (ec) std::fprintf(stderr, "directory walk stopped: %s\n", ec.message().c_str());
    } else if (lower_ext(root.filename().u8string()) == ".bsa") {
        bsas.push_back(root);
    } else {
        check_loose(root, lower_ext(root.filename().u8string()));
    }
#ifdef ASSETINFO_HAS_BSA
    std::sort(bsas.begin(), bsas.end());
    for (const auto& b : bsas) check_archive(b);
#else
    if (!bsas.empty()) std::fprintf(stderr, "built without bsa-reader; %zu archive(s) skipped\n", bsas.size());
#endif
    for (const auto& [ext, t] : g_tally) {
        std::printf("%s\n", ext.c_str());
        for (const auto& [s, c] : t.status) std::printf("   %-16s %zu\n", to_string(s), c);
        for (const auto& [k, c] : t.notes) std::printf("   %-60s %zu\n", k.c_str(), c);
        for (const auto& s : t.samples) std::printf("   e.g. %s\n", s.c_str());
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    size_t bad = 0;
    for (const auto& [ext, t] : g_tally)
        for (const auto& [s, c] : t.status)
            if (s != Status::Ok) bad += c;
    std::printf("\nfiles %zu  not-ok %zu  %.1fs\n", g_files, bad, secs);
    return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "check") == 0) {
        const bool archives = argc >= 4 && std::strcmp(argv[3], "--bsa") == 0;
        return cmd_check(fs::u8path(argv[2]), archives);
    }
    if (argc == 2) return cmd_file(fs::u8path(argv[1]));
    std::fputs("usage:\n  assetinfo <file>\n  assetinfo check <dir|file|archive.bsa> [--bsa]\n", stderr);
    return 2;
}
