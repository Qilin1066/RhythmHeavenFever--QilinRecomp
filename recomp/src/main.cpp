// qilin: Beat the Beat: Rhythm Paradise static recompiler (work in progress).
//
//   qilin info    <main.dol> [--json out.json]   SHA-256, supported build, entry point, sections, BSS
//   qilin insns   <main.dol> [-o out.txt]        every word of the text sections, decoded
//                                                (for the binutils cross-check)
//   qilin disasm  <words.bin> [--base addr] [-o out.txt]
//                                                a file of big-endian words, decoded (encoding sweep)
//   qilin profile <main.dol>                     instruction profile of the text sections
//   qilin cfg     <main.dol> [--json out.json]   function discovery + control flow, summary
//   qilin coverage <main.dol> [--list]           word accounting of the text: code / padding / unknown
//   qilin suspects <main.dol>                    functions with problems, for review
//   qilin lift    <main.dol> --out <dir> [--range lo-hi] [--only addr] [--runtime --symbols <list>]
//                                                lift to C++ (generated code: never commit it);
//                                                --runtime: for qilin_run, where replaced libraries
//                                                call native code (the difftest lifts everything)
//   qilin hwaccess <main.dol> [--json out.json]  loads and stores at constant addresses outside
//                                                cached MEM1 (hardware, GX FIFO, MEM2...), per function
//   qilin symbols <main.dol> --decomp <dir> -o <list>
//                                                the decomp's names ported to this build, the list
//                                                lift --runtime reads (symbols.hpp; as
//                                                tools/analysis/port_symbols.py --symbols-out writes it)
//   qilin identify <extracted disc folder> --json [--build <title>]
//                                                what the folder is, for the players' launcher: one
//                                                JSON object on stdout (identify.hpp); exit 0 the
//                                                supported build, 3 another, 2 unreadable
//
// Listings are one line per word: address, word, text ("8000310c 4e800020 bclr 20,lt,0").
// Exit status is non-zero on any error; nothing fails silently.
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <functional>

#include "cfg.hpp"
#include "command_line.hpp"
#include "decoder.hpp"
#include "dol.hpp"
#include "hwaccess.hpp"
#include "identify.hpp"
#include "libraries.hpp"
#include "lifter.hpp"
#include "path.hpp"
#include "symbols.hpp"

#ifndef QILIN_VERSION
#define QILIN_VERSION "unknown"
#endif

using namespace qilin;

namespace {

struct Failure {
    std::string message;
};

int usage() {
    std::print(stderr,
               "qilin " QILIN_VERSION "\n"
               "usage:\n"
               "  qilin info    <main.dol> [--json out.json]\n"
               "  qilin insns   <main.dol> [-o out.txt]\n"
               "  qilin disasm  <words.bin> [--base addr] [-o out.txt]\n"
               "  qilin profile <main.dol>\n"
               "  qilin cfg     <main.dol> [--json out.json]\n"
               "  qilin coverage <main.dol> [--list]\n"
               "  qilin suspects <main.dol>\n"
               "  qilin lift    <main.dol> --out <dir> [--range lo-hi] [--only addr] [--runtime --symbols <list>]\n"
               "  qilin hwaccess <main.dol> [--json out.json]\n"
               "  qilin symbols <main.dol> --decomp <dir> -o <list>\n"
               "  qilin identify <extracted disc folder> --json [--build <title>]\n"
               "  qilin --version\n");
    return 2;
}

struct Args {
    std::string command, input;
    std::optional<std::string> out, json, out_dir, symbols, decomp;
    std::optional<std::pair<uint32_t, uint32_t>> range;
    uint32_t base = 0;
    bool list = false, runtime = false;
};

uint32_t parse_u32(std::string_view s) {
    int radix = 10;
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
        radix = 16;
    }
    uint32_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, radix);
    if (ec != std::errc{} || end != s.data() + s.size()) throw Failure{std::format("not a number: {}", s)};
    return v;
}

