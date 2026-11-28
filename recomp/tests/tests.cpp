// Self-contained tests: SHA-256, a synthetic DOL built in memory, the
// Broadway decoder, function discovery and the lifter on a synthetic
// program, and the runtime helpers lifted code calls. No game files required (runs in CI).
#include <algorithm>
#include <cmath>
#include <map>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/wait.h>
#endif

#include "cfg.hpp"
#include "decoder.hpp"
#include "dol.hpp"
#include "dpl2.hpp"
#include "guest.hpp"
#include "gx.hpp"
#include "hwaccess.hpp"
#include "identify.hpp"
#include "input.hpp"
#include "libraries.hpp"
#include "lifter.hpp"
#include "native.hpp"
#include "settings.hpp"
#include "sha256.hpp"
#include "symbols.hpp"

using namespace qilin;

namespace {

int failures = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

void put_be32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
    d[o] = static_cast<uint8_t>(v >> 24);
    d[o + 1] = static_cast<uint8_t>(v >> 16);
    d[o + 2] = static_cast<uint8_t>(v >> 8);
    d[o + 3] = static_cast<uint8_t>(v);
}

// ---------------------------------------------------------------- SHA-256

std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + 3);
    return v;
}

void test_sha256() {
    CHECK(sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string_view abc = "abc";
    CHECK(sha256_hex({reinterpret_cast<const uint8_t*>(abc.data()), abc.size()}) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Padding edges: the length fits in the last block, just doesn't, and exact blocks.
    CHECK(sha256_hex(pattern(55)) == "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b");
    CHECK(sha256_hex(pattern(56)) == "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27");
    CHECK(sha256_hex(pattern(63)) == "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055");
    CHECK(sha256_hex(pattern(64)) == "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241");
    CHECK(sha256_hex(pattern(119)) == "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e");
    CHECK(sha256_hex(pattern(1000)) == "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371");
    const std::vector<uint8_t> million(1000000, 'a');
    CHECK(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ---------------------------------------------------------------- DOL

// A synthetic DOL: code at 0x80003100, data at 0x80005000, a small-data
// section inside the BSS range (as Wii linkers lay them out), BSS 0x80006000-0x80007000.
struct SyntheticDol {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x400, 0);

    SyntheticDol() {
        section(0, 0x100, 0x80003100, 0x100);   // text0
        section(7, 0x200, 0x80005000, 0x100);   // data0
        section(8, 0x300, 0x80006800, 0x40);    // data1, inside BSS
        put_be32(bytes, 0xD8, 0x80006000);      // BSS
        put_be32(bytes, 0xDC, 0x1000);
        put_be32(bytes, 0xE0, 0x80003100);      // entry
        for (size_t i = 0x100; i < 0x200; i += 4) put_be32(bytes, i, 0x60000000);  // ori r0,r0,0
        put_be32(bytes, 0x100, 0x38600001);     // addi r3,0,1
        for (size_t i = 0x200; i < 0x340; ++i) bytes[i] = static_cast<uint8_t>(i);  // data0, data1
    }
    void section(unsigned slot, uint32_t offset, uint32_t address, uint32_t size) {
        put_be32(bytes, 4 * slot, offset);
        put_be32(bytes, 0x48 + 4 * slot, address);
        put_be32(bytes, 0x90 + 4 * slot, size);
    }
};

void test_dol() {
    SyntheticDol s;
    auto dol = Dol::parse(s.bytes);
    CHECK(dol.has_value());
    if (!dol) {
        std::println(stderr, "  {}", dol.error());
        return;
    }
    CHECK(dol->sections.size() == 3);
    CHECK(dol->sections[0].text && dol->sections[0].index == 0 && dol->sections[0].name() == "text0");
    CHECK(!dol->sections[1].text && dol->sections[1].name() == "data0");
    CHECK(dol->sections[2].name() == "data1" && dol->sections[2].address == 0x80006800);
    CHECK(dol->entry == 0x80003100);
    CHECK(dol->bss_address == 0x80006000 && dol->bss_size == 0x1000);
    CHECK(dol->base() == 0x80003100 && dol->image_end() == 0x80007000);
    CHECK(dol->u32(0x80003100) == 0x38600001);
    CHECK(dol->u32(0x80003104) == 0x60000000);
    CHECK(dol->u8(0x80005001) == 0x01);       // data0's second byte, file offset 0x201
    CHECK(dol->u8(0x80006000) == 0);           // BSS
    CHECK(dol->u8(0x80006801) == 0x01);       // data1 inside BSS keeps its bytes (offset 0x301)
    CHECK(dol->u8(0x80006840) == 0);           // BSS after data1
    CHECK(dol->u8(0x80004000) == 0);           // gap between sections
    CHECK(dol->mapped(0x80006FFC, 4) && !dol->mapped(0x80006FFD, 4) && !dol->mapped(0x800030FF));
    CHECK(dol->section_at(0x800031FC) == &dol->sections[0]);
    CHECK(dol->section_at(0x80003200) == nullptr);
    CHECK(dol->section_at(0x80005050) == &dol->sections[1]);
    CHECK(dol->text_section_at(0x80005050) == nullptr);
    CHECK(dol->text_section_at(0x80003100) != nullptr);
    CHECK(dol->file_size == 0x400);
    CHECK(dol->sha256 == sha256_hex(s.bytes));
    CHECK(known_build(dol->sha256) == nullptr);
    CHECK(known_build("2e0368359106a2eef773c70ef083e89aafe1e49603eefc7f16b9ed414cd098fe") != nullptr);
    bool threw = false;
    try {
        (void)dol->u32(0x80007000);
    } catch (const DolError&) {
        threw = true;
    }
    CHECK(threw);
    const std::string json = to_json(*dol);
    CHECK(json.contains("\"entry\": \"0x80003100\""));
    CHECK(json.contains("\"name\": \"data1\""));
    CHECK(json.contains("\"build\": null"));

    // Malformed files are error messages, not crashes.
    auto expect_error = [](std::vector<uint8_t> bytes, std::string_view what) {
        auto r = Dol::parse(bytes);
        CHECK(!r.has_value());
        if (!r) {
            if (!r.error().contains(what)) std::println(stderr, "  unexpected error: {}", r.error());
            CHECK(r.error().contains(what));
        }
    };
    expect_error(std::vector<uint8_t>(0x80, 0), "shorter than");
    expect_error(std::vector<uint8_t>(0x100, 0), "no sections");
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x90, 0x400);  // text0 runs past the end of the file
        expect_error(b.bytes, "outside the file");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48, 0x7FFFFF00);  // text0 below MEM1
        expect_error(b.bytes, "outside MEM1");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48, 0x817FFFC0);  // text0 runs past the end of MEM1
        expect_error(b.bytes, "outside MEM1");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48 + 4 * 7, 0x80003180);  // data0 overlaps text0
        expect_error(b.bytes, "overlap");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0x48, 0x80003102);  // misaligned code
        expect_error(b.bytes, "word-aligned");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0xE0, 0x80005000);  // entry in data
        expect_error(b.bytes, "entry point");
    }
    {
        SyntheticDol b;
        put_be32(b.bytes, 0xD8, 0x81800000);  // BSS past MEM1
        expect_error(b.bytes, "BSS");
    }
}

// ---------------------------------------------------------------- decoder

struct Case {
    uint32_t pc, word;
    std::string_view text;
};

// Expected text from GNU objdump 2.42 `-M broadway,raw` (mftb excepted: see
// tools/check/verify_decoder.py). The sweep in CI checks millions more.
constexpr Case kCases[] = {
    {0x80003100, 0x38610008, "addi r3,r1,8"},
    {0x80003104, 0x38600000, "addi r3,0,0"},
    {0x80003108, 0x3C608000, "addis r3,0,-32768"},
    {0x8000310C, 0x3863FFFC, "addi r3,r3,-4"},
    {0x80003110, 0x80010014, "lwz r0,20(r1)"},
    {0x80003114, 0x8061FFF8, "lwz r3,-8(r1)"},
    {0x80003118, 0x9421FFE0, "stwu r1,-32(r1)"},
    {0x8000311C, 0x7C0802A6, "mfspr r0,8"},
    {0x80003120, 0x7C0803A6, "mtspr 8,r0"},
    {0x80003124, 0x7C6C42E6, "mftb r3,268"},
    {0x80003128, 0x4E800020, "bclr 20,lt,0"},
    {0x8000312C, 0x4E800421, "bcctrl 20,lt,0"},
    {0x80003130, 0x48000010, "b 0x80003140"},
    {0x80003134, 0x4BFFFFF1, "bl 0x80003124"},
    {0x80003138, 0x48000102, "ba 0x100"},
    {0x8000313C, 0x4182FFF0, "bc 12,eq,0x8000312c"},
    {0x80003140, 0x409E000C, "bc 4,4*cr7+eq,0x8000314c"},
    {0x80003144, 0x5483103A, "rlwinm r3,r4,2,0,29"},
    {0x80003148, 0x7C632214, "add r3,r3,r4"},
    {0x8000314C, 0x7C632615, "addo. r3,r3,r4"},
    {0x80003150, 0x7C032000, "cmp cr0,0,r3,r4"},
    {0x80003154, 0x2C030000, "cmpi cr0,0,r3,0"},
    {0x80003158, 0x2807FFFF, "cmpli cr0,0,r7,65535"},
    {0x8000315C, 0x4C063342, "crorc lt,4*cr1+eq,4*cr1+eq"},
    {0x80003160, 0x4C840000, "mcrf cr1,cr1"},
    {0x80003164, 0xFC20101E, "fctiwz f1,f2"},
    {0x80003168, 0xEC22182A, "fadds f1,f2,f3"},
    {0x8000316C, 0xFC2208FA, "fmadd f1,f2,f3,f1"},
    {0x80003170, 0xEC201830, "fres f1,f3,0"},
    {0x80003174, 0xFF80210C, "mtfsfi 7,2"},
    {0x80003178, 0xFDFE0D8E, "mtfsf 255,f1"},
    {0x8000317C, 0xFC20048F, "mffs. f1"},
    {0x80003180, 0xE0210008, "psq_l f1,8(r1),0,0"},
    {0x80003184, 0xF0A3FFF8, "psq_st f5,-8(r3),1,7"},
    {0x80003188, 0xE4C1C014, "psq_lu f6,20(r1),1,4"},
    {0x8000318C, 0x1062000C, "psq_lx f3,r2,r0,0,0"},
    {0x80003190, 0x1040208E, "psq_stx f2,r0,r4,0,1"},
    {0x80003194, 0x10821814, "ps_sum0 f4,f2,f0,f3"},
    {0x80003198, 0x102310FA, "ps_madd f1,f3,f3,f2"},
    {0x8000319C, 0x10642420, "ps_merge00 f3,f4,f4"},
    {0x800031A0, 0x10011040, "ps_cmpo0 cr0,f1,f2"},
    {0x800031A4, 0x100307EC, "dcbz_l r3,r0"},
    {0x800031A8, 0x7C6004AA, "lswi r3,0,32"},
    {0x800031AC, 0x44000002, "sc 0"},
    {0x800031B0, 0x7C0004AC, "sync 0"},
    {0x800031B4, 0x4C00012C, "isync"},
    {0x800031B8, 0x7C0006AC, "eieio"},
    {0x800031BC, 0x4C000064, "rfi"},
    {0x800031C0, 0x7C6FF120, "mtcrf 255,r3"},
    {0x800031C4, 0x7C64292D, "stwcx. r3,r4,r5"},
    {0x800031C8, 0x7C642828, "lwarx r3,r4,r5,0"},
    {0x800031CC, 0x7C002264, "tlbie r4,0"},
    {0x800031D0, 0x7C0018AC, "dcbf 0,r3,0"},
    {0x800031D4, 0x0C9F0000, "twi 4,r31,0"},
    {0x800031D8, 0x7FE00008, "tw 31,r0,r0"},
    {0x800031DC, 0x7C631670, "srawi r3,r3,2"},
    {0x800031E0, 0x7C640734, "extsh r4,r3"},
    {0x800031E4, 0x7C6003A6, "mtspr 0,r3"},
    {0x800031E8, 0xBBC1FFF8, "lmw r30,-8(r1)"},
    {0x800031EC, 0x7C8004A6, "mfsr r4,0"},
    {0x800031F0, 0x7C8401A4, "mtsr 4,r4"},
    {0x800031F4, 0x57E3063E, "rlwinm r3,r31,0,24,31"},
    {0x800031F8, 0x7D435378, "or r3,r10,r10"},
};

