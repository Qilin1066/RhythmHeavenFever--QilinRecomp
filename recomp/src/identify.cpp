// `qilin identify` (identify.hpp). Developer tool code: std::format is fine here.
#include "identify.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <iterator>
#include <system_error>
#include <vector>

namespace qilin {

namespace fs = std::filesystem;

namespace {

// A file's bytes, at most `limit` of them; nothing if it can't be read.
std::optional<std::vector<uint8_t>> read_bytes(const fs::path& file, size_t limit) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return std::nullopt;
    std::ifstream f(file, std::ios::binary);
    if (!f) return std::nullopt;
    std::vector<uint8_t> bytes;
    bytes.reserve(std::min<size_t>(limit, 1u << 20));
    for (std::istreambuf_iterator<char> it(f), end; it != end && bytes.size() < limit; ++it)
        bytes.push_back(static_cast<uint8_t>(*it));
    if (f.bad()) return std::nullopt;
    return bytes;
}

std::string shown(const fs::path& p) {
    const std::u8string text = p.u8string();
    return {text.begin(), text.end()};
}

// "Beat the Beat: Rhythm Paradise (Europe, Australia), disc SOMP01 revision 0"
std::string describe(const KnownBuild& b) {
    return std::format("{}, disc {} revision {}", b.title, b.disc_id, b.revision);
}

// The region a disc ID's fourth letter names.
std::string_view region_name(char c) {
    switch (c) {
    case 'E': return "North America";
    case 'P': return "Europe and Australia";
    case 'J': return "Japan";
    case 'K': return "Korea";
    case 'W': return "Taiwan and Hong Kong";
    case 'D': return "Germany";
    case 'F': return "France";
    case 'S': return "Spain";
    case 'I': return "Italy";
    case 'U': return "Australia";
    case 'X': case 'Y': case 'Z': return "Europe";
    default: return "";
    }
}

Identity answer(int code, std::string message) {
    Identity i;
    i.code = code;
    i.ok = code == 0;
    i.message = std::move(message);
    return i;
}

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20) {
            out += std::format("\\u{:04x}", u);
        } else {
            out += c;  // UTF-8 as it is
        }
    }
    return out + "\"";
}

}  // namespace

std::optional<DiscHeader> read_disc_header(const fs::path& file) {
    const auto bytes = read_bytes(file, 0x60);
    if (!bytes || bytes->size() < 0x60) return std::nullopt;
    DiscHeader h;
    for (size_t i = 0; i < 6; ++i) {
        const char c = static_cast<char>((*bytes)[i]);
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) return std::nullopt;  // not a disc ID
        h.id += c;
    }
    h.revision = (*bytes)[7];
    for (size_t i = 0x20; i < 0x60 && (*bytes)[i]; ++i) {
        const auto c = static_cast<char>((*bytes)[i]);
        h.title += c >= 0x20 && c < 0x7F ? c : '?';
    }
    while (!h.title.empty() && h.title.back() == ' ') h.title.pop_back();
    return h;
}