Args parse_args(std::span<char*> argv) {
    Args a;
    a.command = argv[1];
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argv.size()) throw Failure{std::format("{} needs a value", arg)};
            return argv[++i];
        };
        if (arg == "-o") a.out = value();
        else if (arg == "--json") a.json = value();
        else if (arg == "--base") a.base = parse_u32(value());
        else if (arg == "--list") a.list = true;
        else if (arg == "--out") a.out_dir = value();
        else if (arg == "--symbols") a.symbols = value();
        else if (arg == "--decomp") a.decomp = value();
        else if (arg == "--runtime") a.runtime = true;
        else if (arg == "--only") {
            const uint32_t v = parse_u32(value());
            a.range = std::pair{v, v + 1};
        } else if (arg == "--range") {
            const std::string v = value();
            const auto dash = v.find('-');
            if (dash == std::string::npos) throw Failure{"--range needs lo-hi"};
            a.range = std::pair{parse_u32(v.substr(0, dash)), parse_u32(v.substr(dash + 1))};
        }
        else if (arg.starts_with("-")) throw Failure{std::format("unknown option {}", arg)};
        else if (a.input.empty()) a.input = arg;
        else throw Failure{std::format("unexpected argument {}", arg)};
    }
    if (a.input.empty()) throw Failure{"missing input file"};
    return a;
}

Dol load_dol(const std::string& path) {
    auto dol = Dol::load(native_path(path));
    if (!dol) throw Failure{std::format("{}: {}", path, dol.error())};
    return std::move(*dol);
}

// stdout, or the -o file.
class Output {
public:
    explicit Output(const std::optional<std::string>& path) {
        if (!path) return;
#ifdef _WIN32
        file_ = _wfopen(native_path(*path).c_str(), L"w");
#else
        file_ = std::fopen(path->c_str(), "w");
#endif
        if (!file_) throw Failure{std::format("cannot write {}", *path)};
    }
    ~Output() {
        if (file_) std::fclose(file_);
    }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    std::FILE* get() const { return file_ ? file_ : stdout; }

private:
    std::FILE* file_ = nullptr;
};

void list_words(std::FILE* out, std::span<const uint8_t> bytes, uint32_t base) {
    for (size_t i = 0; i + 4 <= bytes.size(); i += 4) {
        const uint32_t pc = base + static_cast<uint32_t>(i);
        const ppc::Insn in = ppc::decode(read_be32(bytes.data() + i));
        std::println(out, "{:08x} {:08x} {}", pc, in.word, ppc::disasm(in, pc));
    }
}

int cmd_info(const Args& a) {
    const Dol dol = load_dol(a.input);
    const std::string json = to_json(dol);
    if (a.json) {
        std::ofstream f(native_path(*a.json), std::ios::binary);
        if (!(f << json)) throw Failure{std::format("cannot write {}", *a.json)};
    } else {
        std::print("{}", json);
    }
    return 0;
}

int cmd_insns(const Args& a) {
    const Dol dol = load_dol(a.input);
    Output out(a.out);
    for (const DolSection& s : dol.sections)
        if (s.text) list_words(out.get(), dol.image().subspan(s.address - dol.base(), s.size), s.address);
    return 0;
}

int cmd_disasm(const Args& a) {
    std::ifstream f(native_path(a.input), std::ios::binary);
    if (!f) throw Failure{std::format("cannot open {}", a.input)};
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (bytes.size() % 4) throw Failure{std::format("{}: size is not a multiple of 4", a.input)};
    Output out(a.out);
    list_words(out.get(), bytes, a.base);
    return 0;
}