// Words that must not decode, and why.
struct Bad {
    uint32_t word;
    std::string_view why;
};
constexpr Bad kInvalid[] = {
    {0x00000000, "primary opcode 0"},
    {0x84630000, "lwzu r3,0(r3): base is the target"},
    {0x84600000, "lwzu r3,0(0): no base"},
    {0x94600000, "stwu r3,0(0): update with rA=0"},
    {0x40E00000, "bc with BO 0b00111: a z bit set"},
    {0x4E000420, "bcctr with BO 0b10000: decrements CTR"},
    {0x2C230000, "cmpi with L=1"},
    {0xFC20102C, "fsqrt: not on the 750CL"},
    {0xE4600000, "psq_lu with rA=0"},
    {0x7C641C2A, "lswx r3,r4,r3: rD = rB"},
    {0x7C83202F, "lwzx with the record bit set"},
    {0x7C632416, "mulhwu with bit 21 (OE) set"},
    {0x7C6304AA, "lswi r3,r3,32: rA among the registers it loads"},
    {0xBB9E0000, "lmw r28,0(r30): base among the loaded registers"},
    {0x44000003, "sc with bit 31 set"},
    {0x7C000265, "tlbie with the record bit set"},
    {0x7C4004AC, "sync with bit 9 set"},
    {0x7C0002E4, "tlbia: not on the 750CL"},
};

// One valid word per instruction: the opcode bits plus operand fields taken
// from a template chosen to satisfy every operand constraint (rD/BO/TO = 20,
// rA = 4, rB/NB = 5, frC = 6).
uint32_t canonical(const ppc::Spec& s) {
    constexpr uint32_t kTemplate = (20u << 21) | (4u << 16) | (5u << 11) | (6u << 6) | (7u << 1);
    uint32_t fields = 0;
    for (ppc::Opnd o : s.operands) fields |= ppc::field_mask(o);
    return s.match | (kTemplate & fields & ~s.mask);
}

// ---------------------------------------------------------------- identify

// An extracted disc in a folder of its own, as Dolphin lays one out.
struct FakeDisc {
    std::filesystem::path dir;
    explicit FakeDisc(std::string_view name) {
        dir = std::filesystem::temp_directory_path() / std::format("qilin-identify-{}-{}", name, std::rand());
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "sys");
        std::filesystem::create_directories(dir / "files");
        std::filesystem::create_directories(dir / "disc");
        write("sys/main.dol", SyntheticDol().bytes);
        write("sys/fst.bin", std::vector<uint8_t>(16, 0));
        header("sys/boot.bin", "SOMP01", 0, "Fixture");
        header("disc/header.bin", "SOMP01", 0, "Fixture");
    }
    ~FakeDisc() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    FakeDisc(const FakeDisc&) = delete;
    FakeDisc& operator=(const FakeDisc&) = delete;
    void write(const std::string& name, const std::vector<uint8_t>& bytes) const {
        std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    }
    void header(const std::string& name, std::string_view id, uint8_t revision, std::string_view title) const {
        std::vector<uint8_t> h(0x440, 0);
        std::copy(id.begin(), id.end(), h.begin());
        h[7] = revision;
        std::copy(title.begin(), title.end(), h.begin() + 0x20);
        write(name, h);
    }
};

void test_identify() {
    const std::string sha = sha256_hex(SyntheticDol().bytes);
    const std::string zeros(64, '0');
    const std::string title = "Fixture: Rhythm Paradise (Europe, Australia)";
    const KnownBuild ours[] = {{sha, "SOMP01", 0, title}};
    const KnownBuild other[] = {{zeros, "SOMP01", 0, title}, {sha, "SOME01", 0, "Fixture Fever (USA)"}};
    const KnownBuild none[] = {{zeros, "SOMP01", 0, title}};
    const std::string for_text = " This download is for " + title + ", disc SOMP01 revision 0.";
    FakeDisc disc("right");

    // The supported build.
    const Identity right = identify(disc.dir, "", ours);
    CHECK(right.ok && right.code == 0 && right.title == title && right.id == "SOMP01" && right.version == "0");
    CHECK(right.message == "This is " + title + ", disc SOMP01 revision 0: the right disc.");
    CHECK(identify(disc.dir, title, ours).code == 0);
    CHECK(identify(disc.dir, "No Such Build", ours).code == 2);
    CHECK(identify(disc.dir, "", known_builds()).code == 3);  // the real list: the fixture is no build of it
    CHECK(known_builds().size() == 1 && known_builds()[0].disc_id == "SOMP01" && known_builds()[0].revision == 0);

    // Another build this port knows.
    const Identity usa = identify(disc.dir, title, other);
    CHECK(!usa.ok && usa.code == 3 && usa.title == "Fixture Fever (USA)" && usa.id == "SOME01");
    CHECK(usa.message == "This is Fixture Fever (USA), disc SOME01 revision 0." + for_text);

    // Unknown main.dols: named by what the disc's headers say.
    {
        FakeDisc d("revision");
        d.header("disc/header.bin", "SOMP01", 1, "Fixture");
        const Identity i = identify(d.dir, "", none);
        CHECK(!i.ok && i.code == 3 && i.title == "Fixture" && i.id == "SOMP01" && i.version == "1");
        CHECK(i.message == "This is " + title + ", disc SOMP01 revision 1. This download is for revision 0 of that disc.");
        std::filesystem::remove(d.dir / "disc" / "header.bin");  // no revision known
        const Identity j = identify(d.dir, "", none);
        CHECK(j.code == 3 && j.version.empty() && j.message.starts_with("This is disc SOMP01, but its main.dol isn't revision 0's (SHA-256 " + sha));
    }
    {
        const Identity i = identify(disc.dir, "", none);  // the right disc's headers, a changed main.dol
        CHECK(i.code == 3 && i.version == "0" && i.message.ends_with("If your disc is revision 0, extract it again."));
    }
    {
        FakeDisc d("region");
        d.header("sys/boot.bin", "SOME01", 0, "Fixture");
        d.header("disc/header.bin", "SOME01", 0, "Fixture");
        const Identity i = identify(d.dir, "", none);
        CHECK(i.code == 3 && i.id == "SOME01" && i.message == "This is the game's release for North America (SOME01)." + for_text);
        d.header("sys/boot.bin", "SOMJ01", 0, "Fixture");
        CHECK(identify(d.dir, "", none).message == "This is the game's release for Japan (SOMJ01)." + for_text);
        d.header("sys/boot.bin", "SOMQ01", 0, "Fixture");
        CHECK(identify(d.dir, "", none).message == "This is the game's release for another region (SOMQ01)." + for_text);
    }
    {
        FakeDisc d("game");
        d.header("sys/boot.bin", "RSPP01", 0, "Wii Sports");
        const Identity i = identify(d.dir, "", none);
        CHECK(i.code == 3 && i.title == "Wii Sports" && i.id == "RSPP01" && i.message == "This is Wii Sports (RSPP01), another game." + for_text);
        std::filesystem::remove(d.dir / "sys" / "boot.bin");
        std::filesystem::remove(d.dir / "disc" / "header.bin");
        const Identity j = identify(d.dir, "", none);
        CHECK(j.code == 3 && j.title.empty() && j.message == "This folder's main.dol isn't a version this download knows (SHA-256 " + sha + ")." + for_text);
    }

    // Unreadable, or not the folder: exit 2, saying what to choose.
    CHECK(identify(disc.dir / "missing", "", ours).code == 2);
    disc.write("two.bin", {9, 0});
    CHECK(identify(disc.dir / "two.bin", "", ours).code == 2);
    const Identity dol_alone = identify(disc.dir / "sys" / "main.dol", "", ours);
    CHECK(dol_alone.code == 2 && dol_alone.message.starts_with("This is main.dol on its own"));
    for (const char* image : {"game.WBFS", "game.iso", "game.rvz"}) {
        disc.write(image, {1, 2, 3});
        const Identity i = identify(disc.dir / image, "", ours);
        CHECK(i.code == 2 && i.message.starts_with("This is a disc image: the game is built from the disc extracted into a folder."));
    }
    const Identity sys = identify(disc.dir / "sys", "", ours);
    CHECK(sys.code == 2 && sys.message.starts_with("This is the disc's sys folder"));
    {
        FakeDisc d("partitions");
        std::filesystem::create_directories(d.dir / "DATA");
        std::filesystem::rename(d.dir / "sys", d.dir / "DATA" / "sys");
        const Identity i = identify(d.dir, "", ours);
        CHECK(i.code == 2 && i.message == "This folder holds the disc's partitions: choose the DATA folder inside it.");
        CHECK(identify(d.dir / "files", "", ours).message.starts_with("There's no sys/main.dol in this folder"));
    }
    {
        FakeDisc d("damaged");
        d.write("sys/main.dol", std::vector<uint8_t>(0x80, 0));
        const Identity i = identify(d.dir, "", ours);
        CHECK(i.code == 2 && i.message.starts_with("Can't read sys/main.dol in this folder: "));
    }
    {
        // Missing parts: the right main.dol, but no files (or fst.bin).
        FakeDisc d("partial");
        std::filesystem::remove_all(d.dir / "files");
        const Identity i = identify(d.dir, "", ours);
        CHECK(!i.ok && i.code == 2 && i.message.starts_with("This folder has the right main.dol but not the rest of the disc"));
        FakeDisc e("no-fst");
        std::filesystem::remove(e.dir / "sys" / "fst.bin");
        CHECK(identify(e.dir, "", ours).code == 2);
    }

    // The headers' reader, and the JSON: one object, the message escaped.
    const auto h = read_disc_header(disc.dir / "disc" / "header.bin");
    CHECK(h && h->id == "SOMP01" && h->revision == 0 && h->title == "Fixture");
    CHECK(!read_disc_header(disc.dir / "two.bin"));
    Identity quoted = right;
    quoted.message = "a \"quote\"\\ and\na line\x01";
    CHECK(to_json(quoted) == "{\"ok\": true, \"title\": \"" + title + "\", \"id\": \"SOMP01\", \"version\": \"0\", "
                             "\"message\": \"a \\\"quote\\\"\\\\ and\\na line\\u0001\"}\n");
    CHECK(to_json(Identity{}).starts_with("{\"ok\": false, "));
}

// What the program prints and its exit status, run with `args` (each quoted;
// these tests' paths hold no quotes). On Windows the command line is UTF-16
// all the way, so the program sees the names as they are, as the launcher
// starts it; qilin must read them back as UTF-8 (command_line.hpp).
std::pair<int, std::string> run_program(const std::filesystem::path& program, const std::vector<std::filesystem::path>& args) {
#ifdef _WIN32
    std::wstring line = L"\"\"" + program.wstring() + L"\"";
    for (const auto& a : args) line += L" \"" + a.wstring() + L"\"";
    line += L"\"";  // cmd /c takes off the outer quotes
    std::FILE* pipe = _wpopen(line.c_str(), L"rb");
#else
    std::string line = "'" + program.string() + "'";
    for (const auto& a : args) line += " '" + a.string() + "'";
    std::FILE* pipe = popen(line.c_str(), "r");
#endif
    if (!pipe) return {-1, {}};
    std::string out;
    char buffer[4096];
    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, pipe)) > 0;) out.append(buffer, n);
#ifdef _WIN32
    const int status = _pclose(pipe);
#else
    const int raw = pclose(pipe);
    const int status = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
    return {status, out};
}

