# RhythmHeavenFever--QilinRecomp

A static recompilation of **Beat the Beat: Rhythm Paradise** (Nintendo, Wii, 2012; *Rhythm Heaven Fever* in North America) into portable C++23 with a Vulkan 1.3 renderer, for 64-bit Windows, Linux and macOS on x86-64 or ARM64. Any GPU with a Vulkan 1.3 driver is in scope, down to the Raspberry Pi 4 (Mesa 24.3+). Macs run it through MoltenVK (macOS 10.15 or later).

> **No game data is included, and none ever will be.** You need your own copy of the game: the Europe/Australia release, disc `SOMP01`, revision 0. The tools read `main.dol` and the disc files from a path you give them. `.gitignore` keeps game files, and anything generated from them, out of the repository.
>
> This is an independent project, not affiliated with or endorsed by Nintendo. *Rhythm Paradise*, *Rhythm Heaven* and Wii are Nintendo's trademarks.

## Status

Plays from boot through the title, save creation, the opening scene, the HOME menu and the first rhythm tests, through the first four rhythm games into the first remix, with sound, saves, the keyboard or a gamepad, and a settings menu. Not yet checked end to end: the later games aren't reached, rhythm timing isn't measured against a console, and a feature not met so far stops the run with a message (listed in `docs/runtime.md`).

| Part | State |
|---|---|
| DOL loader | ✅ the PAL build recognised by SHA-256 |
| Broadway decoder | ✅ all 222 instructions; 0 unexplained disagreements with GNU binutils over ~38 million swept words and all 512,944 of the game's |
| Function discovery and control flow | ✅ 8,171 functions, all 157 jump tables proven; 102 words still unknown to `qilin coverage` (data, `docs/dol-analysis.md`) |
| Names and libraries | ✅ 8,043 of the decomp's 8,103 functions matched; 74% of the code lifted (`docs/code-analysis.md`) |
| Lifter → C++ | ✅ 8,108 of 8,171 functions; the rest is privileged code and FPSCR access in replaced libraries |
| Differential test | ✅ 8,108 functions, 64,864 trials, 0 unexplained mismatches; clean under UBSan |
| OS, threads, disc, saves | ✅ |
| The SDK API the game calls | 🔨 243 of 255 replaced functions native; the other 12 stop if reached, none so far in play |
| GX on Vulkan 1.3 | 🔨 every screen met so far draws correctly (checked by eye) |
| Sound | ✅ stereo and Dolby Pro Logic II; about 7 ms of lag on Windows |
| Input | ✅ keyboard and any SDL gamepad; gamepads not yet confirmed on real hardware |
| Settings menu | ✅ |
| Windows installer | ✅ |
| Lockstep test bot | ✅ plays through the first rhythm tests |
| Rhythm timing, Linux and macOS runs, the rest of the game | ⏳ |

## Playing

There is no download: a built game is compiled from the game's own code, so it can't be shared. You build it from your own disc.

### Windows (installer)

1. Extract your disc to a folder (Dolphin: right-click the game, Properties, Filesystem, right-click "Disc", Extract Entire Disc). It must be the PAL release, `SOMP01`.
2. Double-click **`install.cmd`** and choose that folder.

It checks for CMake 3.25, Visual Studio 2022 17.10's C++ tools and Python 3.9 (naming the `winget` command for any that is missing), checks the disc, and builds the game (several minutes, about 6 GB free). It installs for you alone, without administrator rights, under `%LOCALAPPDATA%\Programs`, with shortcuts and an entry in "Installed apps" that uninstalls it. Leave the disc folder where it is: the game reads it.

| Option (`install.cmd -Option`) | Effect |
|---|---|
| `-Disc FOLDER` | the extracted disc, without the folder picker |
| `-InstallDir FOLDER` | where to install |
| `-Portable` | the folder only: no shortcuts, nothing registered |
| `-NoDesktop`, `-NoShortcuts` | no desktop shortcut, or no shortcuts at all |
| `-Checked` | memory checks that name a fault in the game's code (about half the speed), for reporting a crash |
| `-KeepBuild` | keep the build folder |
| `-Jobs N` | build jobs |