int cmd_profile(const Args& a) {
    const Dol dol = load_dol(a.input);
    std::map<ppc::Op, uint64_t> counts;
    uint64_t words = 0;
    for (const DolSection& s : dol.sections) {
        if (!s.text) continue;
        for (uint32_t pc = s.address; pc < s.end(); pc += 4, ++words) ++counts[ppc::decode(dol.u32(pc)).op];
    }
    std::vector<std::pair<ppc::Op, uint64_t>> sorted(counts.begin(), counts.end());
    std::ranges::sort(sorted, [](const auto& x, const auto& y) { return x.second > y.second; });
    std::println("{} words in text sections, {} distinct instructions", words,
                 sorted.size() - (counts.contains(ppc::Op::invalid) ? 1 : 0));
    uint64_t paired = 0, privileged = 0, floating = 0;
    for (const auto& [op, n] : sorted) {
        const ppc::Spec& s = ppc::spec(op);
        if (s.flags & ppc::kPaired) paired += n;
        if (s.flags & ppc::kPrivileged) privileged += n;
        if (s.flags & ppc::kFloat) floating += n;
        std::println("{:>10} {:6.3f}%  {}", n, 100.0 * double(n) / double(words),
                     op == ppc::Op::invalid ? std::string_view("(not an instruction)") : s.name);
    }
    std::println("floating point {}, paired single {}, privileged {}", floating, paired, privileged);
    return 0;
}

int cmd_cfg(const Args& a) {
    const Dol dol = load_dol(a.input);
    Cfg cfg(dol);
    cfg.run();
    std::print("{}", cfg.summary());
    if (a.json) {
        std::ofstream f(native_path(*a.json), std::ios::binary);
        if (!(f << cfg.to_json())) throw Failure{std::format("cannot write {}", *a.json)};
    }
    return 0;
}

int cmd_coverage(const Args& a) {
    const Dol dol = load_dol(a.input);
    Cfg cfg(dol);
    cfg.run();
    uint64_t unknown = 0;
    for (const Coverage& c : cfg.coverage()) {
        const uint64_t words = c.code + c.padding + c.dead + c.unknown;
        std::println("{} {:08X}-{:08X}: {} words: code {} ({}), padding {}, dead {}, unknown {} ({}) in {} spans", c.section,
                     c.lo, c.hi, words, c.code, percent(c.code, words, 3), c.padding, c.dead, c.unknown,
                     percent(c.unknown, words, 3), c.unknown_spans.size());
        if (a.list)
            for (const auto& [lo, hi] : c.unknown_spans) std::println("  {:08X}-{:08X} ({} words)", lo, hi, (hi - lo) / 4);
        unknown += c.unknown;
    }
    return unknown ? 3 : 0;
}

int cmd_suspects(const Args& a) {
    const Dol dol = load_dol(a.input);
    Cfg cfg(dol);
    cfg.run();
    for (const Suspect& s : cfg.suspects()) std::println("{:08X} {}", s.start, s.why);
    std::println("{} suspects; rejected starts: {}", cfg.suspects().size(), cfg.rejected().size());
    return 0;
}

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream f(p, std::ios::binary);
    if (!(f << text)) throw Failure{std::format("cannot write {}", utf8_path(p))};
}

// Writes `text` unless the file already holds exactly that, so an unchanged
// file keeps its timestamp and the build doesn't recompile it. True if written.
bool update_file(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    if (std::filesystem::file_size(p, ec) == text.size() && !ec) {
        std::ifstream f(p, std::ios::binary);
        std::string old(text.size(), '\0');
        if (f.read(old.data(), static_cast<std::streamsize>(old.size())) && old == text) return false;
    }
    write_file(p, text);
    return true;
}

// Lifted code goes in chunks of a fixed number of consecutive functions,
// counted over every function the CFG found, lifted or not. Chunk boundaries
// then depend only on the function list, so lifting more functions (a new
// slice, a fix) changes only the chunks that hold them, and only those
// recompile.
constexpr size_t kChunkFunctions = 256;