// `qilin identify` itself, the program the launcher runs, on a folder whose
// name is Japanese and accented: outside any one ANSI code page, so it fails
// on Windows if the arguments are read in that code page instead of UTF-8.
void test_identify_unicode(const std::filesystem::path& qilin) {
    FakeDisc disc("unicode");
    const std::filesystem::path named = disc.dir.parent_path() /
                                        std::filesystem::path(std::u8string(u8"qilin-リズム天国 Été ü-")) +=
                                        std::to_string(std::rand());
    std::filesystem::remove_all(named);
    std::filesystem::rename(disc.dir, named);
    disc.dir = named;  // removed with the fixture
    const std::u8string leaf = named.filename().u8string();
    const std::string leaf_utf8(leaf.begin(), leaf.end());

    // The folder found and read: the fixture's headers, a main.dol that isn't
    // the supported build's (exit 3, not 2).
    const auto [code, out] = run_program(qilin, {"identify", named, "--json"});
    CHECK(code == 3);
    CHECK(out.starts_with("{\"ok\": false, \"title\": \"Fixture\", \"id\": \"SOMP01\", \"version\": \"0\", "));
    CHECK(out.find("This is disc SOMP01, but its main.dol isn't revision 0's") != std::string::npos);
    // A path that doesn't exist: named back in UTF-8, exactly.
    const auto [missing_code, missing] = run_program(qilin, {"identify", named / "missing", "--json"});
    CHECK(missing_code == 2 && missing.find(leaf_utf8) != std::string::npos);
    if (failures) std::println(stderr, "qilin identify printed: {}{}", out, missing);
}

// ---------------------------------------------------------------- symbols

void test_symbols() {
    using symbols::Opcode;
    // difflib's opcodes, as Python's SequenceMatcher(None, a, b, autojunk=False).get_opcodes() gives them.
    auto ops = [](std::vector<uint32_t> a, std::vector<uint32_t> b) { return symbols::opcodes(a, b); };
    CHECK((ops({1, 2, 3, 4, 5}, {1, 2, 3, 4, 5}) == std::vector<Opcode>{{'e', 0, 5, 0, 5}}));
    CHECK((ops({8, 8, 8, 16, 4, 12}, {8, 8, 16, 8, 4, 4, 8}) ==
          (std::vector<Opcode>{{'d', 0, 1, 0, 0}, {'e', 1, 4, 0, 3}, {'i', 4, 4, 3, 4}, {'e', 4, 5, 4, 5}, {'r', 5, 6, 5, 7}})));
    CHECK((ops({1, 2, 3, 9, 9, 4, 5, 6}, {1, 2, 3, 7, 4, 5, 6}) ==
          (std::vector<Opcode>{{'e', 0, 3, 0, 3}, {'r', 3, 5, 3, 4}, {'e', 5, 8, 4, 7}})));
    CHECK((ops({}, {3, 4}) == std::vector<Opcode>{{'i', 0, 0, 0, 2}}));
    CHECK((ops({5, 5, 5}, {}) == std::vector<Opcode>{{'d', 0, 3, 0, 0}}));
    CHECK((ops({7, 1, 7, 1, 7}, {1, 7, 1, 7, 1}) == (std::vector<Opcode>{{'i', 0, 0, 0, 1}, {'e', 0, 4, 1, 5}, {'d', 4, 5, 5, 5}})));
    CHECK((ops({12, 8, 16, 4, 4, 20, 4, 12, 20, 4, 20, 8, 4, 4, 16, 16, 4, 8, 4, 20, 16, 4, 20, 4, 8, 20, 4, 20, 20, 16, 4, 8, 4, 20, 8, 12, 16, 8, 20, 4},
              {12, 8, 16, 4, 4, 20, 4, 20, 8, 4, 24, 8, 4, 16, 16, 4, 8, 4, 20, 16, 99, 20, 4, 8, 20, 4, 20, 20, 16, 4, 12, 20, 8, 12, 16, 8, 20, 4}) ==
          (std::vector<Opcode>{{'e', 0, 7, 0, 7}, {'d', 7, 10, 7, 7}, {'e', 10, 13, 7, 10}, {'i', 13, 13, 10, 12}, {'e', 13, 21, 12, 20},
                               {'r', 21, 22, 20, 21}, {'e', 22, 31, 21, 30}, {'r', 31, 33, 30, 31}, {'e', 33, 40, 31, 38}})));

    // The decomp's files, read as port_symbols.py reads them.
    const auto dir = std::filesystem::temp_directory_path() / std::format("qilin-symbols-{}", std::rand());
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "symbols.txt", std::ios::binary)
        << "B = .init:0x80004008; // type:function size:0x8 scope:global\r\n"
           "A = .init:0x80004000; // type:function size:0x8 scope:local\n"
           "Data = .data:0x80005000; // type:object size:0x10\n"
           "Label = .init:0x80004010; // type:label scope:global\n"
           "PPCMfmsr = .init:0x80004010; // type:function size:0x8\n"
           "E = .init:0x80004018; // type:function size:0xC\n";
    std::ofstream(dir / "splits.txt", std::ios::binary) << "Sections:\n\t.init       type:code align:4\n\nrevolution/base/PPCArch.c:\n"
                                                           "\t.init       start:0x80004000 end:0x80004018\n\t.data       start:0x80005000 end:0x80005010\n";
    const auto us = symbols::load_decomp(dir);
    std::filesystem::remove_all(dir);
    CHECK(us && us->size() == 4);
    if (!us || us->size() != 4) return;
    CHECK((*us)[0].name == "A" && (*us)[1].name == "B" && (*us)[2].name == "PPCMfmsr" && (*us)[3].name == "E");
    CHECK((*us)[0].unit == "revolution/base/PPCArch.c" && (*us)[3].unit == "(unsplit)" && (*us)[3].size == 0xC);
    CHECK(!symbols::load_decomp(dir));  // gone: an error, not an empty list

    // The port: A and B one to one, PPCMfmsr moved to the function whose
    // code is `mfmsr r3; blr`, E split in two.
    SyntheticDol s;
    put_be32(s.bytes, 0x124, 0x7C6000A6);  // 0x80003124: mfmsr r3
    put_be32(s.bytes, 0x128, 0x4E800020);  //             blr
    const auto dol = Dol::parse(s.bytes);
    CHECK(dol.has_value());
    if (!dol) return;
    const std::vector<symbols::PalFunction> pal = {{".init", 0x80003100, 8}, {".init", 0x80003108, 8}, {".init", 0x80003110, 8},
                                                   {".init", 0x80003118, 4}, {".init", 0x8000311C, 8}, {".init", 0x80003124, 8}};
    // US sizes 8 8 8 12 against PAL 8 8 8 4 8 8: A, B, PPCMfmsr anchored at
    // the first three; E (12) is 4 + 8; PPCMfmsr moves to the last.
    const auto ported = symbols::port(*dol, pal, *us);
    CHECK(ported.has_value());
    if (!ported) return;
    CHECK(ported->list == "A = .init:0x80003100; // size:0x8 us:0x80004000 match:anchored unit:revolution/base/PPCArch.c\n"
                          "B = .init:0x80003108; // size:0x8 us:0x80004008 match:anchored unit:revolution/base/PPCArch.c\n"
                          "PPCMfmsr = .init:0x80003124; // size:0x8 us:0x80004010 match:signature unit:revolution/base/PPCArch.c\n"
                          "E = .init:0x80003118; // size:0x4 us:0x80004018 match:split unit:(unsplit)\n"
                          "E.part2 = .init:0x8000311C; // size:0x8 us:0x80004018 match:split unit:(unsplit)\n");
    CHECK(std::ranges::find(ported->summary, "moved by its code: PPCMfmsr 80003110 -> 80003124") != ported->summary.end());
    // A name whose code is nowhere near: stop rather than guess.
    put_be32(s.bytes, 0x124, 0x60000000);
    CHECK(!symbols::port(*Dol::parse(s.bytes), pal, *us).has_value());
}

void test_decoder() {
    for (const Case& c : kCases) {
        const ppc::Insn in = ppc::decode(c.word);
        const std::string text = ppc::disasm(in, c.pc);
        if (text != c.text) std::println(stderr, "  {:08x}: got '{}', want '{}'", c.word, text, c.text);
        CHECK(text == c.text);
    }
    for (const Bad& b : kInvalid) {
        const bool invalid = !ppc::decode(b.word).valid();
        if (!invalid) std::println(stderr, "  {:08x} decoded as {} ({})", b.word, ppc::disasm(ppc::decode(b.word), 0), b.why);
        CHECK(invalid);
    }

    // Every instruction decodes from its canonical word, and setting any one
    // bit it doesn't define (a reserved bit) makes the word invalid.
    CHECK(ppc::specs().size() == static_cast<size_t>(ppc::Op::count_) - 1);
    size_t checked = 0;
    for (const ppc::Spec& s : ppc::specs()) {
        const uint32_t word = canonical(s);
        const ppc::Insn in = ppc::decode(word);
        if (in.op != s.op) std::println(stderr, "  {}: canonical word {:08x} decodes as {}", s.name, word, ppc::spec(in.op).name);
        CHECK(in.op == s.op);
        CHECK(ppc::spec(s.op).name == s.name);
        uint32_t defined = s.mask;
        for (ppc::Opnd o : s.operands) defined |= ppc::field_mask(o);
        if (s.flags & (ppc::kRc | ppc::kLk)) defined |= 1;
        if (s.flags & ppc::kOe) defined |= 0x400;
        if (s.flags & ppc::kAa) defined |= 2;
        for (unsigned bit = 0; bit < 32; ++bit) {
            if (defined & (1u << bit)) continue;
            const ppc::Insn flipped = ppc::decode(word ^ (1u << bit));
            if (flipped.valid())
                std::println(stderr, "  {}: reserved bit {} set still decodes ({:08x} -> {})", s.name, bit,
                             word ^ (1u << bit), ppc::disasm(flipped, 0));
            CHECK(!flipped.valid());
        }
        ++checked;
    }
    CHECK(checked == 222);  // the 750CL's instructions, paired singles included

    // Fields and suffixes.
    CHECK(ppc::decode(0x3863FFFC).simm() == -4);
    CHECK(ppc::decode(0x4BFFFFF1).branch_target(0x80003134) == 0x80003124);
    CHECK(ppc::decode(0x4BFFFFF3).branch_target(0x80003134) == 0xFFFFFFF0);  // bla -16
    CHECK(ppc::decode(0x4182FFF0).branch_target(0x8000313C) == 0x8000312C);
    CHECK(ppc::decode(0xF0A3FFF8).psq_d() == -8);
    CHECK(ppc::decode(0xF0A3FFF8).psq_w() == 1 && ppc::decode(0xF0A3FFF8).psq_i() == 7);
    CHECK(ppc::decode(0x1040208E).psqx_w() == 0 && ppc::decode(0x1040208E).psqx_i() == 1);
    CHECK(ppc::decode(0x7C6C42E6).spr() == 268);
    CHECK(ppc::mnemonic(ppc::decode(0x4BFFFFF3)) == "bla");
    CHECK(ppc::mnemonic(ppc::decode(0x7C632615)) == "addo.");
    CHECK(ppc::mnemonic(ppc::decode(0x4E800421)) == "bcctrl");
    CHECK(ppc::spec(ppc::Op::psq_l).flags & ppc::kPaired);
    CHECK(ppc::spec(ppc::Op::mtmsr).flags & ppc::kPrivileged);
    CHECK(ppc::spec(ppc::Op::bc).flags & ppc::kBranch);
    CHECK(ppc::disasm(ppc::decode(0), 0) == ".long 0x0");
}

// ---------------------------------------------------------------- CFG

// A synthetic program with one function for each way a function is found,
// a CodeWarrior switch, a register-save chain, the get-PC idiom, an indirect
// tail call, padding and dead code. text0 0x80003100-0x80003300, data0 0x80005000.
struct Program {
    std::vector<uint8_t> bytes = std::vector<uint8_t>(0x400, 0);
    static constexpr uint32_t kText = 0x80003100, kData = 0x80005000;

    void word(uint32_t a, uint32_t w) {
        put_be32(bytes, a >= kData ? 0x300 + (a - kData) : 0x100 + (a - kText), w);
    }
    static uint32_t b(uint32_t pc, uint32_t to, bool link = false) {
        return 0x48000000u | ((to - pc) & 0x03FFFFFCu) | (link ? 1u : 0u);
    }
    static uint32_t bc(uint32_t pc, unsigned bo, unsigned bi, uint32_t to) {
        return (16u << 26) | (bo << 21) | (bi << 16) | ((to - pc) & 0xFFFCu);
    }
    static uint32_t li(unsigned rd, int16_t v) { return (14u << 26) | (rd << 21) | static_cast<uint16_t>(v); }

