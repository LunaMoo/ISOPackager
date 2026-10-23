# ISOPackager command line guide

`isopackager-cli` does everything the ISOPackager window does, except drawing charts: compress,
convert, extract, pack and unpack ZArchives, verify and benchmark disc images, one file or whole
folders at a time. It uses the same engine and the same settings file as the GUI.

```
isopackager-cli <command> [options] <files or folders...>
```

**Every command works without options.** The defaults are the settings we recommend, so for most
uses the command and the files are all you type:

```sh
isopackager-cli cso game.iso                 # game.cso, libdeflate with 64K blocks
isopackager-cli chd D:\ISO                   # every image in D:\ISO to CHD, 2K hunks
isopackager-cli iso game.chd                 # back to game.iso
isopackager-cli zar "D:\Games\Trails"        # the whole folder into Trails.zar
isopackager-cli iso Trails.zar               # unpacked into the folder Trails\
```

## Contents

- [Commands](#commands)
- [What you get without options](#what-you-get-without-options)
- [Format options: --method, --level, --block](#format-options---method---level---block)
  - [CSO, ZSO, CSO v2, DAX](#cso-zso-cso-v2-dax)
  - [CHD](#chd)
  - [EBOOT.PBP](#ebootpbp)
  - [RVZ and WIA](#rvz-and-wia)
  - [CD images: chd and cue](#cd-images-chd-and-cue)
- [How images are converted](#how-images-are-converted)
- [ZArchive: zar](#zarchive-zar)
- [Where outputs go](#where-outputs-go)
- [Speed, memory and safety](#speed-memory-and-safety)
- [verify and info](#verify-and-info)
- [bench](#bench)
- [Settings files](#settings-files)
- [Exit codes](#exit-codes)
- [More examples](#more-examples)
- [All options at a glance](#all-options-at-a-glance)
- [Older option names](#older-option-names)

## Commands

The command is usually the format you want to make.

| Command | Makes | Notes |
|---|---|---|
| `cso` | CISO v1 (`.cso`) | PPSSPP, PSP custom firmware |
| `zso` | ZISO (`.zso`) | lz4: the fastest to read |
| `cso2` | CISO v2 (`.cso`) | experimental: deflate or lz4 per block |
| `dax` | DAX (`.dax`) | old PSP custom firmware |
| `chd` | MAME CHD (`.chd`) | PPSSPP and other emulators; CD images become CD CHDs |
| `pbp` | PSN EBOOT.PBP | PSP games only |
| `rvz` | RVZ (`.rvz`) | GameCube and Wii discs only (Dolphin) |
| `wia` | WIA (`.wia`) | GameCube and Wii discs only (wit, Dolphin) |
| `cue` | cue sheet + `.bin` tracks | CD images and CD CHDs only |
| `iso` | plain ISO | extracts any image; a `.zar` is unpacked into a folder, a CD image to cue/bin. Alias: `extract` |
| `zar` | ZArchive (`.zar`) | several discs, DLC folders and anything else in one file |
| `verify` | – | decodes images and reports problems; checks a `.zar`'s SHA-256 |
| `info` | – | format, sizes, compression, disc ID and title |
| `bench` | – | compares settings on your images: size, speed, random reads |
| `formats` | – | lists the formats and what each makes without options |
| `help` | – | the short option list (also `-h`, `--help`) |

`convert --to FMT` is the long form of the format commands (`convert --to chd` = `chd`); without
`--to` it makes CSO.

Inputs can be any mix of files and folders. Folders are searched, with their subfolders, for
`.iso`, `.cso`, `.zso`, `.dax`, `.chd`, `.pbp`, `.rvz`, `.wia`, `.gcm`, `.zar` and `.wua` files, and CD
images (`.cue`, `.ccd`, `.mds`, `.gdi`, `.nrg`, raw `.bin` / `.img`; a cue sheet's track files are part of it, not
images of their own).
Images already in the target format are converted again with the new settings, as long as the
output doesn't replace the source (see [Where outputs go](#where-outputs-go)).

## What you get without options

| Format | Default settings | Why |
|---|---|---|
| CSO | libdeflate (level 12), 64K blocks | small files that PPSSPP reads quickly; fast to make |
| ZSO | lz4 + lz4 HC, 64K blocks | the fastest reads |
| CSO v2 | libdeflate, 64K blocks | |
| DAX | libdeflate, 8K frames (fixed by the format) | |
| CHD | chdman's codecs (lzma, zlib, huff, flac), **2K hunks** | a 2K hunk is one disc sector, so every read decompresses only what it needs |
| PBP | LZRC (what `sign_np` makes), written as `Game/EBOOT.PBP` | real PSPs and PPSSPP read it |
| RVZ | LZMA2 level 5, 128K chunks | Dolphin's defaults |
| WIA | LZMA2 level 5, 2M chunks | |
| ZAR | zstd level 22 with a 95% pre-check | already compressed discs are stored as they are, everything else gets the best compression |

`isopackager-cli formats` prints this table for your build, with any settings you loaded.

> Old PSP custom firmware may not read libdeflate's CSO output. For those, choose the classic
> methods: `isopackager-cli cso --method zlib,zlib-brute,7zip game.iso`.

## Format options: --method, --level, --block

Three options tune whichever format is being made. They mean the same thing for every format:

| Option | Means |
|---|---|
| `--method M` | how it's compressed: the methods, codecs or mode |
| `--level N` | how hard (formats with levels only) |
| `--block SIZE` | the unit every read decompresses: CSO block, CHD hunk, RVZ / WIA chunk |

`--method default` goes back to the recommended settings. Sizes take `K`, `M` and `G`
(`16K`, `2M`, `4G`; `KB` works too). Every option also takes the `--option=value` form.

An option that doesn't fit the format is an error, not silently ignored:

```text
$ isopackager-cli cso --level 5 game.iso
error: --level doesn't apply to CSO: CSO-family formats choose methods with --method
```

### CSO, ZSO, CSO v2, DAX

maxcso tries every method you list on each block and keeps the smallest result.

| Option | Values | Default |
|---|---|---|
| `--method` | a list joined by `,` or `+`. **CSO, DAX:** `libdeflate`, `zlib`, `zlib-brute`, `7zip`, `zopfli`. **ZSO:** `lz4`, `lz4hc`, `lz4hc-brute`. **CSO v2:** any of them | `libdeflate` (ZSO: `lz4,lz4hc`) |
| `--block` | a power of two from `2K` to `256K`, or `auto` (2K, 16K for images over 2 GB). Not for DAX: its frames are always 8K | `64K` |
| `--zopfli-splits N` | how many deflate blocks Zopfli may split each block into; `0` = Zopfli's default (15) | 15 |
| `--precheck PCT` | libdeflate compresses each block first; blocks it can't get below `PCT`% of their size skip the slow methods (Zopfli, 7-zip). Speeds up `zopfli` / `7zip` a lot on data that's already compressed (video, audio) | off |
| `--force-all` | compress every block, even when it doesn't save space | off |

What the methods are:

| Method | Speed | Notes |
|---|---|---|
| `libdeflate` | fast | level 12; small output. The default |
| `zlib` | fast | the classic; every reader takes it |
| `zlib-brute` | slow | zlib with every strategy tried |
| `7zip` | slow | 7-zip's deflate encoder |
| `zopfli` | very slow | the smallest deflate there is |
| `lz4` | very fast | ZSO / CSO v2 |
| `lz4hc` | fast | high-compression lz4 |
| `lz4hc-brute` | slow | lz4 HC with every level tried |

```sh
isopackager-cli cso --method zopfli,libdeflate --precheck 98 game.iso   # as small as CSO gets
isopackager-cli cso --block 2K game.iso                                 # for real PSP hardware
isopackager-cli zso --block 128K D:\ISO
isopackager-cli cso2 --method libdeflate,lz4hc game.iso
```

Giving only `--block`, `--zopfli-splits`, `--precheck` or `--force-all` keeps the default methods.

### CHD

| Option | Values | Default |
|---|---|---|
| `--method` | `default`, or up to 4 of `lzma`, `zlib`, `zstd`, `huff`, `flac`, joined by `,` or `+`. Each hunk uses whichever codec makes it smallest | `default` = lzma, zlib, huff, flac |
| `--block` | the hunk size: a multiple of `2K`, up to `1M` | `2K` |

`zstd` decodes fastest, `lzma` is the smallest. Bigger hunks compress better, but every read
decompresses a whole hunk.

```sh
isopackager-cli chd --method lzma,zstd game.iso
isopackager-cli chd --block 16K D:\ISO          # smaller files, slower seeking
isopackager-cli chd --block 4K game.iso         # chdman's own default
```

### EBOOT.PBP

PBP holds PSP games only; other discs are skipped with a message.

| Option | Values | Default |
|---|---|---|
| `--method` | `lzrc`, `max` (the same LZRC, searched much harder: 5–10× slower, a little smaller), `none` | `lzrc` |
| `--pbp-file` | write `Game.pbp` instead of `Game/EBOOT.PBP` | folder |
| `--pbp-folder` | write `Game/EBOOT.PBP` (the default; for overriding a settings file) | |
| `--content-id ID` | the content ID | `UP0000-<DISC_ID>_00-0000000000000000` |
| `--boot-png FILE` | a 480×272 PNG shown while the game boots | none |

Real PSPs need `PSP/GAME/<folder>/EBOOT.PBP`; PPSSPP loads both layouts.

Reading PBPs (any command that reads one):

| Option | Effect |
|---|---|
| `--no-trim` | keep the PBP's block padding when extracting, instead of cutting the ISO at its ISO9660 size |
| `--no-mac-check` | don't check the blocks' MACs while reading |

### RVZ and WIA

RVZ and WIA hold GameCube and Wii discs only; other discs are skipped with a message.

| Option | RVZ | WIA | Default |
|---|---|---|---|
| `--method` | `lzma2`, `zstd`, `lzma`, `bzip2`, `none` | `lzma2`, `lzma`, `bzip2`, `purge`, `none` | `lzma2` |
| `--level` | `bzip2`, `lzma`, `lzma2`: 1–9; `zstd`: −22 to 22 | 1–9 | 5 |
| `--block` | the chunk: a power of two from `32K` below 2M, or a multiple of `2M` (up to 32M) | a multiple of `2M` | RVZ `128K`, WIA `2M` |

`zstd` reads fastest; `purge` only drops runs of zeros.

```sh
isopackager-cli rvz --method zstd --level 19 --block 256K D:\Wii
isopackager-cli wia --method lzma --level 9 game.iso
```

### CD images: chd and cue

CD images — cue sheets (any number of files, AUDIO tracks, INDEX 00/01, PREGAP, MODE2/2336; a
FILE line naming a missing file falls back to the cue sheet's own name), CloneCD `.ccd` + `.img`,
Alcohol 120% `.mds` + `.mdf` (subchannel data skipped, gaps between tracks kept), `.gdi`, `.nrg`,
bare raw `.bin` / `.img` / `.iso` — become CD CHDs like `chdman createcd`: 8 frames per hunk,
`cdlz`, `cdzl` and `cdfl` by default. `chd --method` picks CD codecs too: `lzma` → `cdlz`, `zlib` → `cdzl`,
`zstd` → `cdzs`, `flac` → `cdfl` (`huff` has no CD variant; `--block` doesn't apply). A bare `.bin` is
read through the cue sheet that names it, or the `.ccd` / `.mds` of the same name.

`cue` (or `iso`) writes a CD CHD — or any of the images above — back as a cue sheet with one `.bin` per
track, named the way Redump does (`Game (Track 1).bin`), or one `Game.bin` with `--single-bin`. The
output is decoded again and compared frame by frame, like every other conversion. CD images in a
batch for another format (`cso`, `pbp`, ...) are made into CD CHDs instead; `--delete-source`
deletes every file of the image. In `zar --discs FMT`, CD images go in as CD CHDs.

```sh
isopackager-cli chd D:\PS1                       # every cue/bin, ccd, mds in it -> CD CHDs
isopackager-cli cue --single-bin "Game.chd"      # Game.cue + Game.bin
isopackager-cli verify --against Game.cue Game.chd
```

## How images are converted

- **An ISO** is compressed directly.
- **A compressed image** (CHD → CSO, PBP → CHD, CSO → CSO with other settings, …) is decompressed
  into RAM and compressed from there: no temporary ISO on disk. An image that doesn't fit the RAM
  budget goes through a temporary ISO instead (see [`--temp`](#speed-memory-and-safety)).
- **`iso`** writes the plain image. PBPs are cut to their ISO9660 size (`--no-trim` keeps the
  padding).
- **A `.zar`** given to `iso` (or any convert command) is unpacked next to itself into a folder
  named after it: `Trails in the Sky.zar` → `Trails in the Sky\`. `-o` doesn't move it. If the
  folder exists, it's skipped, renamed `Trails in the Sky (2)` with `--rename`, or extracted into
  with `--overwrite` (files of the same name replaced, others kept).
- **Verify** is on: every output is decoded again and compared byte for byte with its source, as it
  decodes (no extra disk space). `--no-verify` turns it off.
- Each output is written as `name.part` and renamed when it's complete, so a cancelled or failed
  conversion never leaves a half-written file under the real name.

## ZArchive: zar

`zar` packs a game's discs, DLC folders and anything else into one `.zar` that emulators read with
random access.

```sh
isopackager-cli zar "D:\Games\Trails in the Sky"
```

- **The first path, when it's a folder, is the archive's root**: its contents go in, the way the
  `zarchive` tool packs a folder. The archive is named after it (`Trails in the Sky.zar`) and
  goes next to it.
- **More paths** after it are added at the root under their own names.
- **`--no-root`** adds every path under its own name, the first folder too.
- Names that clash get ` (2)`; the archive never packs itself.

| Option | Values | Default |
|---|---|---|
| `--name NAME` | the archive's name, without `.zar` | the first path's name |
| `--zar-level N` | zstd level, 1–22. Every level reads back at the same speed | `22` |
| `--zar-precheck PCT` | each 64K block is first tried at zstd's fastest level; blocks that don't get below `PCT`% are stored as they are, without spending time on the slow level. `0` tries every block | `95` |
| `--store-discs` | store disc images without compressing them (the rest is still compressed). For games already in a compressed format | off |
| `--discs FMT` | put every disc in `FMT` first: ISOs are compressed, discs in another format are converted (to an ISO in RAM, then to `FMT` in RAM), and the result is stored. Discs already in `FMT` go in as they are | off |

With `--discs`, the [format options](#format-options---method---level---block) tune that format:

```sh
isopackager-cli zar --discs chd --block 16K "D:\Games\Trails in the Sky"
isopackager-cli zar --discs cso "D:\Games\Trails"
isopackager-cli zar --name "Trails" --store-discs game1.chd game2.chd D:\DLC\ULUS10000
isopackager-cli zar --no-root --name "Collection" D:\Games\GameA D:\Games\GameB
```

PBP, RVZ and WIA only take their own platforms' discs; other discs go into the archive as they are.
Verify reads every file back from the finished archive and compares it with what went in.

## Where outputs go

| Option | Effect | Default |
|---|---|---|
| `-o DIR`, `--output DIR` | write outputs into `DIR` | next to each source |
| `--flat` | with `-o`: put everything straight into `DIR`, instead of recreating the scanned folders under it | recreate them |
| `--no-recursive` | don't search subfolders | search them (`-r` turns it back on) |
| `--skip` | when the output exists, skip that image | **default** |
| `--overwrite` | replace the existing output | |
| `--rename` | write `Game (2).cso` instead | |
| `--delete-source` | delete each source after its conversion succeeded and (with verify on) was verified | keep sources |

Outputs are named after the source: `Game.iso` → `Game.chd`. A source called `EBOOT.PBP` is named
after its folder (`Game/EBOOT.PBP` → `Game.cso`, next to the `Game` folder). An output that would
replace its own source (`cso game.cso` next to the source) is always renamed (`game (2).cso`)
unless you pass `--overwrite`.

## Speed, memory and safety

| Option | Effect | Default |
|---|---|---|
| `--no-verify` | don't decode outputs again to compare them with the source | verify |
| `--no-ram` | repack compressed images through a temporary ISO instead of RAM | RAM |
| `--ram-budget SIZE` | RAM all jobs together may use for in-memory images, e.g. `4G`; `auto` | about 60% of the free RAM, at most ¾ of the total |
| `--temp DIR` | folder for temporary ISOs and images, when RAM isn't enough | the output folder |
| `-j N`, `--jobs N` | files converted at once | `1` |
| `-t N`, `--threads N` | worker threads per job; `0` = all cores | all cores |
| `--normal-priority` | don't lower the process priority | below normal |
| `-q`, `--quiet` | only warnings, errors and the final summary | |

Every conversion already uses all cores, so `-j 1` is usually fastest. More jobs help with many
small images, or when mixing CHD / PBP jobs. CSO-family jobs take turns, because maxcso runs one
task at a time.

Ctrl+C cancels: the running conversions stop and their partial outputs are deleted. A second
Ctrl+C quits at once.

## verify and info

```sh
isopackager-cli verify D:\Games
isopackager-cli verify --against original.iso game.cso game.chd
isopackager-cli info D:\Games
```

`verify` decodes every image completely and reports `OK` or `BAD` for each:

- **CHD:** checks its checksums.
- **PBP:** checks its block MACs (`--no-mac-check` turns that off).
- **ISO:** checks the file isn't shorter than its ISO9660 volume.
- **`.zar`:** checks the SHA-256 the archive stores over all of its bytes.
- **`--against ISO`:** also compares each decoded image with that ISO byte for byte.

`info` shows each image's format and compression details, the file and uncompressed sizes, the
platform, and the disc ID and title when it finds them (PSP: `UMD_DATA.BIN` and `PARAM.SFO`;
GameCube and Wii: the disc header). For a `.zar` it shows the number of files and their size.

## bench

`bench` measures settings on your own images. For each image and each setting it:

- compresses the image and times it;
- records the output size;
- decodes the result and times it, comparing it with the original;
- times random reads the way an emulator seeks (CSO family and CHD).

Each setting is a `--case`: a format, then any of its
[format options](#format-options---method---level---block):

```sh
isopackager-cli bench --case cso --case "cso --method zopfli --block 256K --precheck 98" \
                      --case chd --case "chd --method zstd --block 16K" --case pbp game.iso
```

Without `--case`, it runs the benchmark list. That list is the GUI's when you add `--gui-settings`
or `--config`; otherwise it's the default list: libdeflate 256K CSO, the default CHD and LZRC PBP.

| Option | Effect | Default |
|---|---|---|
| `--case "FMT [options]"` | one setting to test; repeat for more | the benchmark list |
| `--reads N` | random reads per image, each from a block or hunk not read before | `4000` |
| `--source-on-disk` | compress from the ISO file instead of loading it into RAM first (disk speed then shows in the times) | from RAM |
| `--work DIR` | where results go | the settings folder's `bench` |
| `--no-verify` | don't compare the decoded images | compare |

Compressed images are decoded once first, and every case starts from that. The results table is
printed, and also appended to `summary.txt` and `results.csv` in the results folder. Settings that
don't fit an image (RVZ for a PSP game, PBP for a Wii disc) show up as skipped.

## Settings files

The CLI starts from the built-in defaults. A settings file changes that, and options given on the
command line override it.

| Option | Effect |
|---|---|
| `--gui-settings` | start from the GUI's saved settings: every format's settings, output folder, engine options and the benchmark list |
| `--config FILE` | start from this `.ini` (one the GUI saved, or one from `--save-config`) |
| `--save-config FILE` | save the resulting settings to `FILE` and exit, without converting anything |

```sh
isopackager-cli chd --block 16K --method lzma,zstd --save-config chd16k.ini
isopackager-cli chd --config chd16k.ini D:\ISO
isopackager-cli cso --gui-settings D:\ISO        # exactly what the GUI would do
```

Where the GUI keeps its settings:

| System | File |
|---|---|
| Windows | `%APPDATA%\ISOPackager\isopackager.ini` |
| macOS | `~/Library/Application Support/ISOPackager/isopackager.ini` |
| Linux, FreeBSD | `$XDG_CONFIG_HOME/isopackager/isopackager.ini` (usually `~/.config/isopackager/`) |

A `portable.ini` next to the executables keeps the settings next to them instead.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | everything worked (skipped images count as fine) |
| `1` | bad command line, or nothing to work on |
| `2` | at least one conversion, verification or benchmark case failed |
| `130` | cancelled with Ctrl+C |

## More examples

```sh
# A whole library to CHD in another folder, keeping its subfolders, deleting the ISOs afterwards
isopackager-cli chd -o E:\CHD --delete-source D:\PSP

# Everything straight into one folder; existing outputs replaced
isopackager-cli cso -o E:\CSO --flat --overwrite D:\PSP

# CHD back to CSO: through RAM, no temporary ISO
isopackager-cli cso game.chd

# The smallest CSO, and the classic one that old PSP firmware reads
isopackager-cli cso --method zopfli,libdeflate --precheck 98 game.iso
isopackager-cli cso --method zlib,zlib-brute,7zip --block 2K game.iso

# PSN-style EBOOT for a real PSP, with a boot screen
isopackager-cli pbp --boot-png boot.png -o E:\PSP\GAME game.iso

# Wii discs to RVZ with zstd, two at a time
isopackager-cli rvz --method zstd --level 19 -j 2 D:\Wii

# A game's discs and DLC as one archive, discs converted to CHD first
isopackager-cli zar --discs chd "D:\Games\Trails in the Sky"

# Unpack every archive in a folder, each into its own folder (other compressed images there are
# extracted to ISO too)
isopackager-cli iso D:\Archives

# Check a collection
isopackager-cli verify D:\PSP D:\Archives

# Compare CHD hunk sizes on two games
isopackager-cli bench --case chd --case "chd --block 8K" --case "chd --block 32K" a.iso b.iso
```

On Linux and macOS, quote paths with spaces the same way (`"~/Games/Trails in the Sky"`) or escape
them.

## All options at a glance

| Option | For | Short description |
|---|---|---|
| `--to FMT`, `-f` | convert | the format to make |
| `--method M` | format commands, `zar --discs`, `--case` | methods / codecs / mode |
| `--level N` | rvz, wia | compression level |
| `--block SIZE` | cso, zso, cso2, chd, rvz, wia | block / hunk / chunk size |
| `--zopfli-splits N` | cso family | Zopfli block splits |
| `--precheck PCT` | cso family | skip slow methods on incompressible blocks |
| `--force-all` | cso family | compress every block |
| `--pbp-file`, `--pbp-folder` | pbp | `Game.pbp` or `Game/EBOOT.PBP` |
| `--content-id ID` | pbp | content ID |
| `--boot-png FILE` | pbp | boot screen |
| `--no-trim` | reading PBPs | keep block padding |
| `--no-mac-check` | reading PBPs | skip MAC checks |
| `-o DIR`, `--output DIR` | all that write | output folder |
| `--flat` | all that write | don't recreate subfolders under `-o` |
| `--no-recursive`, `-r` | all | don't / do search subfolders |
| `--skip`, `--overwrite`, `--rename` | all that write | when the output exists |
| `--delete-source` | converting | delete sources afterwards |
| `--no-verify`, `--verify` | converting, zar, bench | compare outputs with sources |
| `--no-ram`, `--ram` | converting | repack through a temporary ISO / RAM |
| `--ram-budget SIZE` | all | RAM for in-memory images |
| `--temp DIR` | converting, zar | folder for temporary files |
| `-j N`, `--jobs N` | converting | files at once |
| `-t N`, `--threads N` | all | threads per job |
| `--normal-priority`, `--low-priority` | all | process priority |
| `-q`, `--quiet` | all | less output |
| `--name NAME` | zar | archive name |
| `--zar-level N` | zar | zstd level |
| `--zar-precheck PCT` | zar | store incompressible blocks |
| `--store-discs` | zar | don't compress discs |
| `--discs FMT` | zar | put every disc in this format first |
| `--no-root` | zar | add the first folder under its name |
| `--against ISO` | verify | compare with this ISO |
| `--case "FMT [options]"` | bench | a setting to test |
| `--reads N` | bench | random reads per image |
| `--source-on-disk` | bench | compress from the file |
| `--work DIR` | bench | results folder |
| `--gui-settings` | all | start from the GUI's settings |
| `--config FILE` | all | start from a settings file |
| `--save-config FILE` | all | save the settings and exit |

## Older option names

Earlier versions had a different option for each format, and presets. The format options still work
under their old names, so existing scripts keep running:

| Old | Now |
|---|---|
| `convert --to chd --hunk SIZE` | `chd --block SIZE` |
| `--codecs LIST` | `chd --method LIST` |
| `--methods LIST` | `--method LIST` |
| `--splits N` | `--zopfli-splits N` |
| `--rvz-method`, `--rvz-level`, `--rvz-chunk` | `rvz --method`, `--level`, `--block` |
| `--pbp-mode M` | `pbp --method M` |
| `--compress-discs FMT` | `zar --discs FMT` |
| `extract` | `iso` (both work) |

Removed:

- **`--preset`:** the defaults are the recommended settings now, and `--method` picks any methods.
- **bench's `--cso-sets`, `--hunks`, `--pbp-modes`, `--extra` and `--no-cso/--no-chd/--no-pbp`:**
  use `--case` instead.