int cmd_lift(const Args& a) {
    if (!a.out_dir) throw Failure{"lift needs --out <dir>"};
    const Dol dol = load_dol(a.input);
    Cfg cfg(dol);
    cfg.run();
    const Lifter lifter(cfg);
    const std::filesystem::path dir = native_path(*a.out_dir);
    std::filesystem::create_directories(dir);

    const std::string banner = std::format("// Generated by qilin {} from main.dol (SHA-256 {}).\n"
                                           "// Derived from the game: never commit it.\n",
                                           QILIN_VERSION, dol.sha256);
    std::string header = banner + "#pragma once\n#include \"guest.hpp\"\n\nnamespace qilin::gen {\n";
    std::string table = banner + "#include \"functions.hpp\"\n\nnamespace qilin::rt {\nextern const FnEntry g_functions[] = {\n";
    std::string stubs = banner + "#include \"functions.hpp\"\n\nnamespace qilin::gen {\n";
    // --runtime: functions of replaced and dropped libraries call the
    // runtime's native code through rt::native, by index in g_natives.
    std::map<uint32_t, SymbolInfo> names;
    if (a.runtime && !a.symbols)
        throw Failure{"lift --runtime needs --symbols <the list qilin symbols writes>"};
    if (a.symbols) {
        try {
            names = load_symbols(native_path(*a.symbols));
        } catch (const std::runtime_error& e) {
            throw Failure{e.what()};
        }
    }
    std::string natives = banner + "#include \"functions.hpp\"\n#include \"native.hpp\"\n\nnamespace qilin::gen {\n";
    std::string native_table = "}  // namespace qilin::gen\n\nnamespace qilin::rt {\nextern const NativeEntry g_natives[] = {\n";
    std::string symbols = banner + "#include \"native.hpp\"\n\nnamespace qilin::rt {\nextern const Symbol g_symbols[] = {\n";
    auto quoted = [](std::string_view s) {
        std::string q = "\"";
        for (char ch : s) {
            if (ch == '"' || ch == '\\') q += '\\';
            q += ch;
        }
        return q + "\"";
    };
    std::set<std::string> seen_names, seen_units;  // to check the plan's names exist
    std::vector<std::string> chunks;
    size_t index = 0, lifted = 0, failed = 0, skipped = 0, native = 0;
    std::map<std::string, std::vector<uint32_t>> failures;  // reason -> functions
    for (const auto& [start, f] : cfg.functions()) {
        if (index++ % kChunkFunctions == 0) chunks.emplace_back();
        header += std::format("void {}(rt::Context& c);\n", fn_name(start));
        table += std::format("    {{0x{:08X}u, gen::{}}},\n", start, fn_name(start));
        if (a.runtime) {
            const auto named = names.find(start);
            const std::string name = named != names.end() ? named->second.name : fn_name(start);
            const std::string unit = named != names.end() ? named->second.unit : "";
            seen_names.insert(name);
            seen_units.insert(unit);
            const auto plan = runtime_plan(start, name, unit);
            if (!plan) throw Failure{std::format("function {:08X} is in no library of the plan (libraries.cpp)", start)};
            symbols += std::format("    {{0x{:08X}u, {}}},\n", start, quoted(name));
            if (*plan != Plan::Lift) {
                natives += std::format("void {}(rt::Context& c) {{ rt::native(c, {}); }}\n", fn_name(start), native++);
                native_table += std::format("    {{0x{:08X}u, {}, {}}},\n", start, quoted(name),
                                            quoted(library_at(start)->name));
                continue;
            }
        }
        const bool wanted = !a.range || (start >= a.range->first && start < a.range->second);
        LiftResult r;
        if (wanted) r = lifter.lift(f);
        if (r.ok) {
            chunks.back() += r.code + "\n";
            ++lifted;
            continue;
        }
        stubs += std::format("void {}(rt::Context&) {{ rt::not_lifted(0x{:08X}u); }}\n", fn_name(start), start);
        if (!wanted) {
            ++skipped;
            continue;
        }
        ++failed;
        failures[r.why.substr(0, r.why.find(':') == std::string::npos ? r.why.size() : r.why.find(':'))].push_back(start);
        failures["  " + r.why].push_back(start);
    }
    header += "}  // namespace qilin::gen\n";
    table += "};\nextern const size_t g_function_count = sizeof(g_functions) / sizeof(g_functions[0]);\n}  // namespace qilin::rt\n";
    stubs += "}  // namespace qilin::gen\n";
    size_t written = 0;
    written += update_file(dir / "functions.hpp", header);
    written += update_file(dir / "table.cpp", table);
    written += update_file(dir / "stubs.cpp", stubs);
    size_t extra = 0;
    if (a.runtime) {
        // A name in the plan that matches nothing is a mistake (a typo, or a
        // symbol list from another build): it would silently change nothing.
        auto check = [&](std::span<const std::string_view> list, const std::set<std::string>& seen, const char* what) {
            for (std::string_view n : list)
                if (!seen.contains(std::string(n)))
                    throw Failure{std::format("libraries.cpp names {} {}, but {} has no such {}", what, n, *a.symbols, what)};
        };
        check(lift_anyway_names(), seen_names, "function");
        check(native_anyway_names(), seen_names, "function");
        check(lift_unit_names(), seen_units, "unit");
        native_table += "};\nextern const size_t g_native_count = sizeof(g_natives) / sizeof(g_natives[0]);\n"
                        "}  // namespace qilin::rt\n";
        symbols += "};\nextern const size_t g_symbol_count = sizeof(g_symbols) / sizeof(g_symbols[0]);\n"
                   "}  // namespace qilin::rt\n";
        written += update_file(dir / "natives.cpp", natives + native_table);
        written += update_file(dir / "symbols.cpp", symbols);
        extra = 2;
    } else {
        // Left by a --runtime run into the same directory: they would define
        // functions twice.
        std::filesystem::remove(dir / "natives.cpp");
        std::filesystem::remove(dir / "symbols.cpp");
    }
    std::set<std::string> chunk_names;
    for (size_t i = 0; i < chunks.size(); ++i) {
        const std::string name = std::format("lifted_{:03}.cpp", i);
        chunk_names.insert(name);
        written += update_file(dir / name, banner + "#include <bit>\n#include \"functions.hpp\"\n\nnamespace qilin::gen {\n\n" +
                                               chunks[i] + "}  // namespace qilin::gen\n");
    }
    size_t removed = 0;  // chunks left by an earlier run over more functions
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = utf8_path(e.path().filename());
        if (name.starts_with("lifted_") && e.path().extension() == ".cpp" && !chunk_names.contains(name)) {
            std::filesystem::remove(e.path());
            ++removed;
        }
    }

    std::string report = std::format("{} functions lifted, {} not lifted, {} outside the range\n", lifted, failed, skipped);
    if (a.runtime) report += std::format("{:6} replaced or dropped: native code, through the runtime's registry\n", native);
    for (const auto& [why, fns] : failures) {
        if (why.starts_with("  ")) continue;
        report += std::format("{:6} {}\n", fns.size(), why);
    }
    report += "\nby instruction:\n";
    std::vector<std::pair<size_t, std::string>> detail;
    for (const auto& [why, fns] : failures)
        if (why.starts_with("  ")) detail.emplace_back(fns.size(), why);
    std::ranges::sort(detail, std::greater<>());
    for (const auto& [n, why] : detail) report += std::format("{:6} {}\n", n, why.substr(2));
    write_file(dir / "lift_report.txt", report);
    std::print("{}", report.substr(0, report.find("\nby instruction")));
    const size_t files = chunks.size() + 3 + extra;
    std::println("{} files in {}: {} written, {} unchanged, {} removed", files, utf8_path(dir), written, files - written, removed);
    return 0;
}