    Program() {
        put_be32(bytes, 0x00, 0x100);  // text0
        put_be32(bytes, 0x48, kText);
        put_be32(bytes, 0x90, 0x200);
        put_be32(bytes, 0x1C, 0x300);  // data0
        put_be32(bytes, 0x64, kData);
        put_be32(bytes, 0xAC, 0x100);
        put_be32(bytes, 0xE0, kText);  // entry

        // 80003100 entry: calls, a lis/addi pointer, then a tail call.
        word(0x80003100, 0x9421FFF0);                     // stwu r1,-16(r1)
        word(0x80003104, b(0x80003104, 0x80003140, true)); // bl switch
        word(0x80003108, b(0x80003108, 0x80003180, true)); // bl save chain, first entry
        word(0x8000310C, b(0x8000310C, 0x80003184, true)); // bl save chain, second entry
        word(0x80003110, b(0x80003110, 0x800031A0, true)); // bl get-PC user
        word(0x80003114, 0x3C608000);                     // lis r3,0x8000
        word(0x80003118, 0x386331D0);                     // addi r3,r3,0x31D0: thread entry
        word(0x8000311C, 0x38210010);                     // addi r1,r1,16
        word(0x80003120, b(0x80003120, 0x800031C0));      // b: tail call
        // 80003140 switch (r3) with cases 0, 1, 2 and a default.
        word(0x80003140, 0x28030002);                     // cmplwi r3,2
        word(0x80003144, bc(0x80003144, 12, 1, 0x80003170)); // bgt default
        word(0x80003148, 0x3C808000);                     // lis r4,0x8000
        word(0x8000314C, 0x5460103A);                     // slwi r0,r3,2
        word(0x80003150, 0x38845000);                     // addi r4,r4,0x5000
        word(0x80003154, 0x7C04002E);                     // lwzx r0,r4,r0
        word(0x80003158, 0x7C0903A6);                     // mtctr r0
        word(0x8000315C, 0x4E800420);                     // bctr
        word(0x80003160, li(3, 0));
        word(0x80003164, 0x4E800020);
        word(0x80003168, li(3, 1));
        word(0x8000316C, 0x4E800020);
        word(0x80003170, li(3, -1));                      // default
        word(0x80003174, 0x4E800020);
        word(0x80003178, li(3, 2));                       // case 2, after the default
        word(0x8000317C, 0x4E800020);
        // 80003180 register-save chain: two entry points, the first falls into the second.
        word(0x80003180, 0x91CBFFF8);                     // stw r14,-8(r11)
        word(0x80003184, 0x91EBFFFC);                     // stw r15,-4(r11)
        word(0x80003188, 0x4E800020);
        // 800031A0 reads the PC: bl to the next instruction is not a call.
        word(0x800031A0, b(0x800031A0, 0x800031A4, true));
        word(0x800031A4, 0x7C6802A6);                     // mflr r3
        word(0x800031A8, 0x4E800020);
        word(0x800031C0, li(3, 5));                       // reached only by the tail call
        word(0x800031C4, 0x4E800020);
        word(0x800031D0, li(3, 6));                       // reached only through lis/addi
        word(0x800031D4, 0x4E800020);
        word(0x800031E0, li(3, 7));                       // reached only through a vtable in data
        word(0x800031E4, b(0x800031E4, 0x800031EC));
        word(0x800031E8, li(3, 9));                       // dead: nothing reaches it
        word(0x800031EC, 0x4E800020);
        word(0x80003200, li(3, 8));                       // referenced by nothing: a gap
        word(0x80003204, 0x4E800020);
        word(0x80003210, 0x81830000);                     // lwz r12,0(r3)
        word(0x80003214, 0x7D8903A6);                     // mtctr r12
        word(0x80003218, 0x4E800420);                     // bctr: an indirect tail call
        // 80003220 tail-calls a function that sets up a frame inside its own
        // bound, leaving its blr unreachable after the tail call.
        word(0x80003220, li(3, 1));
        word(0x80003224, b(0x80003224, 0x8000322C));
        word(0x80003228, 0x4E800020);                     // leftover blr: dead
        word(0x8000322C, 0x9421FFF0);                     // stwu r1,-16(r1): a function
        word(0x80003230, 0x38210010);
        word(0x80003234, 0x4E800020);
        // 80003240 ends with a call through bctr; its epilogue stays behind.
        word(0x80003240, 0x9421FFF0);
        word(0x80003244, 0x7C6903A6);                     // mtctr r3
        word(0x80003248, 0x4E800420);                     // bctr
        word(0x8000324C, 0x38210010);                     // leftover epilogue: dead
        word(0x80003250, 0x4E800020);

        word(0x80005000, 0x80003160);                     // the switch's table
        word(0x80005004, 0x80003168);
        word(0x80005008, 0x80003178);
        word(0x8000500C, 0x800031E0);                     // a vtable
        word(0x80005010, 0x80003210);
        word(0x80005014, 0x80003220);
        word(0x80005018, 0x80003240);
    }
};

void test_cfg() {
    const Program p;
    auto dol = Dol::parse(p.bytes);
    CHECK(dol.has_value());
    if (!dol) return;
    Cfg cfg(*dol);
    cfg.run();
    const auto& fs = cfg.functions();
    const std::vector<std::pair<uint32_t, Origin>> expected = {
        {0x80003100, Origin::Entry},   {0x80003140, Origin::Call},    {0x80003180, Origin::Call},
        {0x80003184, Origin::Call},    {0x800031A0, Origin::Call},    {0x800031C0, Origin::TailCall},
        {0x800031D0, Origin::ImmPtr},  {0x800031E0, Origin::DataPtr}, {0x80003200, Origin::Gap},
        {0x80003210, Origin::DataPtr}, {0x80003220, Origin::DataPtr}, {0x8000322C, Origin::TailCall},
        {0x80003240, Origin::DataPtr},
    };
    CHECK(fs.size() == expected.size());
    for (const auto& [start, origin] : expected) {
        auto it = fs.find(start);
        if (it == fs.end() || it->second.origin != origin)
            std::println(stderr, "  function {:08x}: {}", start, it == fs.end() ? "missing" : origin_name(it->second.origin));
        CHECK(it != fs.end() && it->second.origin == origin);
    }
    if (fs.size() != expected.size()) return;

    const Function& entry = fs.at(0x80003100);
    CHECK(entry.calls == (std::set<uint32_t>{0x80003140, 0x80003180, 0x80003184, 0x800031A0}));
    CHECK(entry.tails == (std::set<uint32_t>{0x800031C0}));
    CHECK(entry.insns.size() == 9);
    const Function& sw = fs.at(0x80003140);
    CHECK(sw.jtabs.size() == 1);
    if (sw.jtabs.size() == 1) {
        CHECK(sw.jtabs[0].proven);
        CHECK(sw.jtabs[0].table == 0x80005000);
        CHECK(sw.jtabs[0].entries == (std::vector<uint32_t>{0x80003160, 0x80003168, 0x80003178}));
    }
    CHECK(sw.insns.size() == 16);
    CHECK(sw.leaders.contains(0x80003178) && sw.leaders.contains(0x80003170));
    CHECK(fs.at(0x80003180).falls_into == 0x80003184 && fs.at(0x80003180).insns.size() == 1);
    CHECK(fs.at(0x800031A0).calls.empty() && fs.at(0x800031A0).insns.size() == 3);
    CHECK(fs.at(0x800031E0).insns.size() == 3);
    CHECK(fs.at(0x80003210).itails == (std::vector<uint32_t>{0x80003218}));
    CHECK(fs.at(0x80003220).tails == (std::set<uint32_t>{0x8000322C}) && fs.at(0x80003220).insns.size() == 2);
    CHECK(fs.at(0x80003240).itails == (std::vector<uint32_t>{0x80003248}));
    CHECK(cfg.suspects().empty());
    CHECK(cfg.rejected().empty());

    const auto cov = cfg.coverage();
    CHECK(cov.size() == 1);
    if (cov.size() == 1) {
        CHECK(cov[0].code == 51);
        CHECK(cov[0].dead == 4);  // 800031E8, the leftover blr at 80003228, the epilogue at 8000324C
        CHECK(cov[0].unknown == 0);
        CHECK(cov[0].padding == 128 - 55);
    }
    CHECK(cfg.to_json().contains("\"falls_into\": \"0x80003184\""));
    CHECK(percent(1, 3, 2) == "33.33%" && percent(2, 3, 0) == "67%" && percent(0, 0, 1) == "n/a");

    // Weak evidence that reaches an invalid word is rejected: a data pointer
    // to a clean boundary whose code runs into a reserved encoding.
    Program q;
    q.word(0x80003260, Program::li(3, 1));
    q.word(0x80003264, 0x7C83202F);  // lwzx with the record bit: invalid
    q.word(0x80005020, 0x80003260);
    auto dol2 = Dol::parse(q.bytes);
    CHECK(dol2.has_value());
    if (!dol2) return;
    Cfg cfg2(*dol2);
    cfg2.run();
    CHECK(!cfg2.functions().contains(0x80003260));
    CHECK(cfg2.rejected() == (std::vector<uint32_t>{0x80003260}));
    CHECK(cfg2.coverage()[0].unknown == 2);
}

// ---------------------------------------------------------------- lifter