Identity identify(const fs::path& input, std::string_view want, std::span<const KnownBuild> builds) {
    const KnownBuild* wanted = nullptr;
    if (!want.empty()) {
        for (const KnownBuild& b : builds)
            if (b.title == want) wanted = &b;
        if (!wanted) return answer(2, std::format("This recompiler knows no build called {}.", want));
    } else if (builds.size() == 1) {
        wanted = &builds.front();
    }
    const std::string for_text = wanted ? " This download is for " + describe(*wanted) + "." : std::string();
    const std::string extract =
        " In Dolphin, right-click the game, choose Properties, and on the Filesystem tab right-click the disc and choose Extract Entire Disc.";
    std::error_code ec;
    if (!fs::exists(input, ec)) return answer(2, "Can't find " + shown(input) + ".");
    if (!fs::is_directory(input, ec)) {
        std::string name = shown(input.filename()), ext = shown(input.extension());
        std::ranges::transform(name, name.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        std::ranges::transform(ext, ext.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
        if (name == "main.dol")
            return answer(2, "This is main.dol on its own: choose the folder you extracted the whole disc into, the one holding sys and files.");
        for (const std::string_view image : {".iso", ".wbfs", ".rvz", ".wia", ".gcz", ".ciso", ".gcm", ".nkit"})
            if (ext == image) return answer(2, "This is a disc image: the game is built from the disc extracted into a folder." + extract);
        return answer(2, "This is a file, not the folder of an extracted disc." + extract);
    }

    // The folder: the one holding sys/ and files/ (a step too deep, sys/
    // itself, or one too high, Dolphin's partitions, are named as such).
    const fs::path sys = input / "sys", dol_path = sys / "main.dol";
    if (!fs::is_regular_file(dol_path, ec)) {
        if (fs::is_regular_file(input / "main.dol", ec) && fs::is_regular_file(input / "boot.bin", ec))
            return answer(2, "This is the disc's sys folder: choose the folder above it, the one holding sys and files.");
        if (fs::is_regular_file(input / "DATA" / "sys" / "main.dol", ec))
            return answer(2, "This folder holds the disc's partitions: choose the DATA folder inside it.");
        return answer(2, "There's no sys/main.dol in this folder: choose the folder you extracted the whole disc into." + extract);
    }
    const auto dol_bytes = read_bytes(dol_path, size_t(kMem1Size) + 0x101);
    if (!dol_bytes) return answer(2, "Can't read sys/main.dol in this folder.");
    const auto dol = Dol::parse(*dol_bytes);
    if (!dol) return answer(2, "Can't read sys/main.dol in this folder: " + dol.error() + ". Extract the disc again.");

    // What the disc says it is: its ID and name from sys/boot.bin, its
    // revision from disc/header.bin (Dolphin writes both).
    const std::optional<DiscHeader> boot = read_disc_header(sys / "boot.bin");
    const std::optional<DiscHeader> disc = read_disc_header(input / "disc" / "header.bin");
    const std::optional<DiscHeader>& says = boot ? boot : disc;
    const std::optional<unsigned> revision = disc ? std::optional(disc->revision) : std::nullopt;

    const KnownBuild* build = nullptr;
    for (const KnownBuild& b : builds)
        if (b.sha256 == dol->sha256) build = &b;
    if (!build) {
        Identity i = answer(3, "");
        if (says) {
            i.title = says->title;
            i.id = says->id;
        }
        if (revision) i.version = std::to_string(*revision);
        if (!says) {
            i.message = "This folder's main.dol isn't a version this download knows (SHA-256 " + dol->sha256 + ")." + for_text;
        } else if (wanted && says->id == wanted->disc_id) {
            if (revision && *revision != wanted->revision)
                i.message = std::format("This is {}, disc {} revision {}. This download is for revision {} of that disc.", wanted->title,
                                        says->id, *revision, wanted->revision);
            else
                i.message = std::format("This is disc {}, but its main.dol isn't revision {}'s (SHA-256 {}): it may be another revision, or "
                                        "changed or damaged. If your disc is revision {}, extract it again.",
                                        says->id, wanted->revision, dol->sha256, wanted->revision);
        } else if (wanted && says->id.substr(0, 3) == std::string_view(wanted->disc_id).substr(0, 3)) {
            const std::string_view region = region_name(says->id[3]);
            // The game's release for another region: another program (the
            // North American one is Rhythm Heaven Fever, SOME01).
            i.message = std::format("This is the game's release for {} ({}).{}", region.empty() ? "another region" : region, says->id,
                                    for_text);
        } else {
            i.message = std::format("This is {} ({}), another game.{}", says->title.empty() ? "a disc" : says->title, says->id,
                                    for_text);
        }
        return i;
    }

    Identity i = answer(0, "");
    i.title = std::string(build->title);
    i.id = std::string(build->disc_id);
    i.version = std::to_string(build->revision);
    if (wanted && wanted != build) {
        i.code = 3;
        i.ok = false;
        i.message = "This is " + describe(*build) + "." + for_text;
        return i;
    }
    // The right main.dol: the game reads the rest of the disc from the folder too.
    if (!fs::is_regular_file(sys / "fst.bin", ec) || !fs::is_regular_file(sys / "boot.bin", ec) || !fs::is_directory(input / "files", ec)) {
        i.code = 2;
        i.ok = false;
        i.message = "This folder has the right main.dol but not the rest of the disc (sys/boot.bin, sys/fst.bin and files): "
                    "extract the whole disc, not only main.dol." + extract;
        return i;
    }
    i.message = "This is " + describe(*build) + ": the right disc.";
    return i;
}

std::string to_json(const Identity& i) {
    return std::string("{\"ok\": ") + (i.ok ? "true" : "false") + ", \"title\": " + json_string(i.title) + ", \"id\": " + json_string(i.id) +
           ", \"version\": " + json_string(i.version) + ", \"message\": " + json_string(i.message) + "}\n";
}

}  // namespace qilin
