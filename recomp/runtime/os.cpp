// revolution/os and revolution/base, native: what lifted code calls of them
// (the runtime's to-do list, tools/analysis/runtime_todo.py), added as
// bring-up reaches each one. Runtime code: no std::format.
#include <cstdio>
#include <vector>

#include "coherence.hpp"
#include "dvd.hpp"
#include "interrupts.hpp"
#include "machine.hpp"
#include "native.hpp"

namespace {

using namespace qilin::rt;

// ---- What the loader hands OSInit ----
// OSInit takes the arenas from low memory, which IOS and the apploader fill
// in before the game starts; nothing in the DOL says what they put there.
//  - MEM1's arena starts where OSInit puts it when low memory gives no start
//    (0x8000310C = 0): the end of the debugger stack that follows the main
//    stack, rounded up to 32 bytes.
//  - MEM1's arena end is OSInit's own default for when neither low memory
//    (0x80003110) nor the boot information (0x80000034) gives one. A console
//    gives one (the start of the disc's file table, which the native DVD
//    code keeps outside guest memory), so this is on the small side.
//  - MEM2's arena: OSInit raises a start below 0x90000800 to that, and takes
//    the end as IOS gives it; 0x933E0000 is the documented start of IOS's own
//    heap at the top of MEM2.
// TO CONFIRM against the original game in Dolphin (the reference): dump
// 0x80000000-0x80003400 at the entry point and set these from it.
constexpr uint32_t kMem1ArenaLo = 0x803E94A0u, kMem1ArenaHi = 0x81730000u, kMem1Top = 0x81800000u;
constexpr uint32_t kMem2ArenaLo = 0x90000800u, kMem2ArenaHi = 0x933E0000u;

// The clocks the SDK's tick conversions read from low memory: the bus at
// 243 MHz (the time base runs at a quarter of it) and the CPU at 729 MHz.
// TO CONFIRM with lowmem.py, as the arenas above.
constexpr uint32_t kBusClockAddr = 0x800000F8u, kCpuClockAddr = 0x800000FCu;
constexpr uint32_t kBusClock = 243'000'000u, kCpuClock = 729'000'000u;
static_assert(kBusClock / 4 == kTimeBaseHz);

struct Os {
    bool initialised = false;
    uint32_t mem1_lo = 0, mem1_hi = 0, mem2_lo = 0, mem2_hi = 0;
    std::vector<uint32_t> shutdown_functions;  // OSRegisterShutdownFunction, for when shutdown is native
} os;

// ---- start-up and shutdown ----
QILIN_NATIVE(OSInit) {
    if (os.initialised) return;  // as the original: a second call does nothing
    os.initialised = true;
    // MEM1's arena ends where a console's apploader puts the disc's file
    // table: at the top of MEM1, rounded down to 32 bytes. The runtime's
    // guest scratch lives in that table's place (the game doesn't read the
    // table there: DVD is native), so the game's heap is the console's size.
    // Without a disc, OSInit's own default end, with the scratch below it.
    if (const uint32_t fst = disc_fst_size(); fst && fst >= kScratchBytes) {
        os.mem1_hi = (kMem1Top - fst) & ~31u;
        set_scratch(os.mem1_hi, kMem1Top);
    } else {
        os.mem1_hi = kMem1ArenaHi - kScratchBytes;
        set_scratch(os.mem1_hi, kMem1ArenaHi);
    }
    os.mem1_lo = kMem1ArenaLo;
    os.mem2_lo = kMem2ArenaLo, os.mem2_hi = kMem2ArenaHi;
    poke32(kBusClockAddr, kBusClock);
    poke32(kCpuClockAddr, kCpuClock);
    // The initialisers of the OS units that are lifted, in OSInit's order.
    call_guest(c, require_function("__OSInitAlarm"));
    call_guest(c, require_function("__OSThreadInit"));
    set_interrupts_enabled(true);  // OSInit ends with OSEnableInterrupts
}

QILIN_NATIVE(OSRegisterShutdownFunction) { os.shutdown_functions.push_back(c.r[3]); }

// The console's power and reset buttons: the game's callbacks, kept for when
// the host maps a "power" or "reset" action to them. Each setter returns the
// previous callback.
uint32_t g_power_cb = 0, g_reset_cb = 0;
QILIN_NATIVE(OSSetPowerCallback) {
    const uint32_t old = g_power_cb;
    g_power_cb = c.r[3];
    c.r[3] = old;
}
QILIN_NATIVE(OSSetResetCallback) {
    const uint32_t old = g_reset_cb;
    g_reset_cb = c.r[3];
    c.r[3] = old;
}

// base: where exit ends after the static destructors (sync; nop; li r3,0).
QILIN_NATIVE_CODE(PPCHalt, 0x7C0004ACu, 0x60000000u, 0x38600000u) { throw Exit{0}; }

// ---- interrupts ----
// Each returns whether interrupts were enabled before. Enabling them is a
// safe point: what is due is delivered there.
QILIN_NATIVE(OSDisableInterrupts) {
    c.r[3] = interrupts_enabled();
    set_interrupts_enabled(false);
}
QILIN_NATIVE(OSEnableInterrupts) {
    const bool was = interrupts_enabled();
    set_interrupts_enabled(true);
    deliver_due(c);
    c.r[3] = was;
}
QILIN_NATIVE(OSRestoreInterrupts) {
    const bool level = c.r[3] != 0, was = interrupts_enabled();
    set_interrupts_enabled(level);
    if (level) deliver_due(c);
    c.r[3] = was;
}

// ---- exceptions and the decrementer ----
QILIN_NATIVE(__OSSetExceptionHandler) { c.r[3] = set_exception_handler(c.r[3] & 0xFF, c.r[4]); }
QILIN_NATIVE(__OSGetExceptionHandler) { c.r[3] = exception_handler(c.r[3] & 0xFF); }
// mtspr DEC, r3; blr. The alarm code sets the next deadline through it.
QILIN_NATIVE_CODE(PPCMtdec, 0x7C7603A6u, 0x4E800020u) { set_decrementer(c.r[3]); }

// ---- arenas ----
QILIN_NATIVE(OSGetMEM1ArenaLo) { c.r[3] = os.mem1_lo; }
QILIN_NATIVE(OSGetMEM1ArenaHi) { c.r[3] = os.mem1_hi; }
QILIN_NATIVE(OSGetMEM2ArenaLo) { c.r[3] = os.mem2_lo; }
QILIN_NATIVE(OSGetMEM2ArenaHi) { c.r[3] = os.mem2_hi; }
QILIN_NATIVE(OSGetArenaLo) { c.r[3] = os.mem1_lo; }  // the plain arena is MEM1's
QILIN_NATIVE(OSGetArenaHi) { c.r[3] = os.mem1_hi; }
QILIN_NATIVE(OSSetMEM1ArenaLo) { os.mem1_lo = c.r[3]; }
QILIN_NATIVE(OSSetMEM1ArenaHi) { os.mem1_hi = c.r[3]; }
QILIN_NATIVE(OSSetMEM2ArenaLo) { os.mem2_lo = c.r[3]; }
QILIN_NATIVE(OSSetMEM2ArenaHi) { os.mem2_hi = c.r[3]; }
QILIN_NATIVE(OSSetArenaLo) { os.mem1_lo = c.r[3]; }
QILIN_NATIVE(OSSetArenaHi) { os.mem1_hi = c.r[3]; }

// ---- caches ----
// Guest memory has no caches behind it, so there is nothing to write back
// or invalidate. A store or flush still marks the range: on the console it
// is what makes the CPU's writes visible to the GPU, and the renderer
// re-checks its textures there (coherence.hpp). (start, bytes)
QILIN_NATIVE(DCStoreRange) { note_guest_write(c.r[3], c.r[4]); }
QILIN_NATIVE(DCFlushRange) { note_guest_write(c.r[3], c.r[4]); }
QILIN_NATIVE(DCFlushRangeNoSync) { note_guest_write(c.r[3], c.r[4]); }
QILIN_NATIVE(DCInvalidateRange) {}
QILIN_NATIVE(ICInvalidateRange) {}

// ---- the locked cache ----
// A 16 KB scratchpad at 0xE0000000 that DMA fills and empties. NW4R turns it
// on and off around its work, but no lifted code addresses it (nothing
// outside the replaced libraries builds 0xE000xxxx, and only the debugger
// stub touches the DMA registers): so there is no DMA, and an access to it
// would stop under checked memory.
[[maybe_unused]] bool g_lc_enabled = false;  // recorded only: nothing reads the locked cache
QILIN_NATIVE(LCEnable) { g_lc_enabled = true; }
QILIN_NATIVE(LCDisable) { g_lc_enabled = false; }
QILIN_NATIVE(LCQueueLength) { c.r[3] = 0; }  // no DMA transfers pending

// ---- the console and its debug output ----
// A retail console (a retail Hollywood revision; the exact value is a boot
// value lowmem.py confirms). What lifted code tests are the development-kit
// bits (0x10000000, 0x20000000), clear here, as on retail.
constexpr uint32_t kConsoleRetail = 0x00000011u;
QILIN_NATIVE(OSGetConsoleType) { c.r[3] = kConsoleRetail; }

// The debug UART (revolution/euart), which MSL's console writes to. On a
// retail console InitializeUART fails and the game's reports go nowhere.
// Here they go to stderr, as a development kit's serial port shows them: a
// deliberate difference, whose only effect the game could see is printf's
// return value, which OSReport ignores.
QILIN_NATIVE(InitializeUART) { c.r[3] = 0; }
QILIN_NATIVE(WriteUARTN) {
    const uint32_t buf = c.r[3], n = c.r[4];
    require_guest_range(buf, n, c.lr - 4, "WriteUARTN's text");
    static bool line_start = true;
    for (uint32_t i = 0; i < n; ++i) {
        const char ch = static_cast<char>(peek8(buf + i));
        if (line_start) std::fputs("[game] ", stderr);
        std::fputc(ch, stderr);
        line_start = ch == '\n';
    }
    c.r[3] = 0;
}

// MetroTRK's console write (mslsupp.c), which MSL's __TRK_write_console
// calls as __write_file(handle 1, buffer, size_t* count, idle): the debugger
// is dropped, so the text goes where the UART's does. Returns __no_io_error.
QILIN_NATIVE(__write_file) {
    require_guest_range(c.r[5], 4, c.lr - 4, "__write_file's count");
    const uint32_t buf = c.r[4], n = peek32(c.r[5]);
    require_guest_range(buf, n, c.lr - 4, "__write_file's text");
    for (uint32_t i = 0; i < n; ++i) std::fputc(static_cast<char>(peek8(buf + i)), stderr);
    c.r[3] = 0;
}

// ---- the game ending itself ----
// OSPanic(file, line, format, ...): the SDK prints and halts. The format is
// shown as written (its arguments aren't formatted), with where it came
// from, and the run stops.
QILIN_NATIVE(OSPanic) {
    const std::string file = guest_cstring(c.r[3], 256, c.lr - 4, "OSPanic's file name");
    const std::string message = guest_cstring(c.r[5], 1024, c.lr - 4, "OSPanic's message");
    std::fprintf(stderr, "qilin_run: OSPanic in %s line %u: %s\n", file.c_str(), c.r[4], message.c_str());
    throw Stop{Stop::Trap, c.lr - 4, c.r[4]};
}
// Leaving the game: to the Wii Menu, the Data Management screen, a restart,
// or off. There is no system to go to, so the run ends (status 0), as when
// the game ends itself.
[[noreturn]] void leave(const char* where) {
    std::fprintf(stderr, "qilin_run: the game %s: the run ends\n", where);
    throw Exit{0};
}
QILIN_NATIVE(OSReturnToMenu) { leave("returns to the Wii Menu"); }
QILIN_NATIVE(OSReturnToDataManager) { leave("returns to Data Management"); }
QILIN_NATIVE(OSRestart) { leave("restarts"); }
QILIN_NATIVE(OSShutdownSystem) { leave("turns the console off"); }

// ---- reports ----
// OSRegisterVersion(const char* id) reports the library's version string.
QILIN_NATIVE(OSRegisterVersion) {
    std::fprintf(stderr, "[game] %s\n", guest_cstring(c.r[3], 256, c.lr - 4, "OSRegisterVersion's text").c_str());
}

}  // namespace
