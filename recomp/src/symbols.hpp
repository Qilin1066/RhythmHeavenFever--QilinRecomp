// `qilin symbols`: the decomp's US function names ported to the PAL build,
// the list `qilin lift --runtime --symbols` reads. The C++ form of
// tools/analysis/port_symbols.py (which keeps its analysis: the library
// table, --list), so the players' downloads need no Python: the launcher
// runs it on the player's own main.dol (game.txt's step.1). The two must
// write the same list for the same main.dol (the Python writes CRLF on
// Windows; the text is otherwise identical).
//
// How (port_symbols.py's docstring has the reasoning): functions are matched
// by their size sequence, per section, qilin's (from the CFG) against the
// decomp's, aligned as Python's difflib.SequenceMatcher(None, us, pal,
// autojunk=False) aligns them; gaps are paired by equal totals (split,
// merge, regroup) or one to one (changed); a few names are checked against
// their first instruction (SIGNATURES). The list is derived from the game:
// it is written on the player's computer and never committed.
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "cfg.hpp"
#include "dol.hpp"

namespace qilin::symbols {

// difflib's opcodes: tag 'e' (equal), 'r' (replace), 'd' (delete), 'i'
// (insert), with a[i1:i2] and b[j1:j2].
struct Opcode {
    char tag;
    size_t i1, i2, j1, j2;
    bool operator==(const Opcode&) const = default;
};
// SequenceMatcher(None, a, b, autojunk=False).get_opcodes(), exactly.
std::vector<Opcode> opcodes(std::span<const uint32_t> a, std::span<const uint32_t> b);

struct UsFunction {
    std::string sec;  // ".init" or ".text"
    uint32_t addr = 0, size = 0;
    std::string name, unit;
};
// The decomp's functions (symbols.txt) with their translation units
// (splits.txt), from a folder holding both (third_party/rhf/SOME01, or the
// player kit's copy), sorted by address.
std::expected<std::vector<UsFunction>, std::string> load_decomp(const std::filesystem::path& dir);

struct PalFunction {
    std::string sec;  // ".init", ".text", or "textN" for a third text section on
    uint32_t addr = 0, size = 0;  // size: to the next start, less trailing padding
};
// qilin's functions with their sizes, in address order.
std::expected<std::vector<PalFunction>, std::string> pal_functions(const Dol& dol, const Cfg& cfg);

struct Ported {
    std::string list;                  // the symbol list's text (LF line ends)
    std::vector<std::string> summary;  // counts by kind, names moved by their code
};
std::expected<Ported, std::string> port(const Dol& dol, const std::vector<PalFunction>& pal, const std::vector<UsFunction>& us);

}  // namespace qilin::symbols
