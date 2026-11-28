#include "dol.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

#include "path.hpp"
#include "sha256.hpp"

namespace qilin {

namespace {

constexpr KnownBuild kKnownBuilds[] = {
    // Europe/Australia, disc revision 0. Hash recorded in the 0.1 phase;
    // docs/dol-analysis.md has what was checked.
    {"2e0368359106a2eef773c70ef083e89aafe1e49603eefc7f16b9ed414cd098fe", "SOMP01", 0,
     "Beat the Beat: Rhythm Paradise (Europe, Australia)"},
};

constexpr size_t kHeaderSize = 0x100;
constexpr unsigned kTextSlots = 7, kDataSlots = 11;

std::string hex32(uint32_t v) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string s = "0x";
    for (int shift = 28; shift >= 0; shift -= 4) s += kHex[(v >> shift) & 15];
    return s;
}

std::unexpected<std::string> fail(std::string message) { return std::unexpected(std::move(message)); }

}  // namespace

std::string DolSection::name() const { return (text ? "text" : "data") + std::to_string(index); }

std::span<const KnownBuild> known_builds() { return kKnownBuilds; }

const KnownBuild* known_build(std::string_view sha256_hex) {
    for (const KnownBuild& b : kKnownBuilds)
        if (b.sha256 == sha256_hex) return &b;
    return nullptr;
}

std::expected<Dol, std::string> Dol::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open " + utf8_path(path));
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (f.bad()) return fail("cannot read " + utf8_path(path));
    return parse(bytes);
}

std::expected<Dol, std::string> Dol::parse(std::span<const uint8_t> file) {
    if (file.size() < kHeaderSize) return fail("not a DOL: shorter than its 256-byte header");
    if (file.size() > kMem1Size + kHeaderSize) return fail("not a DOL: larger than the Wii's MEM1");

    Dol dol;
    dol.file_size = static_cast<uint32_t>(file.size());
    const uint8_t* h = file.data();
    for (unsigned slot = 0; slot < kTextSlots + kDataSlots; ++slot) {
        DolSection s;
        s.text = slot < kTextSlots;
        s.index = s.text ? slot : slot - kTextSlots;
        s.offset = read_be32(h + 4 * slot);
        s.address = read_be32(h + 0x48 + 4 * slot);
        s.size = read_be32(h + 0x90 + 4 * slot);
        if (s.size == 0) continue;
        const std::string what = s.name() + " (" + hex32(s.address) + ", " + std::to_string(s.size) + " bytes)";
        if (s.offset < kHeaderSize || uint64_t(s.offset) + s.size > file.size())
            return fail(what + ": its bytes lie outside the file");
        if (s.address < kMem1Base || uint64_t(s.address) + s.size > uint64_t(kMem1Base) + kMem1Size)
            return fail(what + ": loads outside MEM1");
        if (s.text && (s.address % 4 || s.size % 4)) return fail(what + ": code is not word-aligned");
        dol.sections.push_back(s);
    }
    if (dol.sections.empty()) return fail("not a DOL: no sections");
    std::ranges::sort(dol.sections, {}, &DolSection::address);
    for (size_t i = 1; i < dol.sections.size(); ++i)
        if (dol.sections[i].address < dol.sections[i - 1].end())
            return fail(dol.sections[i - 1].name() + " and " + dol.sections[i].name() + " overlap in memory");

    dol.bss_address = read_be32(h + 0xD8);
    dol.bss_size = read_be32(h + 0xDC);
    dol.entry = read_be32(h + 0xE0);
    if (dol.bss_size && (dol.bss_address < kMem1Base ||
                         uint64_t(dol.bss_address) + dol.bss_size > uint64_t(kMem1Base) + kMem1Size))
        return fail("BSS (" + hex32(dol.bss_address) + ", " + std::to_string(dol.bss_size) + " bytes) lies outside MEM1");

    uint32_t lo = dol.sections.front().address, hi = 0;
    for (const DolSection& s : dol.sections) hi = std::max(hi, s.end());
    if (dol.bss_size) {
        lo = std::min(lo, dol.bss_address);
        hi = std::max(hi, dol.bss_address + dol.bss_size);
    }
    dol.base_ = lo;
    dol.image_.assign(hi - lo, 0);
    for (const DolSection& s : dol.sections)
        std::copy_n(file.begin() + s.offset, s.size, dol.image_.begin() + (s.address - lo));

    const DolSection* entry = dol.text_section_at(dol.entry);
    if (!entry || dol.entry % 4) return fail("the entry point " + hex32(dol.entry) + " is not an instruction in a text section");

    dol.sha256 = sha256_hex(file);
    return dol;
}

uint8_t Dol::u8(uint32_t a) const {
    if (!mapped(a)) throw DolError("read outside the DOL image at " + hex32(a));
    return image_[a - base_];
}

uint32_t Dol::u32(uint32_t a) const {
    if (!mapped(a, 4)) throw DolError("read outside the DOL image at " + hex32(a));
    return read_be32(image_.data() + (a - base_));
}

const DolSection* Dol::section_at(uint32_t a) const {
    // Sorted and disjoint: the last section starting at or below a.
    auto it = std::ranges::upper_bound(sections, a, {}, &DolSection::address);
    if (it == sections.begin()) return nullptr;
    --it;
    return it->contains(a) ? &*it : nullptr;
}

const DolSection* Dol::text_section_at(uint32_t a) const {
    const DolSection* s = section_at(a);
    return s && s->text ? s : nullptr;
}

}  // namespace qilin
