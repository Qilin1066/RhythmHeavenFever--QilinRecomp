// DOL (GameCube/Wii executable) loader.
//
// A DOL is a 0x100-byte header followed by the bytes of up to 7 text and 11
// data sections, each loaded at a fixed address. The header's BSS range
// usually spans the small-data sections too (.sdata, .sdata2 sit inside it):
// bytes a section provides come from the section, the rest of BSS is zero.
//
// The loader builds a flat guest image from the lowest section address to the
// end of the highest section or BSS, which the analysis passes and the
// runtime read from. Runtime code: no std::format (see CMakeLists.txt).
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace qilin {

// Thrown only for programming errors: reading outside the image.
struct DolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The Wii's first 24 MB of RAM (MEM1), cached view: where a DOL loads.
inline constexpr uint32_t kMem1Base = 0x80000000u;
inline constexpr uint32_t kMem1Size = 24u << 20;

struct DolSection {
    bool text = false;   // code (as opposed to data)
    unsigned index = 0;  // header slot: text 0-6, data 0-10
    uint32_t offset = 0; // file offset
    uint32_t address = 0;
    uint32_t size = 0;

    uint32_t end() const { return address + size; }
    bool contains(uint32_t a) const { return a >= address && a - address < size; }
    std::string name() const;  // "text1", "data4": the header slot
};

// Executables this port supports, recognised by the SHA-256 of main.dol.
struct KnownBuild {
    std::string_view sha256;
    std::string_view disc_id;
    unsigned revision;
    std::string_view title;
};
const KnownBuild* known_build(std::string_view sha256_hex);
std::span<const KnownBuild> known_builds();

class Dol {
public:
    // Errors (unreadable file, malformed header, sections outside MEM1 or
    // overlapping, ...) come back as a message, never a crash.
    static std::expected<Dol, std::string> load(const std::filesystem::path& path);
    static std::expected<Dol, std::string> parse(std::span<const uint8_t> file);

    std::vector<DolSection> sections;  // non-empty ones, sorted by address
    uint32_t bss_address = 0, bss_size = 0;
    uint32_t entry = 0;
    uint32_t file_size = 0;
    std::string sha256;  // of the whole file, lower-case hex

    // Flat guest image: image()[a - base()].
    uint32_t base() const { return base_; }
    std::span<const uint8_t> image() const { return image_; }
    uint32_t image_end() const { return base_ + static_cast<uint32_t>(image_.size()); }
    bool mapped(uint32_t a, uint32_t n = 1) const {
        return a >= base_ && uint64_t(a) + n <= image_end();
    }
    uint8_t u8(uint32_t a) const;
    uint32_t u32(uint32_t a) const;  // big-endian

    const DolSection* section_at(uint32_t a) const;
    const DolSection* text_section_at(uint32_t a) const;

private:
    uint32_t base_ = 0;
    std::vector<uint8_t> image_;
};

// `qilin info` output (in qilin_core: developer tools only).
std::string to_json(const Dol& dol);

inline uint32_t read_be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

}  // namespace qilin
