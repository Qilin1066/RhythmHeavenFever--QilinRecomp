# Changelog

All notable changes to this project are recorded here, newest first. Past entries are never rewritten.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/spec/v2.0.0.html):

- **0.x.y** while the recompiler is being built. A **minor** bump (0.**x**) is a milestone, for example a new lifter slice or the runtime booting. A **patch** bump (0.x.**y**) is a fix.
- **1.0.0** will be the first build that plays the game end to end at original spec.

Each release is a git tag (`vX.Y.Z`) with a matching GitHub release. The version is set once, in `project(... VERSION ...)` in `CMakeLists.txt`, and `qilin --version` prints it.

## [Unreleased]

## [0.18.0] - 2026-11-28

### Added
- **Downloads for players** on the Releases page, as the other QilinRecomp projects have them: Windows (`-windows-x64.zip`), Linux on x86-64 and on ARM64 for the Raspberry Pi 4 and 5 (`-linux-x86_64.tar.gz`, `-linux-arm64.tar.gz`) and macOS, universal (`-macos.dmg`). Unpack, double-click **Play Beat the Beat** and choose the extracted disc's folder; the first start builds the game from it with the bundled compiler (LLVM-MinGW, zig; Apple's command line tools on a Mac), then Play. No CMake, Visual Studio or Python is needed. The launcher is Toy Commander's (`tools/player-kit/launcher`, launcher 4, copied unchanged); `kit/game.txt` tells it about this game. Built by `.github/workflows/player-kit*.yml` and attached to a `vX.Y.Z` tag's release. The installer stays; the README says how to remove its copy.
- **`qilin symbols`**: `tools/analysis/port_symbols.py`'s name porting in C++, so a player's computer needs no Python. It writes the same list (checked line for line on the supported `main.dol`), with difflib's alignment ported exactly. The download carries the decomp's `symbols.txt` and `splits.txt` (CC0); the list itself is made on the player's computer from their own disc. `port_symbols.py` stays for its library table and `--list`.
- **`qilin identify`**, the launcher's check of the chosen folder (one JSON object, exit 0, 3 or 2): the supported build by `main.dol`'s SHA-256, and a plain sentence for another game, the game's other regions, another revision, a disc image or `main.dol` chosen instead of the folder, the `sys` folder chosen by itself, Dolphin's partition folder, and an extraction missing `files` or `sys/fst.bin`.
- The player kit (`-DQILIN_PLAYER_KIT=ON`): the runtime as `libbeatthebeat_kit.a` (linked whole, as its native functions register themselves) and `libbeatthebeat_libs.a` (SDL3, Dear ImGui, the DOL loader), with `build-flags.txt` and a link check against stand-ins for the lifted code; `-DQILIN_LAUNCHER=ON` builds the launcher and that check without a kit. volk (MIT) is fetched for the launcher.

