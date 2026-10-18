# ISOPackager

Compress, extract, repack and benchmark disc images — one file or whole folders — with a
Dear ImGui interface and a command line tool that share the same engine.

| In                                   | Out                                      |
|--------------------------------------|------------------------------------------|
| ISO, CSO (v1/v2), ZSO, DAX, CHD, EBOOT.PBP, WIA, RVZ | any of them (RVZ/WIA: GameCube and Wii discs; PBP: PSP games) |
| a folder, or any files and folders   | one ZArchive (`.zar`): several discs, DLC folders and the rest together |

Built on these libraries, linked statically from the submodules in `external/`:

- [mCSO / maxcso](https://github.com/LunaMoo/maxcso) — CSO v1, CSO v2, ZSO and DAX
- [CHDmodern](https://github.com/LunaMoo/CHDmodern) — MAME's CHD code (DVD CHDs, like `chdman createdvd`)
- [iPBP](https://github.com/LunaMoo/iPBP) — PSN-style EBOOT.PBP (NPUMDIMG), LZRC and LZRC max
- [iRVZ](https://github.com/LunaMoo/iRVZ) — WIA and RVZ (GameCube / Wii), from Dolphin's WIABlob
- [ZArchive](https://github.com/LunaMoo/ZArchive) — zstd-compressed, random-access file archives

## What it does

- **Compress** an ISO to CSO / ZSO / CSO v2 / DAX / CHD / PBP, with every option the libraries have.
  CSO defaults to libdeflate with 64K blocks (lz4 HC for ZSO); untick Default for maxcso's presets
  or a custom method set (zlib, brute force, 7-zip, Zopfli, libdeflate, lz4, lz4 HC),
  block size, Zopfli splits and the libdeflate pre-check; CHD codecs (lzma, zlib, zstd, huff, flac)
  and hunk size; PBP compression level, content ID and boot screen; RVZ/WIA method (LZMA2 by
  default, or zstd, LZMA, bzip2, none, purge), level and chunk size.
- **Extract** any of them back to ISO (PBPs trimmed to their ISO9660 size by default).
- **Repack** between compressed formats (CHD → CSO, PBP → CHD, CSO → CSO with stronger settings, …)
  **through RAM**: the source is decompressed into memory through the libraries' writer callbacks
  and compressed from there through their reader callbacks — no temporary ISO on disk. A RAM
  budget (automatic, or set by you) is shared by parallel jobs; an image that doesn't fit falls
  back to a temporary ISO.
- **Verify** every output by decoding it again and comparing it byte for byte with the source,
  as it decodes — no extra disk space and no second copy in memory.
- **Folders**: add a folder and every image in it (recursively) is queued; outputs can go next to
  each source or into one folder that recreates the scanned structure. Existing outputs are
  skipped, overwritten or renamed; sources can be deleted after a verified conversion.
- **Multithreaded**: every library uses all cores, and several files can be converted at once
  (CHD and PBP jobs in parallel; maxcso tasks take turns, since its buffer pool is global).
- **Create ZArchive**: pick a folder (its contents become the archive's root, like the zarchive
  tool packs a folder), name the archive, and it's queued as one entry (*files* → *ZAR*, with the
  combined size); more files and folders from elsewhere can be added in the same dialog. Blocks
  are zstd level 22 by default, compressed on all cores, with a **pre-check**: every 64K block is
  first tried at zstd's fastest level, and blocks that don't get below 95% are stored as they are,
  so already compressed games (CSO, CHD, RVZ…) cost almost no time and read back without a second
  decompression. Settings > ZArchive also has "Don't compress game discs" (for games already in
  compressed formats) and "Compress the game discs before adding them" with a format: plain ISOs
  are compressed **in RAM** through the libraries' writer callbacks and the result goes straight
  into the archive, stored uncompressed (through a temporary file only when the RAM budget is too
  small). Verify reads every file back from the finished archive and compares it with what went in.
- **Benchmark**: the successor of `img_bench` — for every image and every setting it compresses
  (timed), records the size, decodes again (timed, compared byte for byte) and times random reads
  the way an emulator seeks (CSO/ZSO/CHD). What it runs is a list you build with "Add to
  benchmark" under each format in Settings, so any combination of options can be compared
  (it starts with libdeflate 256K CSO, chdman's default CHD and LZRC PBP; each entry has an ×).
  Results go to a table, a size-vs-speed chart, and are appended to `summary.txt` and `results.csv`.

## Command line

```
isopackager-cli convert --to chd --codecs lzma+zstd -o D:\CHD D:\ISO
isopackager-cli convert --to cso --preset max --block 16K game.chd      # repacked through RAM
isopackager-cli convert --to zso -j 2 --delete-source D:\Games
isopackager-cli convert --to rvz --rvz-method zstd --rvz-level 19 --rvz-chunk 256K D:\Wii
isopackager-cli extract EBOOT.PBP
isopackager-cli verify --against original.iso game.cso
isopackager-cli info D:\Games
isopackager-cli bench --no-cso --codecs default,zstd --hunks 8192,32768 D:\ISO
isopackager-cli bench --cso-sets A,B,C,D,L --pbp-modes max game.iso
isopackager-cli zar --compress-discs cso -o D:\ZAR "D:\Games\Trails in the Sky"
isopackager-cli zar --name "Trails" --store-discs game1.chd game2.chd D:\DLC\ULUS10000
```

`isopackager-cli --help` lists everything. The `bench` options match `img_bench`
(`--cso-sets`, `--codecs`, `--hunks`, `--pbp-modes`, `--reads`, `--no-cso/--no-chd/--no-pbp`,
`--no-verify`), plus `--extra zso,cso2,dax`, `--source-on-disk` and `--work DIR`.

`zar` packs one folder's contents, or several files and folders each under its own name, into
`<name>.zar` (`--name`, default the folder's name) with `--zar-level`, `--zar-precheck`,
`--store-discs` and `--compress-discs FMT`. The GUI takes `--zar FOLDER` to open Create ZArchive.

CSO benchmark sets (from img_bench): A zopfli5, B zopfli5 splits 20, C pre-check 98% + zopfli5,
D pre-check 98% + zopfli5 splits 20, L libdeflate12 only (all at 256K blocks), and F / S / X for
the fast / standard / max presets.

## Platforms

| Platform | GUI | Notes |
|----------|-----|-------|
| Windows (x64, ARM64) | Win32 + Direct3D 11 | native file dialogs, drag and drop, per-monitor DPI, UTF-8 paths |
| Linux | SDL2 | zenity / kdialog dialogs when installed, otherwise the built-in browser |
| macOS (universal) | SDL2 | `.app` bundle |
| FreeBSD | SDL2 | |
| Android | SDL2 | asks for "all files access" to reach images on shared storage |
| iOS | SDL2 | works on the app's Documents folder (Files app sharing enabled) |
| UWP | SDL2 | all formats; maxcso runs on uvlite instead of libuv (see below); sandboxed file access |

Every build has the same engine; the CLI is built on desktop platforms.

## Building

```
git clone --recursive https://github.com/LunaMoo/ISOPackager
cmake -S ISOPackager -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
build/isopk_selftest            # round trips through every format
```

Options: `-DISOPK_BUILD_GUI=OFF`, `-DISOPK_BUILD_CLI=OFF`, `-DISOPK_WITH_CSO=OFF`,
`-DISOPK_GUI_BACKEND=sdl2` (use SDL2 on Windows too).

- **Windows**: Visual Studio 2022 or newer; the static CRT is used, so the exe has no runtime
  dependencies. Add `-A ARM64` for ARM64.
- **Linux / FreeBSD**: X11 development headers for the GUI (`libx11-dev libxext-dev …`).
- **macOS**: `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` for a universal build.
- **iOS**: `-G Xcode -DCMAKE_SYSTEM_NAME=iOS`.
- **Android**: `cd android && gradle assembleRelease` (Android SDK + NDK, Gradle 8.11).
- **UWP**: `-DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0`.

### Linking the libraries together

Some third-party code exists more than once when the libraries are linked into one program. Both are resolved in ISOPackager's own `CMakeLists.txt`, without changing the submodules:

- **zlib** — maxcso bundles 1.2.11, CHDmodern 1.3.2 (same API): CHDmodern uses maxcso's.
- **zstd** — CHDmodern and iRVZ bundle the same version: iRVZ and ZArchive use CHDmodern's.
- **LZMA SDK** — CHDmodern and iRVZ bundle the same 2024 files (iRVZ's adds LZMA2), built once
  for both. maxcso's 7-zip deflate encoder bundles the 2017 `LzFind.c`, the others the 2024 one; they export the same `MatchFinder_*` names with different
  struct layouts, so a plain link would silently mix them. CHDmodern's copy is built with those
  symbols renamed (`cmake/lzma_rename.h`).

### uvlite: maxcso without libuv (UWP)

libuv doesn't build for UWP: it uses Win32 desktop APIs for processes, pipes, sockets and the
console. maxcso uses none of those, only 16 libuv functions (an event loop, a thread pool,
mutexes and file I/O). `src/compat/uvlite` implements exactly those on APIs UWP allows
(`CreateFile2`, positional `ReadFile`/`WriteFile`, `std::thread`), and 7-zip's file streams, which
its deflate encoder doesn't use, are left out. `-DISOPK_CSO_PORTABLE=ON` selects this on any
platform (it is the default for UWP); CI runs the whole self-test on it, and its CSO/ZSO output
is byte-identical to the libuv build's.

## Settings and results

**Portable mode** (Windows, Linux): put a `portable.ini` next to the executable. Empty, it keeps
the settings and benchmark results (in `bench/`) in the executable's folder; with a path on its
first line (absolute, or relative to the executable), they go there instead.

Otherwise, settings are saved to `%APPDATA%\ISOPackager\isopackager.ini` (Windows),
`~/Library/Application Support/ISOPackager` (macOS), `~/.config/isopackager` (Linux, FreeBSD) or
the app's private folder (Android, iOS). Benchmark results go to the `bench` folder next to it,
unless another folder is chosen.

## License

ISOPackager is released under the [GNU General Public License v3.0](LICENSE). It links iPBP
(GPL-3.0), maxcso (ISC), CHDmodern (BSD-3-Clause, from MAME), Dear ImGui (MIT) and SDL (zlib).

Made by Claude AI based on LunaMoo's idea.
