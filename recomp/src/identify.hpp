// `qilin identify <extracted disc folder> --json [--build <title>]`: what the
// player's game folder is, for the launcher (tools/player-kit/launcher), as
// the contract every project's recompiler keeps (ADOPTING.md there). One
// JSON object on standard output:
//
//   {"ok": true|false, "title": "...", "id": "...", "version": "...", "message": "..."}
//
// and the exit code: 0 when it is the supported build (`--build`'s, or any
// this port knows without it), 3 when it can be read but isn't (the message
// names what it is and what's needed), 2 when it can't be read at all. The
// message is a plain sentence for the player.
//
// Here the input is the folder Dolphin extracts a Wii disc into: sys/main.dol
// (whose SHA-256 names the build, dol.cpp's known builds), sys/boot.bin (the
// disc ID and the game's name), disc/header.bin (the disc's revision: the
// partition's own boot.bin says 0 on revision 1 discs), sys/fst.bin and
// files/ (what the game reads). Nothing is written.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "dol.hpp"

namespace qilin {

struct Identity {
    int code = 2;  // 0, 3 or 2, as above
    bool ok = false;
    std::string title, id, version, message;
};

// A disc header's first bytes (sys/boot.bin, disc/header.bin): "SOMP01", the
// revision (byte 7), the game's name (from 0x20).
struct DiscHeader {
    std::string id;
    unsigned revision = 0;
    std::string title;
};
std::optional<DiscHeader> read_disc_header(const std::filesystem::path& file);

// `want` is a known build's title ("Beat the Beat: Rhythm Paradise (Europe, Australia)"), or
// empty for any; `builds` the builds this port knows (the tests give their own).
Identity identify(const std::filesystem::path& input, std::string_view want, std::span<const KnownBuild> builds);
inline Identity identify(const std::filesystem::path& input, std::string_view want) { return identify(input, want, known_builds()); }

std::string to_json(const Identity& identity);

}  // namespace qilin