### Changed
- **The repository is now Qilin1066/RhythmHeavenFever--QilinRecomp** (renamed by the owner after the game's North American title, as the sister projects were to GBTGhoulies--QilinRecomp, ApbArcade--QilinRecomp and PunchOutWii--QilinRecomp; GitHub redirects the old names). Links and docs follow. What players see keeps the PAL game's name, and the data folder, the settings file and the installer's entry in Windows are unchanged.
- **Licence: GPL-3.0** (`LICENSE`), as in all the QilinRecomp projects, in place of MIT: anyone may use, share, change and sell it, but whatever they distribute that is built from it must come with its full source under the same licence. Earlier versions stay under MIT.
- README rewritten to the layout shared by the QilinRecomp projects: what works and how to build and play it, without the development history.

### Fixed
- The installer checks `main.dol` with .NET's SHA-256 rather than `Get-FileHash`, which is missing when Windows PowerShell inherits a PowerShell 7 module path (started from pwsh, for example).
- **Every controller works the F1 menu**, not only the first one SDL lists (which can be a virtual pad or a wheel's base): Dear ImGui's SDL3 backend is set to read them all, as APB's menu now does.

## [0.17.1] - 2026-11-25

### Fixed
- NANDRead returns NAND_RESULT_UNKNOWN for a host I/O failure rather than clearing the error and reporting a successful short read. Ordinary EOF still returns the bytes read. Successful short reads mark only returned bytes as changed; hard failures conservatively invalidate the requested buffer.

### Tests
- Added game-free NAND stream tests for full reads, short reads at EOF, zero-length reads and injected host read errors.

## [0.17.0] - 2026-11-21

Played by the owner through the first four rhythm games into the first remix. Less lag and stutter, the settings menu as the other Qilin recomps', and the first remix's black screen fixed.

### Fixed
- **The first remix stayed black and silent** (every time). The game's file manager frees a file slot when a disc read completes, but an archive keeps the slot's address until it has been decompressed (a frame or more later), and a completed read marks the first archive with a matching slot. Reads finished in 1 ms, so a read on a reused slot completed while the older archive still held it; the newer archive was never marked read and the remix waited for good. The drive now takes time as the console's does (one request at a time, 10 ms of access plus the bytes at 8 MB/s; APPROX). Very likely the same cause as the rare black screen after the strap screen. `QILIN_DVD_TRACE=1` logs the disc's opens, reads and completions.
- **A log file and a report:** started without a console, the log goes to `qilin_run.log` in the data folder; the menu's Game page writes a report (counters and each thread's state) for a game that seems stuck.

### Changed: the settings menu as Punch-Out's
- **Close and Quit the game** at the top right of every page (Quit was at the bottom of the Game page).
- **Controls are set by pressing them:** each Wii Remote button lists what presses it; Add, then a key, a controller button or a click; clicking one removes it; an input taken from another button moves and says so; "Back to the defaults". Kept in the settings file's `[controls]` (`a = pad:A, pad:RB, key:A, key:Right Shift, key:Right, key:Space`; malformed or reserved inputs stop at start with the line). Escape, F1, F3 and F11 are kept for the program. Keys go by SDL's scancode names, so the same place on any layout.
- **F3: frame times** in a corner (`performance_overlay`, kept): the frame rate and average, the 1% and 0.1% lows and the worst frame over the last 20 s, a graph, the display's refresh and how many times each frame is shown, how long before its vblank a frame is presented, and the sound's queue.
- **A CRT television look** (`look = crt`, `scanlines`, `mask`; `shaders/crt.frag`): the console's raster lines as beams with gaps that fade where a line gets too few window rows, and an aperture grille's stripes, in linear light. **"Look at the picture"** hides the menu until any key or button.
- **Internal resolution "Auto"** (`internal_scale = 0`): from the display's height (3x on a 1440-line screen), with a note on what it costs the GPU.
- **The refresh settings apply at once,** and **the lock follows full screen:** the display mode, the clock's lock and the vblank lock are worked out again whenever full screen is switched, the display's mode changes or the menu's refresh settings do (before, only at start: F11 into a 120 Hz mode left the game unlocked). The menu says what the display does now.
- About: the version, and where the saves and pipeline cache are.

### Changed: less lag and stutter
- **The renderer records on its own thread** (toward the Raspberry Pi 4, where the game's single thread is the limit): the game's thread still does everything that reads guest memory or decides (vertex transforms, texture checks and decoding, pipelines, batching, the frame's buffers, images' creation); a render thread, fed an in-order queue, records and submits (descriptor sets, draws, upload copies, clears, EFB copies, acquire, submit, present, the vblank lock), at the game's threads' scheduling class. The game's thread waits for it only to reuse a frame slot, for the lockstep bot, and before the menu parks the game; a Vulkan failure there stops the game with it. Measured over 40 s at the title: the game's thread 9.8% of a core where it was 11.6% (the process 20.6% against 19.3%); frames identical, pixel for pixel; the vblank lock as before (spread 0.16 ms, sound held at 7.6 ms, nothing dropped). At normal priority its presents spread by 2 ms and missed vblanks.
- **A sampling profiler** (`QILIN_SAMPLE=1`, Windows; names from a build with `-DQILIN_DEBUG_SYMBOLS=ON`, which keeps the optimisation): every millisecond each thread is paused, its place read and recorded, and the report lists the hottest functions, with time waiting in the OS apart. Its first findings, fixed: reading the clock at every native call was 7% of the game's CPU (now every 16th call, or at once when an interrupt is queued); MSVC's `std::isnan` and `std::isinf` called into the C runtime, 3% (now bit tests, exact: the differential test still finds 0 mismatches). The game's threads went from 0.195 to 0.18 of a core busy at the title. MSVC's `std::fma` is the runtime's call, but that uses the FMA instruction itself (1.08 against 1.00 ns): left.
- **Draws are batched** (toward the Raspberry Pi 4): consecutive draws with the same pipeline, textures, uniform block and dynamic state, their vertices next to each other, become one `vkCmdDraw`; state the rendering pass already has isn't set again, and a draw allocates nothing. Measured over 45 s from the title on: 1.28 million draws in 0.72 million batches (43% fewer draw calls), 1.0 µs a draw where it was 1.7; 18 dumped frames identical to the unbatched build's, pixel for pixel.
- **Exclusive-mode sound on Windows** (`wasapi.*`, `[audio] exclusive`, on by default): the device for the game alone through WASAPI's exclusive mode, at its shortest period (3 ms on a USB interface where sharing gives 10), with SDL still converting and resampling (an audio stream with no device that the output's thread takes from). Windows' volume and mute are applied in the game where the device doesn't do them itself (exclusive mode bypasses them). If the device can't be had exclusively, the shared mode as before; if it goes away, the output opens again. Measured: held at 7 ms (7.3 on average over 60 s), no underruns, where shared mode held 14.
- **Frames are locked to the display's vblanks** (Windows: the compositor's vblank times): with the display lock on, each present is steered to land 3 ms before a vblank by nudging the game's clock (a PI loop, within 0.2%): a frame is shown as soon after it is drawn as the display allows, every frame alike, and the slow drift between the two clocks no longer brings spells of uneven frames. Measured at 120 Hz: 3.08 ms before the vblank on average, a spread of 0.17 ms, no vblank missed in 60 s; the clock steered by 0.004%. Before, a frame waited anywhere from 0 to a whole refresh.
- **Variable refresh** (`[display] variable_refresh`, off by default; the menu's Timing section): for a G-Sync, FreeSync or VRR display with it on in the driver. The game keeps the console's 59.94 Hz (or 50 Hz), presents with FIFO (which variable refresh follows; mailbox can keep a driver at the fixed rate), and neither locks to the display nor switches its mode: any refresh rate shows the game evenly, a 144 Hz desktop as it is, windowed too where the driver allows. Measured on a 144 Hz desktop: every frame presented with no wait (0.1 ms each), sound held at 7.1 ms. It can't be detected from the program (the driver's setting), so it is a switch.
- **The display lock reaches 480 Hz** in the 60 Hz setting (each frame shown up to 8 times); in the PAL 50 Hz setting it locks only to a 50 Hz display, and the game's clock can now be steered while it runs (`set_time_base_speed` from any thread; the clock never goes back).
- **A burst of sound beyond the target is cut back at once** (beyond the target plus 10 ms, or twice the target): a skip once rather than seconds of extra lag.
- **The sound's lag is held at a set depth** (`[audio] latency_ms`, 10 ms by default, 5–200, raised to what the device can keep up: a period plus 4 ms; the menu's Display page). Before, nothing kept the game's 32 kHz in step with the sound card's clock: the queue between them sat anywhere from 0 to 100 ms, different each session, and clicked when it overflowed or ran dry. Now playing starts once the queue reaches the target, SDL resamples by a ratio that follows the queue's average depth (at most 0.5% off), a dry queue is refilled to the target with silence at once, and a burst after a stall beyond the target plus 40 ms is dropped. The device buffer is asked to be small (128 samples, through WASAPI's low-latency shared mode where the driver allows it). Measured on Windows with a 10 ms device period: held at 14 ms (14.2 on average, 4.7–23.0), no underruns. Measured on Windows (44.1 kHz output, 10 ms device buffer): 30.5 ms queued on average (19.6–39.0), no underruns, ratio 1.00011. The run report shows these numbers.
- **The game locks to the display's refresh** (`[display] lock_to_display`, on with vsync): when the display runs within 0.5% of the game's 59.94 Hz (50 Hz) or a whole multiple of it, the game's clock runs at the display's rate, so no frame is shown twice (at 60 Hz one was, every 16.7 s). The sound follows the same clock. **Fullscreen switches to a refresh that suits** (`match_refresh`) when the desktop's can't show frames evenly: a 75 Hz desktop (every fourth frame held for an extra refresh, a constant judder) goes to the same resolution at 120 Hz where the display has it. Both are set at start and named in the log.
- **Pipelines are made before they're needed:** the configurations met before are kept (`pipelines.keys` beside `pipelines.bin`) and a background thread makes them all at start (20 in 2 ms on a GTX 1080 Ti), so play doesn't compile any. A configuration never met before still compiles when first drawn.
- **FIFO presenting (no mailbox: common on Linux, macOS and the Pi) uses two images**, not three: each extra image was a refresh of lag. The log names the present mode.
- **Windows:** the idle wait sleeps on a high-resolution waitable timer and wakes at once when something is posted (a condition variable's wait rounds to the timer period, and Windows 11 ignores `timeBeginPeriod` for a window that isn't visible); the process opts out of power throttling (EcoQoS and the ignored timer request); the game's threads join the multimedia scheduler's "Games" class.
- **The installer builds without checked memory by default** (about twice as fast in the game's code); `-Checked` builds with it, for reporting a crash. `-Unchecked` is still accepted.
- **A sound device that can't be opened is tried again every 2 s** (a USB interface's driver stuck: WASAPI's "can't initialize audio client"), so sound starts once it works, rather than the whole session being silent; a driver that refuses the small buffer gets its own.
- **The controller's shoulder buttons play A (right) and B (left)**, beside the face buttons and the right trigger.
- **The keyboard's layout:** A = A, Right Shift, Right arrow or Space; B = B, Left Shift or Left arrow (pairs for two hands, right A and left B as on the controller); the d-pad moves to I, J, K, L and the Up and Down arrows; 1 and 2 are only 1 and 2.
- **`QILIN_AUDIO_DUMP` says where its time 0 is** against `QILIN_INPUT`'s: 0.605 s after the start, which was all of the "0.6 s from a press to its sound" seen before. The game itself adds no such lag.

### Added
- **The other Qilin recomps' UI conventions** (`docs/naming.md`): the window is named after the game and opens at the largest whole multiple of the picture that fits the display; F11 or Alt+Enter switch borderless fullscreen; the game's data folder is `<per-user data>/qilin/beatthebeat` (an old `BeatTheBeat-QilinComp` folder moves there on the first run).
- **Controllers** (SDL's gamepads, any it knows, hot-plugged): the first one plays the Wii Remote held upright (south face button A, east and the right trigger B, west 1, north 2, Start +, Back −, Guide Home, the d-pad or left stick); the game's rumble reaches it. The keyboard gains the other recomps' keys (Space or K, L, J, I, Tab) beside its own.
- **The settings file** (`beatthebeat.ini` in the data folder, written with comments on the first run; `settings.*`): presets `original`, `low-end` and `desktop`, keys overriding them, and the console's own settings the game reads (language, 60 Hz, widescreen, …), which were fixed in the code before. A malformed line stops at start, naming the file and line (tested).
- **A settings menu in the window, as the other Qilin recomps have** (`menu.*`, Dear ImGui v1.93.0, MIT, fetched pinned by SHA-256): F1 or Esc (a controller: Back and Start together, since alone they're the remote's − and +) opens it; the game pauses at the end of its frame (`pause.*`) and its clock stops with it, so the rhythm and the time base go on from where they were. Its pages: **Controls** (connected controllers, the Wii Remote's buttons on the keyboard and a controller, rumble), **Display** (full screen, which display, the picture's size: whole multiples, fit or fill the window, smooth, vsync, the preset, internal resolution, anisotropic filtering, the graphics device) and **Game** (the Wii's own settings for the next start, the settings file, quit). Closing it saves the file and applies the display settings in place (new EFB images at a new resolution; samplers; the swapchain). A hint names the key for the first 8 s. `QILIN_MENU_TOGGLE=<seconds>,...` opens and closes it in scripted runs.
- **New settings keys:** `monitor`, `scaling` (`integer`, `fit`, `stretch`), `smooth`, `gpu` in `[display]`; `[controls]` `rumble`. The file is rewritten from the values with its comments (a round trip, unit-tested).
- **Enhancements:** internal resolution (`internal_scale`, the frame buffer at 1–8×: viewports, scissors, clears, display and texture copies scaled; texture copies keep the resolution), anisotropic filtering (`anisotropy`), and `vsync` (mailbox or FIFO; off: immediate). At 3× (1920×1368) the title draws the same picture (coverage and mean colour as at 1×), the GPU at 1.75 ms a frame on a GTX 1080 Ti.

### Added
- **Surround: Dolby Pro Logic II**, which the game uses for the console's surround setting (it stopped before). The channel layout is the game's own (L, R, Ls and Rs per bus, with Rs in aux C's mix slots, read from NW4R's and AX's code); the encoding into two channels is the published Pro Logic II matrix with a 90-degree shift by an all-pass pair of this project's own design (`tools/analysis/design_dpl2_allpass.py`; marked APPROX), unit-tested against the matrix. The menu offers it as "Surround (Dolby Pro Logic II)".

- **The HOME button menu works** (it stopped on its first call): WPAD's pairing calls follow WUD's own code for a Bluetooth manager with no new remote in range (a start succeeds unless one is running, nothing is found, a stop reports whether it ended a search), and `WPADSaveConfig` writes the remote's settings and calls back as WPAD does. The game's screen for adding a remote uses the same calls.
- **Display lists are recorded** (`GXBeginDisplayList`/`GXEndDisplayList`): draws go into the game's buffer as the console's FIFO writes them (padded to 32-byte bursts), for RFL's Mii models; any other GX call while recording stops. Untested on real Miis: it needs a Mii database (the console's), which the runtime doesn't have.
- **Hidden runs:** `QILIN_HIDDEN=1` renders in a window that isn't shown, with no sound; `QILIN_FRAME_DUMP=<folder>` writes frames as BMP files (every `QILIN_FRAME_STATS`th, 60 by default), so a scripted run's picture can be checked without a window in the way.
- **Checked by play:** from the strap screen through the title, save creation, the opening scene and the HOME menu to the timing calibration (the first screen that wants presses on the beat), in stereo and surround, progressive, interlaced and PAL 50 Hz.

- **A licence: MIT** (`LICENSE`) for the project's own code and documentation. `third_party/rhf/` keeps its CC0 licence and the fetched dependencies keep theirs; the game is Nintendo's and covered by none of it (README, "Licence").
- **An installer for Windows** (`install.cmd`, `tools/install/`). It can't carry the game or a built game, so it builds here from the player's own disc: it checks the tools (naming the `winget` command for any that's missing), takes the extracted disc's folder (a folder picker; one level too deep is put right; an image file or another region is refused with the reason) and checks it by hash, builds the recompiler, lifts and builds the game in a folder of its own, and installs for the user alone: a folder under `%LOCALAPPDATA%\Programs`, Start menu and desktop shortcuts, an entry in "Installed apps" and an uninstaller that removes exactly what was installed (saves kept unless asked). `-Portable` makes the folder only.
- **A lockstep test bot** (`QILIN_BOT=1`; `recomp/runtime/bot.*`, `tools/bot/`). The runtime sends each frame's picture (160x114) and audio with the game's clock, and waits for the bot's presses, timed in the game's clock, with the clock stopped meanwhile, so a slow bot or a busy computer changes nothing the game sees. The bot hears the beat in the game's own audio (onsets fitted by least squares), ignores its own presses' sounds, recognises screens from measured picture features, reads the countdown's digits from their strokes, and has a watchdog for stalls. With it the game plays from boot through the calibration, the countdown and "Keep the beat!" to the next tutorial, which fixed-time scripts couldn't.
- **The run report counts disc requests still pending**, to tell a stuck read from a game waiting on something else.

### Fixed
- **Controllers: every gamepad plays, focused or not.** Only the first gamepad SDL listed was opened, so on a computer with another game device listed first (a virtual pad, a wheel) the real controller was never read; and SDL drops controller input while the window isn't focused unless told otherwise. Now every gamepad is opened and their buttons add up (as Zelda64Recomp reads all pads and UnleashedRecomp the one last touched), `SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS` is set (as UnleashedRecomp sets it), each device is named in the log with its type and USB ids, and a device with no gamepad layout says so. `qilin_run --controllers` lists what SDL sees and shows the Wii Remote buttons as they're pressed, without the game. Still to be confirmed on a real controller.
- **Frame statistics and dumps no longer slow the game.** A sampled frame's picture was read straight after it was submitted (a stall), from uncached memory the CPU reads very slowly; sampled densely, the game ran at half speed. The readback now uses cached memory where the GPU has it and is looked at when the renderer waits on that frame anyway, so a run sampling every sixth frame keeps the full 60 frames a second. `QILIN_FRAME_DUMP_FROM=<frame>` dumps one stretch densely.
- **Sound timing: the DSP mixed only about half of AX's frames.** A handler posted with no latency (the DSP's "frame done" after AX's mail) waited behind a catch-up of several periodic interrupts, so AX found its DSP still busy at the next audio DMA and skipped that frame. The mix fell to about half the DMA rate, the sound skipped, and the game's beat, which follows the sound clock, wavered by up to 100 ms, so the timing calibration couldn't be passed. Such handlers now run straight after the interrupt that caused them, as the console's DSP answers within the period: DSP frames equal audio frames (the run report shows both), and the calibration's signal is steady. With presses on its beat, the calibration passes and the game moves on to the next test.
- **The game gets past the title.** It had been "waiting" because its title asks for A and B together. With both pressed it stopped in the mixer: AXFX's reverb sends to the surround channel even in stereo, which the mixer took for an unknown. AX's own command list shows stereo has no main surround channel (the DSP set-up for stereo, surround and Pro Logic II is budgeted 733, 829 and 925 cycles, one 96-sample channel more each), so stereo now leaves it out, as the console does. The game now plays through creating a save into its opening scene, its sound clean (no clipping, both channels).

### Changed
- **The project is renamed BeatTheBeat-QilinRecomp** (from BeatTheBeat-QilinComp), now at `github.com/Qilin1066/BeatTheBeat-QilinRecomp`, with its sister projects ApbArcade-QilinRecomp, GBTGhoulies-QilinRecomp and PunchOutWii-QilinRecomp. The CMake project, the README, the docs and the installer's entry in Windows' "Installed apps" carry the new name. Nothing a player has saved moves: the `qilin` tools, the `QILIN_*` options and the data folder (`<per-user data>/qilin/beatthebeat`) are as they were, and a data folder still under the old project name is found and moved as before.
- **Efficiency, measured** (`QILIN_PROFILE=1`, `docs/runtime.md`): at the title the runtime went from 61% of a desktop core to 12% windowed and 8% headless (unchecked build).
  - Textures follow the console's coherency rule (`coherence.*`): a texture is re-checked only when the game flushed or DMA wrote a page under it (`DC*` natives, lifted `dcbf`/`dcbst` through `rt::cache_store`, disc and NAND reads), then hashed once. Texture checks went from 102 µs to 0.3 µs a draw.
  - The renderer's rings and descriptor pools start small and grow on demand (spilling, then resizing): about 20 MB of host-visible memory reserved instead of about 210 MB, which matters where the GPU shares the CPU's memory.
  - `-DQILIN_RUNTIME_CHECKED=OFF` builds without checked memory (half the lifted code's time); the default stays checked for bring-up. `-DQILIN_LTO=ON` builds with link-time optimisation (no measurable gain at the title).
  - **The TEV shader is specialised per configuration** (specialisation constants: the driver compiles straight-line code for each, a pipeline each): GPU time 2.18 → 0.61 ms a frame at the title, the same picture (frame statistics identical). The pipeline cache is kept on disk (`pipelines.bin` in the per-user folder), so later runs don't compile again: first use of a configuration 45 ms without it, 0.1 ms with it.
  - **A thread switch wakes only the next thread** (a condition variable per guest thread): about 9% less CPU.
  - The profile measures GPU time per frame (timestamp queries) and pipeline compiles.

### Fixed
From a full review of the runtime (three independent reads, each finding checked against the code before fixing):
- **DVD:** `DVDCancel` and `DVDClose` finish a pending request before returning, its callback run with "cancelled", as the SDK's synchronous cancel does; before, the completion still wrote the file info 1 ms later, after the game may have freed it. Requests are numbered, so a completion for a reused command block does nothing.
- **Threads:** whether a thread is ending is read before the run token goes (the next thread may rewrite its OSThread); a retired host thread no longer leaves a pointer to its destroyed context for reports. `OSLoadContext` unwinds only from the decrementer's handler, and stops, naming itself, anywhere else, instead of escaping uncaught.
- **The time limit** reports at the game's next safe point (an interrupt), not from its own thread while the game runs; a game stuck without safe points still ends 5 s later.
- **GX:** `GX_TEX_DISABLE` on a TEV stage's map is kept (it was truncated into a lookup of map 0); one or three normals follow the vertex format's element count alone, as the command processor parses, and NBT normals stop in the renderer instead of misreading the rest of the vertex; display lists, textures, TLUTs and copies are checked against guest memory; TEV selectors, ops and channels GX doesn't define, fog type 8, copy and display rectangles outside the EFB, a halving copy larger than its source, and a non-positive viewport stop instead of drawing wrong.
- **Renderer:** the depth buffer starts at the far plane (it was undefined); a texture is re-checked after each texture or TLUT load as well as each frame, so a buffer rewritten between draws isn't drawn stale; textures unused for 10 s are freed (the cache grew without bound); an EFB copy's guest bytes are hashed once a frame, not per draw; the sampler's minimum LOD never exceeds its maximum; the window must offer an 8-bit UNORM format that blits can write (no sRGB fallback that would wash the picture out).
- **Audio:** a zero or out-of-memory address in AX's command list stops, instead of becoming a write into low memory.
- **NAND:** a path component with `\` or `:` is refused (on Windows it could leave the flash's folder); paths longer than the flash's 63 characters stop instead of being cut short; read and write buffers are checked against guest memory; a seek outside the file stops (the console's answer isn't pinned down).
- **WPAD:** `WPADDisconnect` disconnects (the connect callback hears it, reports stop) and a button press reconnects, as on a console.
- **OS:** the UART, `__write_file`, `OSPanic` and `OSRegisterVersion` check their guest text.
- **Input script:** `A@:0.3`, `A@nan` and `A@1:` are refused (tested).
- **Interrupts, efficiency:** lifted loops take the slow path only when something is due now, not whenever an I/O completion is queued 1 ms ahead.
- **VI:** `VIWaitForRetrace` delivers what came due while it slept when it turns interrupts back on.

## [0.16.0] - 2026-11-19
The game is seen, heard and controlled: `qilin_run` opens a window, draws with Vulkan 1.3, mixes the game's sound and plays it, and takes the keyboard (or a script) as a Wii Remote. It boots to the title and past the first A. Not yet checked by eye, and not playable end to end.

### Added
- **A scripted Wii Remote** (`QILIN_INPUT`, `recomp/runtime/input.*`): a virtual remote on channel 0 presses buttons on a schedule for headless, repeatable runs. WPAD drives KPAD as KPAD's code expects (connect callback, then sampling callbacks at the report rate, `WPADRead`'s status), with the data format, pointer control and the remote speaker's control protocol; no motion or pointer data is made up. With it, A at the title takes the game to its first sound.
- **The DSP mixer** (`recomp/runtime/audio.cpp`): AX's command list carried out in full, read from AX's and NW4R's code. Exact where game logic looks (play positions, loops and ends, DSP-ADPCM with its loop context, envelope and mix ramps, aux round trips, the write-back AX reads); approximations marked in the code (the polyphase resampler, the compressor); unsupported features stop. `QILIN_AUDIO_DUMP=<file.wav>` records what AI plays.
- **Approximate estimates in the game's build** (`QILIN_APPROX_ESTIMATES`, the owner's decision): where Broadway's `frsqrte`/`fres` table is needed, the exact result rounded to 12 fraction bits (IBM's stated accuracy), marked APPROX. The differential test's build still stops there. The game now runs past the title.
- **A window, the Vulkan renderer, sound and the keyboard** (`host.cpp`, `gx_vulkan.cpp`, `gx_texture.cpp`, `shaders/`): `qilin_run` opens a window and draws GX with Vulkan 1.3 (GX's transform and lighting on the CPU, the TEV in one fragment shader, GX's texture formats decoded and cached), plays AI's output on the default audio device and takes the keyboard as a Wii Remote. SDL3, Khronos' Vulkan headers and glslang are fetched at configure time, pinned by hash, never committed. `QILIN_HEADLESS=1` (or `-DQILIN_HEADLESS_ONLY=ON`) runs as before. Unsupported GX features stop with a message.
- **More of GX on Vulkan:** copies from the EFB to textures (with the copy formats' conversions), mipmaps with the texture object's filters, LOD range and bias (layout read from the SDK's `GXInitTexObjLOD`), lines and points, fog, manual texture-coordinate scaling; Z textures now stop instead of being ignored.
- **The runtime's to-do list from its own lift:** of the 260 replaced functions the game's code can call, 40 had no native code; NAND's asynchronous calls, `NANDPrivateOpen`, `NANDDelete`, `OSPanic`, the OS's exits, `__DVDTestAlarm`, `GXGetPixelFmt`, `__write_file` and `WPADGetInfoAsync` now do, and `OSAlloc.c` is lifted. What's left (display-list recording, the ROM font, the balance board, MotionPlus, pairing) stops when reached.
- **`QILIN_FRAME_STATS=N`:** numbers about every Nth frame (coverage, mean colour), to tell drawn frames from empty ones without looking.
- **Tests:** GX's texture formats on synthetic tiles (I4, I8, IA8, RGB565, both RGB5A3 forms, RGBA8's split halves, C8 through a TLUT, CMPR's two modes, padded sizes, an unknown format stopping) and the input script's parser (presses, combinations, durations, every malformed form).

### Changed
- **One dependency fewer:** the shaders' SPIR-V is committed beside their GLSL with a hash stamp the configure checks; glslang is fetched only with `-DQILIN_BUILD_SHADERS=ON`. A default build needs SDL3 and the Vulkan headers.
- **The input script** is parsed apart from the environment (`parse_input_script`, tested), and its times count from the runtime's start, as documented, instead of from the first `WPADInit`.

### Fixed
- **The renderer** stops on a vertex's matrix index past GX's matrix memory, and on an indexed vertex attribute outside MEM1 and MEM2, instead of reading beyond them.
- **Copies to textures** compare the guest bytes they cover by the copy's own size, so a copy read in a format of another size isn't dropped.

The differential test: 8,108 functions, 64,864 trials, 0 unexplained mismatches.

## [0.15.0] - 2026-11-19
The game runs: it initialises completely, saves on first boot, reaches its title scene and keeps running, a frame copied out on every retrace, with the audio clock at the console's rate. Nothing is shown or heard yet (headless GX; the DSP stops at the first voice), and with no remote connected the game waits at its title.

### Added
- **Audio** (`recomp/runtime/audio.cpp`): AX lifted whole (eight units: CPU code with no hardware access, whose voice structures NW4R writes directly); AI's DMA interrupt every 3 ms (96 stereo samples at 32 kHz); the DSP takes AX's mail (`0xBABE0080`, the command list), decodes the list's set-up and the voice chain, outputs silence while no voice plays and stops when one does. The protocol is AX's own, read from its code.
- **NAND** (`recomp/runtime/nand.*`): saves in a per-user host folder laid out as the flash (`QILIN_NAND` to choose it); create, open, read, write, seek, length, close, move, type, status, check, the home directory, and the asynchronous safe open for reading. Results as the game's backup manager reads them. The first boot writes `Riq.dat` and its banner.
- **Safe points in lifted code:** at every call into native code, and on every backward branch (the lifter emits a poll with the loop's stack pointer: 2,594 in the runtime's lift). A loop waiting for an interrupt gets it, as on the console.
- **The estimate instructions:** `frsqrte` and `fres` lift, exact where the architecture defines them (zeros, infinities, NaNs, negative square roots) and stopping where Broadway's table would be needed. The last five functions that weren't lifted (`PSMTXInverse`, `PSMTXInvXpose`, `PSVECNormalize`, `PSVECMag`, NW4R's `FrSqrt`) now lift: 8,108 functions in the differential test, all 6,202 in the runtime's lift.
- **More lifted out of replaced libraries:** `OSReport`, `OSVReport`, `OSCrc.c`, `NANDInitBanner`, `NANDSetUserData`/`NANDGetUserData`, and the debugger stub's console write (`__TRK_write_console`, `GetUseSerialIO`), which MSL calls on every console.
- **Natives:** the console type (retail), the debug UART (the game's reports to stderr, `[game]`), power and reset callbacks, GameCube pads (none plugged in), `WPADProbe` and `WPADDisconnect` (no remote), `DCFlushRangeNoSync`.
- **Diagnostics:** a guest call stack with every stop; `QILIN_RUN_SECONDS` (ends a run with the counters, every thread's state and stack, and the scheduler's state; exit status 3); counters for audio frames, dropped periodic interrupts and where deliveries happen.
- **Tests:** the estimates, a callee-saved register keeping its constant across a call, a loop's poll.

### Changed
- **Periodic interrupts catch up** when delivered late (each missed audio frame or retrace runs; only a stall beyond 32 periods drops, counted), and `qilin_run` asks Windows for 1 ms timers. Before, four of five audio frames were lost.
- **I/O completions take 1 ms** (queued interrupts have a due time): an instant completion overtook the game's own bookkeeping (it sets its "loading" flag after issuing a read) and left it waiting forever.
- **MEM1's arena ends below the disc's file table**, as a console's apploader puts it (from `sys/fst.bin`'s size), and the runtime's guest scratch lives in the table's place: the game's heap is the console's size (it was 480 KB short, and the game said so).
- **`hwaccess` keeps callee-saved registers across calls** (r1, r2, r13, r14-r31, as the EABI does): 27 more FIFO stores found in lifted code (349 in the runtime's lift), among them the game's sprite drawing (`CCellAnim::draw`).
- The time base has its own file (`recomp/runtime/timebase.cpp`).

## [0.14.0] - 2026-11-19
GX's CPU side: the state the game's GX calls set up, the vertex stream lifted code writes to the FIFO, and VI's retrace. The game initialises its graphics (with the SDK's own GX defaults), controllers and Home Button Menu, loads files from the disc through asynchronous reads, sleeps on alarms, and stops where the Home Button Menu starts its sound. The thread, interrupt, alarm and DVD code from v0.13.0 has now run.

### Added
- **The GX FIFO in lifted code:** the lifter turns the stores `hwaccess` finds on the write-gather pipe (322 in 12 functions in the runtime's lift; `stw`, `sth`, `stfs`; and `psq_st` in the GX library itself) into `rt::fifo_st8/16/32` and `rt::fifo_psq_st`. Other stores stay plain. In the differential test they stop as the original does under Unicorn: 64,824 trials, 0 mismatches, as before.
- **Native GX** (`recomp/runtime/gx.*`, `gx_fifo.cpp`): about 110 setters, loaders and copies keeping GX's state as the API describes it; `GXBegin` and the FIFO stores make draws, `GXCallDisplayList` walks a list's draws, anything else stops. A headless sink counts draws and copies until the Vulkan renderer exists.
- **The SDK's own GX defaults and numbers:** native `GXInit` calls the lifted `__GXInitGX`; `GXSetTevOp` takes its presets from the SDK's tables in the game's data (found through its code); `GXSetDispCopyYScale` ports the SDK's line arithmetic. Lifted out of GX: the texture, TLUT and light object functions, `GXGetTexBufferSize`, `GXGetYScaleFactor`, `GXAdjustForOverscan`, `GXSetTevDirect`, `__GXInitGX`; the natives decode those objects in the SDK's layout, read from its getters.
- **VI:** `VIConfigure`, frame buffers, flush, black, retrace callbacks, and the retrace as a periodic interrupt at the field rate (59.94 or 50 Hz) that wakes `VIWaitForRetrace` (which sleeps through the lifted `OSSleepThread`). Periodic interrupts in `interrupts.cpp`.
- **Input:** WPAD's start-up as KPAD and the controller manager use it, with no remote connected until there is a host input backend; `WPADGetWorkMemorySize` lifted (the console's heap size); GameCube pads: none. Console settings for the pointer and the sound mode.
- **SC** start-up (`SCInit`, `SCCheckStatus`, `SCGetSoundMode`), the **locked cache** (no lifted code addresses it), and **guest scratch** for the runtime's own guest objects.
- `qilin_run` also reports draws, vertices, frames copied out, copies to textures and retraces.
- **Tests:** the lifter hands exactly the FIFO stores to the runtime.

## [0.13.0] - 2026-11-19
The OS and the disc under the game: the OS's threads, mutexes, messages, time and alarms are lifted over a native thread switch and interrupt delivery, and the game's files come from an extracted disc. `qilin_run` gets through the static constructors and `main`'s start-up (video mode, file manager) to its graphics manager's `GXInit`. The new thread, interrupt, alarm and DVD read code is wired in but not yet exercised: the game reaches `GXInit` before it uses any of it.

### Added
- **The OS's software lifted, by translation unit** (`docs/runtime.md`): `OSThread.c`, `OSMutex.c`, `OSMessage.c`, `OSTime.c` and `OSAlarm.c` are lifted out of the replaced OS (`kLiftUnits` in `libraries.cpp`), with exceptions by name (`kNativeAnyway`: `SelectThread`). Names, not addresses. `qilin lift --runtime` fails if a unit or function the plan names isn't in the symbol list, which now carries each function's unit.
- **The thread switch** (`recomp/runtime/threads.cpp`): `SelectThread` ported step by step from the build's own code, each guest thread on its own host thread with a run token handed between them; its globals are read from its instructions and checked at start-up. A new thread starts with the registers `OSCreateThread` left; an exited one ends its host thread. `OSSetCurrentContext`, `OSInitContext` (retires a host thread whose context is reused) and `OSLoadContext` native.
- **Interrupts at safe points** (`recomp/runtime/interrupts.*`): delivered when the game enables interrupts and while the scheduler is idle; queued handlers run as the SDK's dispatcher runs them (scheduler off, handler, scheduler on, reschedule); the decrementer runs the OS's installed handler, whose closing `OSLoadContext` unwinds back to the delivery. `PPCMtdec` sets the deadline; the exception table is native. Idle with nothing that could ever wake a thread stops (`Stop::Idle`).
- **DVD over an extracted disc** (`recomp/runtime/dvd.*`): `qilin_run` takes the folder Dolphin's Extract Entire Disc writes. The disc's own `fst.bin` gives entry numbers and directories; lookup as the SDK's (from the root, `.`/`..`, no case); files found again by the disc offset in the file info, as the SDK does; asynchronous completions arrive as interrupts after the call returns. Eleven DVD functions.
- **Natives that check the code they replace** (`QILIN_NATIVE_CODE`): `qilin_run` refuses to start if the function of that name doesn't begin with the stated instructions (`PPCMtdec`, `PPCHalt`). Natives are bound after the DOL is loaded.
- **VI and SC basics** (`recomp/runtime/vi.cpp`, `console.hpp`): `VIInit`, the TV format and cable, and the console settings the game picks its video mode and language from (60 Hz, progressive, 16:9, English by default; runtime settings until the settings file exists).
- **What a run exercised:** with a stop, `qilin_run` prints its thread switches, threads started, interrupts, files opened and bytes read. Stops are reported from any host thread with function names (`rt::report_stop`); native code calls lifted functions with `rt::call_guest`; `rt::time_base()` is the one 64-bit time base for `mftb` and the decrementer.
- `OSInit` writes the bus and CPU clocks to low memory (243 and 729 MHz), which the SDK's tick conversions read, and runs `__OSInitAlarm` and `__OSThreadInit` in its own order.
- **`tools/analysis/lowmem.py`**: reads a Dolphin MEM1 dump taken at the entry point and prints the low-memory words `OSInit` and `__start` read, next to the runtime's values, flagging those that differ. For confirming the boot values in `docs/runtime.md`.

### Changed
- Low memory is open to lifted code under checked memory: the lifted OS keeps its globals there.
- **`port_symbols.py` checks names against code:** the base library's register accessors must read or write their register (`SIGNATURES`); a name the size alignment put elsewhere moves to the matching function nearby, or the port stops. Eight names moved.
- **The game/base boundary** (`code-analysis.md`, `libraries.cpp`) moves from `0x800DA6A4` to `0x800DA6C0`: the three functions before it are the game's (they return constants and game code calls them), so they are lifted.

### Fixed
- **Eight wrong names in the base library.** A run of equal-size functions had put `PPCMfmsr` … `PPCSync` three functions early, `PPCMtdec` on the function that writes HID0. Natives bound by those names would have replaced the wrong functions; none was yet.
- **`guest.hpp` on zig's macOS target:** with `<chrono>` included before `<cfloat>`, zig's bundled macOS headers lose `DBL_MIN` and `FLT_MIN`, so `dispatch.cpp` didn't compile. The smallest normal values now come from `std::numeric_limits`: same values, no macros. Found by cross-compiling the runtime for Linux and macOS on x86-64 and ARM64 with zig, which now all pass (the first check of `machine.cpp`'s non-Windows memory mapping).

## [0.12.0] - 2026-11-18
The runtime skeleton: `qilin_run` starts the game. It runs `OSInit` natively and the static constructors as lifted code, and stops at the first replaced function it has no native code for, naming it and its caller.

### Added
- **`qilin lift --runtime --symbols <list>`**: the runtime's own lift. Functions of replaced and dropped libraries become calls to native code (`rt::native`), listed in a generated `natives.cpp`; `symbols.cpp` names every function for reports. The differential test's lift is unchanged and still lifts every library.
- **The library plan in code** (`recomp/src/libraries.*`): the lifted, replaced and dropped ranges of `docs/code-analysis.md`, and the functions lifted out of replaced libraries by name: `__init_user`, `__init_cpp`, `exit`, and GX's light-object functions, which only fill in a structure.
- **The registry of native functions** (`recomp/runtime/native.*`): native code registers by the function's name with `QILIN_NATIVE(name)`, so the repository holds no list of PAL addresses. Start-up refuses a registration that matches no replaced function, several, or is repeated. A replaced function without native code stops the game when called (`Stop::Unregistered`).
- **`qilin_run`** (`recomp/runtime/run.cpp`, `machine.*`; CMake `QILIN_RUNTIME_GEN_DIR`): maps MEM1 and MEM2 in a reserved 4 GB guest space, loads the DOL, sets up what `__start` sets up, then calls `OSInit`, `__init_user`, `main` and `exit`. Every stop is reported with function names. Bring-up runs with checked memory: low memory, the uncached mirrors and hardware registers stop lifted code.
- **The first native OS functions** (`recomp/runtime/os.cpp`, 22): `OSInit`, the interrupt enable, the arenas, the cache range functions, `OSRegisterVersion`, `PPCHalt`.
- **`docs/runtime.md`**: how the runtime is built and reached, the boot values still to confirm in Dolphin, and a proposal for the next step: lift the OS's thread, mutex and alarm bookkeeping and replace only the context switch, interrupts and timers under it.
- **Tests** for the library plan, the symbol list reader and the registry.

## [0.11.0] - 2026-11-18
The runtime's to-do list: what lifted code needs from the libraries and hardware the runtime replaces.

### Added
- **`qilin hwaccess`** (`recomp/src/hwaccess.*`): the loads and stores in each function whose address is a constant outside cached MEM1, found by constant propagation over the GPRs through the control flow, classified by region (hardware block, GX FIFO, uncached mirrors, MEM2, EFB, locked cache).
- **`tools/analysis/runtime_todo.py`**: joins the call graph, `qilin hwaccess` and the library plan into the to-do list. Lifted code calls 309 replaced functions directly, and its only direct hardware access is the GX FIFO: 322 stores in 12 functions, from inlined GX calls in NW4R, the Home Button Menu, the Mii library and the game. 5 lift-side functions need the estimate tables. The report is derived from the game and stays out of git.
- **Tests** for the address map and the constant propagation (GX FIFO and VI addresses found, cached MEM1 ignored, registers unknown after a call).

## [0.10.0] - 2026-11-18
The differential test under UBSan, finding no undefined behaviour in lifted code or the runtime across all 64,824 trials. The difftest is now deterministic and isolates its trials.

### Added
- **`QILIN_UBSAN`** (CMake): builds lifted code and `qilin_difftest` with `-fsanitize=undefined -fno-sanitize-recover=all`, for GCC and Clang. CMake refuses it under MSVC, which has no UBSan.
- **`tools/difftest/build_ubsan.py`**: builds the UBSan harness with zig's Clang (`pip install ziglang`) on any host, Windows included, with the project's warnings as errors.
- **A deterministic time base in the difftest** (`QILIN_DETERMINISTIC_TIME`): the k-th time-base read of a trial gives the same value on both sides, so trials that read it are compared (they were known differences) and every trial replays exactly.
- **Trial isolation checks:** Unicorn's whole CPU state is restored before each trial; every 16th trial checks on both sides that MEM1 is the image again; a failure lists the functions the worker ran, for `--only` to replay; a memory mismatch shows which side changed the block, and to what.

### Changed
- **Found:** the rare non-reproducible stops these checks caught were bit flips in the development machine's overclocked non-ECC RAM, not the code. A stress test without any project code reproduced them.
- The roadmap drops "CR bits only where read". `cr` is a local, so the host compiler already removes CR updates nothing reads: Clang folds a record-form update followed by a compare to the compare alone. CR that reaches a call or return is observable, so it has to be computed anyway.

## [0.9.0] - 2026-11-18
Register dataflow around calls: lifted code stores only the registers it changed since the last sync, and reloads only those it reads before overwriting. The game's lifted objects are 10.6% smaller, and the differential test results are unchanged (64,824 trials, 0 unexplained mismatches).

### Changed
- **Sync points** (before a call, return or tail call; after a call) move what two dataflow passes over the function choose, where they used to move every register the function uses: the dirty registers (forward, may) are stored; the live ones (backward, a later store counting as a read) are reloaded. Reads and certain writes come from the emitted code statement by statement, erring towards one store or reload too many. The `out()`/`in()` lambdas are gone.

### Added
- **Tests** for the minimal stores and reloads on the synthetic program. A planted dataflow bug (no reloads after calls) shows up in the difftest as 24 mismatching functions.
- **A Clang check on Windows** without installing LLVM (`ziglang`), described in CLAUDE.md: the generated code is warning-free under Clang 22 with `-Werror`.

## [0.8.0] - 2026-11-18
Lifter slice 4, paired singles: 8,103 of the game's 8,171 functions lift (554 more). The rest are privileged code and FPSCR access in libraries that get replaced, plus five functions that use Broadway's estimate instructions. A reference model stands in for Unicorn, which has no paired singles: 64,824 trials, 0 unexplained mismatches.

### Added
- **Lifter, slice 4**, from IBM's 750CL user's manual: each FPR's ps1 (`c.ps1[]`); quantised loads and stores through the GQRs (types, scales, rounding toward zero, clamping, NaN and infinity by sign, W = 1); paired arithmetic, multiply-adds, sums, merges, moves and compares. `lfs` and single-precision results now fill both halves; double-precision instructions leave ps1 alone, which CodeWarrior's FPR saves in prologues rely on. A reserved GQR type, or a multiplier lane that isn't a single, stops.
- **`fsel`**, which stops on a denormal selector under non-IEEE mode.
- **Difftest reference model for paired singles** (`tools/difftest/paired.py`), independent of the lifted code's helpers: emulates paired-single instructions and GQR moves in a code hook and keeps ps1. Trials get random ps1 values and GQRs; ps1 is compared bit for bit.
- **Tests** for GQR decoding, quantisation, `psq_l`/`psq_st` and the lifter's paired-single output.

### Changed
- **Difftest hooks:** one code hook per text section, filtered in the driver, where there was one per instruction of interest. Unicorn scans its hooks linearly; trials are now faster than before slice 4 (19 ms on one core).
- **Time-base and special-register reads are found dynamically**, in the trial that makes them, replacing a static call-tree analysis that couldn't see calls through function tables and excused whole call trees.

## [0.7.0] - 2026-11-18
Lifter slice 3, floating point: 7,549 of the game's 8,171 functions lift (1,175 more), in the non-IEEE mode the game sets, and every one runs against the original code: 60,392 trials, 0 unexplained mismatches.

### Added
- **Lifter, slice 3:** FP loads and stores, single- and double-precision add, subtract, multiply and divide, fused multiply-adds (`std::fma`), `frsp`, `fctiwz`, moves, sign operations and compares. Through runtime helpers: non-IEEE mode (FPSCR[NI], which `OSInit` sets), with denormal operands and results flushed to zero of their sign; PowerPC's NaN rules (operand order, the positive default QNaN); the architecture's bit conversions for `lfs` and `stfs`; `fctiwz` saturation and the 750 family's high word.
- **Fail loudly on unsourced Broadway behaviour:** a single-precision multiply whose frC isn't a single, and a compare of a denormal, stop (`rt::fp_unknown`). FPSCR access (only in replaced code), FP record forms and `fres`/`frsqrte` aren't lifted, and the reason is reported.
- **Differential test with FP state:** random FPRs (singles, specials, NaN payloads, denormals) compared bit for bit; MSR[FP] set in Unicorn; hooks correct Unicorn's `fctiwz` (high word, positive overflow); the harness counts non-IEEE flushes, a known difference since Unicorn ignores NI.
- **Tests** for the FP helpers (non-IEEE flushing, NaN selection, fused rounding, `fctiwz`, `lfs`/`stfs` round trips) and the lifter's FP output.

### Changed
- The harness checks a top-level function's return as a lifted caller does: a return anywhere but the caller (a stack smashed by random input) is a bad return, a known difference where Unicorn jumps there.

## [0.6.0] - 2026-11-18
A faster loop: the differential test runs in parallel on Linux, macOS and Windows, a full run at 8 trials per function takes about 2 minutes on 16 cores (v0.5.0 took about 45 minutes at 4 trials on two cores), and `qilin lift` rewrites only what changed, so rebuilds are incremental. 50,992 trials, 0 unexplained mismatches.

### Added
- **Parallel differential test:** `--jobs` workers (default: one per CPU), each with its own Unicorn and `qilin_difftest`. Trials are seeded per function (`--seed` and its address), so results don't depend on scheduling, and `--only` (now a comma-separated list) or the new `--range LO-HI` replays exactly the trials of a full run.
- **The differential test on Windows:** a reader thread replaces `select` for the harness's time-out; the harness reserves the guest space but commits only MEM1, and sets binary mode on its pipes.

### Changed
- **`qilin lift`:** files of 256 consecutive functions counted over every function, lifted or not (35 files, was 25 of 300 lifted functions), so boundaries don't move when more functions lift; a file is written only when its content changes, and stale chunks are removed.
- **Generated code** declares each local separately: MSVC applies `[[maybe_unused]]` only to a declaration's first declarator. It now builds with MSVC `/W4 /WX`, less C4702 (unreachable code after inlining a callee that always stops), which is disabled for generated code.
- **Faster trials** (39 ms to 19 ms on one core): Unicorn's MEM1 is a buffer of the driver's, compared with `memcmp`; neither side copies all of MEM1 per trial; the random heap is drawn in bulk. The trials themselves changed with this, so counts differ from v0.5.0's.
- The default is 8 trials per function, and the results are 50,992 trials.

### Fixed
- **Difftest:** the entry stub at `0x80002F00` was written only into Unicorn's memory, so a function reading memory down to it saw different bytes on each side (a false mismatch, `801E9140`). It is now part of the starting state on both sides.

## [0.5.0] - 2026-11-18
The lifter's first slice and the differential test: 6,374 of the game's functions become C++, and every one runs against the original code under Unicorn: 25,496 trials, 0 unexplained mismatches.

### Added
- **Lifter, slice 1** (`recomp/src/lifter.*`): integer arithmetic with carry and overflow, compares, logical, rotate and shift, every integer load and store, the condition-register and XER instructions, traps, `dcbz`, and all control flow: labels and `goto`, jump tables as a `switch`, direct calls with a check of the return address, tail calls, fall-through into the next function, indirect calls through the function table. Registers live in host locals and are written back only around calls and returns. Floating point, paired singles and privileged instructions stop the lifter with the reason.
- **Runtime** (`recomp/runtime/`): `guest.hpp` (the context, big-endian memory, checked in bring-up builds, and the arithmetic and flag helpers, with Broadway's results for division by zero), `dispatch.cpp` (the function table, special registers, the time base).
- **`qilin lift`:** C++ in 300-function files, stubs that stop for what isn't lifted, the function table, and `lift_report.txt` with the reasons.
- **Differential test** (`tools/difftest/`): lifted functions against the original code under Unicorn's PowerPC 750CL model, from random states, comparing registers, memory and how each stopped; works around four Unicorn problems (XER flags, stale translations, a write hook that misses `dcbz`, `stmw` and `stswi`, `dcbz` fault addresses).
- **Tests:** the lifter's output for the synthetic program, the runtime helpers.

### Fixed
- `ori`, `oris`, `xori` and `xoris` with an odd immediate no longer update CR0 (found by the differential test).

## [0.4.0] - 2026-11-18
The decomp's names on the PAL build, and the library boundaries: 8,043 of the decomp's 8,103 functions match ours exactly, and every library has a PAL range and a plan (lift, replace or drop).

### Added
- **`third_party/rhf`:** the Rhythm Heaven Fever decomp's `symbols.txt` and `splits.txt` (US build, CC0).
- **`tools/analysis/port_symbols.py`:** ports the names to the PAL build by aligning function size sequences, classifies every difference (split, merge, regional change, one side only), and prints each library's PAL range. `--symbols-out` writes a PAL symbol list (derived from the game: not committed).
- **`docs/code-analysis.md`:** the 44 library ranges of the PAL build with function, instruction, floating-point, paired-single and privileged counts, and the lifted/replaced/dropped boundary: 74% of the code lifted, 25% replaced, 1% dropped. Every privileged instruction and every unknown word is on the replaced or dropped side.
- **Tests** for the new CFG rules.

### Fixed
- **Function boundaries, checked against the decomp's:** the `blr` CodeWarrior keeps after a final tail call or trap, and the epilogue after a final call through `bctr`, belong to the function (dead code), not to a new one; a branch to a stack-frame set-up is a tail call to a new function, even inside the branching function's range. 8,301 functions became 8,171, and exact matches with the decomp rose from 7,849 to 8,043.

## [0.3.0] - 2026-11-18
Function discovery and control flow, and the first analysis of the PAL `main.dol`: the build hash confirmed, the decoder agreeing with binutils on every word of the game's code, 8,301 functions, every jump table proven, and every word of the code accounted for.

### Added
- **Function discovery + CFG** (`recomp/src/cfg.*`): strong seeds (entry, `bl` targets), recursive descent bounded by the next start, tail calls, `lis`/`addi` pointers, data pointers outside jump tables, gaps; CodeWarrior jump tables proven by their bound check, including bases held in a data-pool register and bounds checked with a conditional return; functions that fall into the next (register save/restore chains); the get-PC idiom; validation of weak seeds. Details in `docs/recompiler-core.md`.
- **Coverage accounting:** every word of the text is code, padding (zero or `nop`), dead (unreachable code the compiler left inside a function, with nothing pointing at it) or unknown.
- **`qilin cfg`, `coverage`, `suspects`.**
- **Tests:** a synthetic program with a function found each way, a switch, a save chain, the get-PC idiom, an indirect tail call, padding and dead code, and a rejected weak seed.
- **`docs/dol-analysis.md`:** the PAL build's sections (named from `__init_data`'s own tables), instruction profile, decoder cross-check, functions and coverage; the 102 unknown words identified (data in `.init`, an SDK template, the debugger stub). The 0.1 phase's observation about `__init_data`'s tables is confirmed.

### Changed
- The supported build's hash is confirmed on the user's copy of `SOMP01` revision 0.

### Removed
- `COPYING` (GPLv3). It was added in the 0.1 phase because that code patched GPL-licensed projects; nothing in the repository derives from them now.

## [0.2.0] - 2026-11-18
A restart on GBTG-Qilincomp's architecture and requirements: our own PowerPC → C++23 recompiler, the system libraries replaced natively, Vulkan 1.3 only. The first pieces are the DOL loader and a Broadway decoder checked against GNU binutils.

### Added
- **The design:** `docs/roadmap-and-design-targets.md` (the Ghoulies targets applied to the Wii: register model, endianness, memory map and mirrors, the system-library boundary, GX and AX, hardware scope, Vulkan baseline, roadmap), `docs/prior-art.md` (the 0.1 phase, DolRecomp/ModernGekko, the Rhythm Heaven Fever decomp, other Wii and PowerPC recompilations, Dolphin, binutils), `docs/naming.md`, `docs/dol-analysis.md`, `docs/recompiler-core.md`.
- **DOL loader** (`recomp/src/dol.*`): sections, BSS (with the small-data sections inside it), entry point, a flat guest image; `std::expected` errors for anything malformed. The supported build is recognised by the SHA-256 of `main.dol` (our own SHA-256, `recomp/src/sha256.*`).
- **Broadway decoder** (`recomp/src/decoder.*`): all 222 instructions of the 750CL with the paired singles, table-driven, with operand fields and flags for the analysis passes and the lifter. Reserved bits set, invalid forms and instructions the 750CL lacks decode as invalid. Text in objdump's `-M broadway,raw` form.
- **`qilin` CLI:** `info`, `insns`, `disasm`, `profile`, `--version`.
- **Decoder cross-check** (`tools/check/verify_decoder.py`) against GNU objdump: every word of the game's text sections, or a sweep of the encoding space with every single-bit neighbour of each valid word. Over ~38 million words (8 seeds), 0 unexplained disagreements; the reviewed differences are listed in the script and in `docs/recompiler-core.md`.
- **Tests** (`qilin_tests`, no game files): SHA-256 vectors, a synthetic DOL and nine malformed ones, 63 encodings pinned to objdump's text, 18 words that must not decode, and for each of the 222 instructions a canonical word plus every reserved bit set.
- **CI** with warnings as errors on Linux x64 (GCC 14, Clang 19), Linux ARM64 (GCC 14), macOS ARM64 and x64, Windows x64 (MSVC), plus the decoder sweep.

### Changed
- **Scope** is GBTG-Qilincomp's: 64-bit Windows, Linux and macOS on x86-64 or ARM64, and ARM boards with Vulkan 1.3. iPadOS is no longer a target.
- **Version** lives in `CMakeLists.txt` (the `VERSION` file is gone).

### Removed
- **The 0.1 phase's code:** the Windows build script and launcher, the ModernGekko and RecompCore patches, and the native boot probe (`native/`). They depended on DolRecomp (generated C, an interpreter fallback) and on a Dolphin-based runtime. The `native-boot` branch was merged into `main` first, so its history is kept; `v0.1.0-alpha.1` is commit `751b042`.
- The 0.1 phase's documents (`BUILD.md`, `NATIVE-PORT.md`, `PROJECT-STATUS.md`, `PROVENANCE.md`, `VALIDATION.md`). What still holds is in `docs/prior-art.md` §1 and `docs/dol-analysis.md`.

## 0.1.x native probe (never released; superseded by 0.2.0)

These were the 0.1 phase's "Unreleased" entries, kept as written.

- Added a C++23 startup probe that runs locally compiled PAL `SOMP01` code and stops at the first unsupported operation.
- Lowered `dcbf` and `dcbst` at compile time for the native probe's coherent RAM model.
- Verified that Windows x64 passes the initial cache-flush loop and reaches the SDK's `mfhid0` instruction.
- Replaced the cold-boot data and cache setup with native initialization while retaining the SDK DMA error-handler registration. Verified that the PAL WBFS build reaches the performance monitor setup in `OSInit` on Windows x64.
- Replaced the PAL startup monitor reset with an inactive native profiling state. The Windows x64 build now reaches the HID4 configuration in `OSInit`.

## 0.1.0-alpha.1 — 2026-11-18

- Added the initial build for PAL `SOMP01` revision 0 with a C++23 host and a Windows x64 ahead-of-time module.
- Required Vulkan 1.3 in the renderer and added device enumeration and extension handling for MoltenVK. Added a macOS build option for a supplied MoltenVK dynamic library.
- Verified Windows Vulkan startup, native dispatch, and the A+B transition from the title screen to save-slot selection.
- Recorded the exact source revisions, local build procedure, and the Windows gameplay validation milestone.