// Constant-address accesses outside cached MEM1, by region; --json lists
// them per function. Derived from the game: never commit the output.
int cmd_hwaccess(const Args& a) {
    const Dol dol = load_dol(a.input);
    Cfg cfg(dol);
    cfg.run();
    std::map<std::string, std::pair<size_t, std::set<uint32_t>>> by_region;  // accesses, functions
    std::string json = "{\n  \"functions\": [";
    bool first = true;
    for (const auto& [start, f] : cfg.functions()) {
        const std::vector<HwAccess> found = hardware_accesses(cfg, f);
        if (found.empty()) continue;
        json += std::format("{}\n    {{\"start\": \"0x{:08X}\", \"accesses\": [", first ? "" : ",", start);
        first = false;
        for (size_t i = 0; i < found.size(); ++i) {
            const HwAccess& h = found[i];
            const std::string region(hw_region(h.ea));
            auto& [count, fns] = by_region[region];
            ++count;
            fns.insert(start);
            json += std::format("{}{{\"pc\": \"0x{:08X}\", \"ea\": \"0x{:08X}\", \"size\": {}, \"store\": {}, \"region\": \"{}\"}}",
                                i ? ", " : "", h.pc, h.ea, h.size, h.store ? "true" : "false", region);
        }
        json += "]}";
    }
    json += "\n  ]\n}\n";
    for (const auto& [region, cf] : by_region)
        std::println("{:6} accesses in {:5} functions: {}", cf.first, cf.second.size(), region);
    if (a.json) {
        std::ofstream out(native_path(*a.json), std::ios::binary);
        if (!(out << json)) throw Failure{std::format("cannot write {}", *a.json)};
    }
    return 0;
}

