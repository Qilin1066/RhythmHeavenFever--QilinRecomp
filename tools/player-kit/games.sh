# The game's details for the packaging scripts (sourced), as Toy Commander's
# and Punch-Out's games.sh. Everything game-specific in a download comes from
# here: the README templates' @NAME@ places (fill) and kit/game.txt
# (write_game_txt), which is all the launcher knows about the game
# (tools/player-kit/launcher/ADOPTING.md).
#
# `game` is the per-user data folder's name, qilin/beatthebeat, which the
# game itself has used since v0.16.0 (settings.cpp's game_data_dir): the
# launcher's remembered folder and build cache sit beside the player's saves.
game=beatthebeat
REPO="Qilin1066/RhythmHeavenFever--QilinRecomp"
TITLE="Beat the Beat"; PACKAGE="RhythmHeavenFever--QilinRecomp"; EXE="BeatTheBeat.exe"; BOOT="main.dol"
BUILD_TITLE="Beat the Beat: Rhythm Paradise (Europe, Australia)"; PRODUCT="SOMP01"; DISC_VERSION="0"
RELEASE="the Europe/Australia release, disc SOMP01, revision 0"
STUDIO="Nintendo, Wii, 2012"
# The game's full name, for the READMEs' prose; TITLE ("Play Beat the Beat",
# the app, the menu entry) is short and holds nothing a file name can't.
FULL_TITLE="Beat the Beat: Rhythm Paradise"
# Fills a template's @NAME@ places with the game's details.
fill() {
    sed -e "s|@GAME@|$game|g" -e "s|@FULL_TITLE@|$FULL_TITLE|g" -e "s|@TITLE@|$TITLE|g" -e "s|@PACKAGE@|$PACKAGE|g" -e "s|@EXE@|$EXE|g" \
        -e "s|@BOOT@|$BOOT|g" -e "s|@BUILD_TITLE@|$BUILD_TITLE|g" -e "s|@RELEASE@|$RELEASE|g" -e "s|@STUDIO@|$STUDIO|g" \
        -e "s|@REPO@|$REPO|g" "$1"
}
# Writes kit/game.txt, one name=value a line:
#   write_game_txt <file> <exe> <compiler> [<compiler_args>]
# with Windows line ends where GAME_TXT_CRLF=1. Every key is written, the
# launcher's defaults too, so the file says the whole story: the game's
# names, the data it builds from (an extracted disc's folder; `build` is
# recomp/src/dol.cpp's known build's title, which `qilin identify --build`
# checks), how it is recompiled, what a compile needs, how the game starts
# (BeatTheBeat <folder>, qilin_run's command line), and where releases are
# published (tags vX.Y.Z).
#
# The steps, as CLAUDE.md's "The runtime" builds qilin_run:
#   1. qilin symbols: the decomp's names (kit/rhf, CC0) ported to the
#      player's own main.dol, into work/pal_symbols.txt (derived from the
#      game, so made there and never shipped; port_symbols.py in C++);
#   2. qilin lift --runtime: main.dol to C++ in work/gen, the replaced
#      libraries calling the kit's native code by those names.
# Both write the same text every time for the same main.dol, so a second
# build recompiles nothing (qilin leaves unchanged files alone).
#
# compile_memory_mb: measured, not guessed. The largest of the 37 files
# qilin lift --runtime writes from the supported main.dol (lifted_003.cpp,
# 1.9 MB of C++) peaked at 420 MB (zig and its clang together) compiled with
# the kit's cxxflags and -march=native by zig 0.17.1's clang 22 on Windows
# x86-64 (2026-11-28; the next largest, also 1.9 MB, at 403 MB). 800 leaves
# about 90% for the other compilers (llvm-mingw's, Apple's), ARM64 and a
# larger file after a lifter change. The whole lift's output (gen/) is 32 MB.
#
# old_files: none. These downloads are the project's first; before them
# install.cmd built the game into %LOCALAPPDATA%\Programs (with its own
# uninstaller), never into a download folder, so nothing of it is here.
write_game_txt() {
    {
        printf 'game=%s\ntitle=%s\nexe=%s\nboot=%s\n' "$game" "$TITLE" "$2" "$BOOT"
        printf 'build=%s\nproduct=%s\ndisc_version=%s\nrelease=%s\n' "$BUILD_TITLE" "$PRODUCT" "$DISC_VERSION" "$RELEASE"
        printf 'package=%s\nrepo=%s\nrelease_tag_prefix=v\nrelease_asset_prefix=%s-\n' "$PACKAGE" "$REPO" "$PACKAGE"
        printf 'input=folder\ninput_noun=extracted disc\n'
        printf 'input_hint=Choose the folder you extracted the disc into with Dolphin (right-click the game, Properties, Filesystem tab, right-click the disc, Extract Entire Disc): the one holding sys and files, or the DATA folder inside it.\n'
        printf 'identify={qilin} identify {input} --json --build {game:build}\n'
        printf 'step.1={qilin} symbols {input}/sys/main.dol --decomp {kit}/rhf -o {work}/pal_symbols.txt\n'
        printf 'step.2={qilin} lift {input}/sys/main.dol --out {gen} --runtime --symbols {work}/pal_symbols.txt\n'
        printf 'compile_memory_mb=800\nbuild_space_mb=2048\ncache_mb=2048\nrun_args={input}\n'
        printf '# Nothing from before the launcher is in a download folder (install.cmd installed elsewhere).\n'
        printf 'old_files=\nold_input_file=\n'
        printf 'compiler=%s\n' "$3"
        if [ -n "${4:-}" ]; then printf 'compiler_args=%s\n' "$4"; fi
    } | if [ "${GAME_TXT_CRLF:-0}" = 1 ]; then sed 's/$/\r/'; else cat; fi > "$1"
}
