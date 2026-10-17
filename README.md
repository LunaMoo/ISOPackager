# ISOPackager

Compress, extract, repack and benchmark PSP disc images — one file or whole folders — with a
Dear ImGui interface and a command line tool that share the same engine.

| In                                   | Out                                      |
|--------------------------------------|------------------------------------------|
| ISO, CSO (v1/v2), ZSO, DAX, CHD, EBOOT.PBP | any of them                        |

Built on three libraries, linked statically from the submodules in `external/`:

- [mCSO / maxcso](https://github.com/LunaMoo/maxcso) — CSO v1, CSO v2, ZSO and DAX
- [CHDmodern](https://github.com/LunaMoo/CHDmodern) — MAME's CHD code (DVD CHDs, like `chdman createdvd`)
- [iPBP](https://github.com/LunaMoo/iPBP) — PSN-style EBOOT.PBP (NPUMDIMG), LZRC and LZRC max

## What it does

- **Compress** an ISO to CSO / ZSO / CSO v2 / DAX / CHD / PBP, with every option the libraries have.
  CSO defaults to libdeflate with 64K blocks (lz4 HC for ZSO); untick Default for maxcso's presets
  or a custom method set (zlib, brute force, 7-zip, Zopfli, libdeflate, lz4, lz4 HC),
  block size, Zopfli splits and the libdeflate pre-check; CHD codecs (lzma, zlib, zstd, huff, flac)
  and hunk size; PBP compression level, content ID and boot screen.
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
isopackager-cli extract EBOOT.PBP
isopackager-cli verify --against original.iso game.cso
isopackager-cli info D:\Games
isopackager-cli bench --no-cso --codecs default,zstd --hunks 8192,32768 D:\ISO
isopackager-cli bench --cso-sets A,B,C,D,L --pbp-modes max game.iso
```

`isopackager-cli --help` lists everything. The `bench` options match `img_bench`
(`--cso-sets`, `--codecs`, `--hunks`, `--pbp-modes`, `--reads`, `--no-cso/--no-chd/--no-pbp`,
`--no-verify`), plus `--extra zso,cso2,dax`, `--source-on-disk` and `--work DIR`.

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

### Linking the three libraries together

Two pieces of third-party code exist twice when mCSO, CHDmodern and iPBP are linked into one
program. Both are resolved in ISOPackager's own `CMakeLists.txt`, without changing the submodules:

- **zlib** — maxcso bundles 1.2.11, CHDmodern 1.3.2 (same API): CHDmodern uses maxcso's.
- **LZMA SDK match finder** — maxcso's 7-zip deflate encoder bundles the 2017 `LzFind.c`,
  CHDmodern's LZMA codec the 2024 one; they export the same `MatchFinder_*` names with different
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

Settings are saved to `%APPDATA%\ISOPackager\isopackager.ini` (Windows),
`~/Library/Application Support/ISOPackager` (macOS), `~/.config/isopackager` (Linux, FreeBSD) or
the app's private folder (Android, iOS). Benchmark results go to the `bench` folder next to it,
unless another folder is chosen.

## License

No license has been chosen for ISOPackager's own code yet. Note that **iPBP is GPL-3.0**, so
binaries that link it (all of them, by default) must be distributed under GPL-3.0 terms. The other
components are permissive: maxcso ISC, CHDmodern BSD-3-Clause (MAME), Dear ImGui MIT, SDL zlib.