// The decomp's names ported to this main.dol (symbols.hpp). Derived from the
// game: the list is written where the caller says, never committed.
int cmd_symbols(const Args& a) {
    if (!a.decomp) throw Failure{"symbols needs --decomp <the folder holding the decomp's symbols.txt and splits.txt>"};
    if (!a.out) throw Failure{"symbols needs -o <list>"};
    const Dol dol = load_dol(a.input);
    Cfg cfg(dol);
    cfg.run();
    const auto us = symbols::load_decomp(native_path(*a.decomp));
    if (!us) throw Failure{us.error()};
    const auto pal = symbols::pal_functions(dol, cfg);
    if (!pal) throw Failure{pal.error()};
    const auto ported = symbols::port(dol, *pal, *us);
    if (!ported) throw Failure{ported.error()};
    for (const std::string& line : ported->summary) std::println("{}", line);
    const bool written = update_file(native_path(*a.out), ported->list);
    std::println("{} {}", written ? "wrote" : "unchanged:", *a.out);
    return 0;
}

// The launcher's contract (identify.hpp): JSON whatever happens, and only
// exit codes 0, 3 and 2.
int cmd_identify(int argc, char** argv) {
    std::optional<std::string> path, build;
    bool json = false;
    std::string problem;
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--build" && i + 1 < argc) build = argv[++i];
        else if (!a.starts_with("-") && !path) path = std::string(a);
        else problem = std::format("identify: unexpected argument {}", a);
    }
    if (!path) problem = "identify takes one folder";
    if (!json) problem = "identify needs --json";
    Identity id;
    if (!problem.empty()) {
        id.message = problem;
    } else {
        try {
            id = identify(native_path(*path), build.value_or(""));
        } catch (const std::exception& e) {
            id = Identity{};
            id.message = std::format("The recompiler failed: {}.", e.what());
        }
    }
    std::print("{}", to_json(id));
    std::fflush(stdout);
    return id.code;
}

}  // namespace

int main(int argc, char** argv) {
    // UTF-8 on Windows too (not the ANSI code page): a disc folder may have any name.
    const CommandLine command_line(argc, argv);
    argc = command_line.count();
    argv = command_line.values();
    if (argc >= 2 && std::string_view(argv[1]) == "identify") return cmd_identify(argc, argv);
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::println("qilin " QILIN_VERSION);
        return 0;
    }
    if (argc < 3) return usage();
    try {
        const Args a = parse_args(std::span(argv, static_cast<size_t>(argc)));
        if (a.command == "info") return cmd_info(a);
        if (a.command == "insns") return cmd_insns(a);
        if (a.command == "disasm") return cmd_disasm(a);
        if (a.command == "profile") return cmd_profile(a);
        if (a.command == "cfg") return cmd_cfg(a);
        if (a.command == "coverage") return cmd_coverage(a);
        if (a.command == "suspects") return cmd_suspects(a);
        if (a.command == "lift") return cmd_lift(a);
        if (a.command == "hwaccess") return cmd_hwaccess(a);
        if (a.command == "symbols") return cmd_symbols(a);
        return usage();
    } catch (const Failure& f) {
        std::println(stderr, "qilin: {}", f.message);
        return 1;
    } catch (const std::exception& e) {
        std::println(stderr, "qilin: internal error: {}", e.what());
        return 1;
    }
}