void test_lifter() {
    const Program p;
    auto dol = Dol::parse(p.bytes);
    CHECK(dol.has_value());
    if (!dol) return;
    Cfg cfg(*dol);
    cfg.run();
    const Lifter lifter(cfg);
    std::map<uint32_t, std::string> code;
    for (const auto& [start, f] : cfg.functions()) {
        const LiftResult r = lifter.lift(f);
        if (!r.ok) std::println(stderr, "  {:08x} not lifted: {} at {:08x}", start, r.why, r.fail_at);
        CHECK(r.ok);
        code[start] = r.code;
    }
    if (code.size() != cfg.functions().size()) return;
    auto has = [&](uint32_t fn, std::string_view text) {
        const bool ok = code[fn].contains(text);
        if (!ok) std::println(stderr, "  {:08x} lacks: {}\n{}", fn, text, code[fn]);
        CHECK(ok);
    };
    has(0x80003100, "void fn_80003100(rt::Context& c) {");
    // Register dataflow: a call stores only what changed since the last sync
    // and reloads only what is read before being overwritten.
    has(0x80003100, "lr = 0x80003108u; c.r[1] = r1; c.lr = lr; fn_80003140(c); if (c.ret != 0x80003108u) rt::bad_return(0x80003104u, c.ret);");
    has(0x80003100, "lr = 0x8000310Cu; c.lr = lr; fn_80003180(c); if");          // r1 is already in the context
    has(0x80003100, "fn_800031A0(c); r1 = c.r[1]; if");                         // r1 is read afterwards, r3 overwritten
    has(0x80003100, "r3 = 0x80000000u;");                      // lis r3,0x8000
    has(0x80003100, "c.r[1] = r1; c.r[3] = r3; fn_800031C0(c); return;");          // tail call
    has(0x80003140, "switch (ctr) { case 0x80003160u: goto L_80003160; case 0x80003168u: goto L_80003168; "
                    "case 0x80003178u: goto L_80003178; default: rt::bad_jump(0x8000315Cu, ctr); }");
    has(0x80003140, "if (rt::crb(cr, 1)) { goto L_80003170; }");  // bgt
    has(0x80003140, "L_80003170:;");
    has(0x80003140, "c.r[3] = r3; c.cr = cr; c.ret = lr; return;");  // xer is only read
    has(0x80003180, "    fn_80003184(c); return;");                  // nothing written          // falls into the next entry
    has(0x800031A0, "lr = 0x800031A4u;");                       // the get-PC idiom
    CHECK(!code[0x800031A0].contains("rt::bad_return"));
    has(0x80003210, "c.r[12] = r12; c.ctr = ctr; rt::call(c, ctr, 0x80003218u); return;");
    has(0x80003220, "c.r[3] = r3; fn_8000322C(c); return;");

    // ori/xori have no record bit: an odd immediate must not update CR0
    // (found by the differential test).
    CHECK(!code[0x800031A0].contains("set_cr0"));
    {
        Program o;
        o.word(0x80003260, 0x60630001);  // ori r3,r3,1
        o.word(0x80003264, 0x68630001);  // xori r3,r3,1
        o.word(0x80003268, 0x4E800020);
        o.word(0x80005020, 0x80003260);
        auto d = Dol::parse(o.bytes);
        CHECK(d.has_value());
        if (d) {
            Cfg c(*d);
            c.run();
            const LiftResult lr = Lifter(c).lift(c.functions().at(0x80003260));
            CHECK(lr.ok && lr.code.contains("r3 = r3 | 0x1u;") && !lr.code.contains("set_cr0"));
        }
    }

    // Slice 3: floating point through the runtime's helpers; FPSCR access
    // (only in replaced code) is refused with the reason.
    auto lift_one = [](std::initializer_list<uint32_t> words) {
        Program q;
        uint32_t at = 0x80003260;
        for (uint32_t w : words) q.word(at, w), at += 4;
        q.word(0x80005020, 0x80003260);
        auto d = Dol::parse(q.bytes);
        CHECK(d.has_value());
        if (!d) return LiftResult{};
        Cfg c(*d);
        c.run();
        return Lifter(c).lift(c.functions().at(0x80003260));
    };
    LiftResult r = lift_one({0xC0230000,    // lfs f1,0(r3)
                             0xC8430008,    // lfd f2,8(r3)
                             0xEC6100B2,    // fmuls f3,f1,f2
                             0xFC011040,    // fcmpo cr0,f1,f2
                             0xD0630010,    // stfs f3,16(r3)
                             0x4E800020});  // blr
    CHECK(r.ok);
    CHECK(r.code.contains("f1 = rt::lfs(rt::ld32(ea));"));
    CHECK(r.code.contains("f2 = rt::dbl(rt::ld64(ea));"));
    CHECK(r.code.contains("if (!rt::single_significand(f2)) rt::fp_unknown(0x80003268u); f3 = rt::fmuls(f1, f2);") ||
          (r.code.contains("if (!rt::single_significand(f2)) rt::fp_unknown(0x80003268u);") &&
           r.code.contains("f3 = rt::fmuls(f1, f2);")));
    CHECK(r.code.contains("if (rt::denormal_d(f1) || rt::denormal_d(f2)) rt::fp_unknown(0x8000326Cu); "
                          "cr = rt::set_crf(cr, 0, rt::fcmp(f1, f2));"));
    CHECK(r.code.contains("rt::st32(ea, rt::stfs(f3));"));
    CHECK(r.code.contains("[[maybe_unused]] double f1 = c.f[1];") && r.code.contains("c.f[3] = f3;"));
    r = lift_one({0xFC00048E, 0x4E800020});  // mffs f0
    CHECK(!r.ok && r.fail_at == 0x80003260 &&
          r.why == "FPSCR access (only in replaced code; FPSCR not modelled): mffs");
    r = lift_one({0xEC22182B, 0x4E800020});  // fadds. f1,f2,f3
    CHECK(!r.ok && r.why.starts_with("floating-point record form"));
}

// ---------------------------------------------------------------- runtime helpers

void test_float() {
    using rt::bits;
    using rt::dbl;
    // Non-IEEE mode: denormal operands and results are zero of their sign.
    CHECK(bits(rt::fmul(0x1p-1000, 0x1p-30)) == 0);                       // denormal result
    CHECK(bits(rt::fmul(-0x1p-1000, 0x1p-30)) == 0x8000000000000000ull);   // keeps the sign
    CHECK(rt::fadd(0x1p-1060, 1.0) == 1.0 && rt::fadd(0x1p-1060, 0x1p-1060) == 0.0);  // denormal operands
    CHECK(rt::fadds(1e-40, 0.0) == 0.0);                       // single precision: by the single format
    CHECK(rt::fadds(0x1p-120, 0x1p-120) == 0x1p-119);          // a normal single stays
    CHECK(rt::fadd(1e-300, 0.0) == 1e-300);                    // a normal double stays in double precision
    // NaNs: the first NaN operand (frA, frB, frC), quieted; else the positive default QNaN.
    const double inf = dbl(0x7FF0000000000000ull);
    CHECK(bits(rt::fsub(inf, inf)) == 0x7FF8000000000000ull);
    CHECK(bits(rt::fmul(0.0, inf)) == 0x7FF8000000000000ull);
    CHECK(bits(rt::fadd(dbl(0x7FF0000000000001ull), dbl(0xFFF8000000000002ull))) == 0x7FF8000000000001ull);
    CHECK(bits(rt::fadd(1.0, dbl(0xFFF8000000000002ull))) == 0xFFF8000000000002ull);
    CHECK(bits(rt::fma_(1.0, 2.0, dbl(0x7FF4000000000000ull), false, true)) == 0x7FFC000000000000ull);  // no negation
    CHECK(bits(rt::fadds(dbl(0x7FF800000FFFFFFFull), 1.0)) == 0x7FF8000000000000ull);  // single keeps 23 bits
    // Fused multiply-add rounds once: (1 + 2^-30)^2 - 1 keeps the 2^-60 term.
    CHECK(rt::fma_(1.0 + 0x1p-30, 1.0 + 0x1p-30, 1.0, true, false) == 0x1p-29 + 0x1p-60);
    CHECK(rt::fma_(2.0, 3.0, 1.0, false, true) == -7.0 && rt::fma_(2.0, 3.0, 1.0, true, true) == -5.0);
    // Single precision rounds the double result.
    CHECK(rt::fadds(1.0, 0x1p-24 + 0x1p-40) == 1.0 + 0x1p-23);
    CHECK(rt::frsp(1.0 + 0x1p-25) == 1.0);
    // fctiwz: truncation, saturation, NaN, and the 750 family's high word.
    CHECK(bits(rt::fctiwz(3.7)) == 0xFFF8000000000003ull && bits(rt::fctiwz(-3.7)) == 0xFFF80000FFFFFFFDull);
    CHECK(bits(rt::fctiwz(1e10)) == 0xFFF800007FFFFFFFull && bits(rt::fctiwz(-1e10)) == 0xFFF8000080000000ull);
    CHECK(bits(rt::fctiwz(dbl(0x7FF8000000000000ull))) == 0xFFF8000080000000ull);
    CHECK(bits(rt::fctiwz(-2147483648.9)) == 0xFFF8000080000000ull);
    // Compares: FL FG FE FU.
    CHECK(rt::fcmp(1.0, 2.0) == rt::kLt && rt::fcmp(2.0, 1.0) == rt::kGt && rt::fcmp(-0.0, 0.0) == rt::kEq);
    CHECK(rt::fcmp(dbl(0x7FF8000000000000ull), 1.0) == 1);
    // lfs: exact, NaN payloads kept (a signalling NaN stays signalling).
    CHECK(rt::lfs(0x3F800000u) == 1.0 && rt::lfs(0x00000001u) == 0x1p-149);
    CHECK(bits(rt::lfs(0x7F800001u)) == 0x7FF0000020000000ull && bits(rt::lfs(0xFF800000u)) == 0xFFF0000000000000ull);
    // stfs: the architecture's bit copy (no rounding) and denormalisation.
    CHECK(rt::stfs(1.0 + 0x1p-24 + 0x1p-40) == 0x3F800000u);  // truncated, where a conversion rounds up
    CHECK(rt::stfs(0x1p-149) == 0x00000001u && rt::stfs(0x1p-127) == 0x00400000u && rt::stfs(-0x1p-126) == 0x80800000u);
    CHECK(rt::stfs(0x1p-200) == 0 && rt::stfs(-0.0) == 0x80000000u);
    CHECK(rt::stfs(dbl(0x7FF4000000000000ull)) == 0x7FA00000u);  // a signalling NaN stays signalling
    for (const uint32_t w : {0x3F800000u, 0xC2F6E979u, 0x00000001u, 0x807FFFFFu, 0x7F7FFFFFu, 0x7FC00001u, 0xFF800000u})
        CHECK(rt::stfs(rt::lfs(w)) == w);  // lfs then stfs is the identity
    // fmuls's frC check.
    CHECK(rt::single_significand(1.5) && rt::single_significand(rt::lfs(0x3DCCCCCDu)) && !rt::single_significand(0.1));
}

void test_hwaccess() {
    // Regions of the address map.
    CHECK(hw_region(0x80001234u).empty() && hw_region(0x817FFFFFu).empty());
    CHECK(hw_region(0xCC008000u) == "GX FIFO (write-gather pipe)" && hw_region(0xCC002000u) == "VI (video interface)");
    CHECK(hw_region(0xCD006C00u) == "AI (audio interface)" && hw_region(0xCD000004u) == "Hollywood registers (IPC and others)");
    CHECK(hw_region(0x90000000u) == "MEM2" && hw_region(0xC0001000u) == "MEM1 uncached" && hw_region(0x12345678u) == "unmapped");

    // Constant propagation: lis/ori addresses are followed, a cached MEM1
    // store isn't reported, and a call makes registers unknown.
    Program q;
    const uint32_t words[] = {
        0x3C60CC01,  // lis r3,0xCC01
        0xD0238000,  // stfs f1,-0x8000(r3)   -> GX FIFO store at 0xCC008000
        0x3C80CC00,  // lis r4,0xCC00
        0x60842000,  // ori r4,r4,0x2000
        0xA0A40000,  // lhz r5,0(r4)          -> VI load at 0xCC002000
        0x3CC08000,  // lis r6,0x8000
        0x90A60000,  // stw r5,0(r6)          -> cached MEM1: not reported
        0x48000085,  // bl 0x80003300
        0x90A30004,  // stw r5,4(r3)          -> r3 unknown after the call: not reported
        0x4E800020,  // blr
    };
    uint32_t at = 0x80003260;
    for (uint32_t w : words) q.word(at, w), at += 4;
    q.word(0x80003300, 0x4E800020);  // the callee: blr
    q.word(0x80005020, 0x80003260);
    auto d = Dol::parse(q.bytes);
    CHECK(d.has_value());
    if (!d) return;
    Cfg c(*d);
    c.run();
    const std::vector<HwAccess> found = hardware_accesses(c, c.functions().at(0x80003260));
    CHECK(found.size() == 2);
    if (found.size() != 2) return;
    CHECK(found[0].pc == 0x80003264u && found[0].ea == 0xCC008000u && found[0].size == 4 && found[0].store);
    CHECK(found[1].pc == 0x80003270u && found[1].ea == 0xCC002000u && found[1].size == 2 && !found[1].store);

    // The lifter hands exactly the FIFO stores to the runtime; the other
    // stores stay memory stores.
    const LiftResult r = Lifter(c).lift(c.functions().at(0x80003260));
    CHECK(r.ok);
    CHECK(r.code.contains("rt::fifo_st32(ea, rt::stfs(f1))"));
    CHECK(r.code.contains("rt::st32(ea, r5)"));
    CHECK(!r.code.contains("rt::fifo_st32(ea, r5)"));

    // A callee-saved register keeps its constant across a call (the EABI);
    // a volatile one doesn't.
    Program k;
    const uint32_t keep[] = {
        0x3F00CC01,  // lis r24,0xCC01
        0x3C60CC01,  // lis r3,0xCC01
        0x48000099,  // bl 0x80003300
        0xD0388000,  // stfs f1,-0x8000(r24)  -> still the GX FIFO
        0xD0438000,  // stfs f2,-0x8000(r3)   -> r3 unknown after the call
        0x4E800020,  // blr
    };
    at = 0x80003260;
    for (uint32_t w : keep) k.word(at, w), at += 4;
    k.word(0x80003300, 0x4E800020);
    k.word(0x80005020, 0x80003260);
    auto dk = Dol::parse(k.bytes);
    CHECK(dk.has_value());
    if (!dk) return;
    Cfg ck(*dk);
    ck.run();
    const std::vector<HwAccess> kept = hardware_accesses(ck, ck.functions().at(0x80003260));
    CHECK(kept.size() == 1 && kept[0].pc == 0x8000326Cu && kept[0].ea == 0xCC008000u);

    // A loop's backward branch is a safe point, with the loop's stack pointer.
    Program lp;
    const uint32_t loop[] = {
        0x3863FFFF,  // addi r3,r3,-1
        0x2C030000,  // cmpwi r3,0
        0x4082FFF8,  // bne 0x80003260
        0x4E800020,  // blr
    };
    at = 0x80003260;
    for (uint32_t w : loop) lp.word(at, w), at += 4;
    lp.word(0x80005020, 0x80003260);
    auto dl = Dol::parse(lp.bytes);
    CHECK(dl.has_value());
    if (!dl) return;
    Cfg cl(*dl);
    cl.run();
    const LiftResult lr = Lifter(cl).lift(cl.functions().at(0x80003260));
    CHECK(lr.ok && lr.code.contains("if (rt::poll_due()) [[unlikely]] { c.r[1] = r1; rt::poll(c); }"));
}

