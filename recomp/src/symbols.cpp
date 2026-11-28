// `qilin symbols` (symbols.hpp): tools/analysis/port_symbols.py in C++, step
// for step. Developer tool code: std::format is fine here.
#include "symbols.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <string_view>
#include <tuple>

#include "path.hpp"

namespace qilin::symbols {

namespace {

// ---------------------------------------------------------------- difflib

// CPython's difflib.SequenceMatcher with no junk and autojunk off: b2j holds
// every element of b, so neither the junk nor the popular-element rules apply.
class Matcher {
public:
    Matcher(std::span<const uint32_t> a, std::span<const uint32_t> b) : a_(a), b_(b) {
        for (size_t j = 0; j < b.size(); ++j) b2j_[b[j]].push_back(j);
        for (auto& row : rows_) row.assign(b.size(), {0, 0});
    }

    struct Match {
        size_t i, j, k;
        auto operator<=>(const Match&) const = default;
    };

    // find_longest_match(alo, ahi, blo, bhi): the longest a[i:i+k] ==
    // b[j:j+k] in the ranges; of those, the earliest in a, then in b.
    Match longest(size_t alo, size_t ahi, size_t blo, size_t bhi) {
        size_t besti = alo, bestj = blo, bestsize = 0;
        // j2len: the length of the match ending at a[i-1], b[j], for the j
        // the previous row touched (a stamp says which row wrote it).
        ++call_;
        for (size_t i = alo; i < ahi; ++i) {
            auto& prev = rows_[(i + 1) & 1];
            auto& cur = rows_[i & 1];
            const auto it = b2j_.find(a_[i]);
            if (it == b2j_.end()) continue;
            for (const size_t j : it->second) {
                if (j < blo) continue;
                if (j >= bhi) break;
                size_t k = 1;
                if (j > 0 && i > alo && prev[j - 1].stamp == stamp(i - 1)) k = prev[j - 1].len + 1;
                cur[j] = {k, stamp(i)};
                if (k > bestsize) besti = i - k + 1, bestj = j - k + 1, bestsize = k;
            }
        }
        // Extending by equal non-junk elements on each side (a no-op without
        // popular elements, kept as difflib has it).
        while (besti > alo && bestj > blo && a_[besti - 1] == b_[bestj - 1]) --besti, --bestj, ++bestsize;
        while (besti + bestsize < ahi && bestj + bestsize < bhi && a_[besti + bestsize] == b_[bestj + bestsize]) ++bestsize;
        return {besti, bestj, bestsize};
    }

    // get_matching_blocks(): adjacent blocks collapsed, the (la, lb, 0) sentinel last.
    std::vector<Match> blocks() {
        std::vector<std::tuple<size_t, size_t, size_t, size_t>> queue{{0, a_.size(), 0, b_.size()}};
        std::vector<Match> found;
        while (!queue.empty()) {
            const auto [alo, ahi, blo, bhi] = queue.back();
            queue.pop_back();
            const Match m = longest(alo, ahi, blo, bhi);
            if (!m.k) continue;
            found.push_back(m);
            if (alo < m.i && blo < m.j) queue.emplace_back(alo, m.i, blo, m.j);
            if (m.i + m.k < ahi && m.j + m.k < bhi) queue.emplace_back(m.i + m.k, ahi, m.j + m.k, bhi);
        }
        std::ranges::sort(found);
        std::vector<Match> out;
        size_t i1 = 0, j1 = 0, k1 = 0;
        for (const Match& m : found) {
            if (i1 + k1 == m.i && j1 + k1 == m.j) {
                k1 += m.k;
            } else {
                if (k1) out.push_back({i1, j1, k1});
                i1 = m.i, j1 = m.j, k1 = m.k;
            }
        }
        if (k1) out.push_back({i1, j1, k1});
        out.push_back({a_.size(), b_.size(), 0});
        return out;
    }

private:
    struct Cell {
        size_t len;
        uint64_t stamp;
    };
    uint64_t stamp(size_t i) const { return call_ * (uint64_t(a_.size()) + 1) + i; }