### From the source (Windows, Linux and macOS)

Build the tools ([Building](#building)), then lift the game with the runtime and build `qilin_run`:

```sh
python3 tools/analysis/port_symbols.py --qilin build/qilin --dol path/to/main.dol --symbols-out pal_symbols.txt
build/qilin lift path/to/main.dol --out gen-rt --runtime --symbols pal_symbols.txt
cmake -S . -B build-rt -DQILIN_RUNTIME_GEN_DIR=$PWD/gen-rt -DQILIN_RUNTIME_CHECKED=OFF && cmake --build build-rt --target qilin_run
build-rt/qilin_run path/to/extracted-disc
build-rt/qilin_run --controllers        # lists the controllers SDL sees and shows their presses
```

`qilin_run` takes the folder Dolphin's Extract Entire Disc writes (`sys/` and `files/`), not a `.wbfs` or `.iso`. Keep `pal_symbols.txt`, `gen-rt/` and the disc outside the repository. Leave out `-DQILIN_RUNTIME_CHECKED=OFF` to check every guest memory access, at about half the speed of the game's code.

### Controls

**F11** or **Alt+Enter** switches fullscreen. **F1** or **Esc** (a controller: Back and Start together) opens the settings menu. **F3** shows frame times, the display's refresh and the sound's lag.

The Wii Remote is held upright. The menu's Controls page rebinds a button by pressing the new input; bindings are kept in `[controls]` of the settings file.

| Wii Remote | Controller (any SDL knows) | Keyboard |
|---|---|---|
| A | south face button (Xbox A, PlayStation ✕), right shoulder | A, Right Shift, Right arrow or Space |
| B (the trigger) | east face button, left shoulder, right trigger | B, Left Shift or Left arrow |
| 1, 2 | west, north face buttons | 1, 2 |
| +, − | Start, Back | Enter, Backspace or Tab |
| Home | Guide | H |
| D-pad | d-pad or left stick | I, J, K, L, or the Up and Down arrows |

Every connected controller plays the remote, and rumble reaches them.

### Saves and settings

The data folder is `qilin/beatthebeat` in the per-user application data (`%LOCALAPPDATA%`, `~/Library/Application Support` or `~/.local/share`): saves in `nand/` (or `QILIN_NAND`), the shader cache `pipelines.bin`, and the settings file **`beatthebeat.ini`**, written with comments on the first run (`QILIN_SETTINGS=<file>` uses another). A malformed line stops the game at start, naming the line.

| `[display]` | |
|---|---|
| `preset` | `original` (the console's picture), `low-end` (Raspberry Pi 4–class), `desktop` (3× resolution, 16× anisotropic); later keys override it |
| `fullscreen` | `true` / `false` |
| `internal_scale` | 1–8× the console's 640×528, or `0` for automatic |
| `anisotropy` | 1, 2, 4, 8 or 16 |
| `vsync` | `true` (no tearing) / `false` (lowest latency) |
| `variable_refresh` | `true` for a G-Sync, FreeSync or VRR display |
| `look` | `plain` or `crt` |

`[audio]` holds `latency_ms` (10 by default) and `exclusive` (Windows' exclusive mode, on by default). `[console]` holds the Wii's own settings: `language`, `sixty_hz`, `widescreen`, `progressive`, `sound_mode`, `sensor_bar_above`, `dpd_sensitivity`.

## Building

Everything is **C++23**. You need CMake 3.25 or later and one of:

| Compiler | Minimum | Notes |
|---|---|---|
| GCC | 14 | Debian 13 / Raspberry Pi OS (Trixie) and Ubuntu 24.04 (`g++-14`) have it |
| Clang | 19 | Older Clang can't use libstdc++'s `std::expected` |
| Apple Clang | Xcode 16 | |
| MSVC | Visual Studio 2022 17.10 | |

The recompiler and its tests have no other dependencies. `qilin_run` needs SDL3 (a system copy is used if there is one), Khronos' Vulkan headers and Dear ImGui, which CMake fetches and checks against pinned SHA-256 hashes; no Vulkan SDK is needed. `-DQILIN_HEADLESS_ONLY=ON` builds it without them.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Other options: `-DQILIN_WERROR=ON`, `-DQILIN_UBSAN=ON`, `-DQILIN_LTO=ON`, `-DQILIN_DEBUG_SYMBOLS=ON` (profiling), `-DQILIN_BUILD_SHADERS=ON` (regenerates the committed SPIR-V with glslang).

CI builds and tests on Linux x64 (GCC 14, Clang 19), Linux ARM64, macOS ARM64 and x64, and Windows x64, with warnings as errors, and runs the decoder sweep.

## Using it

Extract `main.dol` from your disc first (`docs/dol-analysis.md` says how).

```sh
build/qilin --version
build/qilin info     path/to/main.dol [--json info.json]   # SHA-256, supported build, entry point, sections, BSS
build/qilin profile  path/to/main.dol                      # instruction profile of the code
build/qilin insns    path/to/main.dol [-o listing.txt]     # every word of the code, decoded
build/qilin disasm   words.bin [--base 0x80004000]         # a file of big-endian words, decoded
build/qilin cfg      path/to/main.dol [--json cfg.json]    # function discovery and control flow, summary
build/qilin coverage path/to/main.dol [--list]             # every word: code, padding, dead or unknown (exit 3 if any unknown)
build/qilin suspects path/to/main.dol                      # functions with problems, for review
build/qilin hwaccess path/to/main.dol [--json out.json]    # direct hardware accesses in lifted code
build/qilin lift     path/to/main.dol --out gen            # lift to C++ (gen/ is gitignored: never commit it)
```

### Differential test

```sh
cmake -S . -B build-gen -DQILIN_GEN_DIR=$PWD/gen && cmake --build build-gen --target qilin_difftest
python3 tools/difftest/difftest.py --harness build-gen/qilin_difftest --dol path/to/main.dol --gen gen
python3 tools/difftest/difftest.py ... --only 801E9140         # replays exactly that function's trials from the full run
```

Proves that lifted code behaves as the original: every lifted function runs from random states both as C++ and under Unicorn's PowerPC 750CL model (`pip install unicorn`; paired singles against `tools/difftest/paired.py`), and registers, memory and how each stopped must match. One worker per CPU (`--jobs`), under 3 minutes on 16 cores. For UBSan, configure with `-DQILIN_UBSAN=ON` or use `tools/difftest/build_ubsan.py`. Details in `docs/recompiler-core.md`.

### Decoder check

```sh
python3 tools/check/verify_decoder.py --qilin build/qilin --dol path/to/main.dol   # every word of the game's code
python3 tools/check/verify_decoder.py --qilin build/qilin --sweep                  # the encoding space (CI runs this)
```

Proves the decoder against GNU objdump (`-M broadway,raw`): mnemonic and operands must match, and a word must be valid for both or neither. Reviewed differences are counted by class; anything else fails. Needs a PowerPC objdump: `apt install binutils-powerpc-linux-gnu`, or devkitPPC's `powerpc-eabi-objdump` with `OBJDUMP` set.

### Names and libraries

```sh
python3 tools/analysis/port_symbols.py --qilin build/qilin --dol path/to/main.dol [--symbols-out pal_symbols.txt]
```

Ports the decomp's US function names to PAL addresses and prints each library's range (`docs/code-analysis.md`). The output is derived from the game: don't commit it.

### Scripted runs and the test bot

| Variable | Effect |
|---|---|
| `QILIN_HEADLESS=1` | no window, sound or keyboard |
| `QILIN_HIDDEN=1` | renders in a hidden window, no sound |
| `QILIN_INPUT=A@5,B+A@12:0.3` | timed presses (seconds after start, optional duration) |
| `QILIN_RUN_SECONDS=N` | end after N seconds with counters and each thread's state (exit status 3) |
| `QILIN_AUDIO_DUMP=out.wav` | record what the console would play |
| `QILIN_FRAME_STATS=N` | numbers about every Nth frame (coverage, mean colour) |
| `QILIN_FRAME_DUMP=<folder>` | sampled frames as BMP files (`QILIN_FRAME_DUMP_FROM=<frame>` for a dense stretch) |
| `QILIN_PROFILE=1` | where the CPU time goes, and the renderer's memory use |
| `QILIN_SAMPLE=1` | sampling profiler (Windows) |
| `QILIN_DVD_TRACE=1` | log the disc's operations |

Timed presses don't get past the timing calibration reliably, since the beat starts at a different moment each run. The lockstep bot does: it hears the beat in the game's audio, and the game's clock stops while it decides.

```sh
python3 tools/bot/beatthebeat.py --exe build-rt/qilin_run --disc DISC --out DIR [--seconds N] [--discover]
```

Keep `DIR` out of the repository: its pictures come from the game.

## Layout

| Path | What |
|---|---|
| `recomp/src/` | Recompiler: `dol`, `sha256`, `decoder`, `cfg`, `lifter`, `libraries` (lifted, replaced or dropped), `hwaccess`, `main` (the `qilin` CLI) |
| `recomp/runtime/` | `guest.hpp` and `dispatch.cpp` for lifted code; the game's runtime: `run.cpp` (`qilin_run`), `machine`, `native`, `os.cpp`, `threads`, `interrupts`, `dvd`, `nand`, `vi.cpp`, `gx*` and `shaders/` (graphics), `audio.cpp` and `wasapi` (sound), `wpad.cpp` and `input`, `host`, `settings`, `menu`, `bot` |
| `recomp/tests/` | Unit tests on synthetic data, needing no game files |
| `tools/check/` | The decoder check against GNU objdump |
| `tools/difftest/` | The differential test |
| `tools/analysis/` | `port_symbols.py` and other analysis scripts |
| `tools/bot/` | The lockstep test bot |
| `tools/install/` | The Windows installer behind `install.cmd` |
| `third_party/rhf/` | The Rhythm Heaven Fever decomp's symbols and splits (CC0) |
| `docs/` | Roadmap and design targets, prior art, analysis and status notes |

## Versions and changes

Releases are tagged `vX.Y.Z` on GitHub, and every release is recorded in [`CHANGELOG.md`](CHANGELOG.md). The version lives in one place, `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it. A minor bump (0.**x**) marks a milestone and a patch bump (0.x.**y**) a fix. 1.0.0 will be the first build that plays the whole game.

## Licence

This project's own code and documentation are under the [GNU General Public License, version 3](LICENSE) (GPL-3.0): you may use, share, change and sell it, but anything you distribute that is built from it must come with its full source under the same licence. Versions before 2026-11-21 were released under MIT, and those copies stay under MIT. `third_party/rhf/` keeps its CC0 1.0 licence, and the dependencies CMake fetches keep theirs: SDL3 (zlib), Khronos' Vulkan headers (Apache-2.0), Dear ImGui (MIT) and glslang when shaders are rebuilt.

The game itself is Nintendo's and is not covered by anything here: you need your own copy, and nothing built from it may be shared.

## Design targets

- **Efficiency and reliability come first.** A Raspberry Pi 4–class board is the reference, and the code fails loudly rather than misbehaving quietly.
- **Static recompilation, PowerPC → C++, compiled ahead of time.** No interpreter and no JIT. Hand-written C++ can override any lifted function.
- **Vulkan 1.3 only**, GX translated at API level, with a small baseline (1.3 core plus what the Pi 4's driver offers) so one render path runs everywhere. macOS runs through MoltenVK.
- **The SDK's hardware-facing libraries are replaced natively** (OS, GX, AX, DVD, VI, WPAD, NAND). The C/C++ runtime, the pure-software libraries and NintendoWare (NW4R) are lifted with the game.
- **Ground truth is Dolphin** running the original game, rhythm timing included. Names come from the CC0 Rhythm Heaven Fever decompilation.

See [`docs/`](docs/) for the roadmap and the analysis notes.