void test_paired() {
    using rt::bits;
    using rt::dbl;
    // GQR fields (750CL manual, table 2-19): LD_SCALE bits 2-7, LD_TYPE 13-15, ST_SCALE 18-23, ST_TYPE 29-31.
    const uint32_t g = 0x3F07'0205u;  // load: scale -1, type 7 (s16); store: scale 2, type 5 (u16)
    CHECK(rt::gqr_ld_scale(g) == -1 && rt::gqr_ld_type(g) == 7 && rt::gqr_st_scale(g) == 2 && rt::gqr_st_type(g) == 5);
    CHECK(rt::quant_size(0) == 4 && rt::quant_size(4) == 1 && rt::quant_size(5) == 2 && rt::quant_size(6) == 1 &&
          rt::quant_size(7) == 2);

    std::vector<uint8_t> mem(0x100, 0);
    uint8_t* const saved = rt::g_mem;
    rt::g_mem = mem.data() - 0x1000;  // guest 0x1000-0x10FF (unchecked memory in this build)
    // Stores: round toward zero, then clamp; +inf and positive NaNs clamp high, -inf and negative NaNs low.
    auto st = [&](double v, unsigned type, int scale) {
        rt::quantize(0x1000, v, type, scale);
        return type == 0 ? uint32_t(mem[0]) << 24 | uint32_t(mem[1]) << 16 | uint32_t(mem[2]) << 8 | mem[3]
               : (type & 1) ? uint32_t(mem[0]) << 8 | mem[1]
                            : uint32_t(mem[0]);
    };
    CHECK(st(100.0, 4, 1) == 200 && st(100.0, 4, 2) == 255 && st(100.0, 6, 1) == 127 && st(100.0, 5, 2) == 400);
    CHECK(st(100.0, 4, -2) == 25 && st(2.9, 4, 0) == 2 && st(-2.9, 6, 0) == 0xFE && st(-5.0, 4, 0) == 0);
    CHECK(st(-1e9, 7, 0) == 0x8000 && st(1e9, 7, 0) == 0x7FFF && st(1e9, 5, 0) == 0xFFFF);
    CHECK(st(dbl(0x7FF0000000000000ull), 6, 0) == 127 && st(dbl(0xFFF0000000000000ull), 6, 0) == 0x80);
    CHECK(st(dbl(0x7FF8000000000000ull), 7, 0) == 0x7FFF && st(dbl(0xFFF8000000000000ull), 7, 0) == 0x8000);
    CHECK(st(1.5, 0, 5) == 0x3FC00000u);  // single: no scaling, no conversion
    CHECK(st(1e-40, 0, 0) == 0 && st(-1e-40, 0, 0) == 0);  // a denormal stores 0.0
    CHECK(st(-0.0, 0, 0) == 0x80000000u);
    // Loads: I * 2^-S; W = 1 loads one value and sets ps1 to 1.0.
    mem[0] = 0xFF, mem[1] = 0x80;  // s16 -128, or u8 255 and 128
    double p0 = 0, p1 = 0;
    rt::psq_l(p0, p1, 0x1000, false, 4u << 16 | 1u << 24, 0);  // u8, scale 1
    CHECK(p0 == 127.5 && p1 == 64.0);
    rt::psq_l(p0, p1, 0x1000, true, 7u << 16 | 0x3Fu << 24, 0);  // s16, scale -1, W = 1
    CHECK(p0 == -256.0 && p1 == 1.0);
    rt::psq_l(p0, p1, 0x1000, false, 6u << 16, 0);  // s8: -1, -128
    CHECK(p0 == -1.0 && p1 == -128.0);
    rt::psq_st(3.0, -2.0, 0x1000, false, 0x0000'0007u | 0x01u << 8, 0);  // s16, scale 1
    CHECK(mem[0] == 0x00 && mem[1] == 0x06 && mem[2] == 0xFF && mem[3] == 0xFC);
    rt::g_mem = saved;

    // The lifter: GQRs from the context, both halves of a pair, frD written after both lanes.
    auto lift_one = [](std::initializer_list<uint32_t> words) {
        Program q;
        uint32_t at = 0x80003260;
        for (uint32_t w : words) q.word(at, w), at += 4;
        q.word(0x80005020, 0x80003260);
        auto d = Dol::parse(q.bytes);
        CHECK(d.has_value());
        if (!d) return LiftResult{};
        Cfg c(*d);
        c.run();
        return Lifter(c).lift(c.functions().at(0x80003260));
    };
    LiftResult r = lift_one({0xF3E10028,    // psq_st f31,40(r1),0,0
                             0xE3E1F028,    // psq_l f31,40(r1),1,7
                             0x102214A0,    // ps_merge10 f1,f2,f2
                             0xC0230000,    // lfs f1,0(r3)
                             0xFC40F890,    // fmr f2,f31
                             0x4E800020});  // blr
    CHECK(r.ok);
    CHECK(r.code.contains("rt::psq_st(f31, p31, ea, false, c.spr[912], 0x80003260u);"));
    CHECK(r.code.contains("rt::psq_l(f31, p31, ea, true, c.spr[919], 0x80003264u);"));
    CHECK(r.code.contains("{ const double x0 = p2, x1 = f2; f1 = x0; p1 = x1; }"));
    CHECK(r.code.contains("f1 = rt::lfs(rt::ld32(ea)); p1 = f1;"));  // lfs fills both halves
    CHECK(r.code.contains("f2 = f31;") && !r.code.contains("p2 = f2;"));  // fmr leaves ps1 alone
    r = lift_one({0x10200834, 0x4E800020});  // ps_rsqrte f1,f1
    CHECK(!r.ok && r.why == "floating-point estimate (Broadway's table not known): ps_rsqrte");
}

void test_runtime() {
    // add3: carry out and signed overflow.
    auto s = rt::add3(0xFFFFFFFFu, 1, 0);
    CHECK(s.v == 0 && s.carry == 1 && !s.ov);
    s = rt::add3(0x7FFFFFFFu, 1, 0);
    CHECK(s.v == 0x80000000u && s.carry == 0 && s.ov);
    s = rt::add3(~5u, 7, 1);  // subf: 7 - 5
    CHECK(s.v == 2 && s.carry == 1);
    s = rt::add3(~7u, 5, 1);  // 5 - 7 borrows: CA = 0
    CHECK(s.v == 0xFFFFFFFEu && s.carry == 0);
    s = rt::add3(~0x80000000u, 0, 1);  // neg 0x80000000 overflows
    CHECK(s.v == 0x80000000u && s.ov);
    // Broadway's divide-by-zero and overflow results.
    CHECK(rt::divw(7, 0) == 0 && rt::divw(0xFFFFFFF9u, 0) == 0xFFFFFFFFu);
    CHECK(rt::divw(0x80000000u, 0xFFFFFFFFu) == 0xFFFFFFFFu && rt::divw_ov(0x80000000u, 0xFFFFFFFFu));
    CHECK(rt::divw(0xFFFFFFF9u, 2) == 0xFFFFFFFDu);  // -7 / 2 = -3, truncated
    CHECK(rt::divwu(7, 0) == 0 && rt::divwu(0xFFFFFFFFu, 2) == 0x7FFFFFFFu);
    CHECK(rt::mulhw(0xFFFFFFFFu, 0xFFFFFFFFu) == 0 && rt::mulhwu(0xFFFFFFFFu, 0xFFFFFFFFu) == 0xFFFFFFFEu);
    CHECK(rt::mullw_ov(0x10000u, 0x8000u) && !rt::mullw_ov(0xFFFFFFFFu, 0x7FFFFFFFu));
    // Shifts by 32-63 give 0 (or all sign bits); only bits 0-5 of the count matter.
    CHECK(rt::slw(1, 32) == 0 && rt::slw(1, 31) == 0x80000000u && rt::slw(1, 64) == 1);
    CHECK(rt::srw(0x80000000u, 63) == 0 && rt::srw(0x80000000u, 31) == 1);
    auto a = rt::sraw(0x80000001u, 1);
    CHECK(a.v == 0xC0000000u && a.carry == 1);  // negative, a 1 shifted out
    a = rt::sraw(0x80000000u, 1);
    CHECK(a.v == 0xC0000000u && a.carry == 0);  // nothing lost
    a = rt::sraw(0x80000000u, 40);
    CHECK(a.v == 0xFFFFFFFFu && a.carry == 1);
    a = rt::sraw(0x7FFFFFFFu, 40);
    CHECK(a.v == 0 && a.carry == 0);
    // Condition register: field layout, SO from XER.
    CHECK(rt::cmps(0xFFFFFFFFu, 1, 0) == rt::kLt && rt::cmpu(0xFFFFFFFFu, 1, 0) == rt::kGt);
    CHECK(rt::cmps(3, 3, rt::kXerSo) == (rt::kEq | rt::kSo));
    CHECK(rt::set_crf(0, 0, rt::kLt) == 0x80000000u && rt::set_crf(0, 7, rt::kSo) == 1);
    CHECK(rt::set_cr0(0x0FFFFFFFu, 0, 0) == 0x2FFFFFFFu);
    CHECK(rt::crb(0x20000000u, 2) == 1 && rt::set_crb(0, 31, 1) == 1);
    CHECK(rt::set_ov(0, true) == (rt::kXerOv | rt::kXerSo) && rt::set_ov(rt::kXerOv | rt::kXerSo, false) == rt::kXerSo);
    CHECK(rt::ca(rt::set_ca(0, 1)) == 1);

    // Big-endian guest memory over a little-endian host.
    std::vector<uint8_t> mem(64, 0);
    uint8_t* saved = rt::g_mem;
    rt::g_mem = mem.data() - 0x100;  // guest 0x100 is mem[0]
    rt::st32(0x100, 0x11223344u);
    CHECK(mem[0] == 0x11 && mem[3] == 0x44 && rt::ld32(0x100) == 0x11223344u);
    CHECK(rt::ld16(0x102) == 0x3344 && rt::ld8(0x101) == 0x22);
    CHECK(rt::ld32r(0x100) == 0x44332211u && rt::ld16r(0x100) == 0x2211);
    rt::st16(0x104, 0xFFFE);
    CHECK(rt::lha(0x104) == 0xFFFFFFFEu);
    rt::st32(0x120, 0xFFFFFFFFu);
    rt::dcbz(0x13F);  // zeroes the block 0x120-0x13F
    CHECK(rt::ld32(0x120) == 0 && rt::ld32(0x13C) == 0 && rt::ld32(0x100) == 0x11223344u);
    rt::g_mem = saved;
}

// ---------------------------------------------------------------- estimates
// frsqrte and fres: exact where the architecture defines them, a stop
// wherever Broadway's table would be needed.
void test_estimates() {
    using rt::bits;
    const double inf = HUGE_VAL;
    CHECK(rt::frsqrte(0.0, 0) == inf && rt::frsqrte(-0.0, 0) == -inf);
    CHECK(rt::frsqrte(inf, 0) == 0.0 && !std::signbit(rt::frsqrte(inf, 0)));
    CHECK(bits(rt::frsqrte(-2.0, 0)) == 0x7FF8000000000000ull && bits(rt::frsqrte(-inf, 0)) == 0x7FF8000000000000ull);
    CHECK(bits(rt::frsqrte(rt::dbl(0x7FF0000000000001ull), 0)) == 0x7FF8000000000001ull);  // quietened
    CHECK(rt::frsqrte(rt::dbl(1), 0) == inf);                                             // a denormal is a zero (NI)
    CHECK(rt::fres(0.0, 0) == inf && rt::fres(-0.0, 0) == -inf);
    CHECK(rt::fres(-inf, 0) == 0.0 && std::signbit(rt::fres(-inf, 0)));
    bool stopped = false;
    try {
        rt::frsqrte(4.0, 0x80001234u);
    } catch (const rt::Stop& s) {
        stopped = s.kind == rt::Stop::Unsupported && s.pc == 0x80001234u;
    }
    CHECK(stopped);
    stopped = false;
    try {
        rt::fres(3.0, 0x80001238u);
    } catch (const rt::Stop& s) {
        stopped = s.kind == rt::Stop::Unsupported && s.pc == 0x80001238u;
    }
    CHECK(stopped);
    // The runtime's approximation (QILIN_APPROX_ESTIMATES): 12 fraction bits,
    // rounded to nearest, so within IBM's stated 1/4096 relative accuracy.
    CHECK(bits(rt::estimate_12(1.0)) == bits(1.0));
    CHECK((bits(rt::estimate_12(1.0 / 3.0)) & ((1ull << 40) - 1)) == 0);
    CHECK(rt::estimate_12(2.0 - 0x1p-20) == 2.0);  // rounding carries into the exponent
    for (double x = 0.01; x < 1000.0; x *= 1.37) {
        const double exact = 1.0 / std::sqrt(x), e = rt::estimate_12(exact);
        CHECK(std::fabs(e - exact) / exact <= 1.0 / 4096.0);
    }
}

// ---------------------------------------------------------------- library plan

void test_libraries() {
    // In address order, without overlaps: library_at searches by that.
    const auto libs = libraries();
    CHECK(!libs.empty());
    for (size_t i = 0; i < libs.size(); ++i) {
        CHECK(libs[i].lo < libs[i].hi);
        if (i) CHECK(libs[i - 1].hi <= libs[i].lo);
    }
    const Library* game = library_at(0x80038314u);
    CHECK(game && game->name == "game" && game->plan == Plan::Lift);
    const Library* os = library_at(0x800DB3A0u);
    CHECK(os && os->name == "os" && os->plan == Plan::Replace);
    CHECK(library_at(0x800DA8E0u) == os && library_at(0x800E81F8u) == os);  // first and last word
    CHECK(library_at(0x800DA8DCu)->name == "base");
    const Library* trk = library_at(0x80115FCCu);
    CHECK(trk && trk->plan == Plan::Drop);
    CHECK(library_at(0x80006800u) == nullptr);  // extab, between .init and .text
    CHECK(library_at(0x7FFFFFFFu) == nullptr && library_at(0x801F968Cu) == nullptr);
    CHECK(lift_anyway("exit") && lift_anyway("__init_cpp") && !lift_anyway("OSInit") && !lift_anyway(""));
    for (std::string_view n : lift_anyway_names()) CHECK(lift_anyway(n));
    for (std::string_view n : lift_unit_names()) CHECK(lift_unit(n));
    // The runtime's plan: library, then unit, then the exceptions by name.
    CHECK(runtime_plan(0x80038314u, "main", "Game/main.cpp") == Plan::Lift);
    CHECK(runtime_plan(0x800DB3A0u, "OSInit", "revolution/os/OS.c") == Plan::Replace);
    CHECK(runtime_plan(0x800E28B0u, "OSInitMutex", "revolution/os/OSMutex.c") == Plan::Lift);    // a lifted unit
    CHECK(runtime_plan(0x800E46E0u, "SelectThread", "revolution/os/OSThread.c") == Plan::Replace);  // native in it
    CHECK(runtime_plan(0x800E81B0u, "exit", "revolution/os/__ppc_eabi_init.c") == Plan::Lift);    // by name
    CHECK(runtime_plan(0x80115FCCu, "x", "") == Plan::Drop);
    CHECK(!runtime_plan(0x80006800u, "x", ""));

    // The PAL symbol list, as port_symbols.py writes it.
    const auto path = std::filesystem::temp_directory_path() / "qilin_test_symbols.txt";
    {
        std::ofstream f(path);
        f << "main = .text:0x80038314; // size:0x14C us:0x80038204 match:anchored unit:Game/main.cpp\n"
             "__start = .init:0x80004050; // size:0x168\n"
             "a line that is not a symbol\n"
             "__save_gpr.part2 = .text:0x80105000; // size:0x4\n";
    }
    const auto names = load_symbols(path);
    CHECK(names.size() == 3 && names.at(0x80038314u).name == "main" && names.at(0x80004050u).name == "__start");
    CHECK(names.at(0x80038314u).unit == "Game/main.cpp" && names.at(0x80004050u).unit.empty());
    CHECK(names.at(0x80105000u).name == "__save_gpr.part2");
    std::filesystem::remove(path);
    bool threw = false;
    try {
        load_symbols(path);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------- native registry

QILIN_NATIVE(TestDouble) { c.r[3] *= 2; }

void test_natives() {
    CHECK(rt::bind_natives() && rt::natives_bound() == 1);
    rt::Context c;
    c.r[3] = 21;
    c.lr = 0x80001234u;
    rt::native(c, 0);
    CHECK(c.r[3] == 42 && c.ret == 0x80001234u);  // returns to the caller, as a blr
    // A replaced function without native code stops, naming itself and the caller.
    bool stopped = false;
    try {
        rt::native(c, 1);
    } catch (const rt::Stop& s) {
        stopped = s.kind == rt::Stop::Unregistered && s.pc == 0x80002000u && s.value == 0x80001234u;
    }
    CHECK(stopped);
    CHECK(rt::symbol_at(0x80001000u) && std::string_view(rt::symbol_at(0x80001000u)->name) == "TestDouble");
    CHECK(rt::symbol_at(0x80001004u) == nullptr);
    CHECK(rt::symbol_containing(0x80001004u) == rt::symbol_at(0x80001000u));
    CHECK(rt::symbol_containing(0x80000050u) == nullptr);
    CHECK(rt::symbol_address("alpha") == 0x80000100u);
    CHECK(rt::symbol_address("twice") == 0 && rt::symbol_address("absent") == 0);  // ambiguous, missing
}

}  // namespace

// What `qilin lift --runtime` generates, for test_natives.
namespace qilin::rt {
extern const NativeEntry g_natives[] = {{0x80001000u, "TestDouble", "os"}, {0x80002000u, "TestMissing", "gx"}};
extern const size_t g_native_count = 2;
extern const Symbol g_symbols[] = {{0x80000100u, "alpha"},
                                   {0x80001000u, "TestDouble"},
                                   {0x80002000u, "TestMissing"},
                                   {0x80003000u, "twice"},
                                   {0x80003100u, "twice"}};
extern const size_t g_symbol_count = 5;
// The function table isn't linked into the tests: native.cpp's call_guest
// would stop at any call.
void call(Context&, uint32_t target, uint32_t from) { bad_jump(from, target); }
}  // namespace qilin::rt

namespace {

// ---------------------------------------------------------------- GX textures
// Synthetic tiles in GX's documented layouts, decoded to RGBA8.
void test_textures() {
    using namespace rt::gx;
    std::vector<uint8_t> mem(0x400);
    uint8_t* const saved = rt::g_mem;
    rt::g_mem = mem.data() - 0x100;  // guest 0x100 is mem[0]
    auto px = [](const std::vector<uint8_t>& out, uint32_t w, uint32_t x, uint32_t y) {
        const uint8_t* p = out.data() + (size_t(y) * w + x) * 4;
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };

    // sizes: whole tiles, padded
    CHECK(texture_bytes(kTexI4, 8, 8) == 32 && texture_bytes(kTexRgba8, 4, 4) == 64);
    CHECK(texture_bytes(kTexI8, 9, 5) == 4 * 32);  // 2x2 tiles of 8x4
    CHECK(texture_bytes(kTexCmpr, 16, 8) == 64 && texture_bytes(3, 4, 4) == 32 && texture_bytes(7, 4, 4) == 0);

    // I8, two tiles side by side (16x4): texel (x, y) of tile t is byte t*32 + y*8 + x
    for (uint32_t i = 0; i < 64; ++i) mem[i] = uint8_t(i);
    std::vector<uint8_t> out(16 * 4 * 4);
    decode_texture(kTexI8, 0x100, 16, 4, nullptr, out.data());
    CHECK(px(out, 16, 0, 0) == 0x00000000u && px(out, 16, 7, 0) == 0x07070707u);
    CHECK(px(out, 16, 1, 2) == 0x11111111u && px(out, 16, 8, 0) == 0x20202020u && px(out, 16, 15, 3) == 0x3F3F3F3Fu);

    // I4: two texels a byte, high nibble first, each times 17
    mem[0] = 0xF1;
    std::vector<uint8_t> o4(8 * 8 * 4);
    decode_texture(kTexI4, 0x100, 8, 8, nullptr, o4.data());
    CHECK(px(o4, 8, 0, 0) == 0xFFFFFFFFu && px(o4, 8, 1, 0) == 0x11111111u);

    // IA8: alpha in the high byte; RGB5A3 both forms; RGB565
    mem[0] = 0x80, mem[1] = 0x40;                // IA8: a 0x80, i 0x40
    mem[2] = 0xFC, mem[3] = 0x00;                // RGB5A3 opaque: r 31
    mem[4] = 0x0F, mem[5] = 0x00;                // RGB5A3 translucent: a 0, r 15
    mem[6] = 0x07, mem[7] = 0xE0;                // RGB565: g 63
    std::vector<uint8_t> o16(4 * 4 * 4);
    decode_texture(kTexIA8, 0x100, 4, 4, nullptr, o16.data());
    CHECK(px(o16, 4, 0, 0) == 0x40404080u);
    decode_texture(kTexRgb5A3, 0x100, 4, 4, nullptr, o16.data());
    CHECK(px(o16, 4, 1, 0) == 0xFF0000FFu && px(o16, 4, 2, 0) == 0xFF000000u);
    decode_texture(kTexRgb565, 0x100, 4, 4, nullptr, o16.data());
    CHECK(px(o16, 4, 3, 0) == 0x00FF00FFu);

    // RGBA8: 32 bytes of (A, R) pairs, then 32 of (G, B)
    for (uint32_t i = 0; i < 16; ++i) mem[2 * i] = 0xA0, mem[2 * i + 1] = uint8_t(i), mem[32 + 2 * i] = 0x11, mem[33 + 2 * i] = 0x22;
    decode_texture(kTexRgba8, 0x100, 4, 4, nullptr, o16.data());
    CHECK(px(o16, 4, 1, 1) == 0x051122A0u);

    // C8 through an RGB565 TLUT at guest 0x300
    std::fill(mem.begin(), mem.begin() + 32, uint8_t{0});
    mem[0] = 2;
    mem[0x204] = 0xF8, mem[0x205] = 0x00;  // entry 2: red
    Tlut tlut;
    tlut.loaded = true, tlut.data = 0x300, tlut.format = 1, tlut.entries = 16;
    std::vector<uint8_t> o8(8 * 4 * 4);
    decode_texture(kTexC8, 0x100, 8, 4, &tlut, o8.data());
    CHECK(px(o8, 8, 0, 0) == 0xFF0000FFu);

    // CMPR: four 4x4 blocks; c0 > c1 gives four opaque colours, c0 <= c1 a
    // transparent fourth
    std::fill(mem.begin(), mem.begin() + 32, uint8_t{0});
    mem[0] = 0xFF, mem[1] = 0xFF, mem[2] = 0x00, mem[3] = 0x00, mem[4] = 0x1B;  // white, black; row 0: 0 1 2 3
    mem[8] = 0x00, mem[9] = 0x00, mem[10] = 0xFF, mem[11] = 0xFF, mem[12] = 0xC0;  // black <= white; texel 0: index 3
    std::vector<uint8_t> oc(8 * 8 * 4);
    decode_texture(kTexCmpr, 0x100, 8, 8, nullptr, oc.data());
    CHECK(px(oc, 8, 0, 0) == 0xFFFFFFFFu && px(oc, 8, 1, 0) == 0x000000FFu);
    CHECK(px(oc, 8, 2, 0) == 0xAAAAAAFFu && px(oc, 8, 3, 0) == 0x555555FFu);
    CHECK(px(oc, 8, 4, 0) == 0x00000000u);  // second block (top right): transparent

    bool stopped = false;
    try {
        decode_texture(7, 0x100, 4, 4, nullptr, o16.data());
    } catch (const rt::Stop&) {
        stopped = true;
    }
    CHECK(stopped);
    rt::g_mem = saved;
}

// ---------------------------------------------------------------- input script
void test_input_script() {
    std::vector<rt::ScriptedPress> p;
    std::string bad;
    CHECK(rt::parse_input_script("A@5,A@8.5,B+A@12:0.3", p, bad) && p.size() == 3);
    CHECK(p.size() == 3 && p[0].from == 5.0 && p[0].to == 5.0 + 0.1 && p[0].buttons == rt::kButtonA);
    CHECK(p.size() == 3 && p[2].buttons == (rt::kButtonA | rt::kButtonB) && p[2].to == 12.0 + 0.3);
    CHECK(rt::parse_input_script("UP@1,,HOME@2", p, bad) && p.size() == 2);
    for (const char* s : {"Q@1", "A@", "A@-1", "A@1:0", "A1", "A+@1", "A@1x", "A@:0.3", "A@nan", "A@1:", "A@inf"}) {
        CHECK(!rt::parse_input_script(s, p, bad));
    }
    CHECK(!rt::parse_input_script("A@1,Z@2", p, bad) && bad == "Z@2");
}

// ---------------------------------------------------------------- Pro Logic II
// The encoder against its matrix: Lt = L - j(a Ls + b Rs), Rt = R + j(b Ls + a Rs).
void test_dpl2() {
    constexpr uint32_t kFrame = 96;
    constexpr double kPi = 3.14159265358979323846;
    // Feeds `frames` frames of each channel's signal (a function of the
    // sample index), and returns Lt and Rt from the last frames, once the
    // filter has settled.
    auto run = [&](auto l, auto r, auto ls, auto rs, std::vector<float>& lt, std::vector<float>& rt) {
        rt::Dpl2Encoder<kFrame> e;
        lt.clear(), rt.clear();
        for (uint32_t f = 0; f < 300; ++f) {  // 0.9 s: settled after 200 frames, then 0.3 s measured (whole periods at the test tones)
            int32_t in[4][kFrame];
            for (uint32_t i = 0; i < kFrame; ++i) {
                const uint32_t n = f * kFrame + i;
                in[0][i] = l(n), in[1][i] = r(n), in[2][i] = ls(n), in[3][i] = rs(n);
            }
            float a[kFrame], b[kFrame];
            e.encode(in[0], in[1], in[2], in[3], a, b);
            if (f >= 200) lt.insert(lt.end(), a, a + kFrame), rt.insert(rt.end(), b, b + kFrame);
        }
    };
    auto zero = [](uint32_t) { return 0; };
    // 1 kHz at 32 kHz, amplitude 10000
    auto tone = [&](uint32_t n) { return int32_t(std::lrint(10000.0 * std::sin(2 * kPi * 1000.0 * n / 32000.0))); };
    auto rms = [](const std::vector<float>& v) {
        double s = 0;
        for (float x : v) s += double(x) * x;
        return std::sqrt(s / double(v.size()));
    };
    auto corr = [](const std::vector<float>& a, const std::vector<float>& b) {
        double ab = 0, aa = 0, bb = 0;
        for (size_t i = 0; i < a.size(); ++i) ab += double(a[i]) * b[i], aa += double(a[i]) * a[i], bb += double(b[i]) * b[i];
        return ab / std::sqrt(aa * bb);
    };
    const double tone_rms = 10000.0 / std::sqrt(2.0);
    std::vector<float> lt, rt;
    // front only: its own channel at unity gain (all-pass), nothing on the other
    for (double hz : {60.0, 1000.0, 12000.0}) {
        auto t = [&](uint32_t n) { return int32_t(std::lrint(10000.0 * std::sin(2 * kPi * hz * n / 32000.0))); };
        run(t, zero, zero, zero, lt, rt);
        CHECK(std::abs(rms(lt) / tone_rms - 1.0) < 0.01 && rms(rt) == 0);
        // and the surround at the matrix's level at every frequency
        run(zero, zero, t, zero, lt, rt);
        CHECK(std::abs(rms(lt) / tone_rms - 0.8718) < 0.01 && std::abs(rms(rt) / tone_rms - 0.4899) < 0.01);
    }
    // Ls: 0.8718 on Lt, 0.4899 on Rt, in opposite phase (-j and +j)
    run(zero, zero, tone, zero, lt, rt);
    CHECK(std::abs(rms(lt) / tone_rms - 0.8718) < 0.01 && std::abs(rms(rt) / tone_rms - 0.4899) < 0.01);
    CHECK(corr(lt, rt) < -0.99);
    // Rs: mirrored
    run(zero, zero, zero, tone, lt, rt);
    CHECK(std::abs(rms(lt) / tone_rms - 0.4899) < 0.01 && std::abs(rms(rt) / tone_rms - 0.8718) < 0.01);
    CHECK(corr(lt, rt) < -0.99);
    // the surround is 90 degrees from the front: Ls with the same tone on L
    // adds in quadrature (Lt's power is L's plus the surround's), within what
    // the filters' worst 1.4-degree phase error allows (1.2% here)
    run(tone, zero, tone, zero, lt, rt);
    CHECK(std::abs(rms(lt) / tone_rms - std::sqrt(1.0 + 0.8718 * 0.8718)) < 0.02);
}

// ---------------------------------------------------------------- settings
void test_settings() {
    auto parse = [](const char* text, rt::Settings& s, std::string& error, unsigned& line) {
        std::istringstream in(text);
        s = rt::Settings{};
        return rt::parse_settings(in, s, error, line);
    };
    rt::Settings s;
    std::string error;
    unsigned line = 0;
    // a preset, then a key overriding it; comments and blank lines
    CHECK(parse("# comment\n[display]\npreset = desktop\nanisotropy = 4  # trailing\n\n[console]\nlanguage = 2\nwidescreen = false\n",
                s, error, line));
    CHECK(s.preset == "desktop" && s.internal_scale == 3 && s.anisotropy == 4 && s.language == 2 && !s.widescreen);
    CHECK(parse("[display]\npreset = original\n", s, error, line) && s.internal_scale == 1 && s.anisotropy == 1);
    // each malformed form names its line
    struct BadFile {
        const char* text;
        unsigned line;
    };
    for (const BadFile& b : {BadFile{"[display]\ninternal_scale = 9\n", 2}, BadFile{"[display]\nanisotropy = 3\n", 2},
                         BadFile{"[display]\nvsync = yes\n", 2}, BadFile{"[nowhere]\n", 1}, BadFile{"[display]\npreset = fast\n", 2},
                         BadFile{"[display]\nnot_a_key = 1\n", 2}, BadFile{"[display]\nfullscreen\n", 2},
                         BadFile{"[console]\nlanguage = -1\n", 2}, BadFile{"[display\n", 1}, BadFile{"[console]\nsound_mode = 3\n", 2},
                         BadFile{"[audio]\nlatency_ms = 4\n", 2}, BadFile{"[audio]\nlatency_ms = 201\n", 2},
                         BadFile{"[display]\nlatency_ms = 30\n", 2}, BadFile{"[display]\nlock_to_display = 1\n", 2},
                         BadFile{"[controls]\na = pad:Z\n", 2}, BadFile{"[controls]\na = key:Escape\n", 2},
                         BadFile{"[controls]\nb = joystick:1\n", 2}, BadFile{"[controls]\nb = Space\n", 2},
                         BadFile{"[controls]\none = key:1,, key:2\n", 2}, BadFile{"[display]\nlook = vhs\n", 2},
                         BadFile{"[display]\nscanlines = 101\n", 2}, BadFile{"[display]\ninternal_scale = 9\n", 2}}) {
        CHECK(!parse(b.text, s, error, line) && line == b.line && !error.empty());
    }
    // what the menu saves reads back as the same settings (every key, set
    // away from its default)
    rt::Settings w;
    w.preset = "desktop", w.fullscreen = true, w.monitor = "Second Display", w.scaling = rt::Scaling::integer;
    w.smooth = false, w.internal_scale = 5, w.anisotropy = 8, w.vsync = false, w.gpu = "Some GPU", w.rumble = false;
    w.language = 4, w.sixty_hz = false, w.widescreen = false, w.progressive = false, w.sound_mode = 2;
    w.sensor_bar_above = true, w.dpd_sensitivity = 5;
    w.lock_to_display = false, w.match_refresh = false, w.audio_latency_ms = 85, w.exclusive_audio = false, w.variable_refresh = true;
    w.look = rt::Look::crt, w.scanlines = 75, w.mask = 10, w.performance_overlay = true;
    w.controls[rt::kCtlA] = {{rt::Binding::kMouse, 0, {}}, {rt::Binding::kPad, rt::kPadR3, {}}};
    w.controls[rt::kCtlHome].clear();  // nothing presses it
    w.controls[rt::kCtlB].push_back({rt::Binding::kKey, 0, "Keypad 5"});
    const std::string text = rt::settings_text(w);
    CHECK(parse(text.c_str(), s, error, line));
    CHECK(rt::settings_text(s) == text);
    CHECK(!s.lock_to_display && !s.match_refresh && s.audio_latency_ms == 85 && !s.exclusive_audio && s.variable_refresh);
    CHECK(s.look == rt::Look::crt && s.scanlines == 75 && s.mask == 10 && s.performance_overlay);
    CHECK(s.controls == w.controls && s.controls[rt::kCtlHome].empty() && s.controls[rt::kCtlB].back().key == "Keypad 5");
    CHECK(s.monitor == "Second Display" && s.scaling == rt::Scaling::integer && !s.smooth && s.internal_scale == 5 &&
          s.anisotropy == 8 && s.gpu == "Some GPU" && !s.rumble && s.language == 4 && s.dpd_sensitivity == 5);
    CHECK(parse("[display]\ninternal_scale = 0\n", s, error, line) && s.internal_scale == 0);  // 0: from the screen's height
    // an input listed twice is kept once; the default layout presses A with the right shoulder
    CHECK(parse("[controls]\none = key:1, key:1, pad:X\n", s, error, line) && s.controls[rt::kCtlOne].size() == 2);
    const rt::Settings defaults;
    CHECK(std::find(defaults.controls[rt::kCtlA].begin(), defaults.controls[rt::kCtlA].end(),
                    rt::Binding{rt::Binding::kPad, rt::kPadRB, {}}) != defaults.controls[rt::kCtlA].end());
    // the default file is the defaults
    CHECK(parse(rt::settings_text(rt::Settings{}).c_str(), s, error, line) && rt::settings_text(s) == rt::settings_text(rt::Settings{}));
    CHECK(!parse("[display]\nscaling = zoom\n", s, error, line) && line == 2);
}

}  // namespace

int main(int argc, char** argv) {
    // qilin_tests --identify-unicode <qilin>: the one test that runs the program.
    if (argc == 3 && std::string_view(argv[1]) == "--identify-unicode") {
        test_identify_unicode(argv[2]);
        if (failures) return 1;
        std::println("all tests passed");
        return 0;
    }
    test_dpl2();
    test_settings();
    test_textures();
    test_input_script();
    test_estimates();
    test_libraries();
    test_natives();
    test_sha256();
    test_dol();
    test_identify();
    test_symbols();
    test_decoder();
    test_cfg();
    test_lifter();
    test_runtime();
    test_float();
    test_paired();
    test_hwaccess();
    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("all tests passed");
    return 0;
}