    std::span<const uint32_t> a_, b_;
    std::map<uint32_t, std::vector<size_t>> b2j_;
    std::vector<Cell> rows_[2];
    uint64_t call_ = 0;
};

// ---------------------------------------------------------------- reading

std::optional<uint32_t> hex(std::string_view s) {
    if (s.empty() || s.size() > 8) return std::nullopt;
    uint32_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, 16);
    if (ec != std::errc{} || end != s.data() + s.size()) return std::nullopt;
    return v;
}

size_t hex_run(std::string_view s, size_t at) {
    size_t end = at;
    while (end < s.size() && ((s[end] >= '0' && s[end] <= '9') || (s[end] >= 'a' && s[end] <= 'f') || (s[end] >= 'A' && s[end] <= 'F'))) ++end;
    return end;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

std::string_view strip(std::string_view s) {
    while (!s.empty() && is_space(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_space(s.back())) s.remove_suffix(1);
    return s;
}

// The text after `prefix` at `at`, or npos.
size_t after(std::string_view s, size_t at, std::string_view prefix) {
    return s.substr(at).starts_with(prefix) ? at + prefix.size() : std::string_view::npos;
}

// SYMBOL's regex: ^(\S+) = (\.init|\.text):0x(hex+); // type:function size:0x(hex+)
std::optional<UsFunction> symbol_line(std::string_view line) {
    size_t name_end = 0;
    while (name_end < line.size() && !is_space(line[name_end])) ++name_end;
    if (name_end == 0) return std::nullopt;
    UsFunction f;
    f.name = std::string(line.substr(0, name_end));
    size_t at = after(line, name_end, " = ");
    if (at == std::string_view::npos) return std::nullopt;
    if (line.substr(at).starts_with(".init")) f.sec = ".init";
    else if (line.substr(at).starts_with(".text")) f.sec = ".text";
    else return std::nullopt;
    if ((at = after(line, at + 5, ":0x")) == std::string_view::npos) return std::nullopt;
    size_t end = hex_run(line, at);
    const auto addr = hex(line.substr(at, end - at));
    if (!addr || (at = after(line, end, "; // type:function size:0x")) == std::string_view::npos) return std::nullopt;
    end = hex_run(line, at);
    const auto size = hex(line.substr(at, end - at));
    if (!size) return std::nullopt;
    f.addr = *addr, f.size = *size;
    return f;
}

std::expected<std::vector<std::string>, std::string> read_lines(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return std::unexpected(std::format("can't open {}", utf8_path(file)));
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();  // as Python's text mode reads it
        lines.push_back(std::move(line));
    }
    if (f.bad()) return std::unexpected(std::format("can't read {}", utf8_path(file)));
    return lines;
}

// ---------------------------------------------------------------- signatures

constexpr uint32_t spr(unsigned n) { return ((n & 0x1F) << 5 | n >> 5) << 11; }
constexpr uint32_t kMfspr = 0x7C6002A6, kMtspr = 0x7C6003A6, kBlr = 0x4E800020;

// port_symbols.py's SIGNATURES: functions whose name says what they do,
// checked against their first instruction (PowerPC encodings, r3).
constexpr std::pair<std::string_view, uint32_t> kSignatures[] = {
    {"PPCMfmsr", 0x7C6000A6},
    {"PPCMtmsr", 0x7C600124},
    {"PPCMfhid0", kMfspr | spr(1008)},
    {"PPCMthid0", kMtspr | spr(1008)},
    {"PPCMfl2cr", kMfspr | spr(1017)},
    {"PPCMtl2cr", kMtspr | spr(1017)},
    {"PPCMtdec", kMtspr | spr(22)},
    {"PPCSync", 0x44000002},
    {"PPCMtmmcr0", kMtspr | spr(952)},
    {"PPCMtmmcr1", kMtspr | spr(956)},
    {"PPCMtpmc1", kMtspr | spr(953)},
    {"PPCMtpmc2", kMtspr | spr(954)},
    {"PPCMtpmc3", kMtspr | spr(957)},
    {"PPCMtpmc4", kMtspr | spr(958)},
    {"PPCMfhid2", kMfspr | spr(920)},
    {"PPCMthid2", kMtspr | spr(920)},
    {"PPCMtwpar", kMtspr | spr(921)},
};
constexpr size_t kNear = 16;  // how many functions either side of its aligned place a name may move

std::optional<uint32_t> word(const Dol& dol, uint32_t a) {
    if (!dol.section_at(a) || !dol.mapped(a, 4)) return std::nullopt;
    return dol.u32(a);
}

// One aligned pair: a decomp function (or none) and the PAL functions it is.
struct Pair {
    const UsFunction* us;
    std::vector<const PalFunction*> pal;
    std::string kind;
};

// align() for one section.
void align(std::span<const UsFunction* const> us, std::span<const PalFunction* const> pal, std::vector<Pair>& out) {
    std::vector<uint32_t> U, P;
    for (const UsFunction* f : us) U.push_back(f->size);
    for (const PalFunction* f : pal) P.push_back(f->size);
    auto range = [&](size_t lo, size_t hi) {
        std::vector<const PalFunction*> v;
        for (size_t k = lo; k < hi; ++k) v.push_back(pal[k]);
        return v;
    };
    for (const Opcode& op : opcodes(U, P)) {
        const size_t i1 = op.i1, i2 = op.i2, k1 = op.j1, k2 = op.j2;
        if (op.tag == 'e') {
            const char* kind = i2 - i1 >= 3 ? "anchored" : "single";
            for (size_t n = 0; n < i2 - i1; ++n) out.push_back({us[i1 + n], {pal[k1 + n]}, kind});
            continue;
        }
        // Pair groups with equal totals, walking both sides together.
        size_t i = i1, k = k1;
        while (i < i2 && k < k2) {
            uint64_t su = U[i], sp = P[k];
            size_t ie = i + 1, ke = k + 1;
            while (su != sp && ie <= i2 && ke <= k2) {
                if (su < sp) {
                    if (ie == i2) break;
                    su += U[ie++];
                } else {
                    if (ke == k2) break;
                    sp += P[ke++];
                }
            }
            if (su != sp) break;
            const size_t nu = ie - i, np = ke - k;
            const char* kind = nu == 1 && np > 1 ? "split" : np == 1 && nu > 1 ? "merge" : "regroup";
            if (nu == 1) {
                out.push_back({us[i], range(k, ke), kind});
            } else {
                for (size_t n = 0; n < nu; ++n) out.push_back({us[i + n], n == 0 ? range(k, ke) : std::vector<const PalFunction*>{}, kind});
            }
            i = ie, k = ke;
        }
        if (i2 - i == k2 - k) {
            for (size_t n = 0; n < i2 - i; ++n) out.push_back({us[i + n], {pal[k + n]}, "changed"});
        } else {
            for (size_t a = i; a < i2; ++a) out.push_back({us[a], {}, "us-only"});
            for (size_t b = k; b < k2; ++b) out.push_back({nullptr, {pal[b]}, "pal-only"});
        }
    }
}

}  // namespace

std::vector<Opcode> opcodes(std::span<const uint32_t> a, std::span<const uint32_t> b) {
    Matcher m(a, b);
    std::vector<Opcode> out;
    size_t i = 0, j = 0;
    for (const auto& block : m.blocks()) {
        char tag = 0;
        if (i < block.i && j < block.j) tag = 'r';
        else if (i < block.i) tag = 'd';
        else if (j < block.j) tag = 'i';
        if (tag) out.push_back({tag, i, block.i, j, block.j});
        i = block.i + block.k, j = block.j + block.k;
        if (block.k) out.push_back({'e', block.i, i, block.j, j});
    }
    return out;
}

std::expected<std::vector<UsFunction>, std::string> load_decomp(const std::filesystem::path& dir) {
    const auto symbols = read_lines(dir / "symbols.txt");
    if (!symbols) return std::unexpected(symbols.error());
    const auto splits = read_lines(dir / "splits.txt");
    if (!splits) return std::unexpected(splits.error());
    std::vector<UsFunction> funcs;
    for (const std::string& line : *symbols)
        if (auto f = symbol_line(line)) funcs.push_back(std::move(*f));
    if (funcs.empty()) return std::unexpected(std::format("{}: no functions", utf8_path(dir / "symbols.txt")));
    std::ranges::stable_sort(funcs, {}, &UsFunction::addr);

    // Translation units: "path:" then indented "section start:0x.. end:0x.." lines.
    std::vector<std::tuple<uint32_t, uint32_t, std::string>> units;
    std::string unit;
    for (const std::string& line : *splits) {
        const std::string_view stripped = strip(line);
        if (!line.empty() && !is_space(line[0]) && stripped.ends_with(":") && stripped != "Sections:") {
            unit = std::string(stripped.substr(0, stripped.size() - 1));
            continue;
        }
        // \s+(\.init|\.text)\s+start:0x(hex+) end:0x(hex+)
        size_t at = 0;
        while (at < line.size() && is_space(line[at])) ++at;
        if (at == 0 || unit.empty()) continue;
        const std::string_view rest = std::string_view(line).substr(at);
        if (!rest.starts_with(".init") && !rest.starts_with(".text")) continue;
        at += 5;
        const size_t gap = at;
        while (at < line.size() && is_space(line[at])) ++at;
        if (at == gap || (at = after(line, at, "start:0x")) == std::string_view::npos) continue;
        size_t end = hex_run(line, at);
        const auto lo = hex(std::string_view(line).substr(at, end - at));
        if (!lo || (at = after(line, end, " end:0x")) == std::string_view::npos) continue;
        end = hex_run(line, at);
        const auto hi = hex(std::string_view(line).substr(at, end - at));
        if (!hi) continue;
        units.emplace_back(*lo, *hi, unit);
    }
    std::ranges::sort(units);
    for (UsFunction& f : funcs) {
        f.unit = "(unsplit)";
        for (const auto& [lo, hi, name] : units)
            if (lo <= f.addr && f.addr < hi) {
                f.unit = name;
                break;
            }
    }
    return funcs;
}

std::expected<std::vector<PalFunction>, std::string> pal_functions(const Dol& dol, const Cfg& cfg) {
    // CodeWarrior's layout: the first text section is .init, the second .text.
    std::vector<const DolSection*> text;
    for (const DolSection& s : dol.sections)
        if (s.text) text.push_back(&s);  // sorted by address
    std::vector<uint32_t> starts;
    for (const auto& [start, f] : cfg.functions()) starts.push_back(start);
    std::vector<PalFunction> funcs;
    for (size_t i = 0; i < starts.size(); ++i) {
        const uint32_t s = starts[i];
        const auto sec = std::ranges::find_if(text, [s](const DolSection* t) { return t->contains(s); });
        if (sec == text.end()) return std::unexpected(std::format("{:08X} is not in a text section", s));
        const size_t k = size_t(sec - text.begin());
        const uint32_t sec_end = (*sec)->end();
        uint32_t end = i + 1 < starts.size() && starts[i + 1] < sec_end ? starts[i + 1] : sec_end;
        while (end > s + 4) {  // trailing alignment: zero, nop
            const auto w = word(dol, end - 4);
            if (!w || (*w != 0 && *w != 0x60000000u)) break;
            end -= 4;
        }
        funcs.push_back({k == 0 ? ".init" : k == 1 ? ".text" : std::format("text{}", k), s, end - s});
    }
    return funcs;
}

std::expected<Ported, std::string> port(const Dol& dol, const std::vector<PalFunction>& pal, const std::vector<UsFunction>& us) {
    std::vector<Pair> pairs;
    for (const std::string_view sec : {".init", ".text"}) {
        std::vector<const UsFunction*> U;
        std::vector<const PalFunction*> P;
        for (const UsFunction& f : us)
            if (f.sec == sec) U.push_back(&f);
        for (const PalFunction& f : pal)
            if (f.sec == sec) P.push_back(&f);
        align(U, P, pairs);
    }

    // check_signatures: a name in kSignatures moves to the function near its
    // aligned place whose code matches it; a guess is never made.
    std::map<uint32_t, size_t> index;
    for (size_t k = 0; k < pal.size(); ++k) index[pal[k].addr] = k;
    Ported out;
    for (Pair& p : pairs) {
        if (!p.us || p.pal.empty()) continue;
        const auto sig = std::ranges::find(kSignatures, std::string_view(p.us->name), &std::pair<std::string_view, uint32_t>::first);
        if (sig == std::end(kSignatures)) continue;
        const uint32_t want = sig->second;
        if (word(dol, p.pal[0]->addr) == want) continue;
        const size_t k = index.at(p.pal[0]->addr);
        std::vector<const PalFunction*> found;
        for (size_t n = k >= kNear ? k - kNear : 0; n < std::min(pal.size(), k + kNear + 1); ++n)
            if (pal[n].size == 8 && word(dol, pal[n].addr) == want && word(dol, pal[n].addr + 4) == kBlr) found.push_back(&pal[n]);
        if (found.size() != 1)
            return std::unexpected(std::format("{}: its instruction {:08X} is in {} functions; the name can't be placed", p.us->name, want,
                                               found.size()));
        out.summary.push_back(std::format("moved by its code: {} {:08X} -> {:08X}", p.us->name, p.pal[0]->addr, found[0]->addr));
        p.pal = {found[0]};
        p.kind = "signature";
    }
    // A function now claimed by two names keeps only the one its code matches.
    std::map<uint32_t, size_t> claimed;
    for (const Pair& p : pairs)
        if (p.us && !p.pal.empty()) ++claimed[p.pal[0]->addr];
    for (Pair& p : pairs)
        if (p.us && !p.pal.empty() && claimed[p.pal[0]->addr] > 1 && p.kind != "signature") p.pal.clear(), p.kind = "us-only";

    std::map<std::string, size_t> kinds;
    for (const Pair& p : pairs) ++kinds[p.kind];
    out.summary.insert(out.summary.begin(), std::format("US functions {}, PAL functions {}", us.size(), pal.size()));
    for (const char* kind : {"anchored", "single", "signature", "split", "merge", "regroup", "changed", "us-only", "pal-only"})
        if (kinds[kind]) out.summary.push_back(std::format("{:9} {}", kind, kinds[kind]));

    for (const Pair& p : pairs) {
        if (!p.us) continue;
        for (size_t n = 0; n < p.pal.size(); ++n) {
            const PalFunction& f = *p.pal[n];
            const std::string name = n == 0 ? p.us->name : std::format("{}.part{}", p.us->name, n + 1);
            out.list += std::format("{} = {}:0x{:08X}; // size:0x{:X} us:0x{:08X} match:{} unit:{}\n", name, f.sec, f.addr, f.size,
                                    p.us->addr, p.kind, p.us->unit);
        }
    }
    return out;
}

}  // namespace qilin::symbols
