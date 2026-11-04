# beth-asset-metadata

Header facts from Bethesda game assets, without decoding them. C++17, no
dependencies, GPL-3.0. Companion to [bsa-reader](https://github.com/cecell/bsa-reader).
CMake target `asset::metadata`, namespace `asset`.

| Type | Facts | Prefix needed |
|---|---|---|
| `.dds` | width, height, depth, mipmaps, array size, format (BC1..BC7, RGBA8 variants, 16-bit, float), DXGI id, FourCC, sRGB, alpha, cubemap, volume | 148 bytes |
| `.tga` | width, height, bits per pixel, alpha bits, RLE, color-mapped, grayscale, origin | 18 bytes |
| `.nif` | version line, binary version, user version, BS version, block count, block-type table, per-block type index, SE vs legacy geometry, Skyrim LE/SE engine guess | 64 KiB (more for huge meshes) |
| `.nif` textures | every texture path a Skyrim mesh names (texture sets, effect shaders) | the whole file |
| `.wav` `.xwm` `.fuz` | container and payload, codec, channels, sample rate, bits, bit rate, data size, duration, FUZ lip block size | 64 KiB |

Every reader takes a byte buffer, so it works on a file prefix or on bytes
pulled out of a BSA. Nothing throws; every call returns a `Status`
(`Truncated` means "give me more bytes", never a guess). Hostile input is a
status, never a crash. The `probe_*` helpers read the prefix for you with
share read/write/delete and retry once with a larger prefix when a header
is longer than usual.

## Use

```cpp
#include <asset/metadata.hpp>

asset::DdsInfo dds;
if (asset::probe_dds(L"textures\\foo.dds", dds) == asset::Status::Ok)
    printf("%ux%u %s mips=%u\n", dds.width, dds.height, asset::to_string(dds.format), dds.mipmaps);

asset::AudioInfo au;                      // WAV, XWM or FUZ, picked by magic
if (asset::probe_audio(L"sound\\voice\\x.fuz", au) == asset::Status::Ok)
    printf("%u Hz %u ch %.2f s\n", au.sampleRate, au.channels, au.durationSeconds);

// From a BSA (bytes already in hand):
asset::NifInfo nif;
asset::read_nif(bytes.data(), bytes.size(), nif);
```

CMake: `FetchContent` or `add_subdirectory` this repo and link
`asset::metadata`. Readers are pure functions; call them from any thread.

## Format notes

- **DDS.** `dwSize` must be 124 and the pixel-format size 32; anything else
  is reported `Corrupt`. Cubemaps come from either `DDSCAPS2_CUBEMAP` or the
  DX10 `TEXTURECUBE` flag. Mipmaps are 1 unless `DDSD_MIPMAPCOUNT` is set.
  DXGI 49 is R8G8; 65 is A8.
- **DDS, continued.** 8-bit palettised (`DDPF_PALETTEINDEXED8`) and other
  rare layouts report `DdsFormat::Other` with the raw fields filled in.
- **TGA** has no magic, so the 18-byte header is validated field by field
  (image type, depth, color-map consistency, reserved bits). The Creation
  Kit writes FaceTint files that are DDS data under a `.tga` name; check
  `detect()` first and read those as DDS, as `assetinfo` does.
- **NIF.** Version conditions follow nif.xml, so Oblivion (20.0.0.5) and
  Morrowind (4.0.0.2) headers return what they have instead of failing.
  The header cannot tell LE from SE: of the 18,862 meshes in vanilla
  `Skyrim - Meshes0.bsa`, 18,861 carry BS version 100 and one carries 83,
  exactly like LE. The geometry block types decide. `seGeometry` is the
  BSTriShape family (`BSTriShape`, `BSDynamicTriShape`, `BSMeshLODTriShape`,
  `BSSubIndexTriShape`), which LE does not have. `legacyGeometry` is the
  NiTri* family plus `bhkPackedNiTriStrips*`; `stripGeometry` is
  `NiTriStrips` / `NiTriStripsData`. `engine` follows Cathedral Assets
  Optimizer's "necessary" mesh fixes (5.3.x): SE when SE geometry is
  present, LE when the mesh has strips (SE cannot use them; CAO converts
  them), and "either" for `NiTriShape` without strips or no geometry at all,
  which both games load unchanged. Strips can also hide in a skin
  partition: `read_nif_body` reports `skinStrips`, and `nif_engine(header,
  body)` combines the two. BS version 130 is Fallout 4 and some
  SSE tools; it is accepted as a Skyrim header but is not evidence of SE.
- **NIF textures.** `read_nif_textures` walks the header to the block
  sizes and reads only `BSShaderTextureSet` (every slot) and, below BS
  version 130, `BSEffectShaderProperty` (source and greyscale textures);
  every other block is skipped by its recorded size. Paths are returned as
  stored, so normalise them yourself (case, slashes, a missing `textures\`
  prefix). Needs the whole file: a cut file is `Truncated`.
- **Audio.** FUZ payloads are usually xWMA, but plain WAV happens too: of
  280,715 loose FUZ files on one modded install, 569 wrapped PCM or IEEE
  float WAV. Files that are not RIFF at all (a mod shipped MP3s named
  `.wav`) report `NotRecognized`; the game cannot play those either.
- **Audio, duration.** xWMA duration comes from the last `dpds` entry (cumulative
  decoded PCM bytes), which is exact; `fact` is used next, then data size
  over average byte rate (exact for PCM WAV). `durationSource` says which.
  FUZ = `FUZE`, u32 version, u32 lip size, lip block, then a complete RIFF
  file, WAVE or XWMA.

## assetinfo

`assetinfo <file>` prints the facts. `assetinfo check <dir> [--bsa]` sweeps
every asset under a folder, and with `--bsa` the archives too (via
bsa-reader), then tallies statuses and anomalies per type. It is the corpus
test: run it over a game's Data folder after any change.

## Build

Windows: `build.cmd` (MSVC 14.44 + Ninja). Anywhere: `cmake -B build &&
cmake --build build && ctest --test-dir build`. `-DASSET_METADATA_TOOL_BSA=OFF`
builds the tool without fetching bsa-reader.
