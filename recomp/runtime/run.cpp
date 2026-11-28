// qilin_run: the game.
//
//   qilin_run <extracted disc folder>     (or main.dol, until the game reads the disc)
//
// Loads the DOL into MEM1 and runs start-up as the SDK's __start does, with
// native code where the plan replaces a library (docs/code-analysis.md).
// Whatever the runtime doesn't provide yet stops the game with a report: the
// replaced function, the hardware address or the instruction, and where it
// was reached from. Exit status: 0 the game ended itself, 1 it stopped,
// 2 the runtime couldn't start, 3 the time limit (QILIN_RUN_SECONDS) ended
// it. Runtime code: no std::format.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <thread>

#include "bot.hpp"
#include "command_line.hpp"
#include "dol.hpp"
#include "dvd.hpp"
#include "host.hpp"
#include "interrupts.hpp"
#include "machine.hpp"
#include "nand.hpp"
#include "native.hpp"
#include "path.hpp"
#include "profile.hpp"
#include "threads.hpp"

using namespace qilin;

namespace {

// What __start has done by the time it calls OSInit, in the supported build
// (docs/dol-analysis.md): __init_registers' stack pointer and small-data
// bases, the 8-byte terminating stack frame, and __init_hardware's
// paired-single set-up (HID2's LSQE and PSE bits; the GQRs cleared).
// __init_data has nothing to do: its ROM copy table maps each section onto
// itself and the loader leaves BSS zeroed.
constexpr uint32_t kStackTop = 0x803E7488u, kSmallData2 = 0x8032A680u, kSmallData = 0x80328500u;
constexpr uint32_t kHid2 = 920, kHid2Lsqe = 0x80000000u, kHid2Pse = 0x20000000u;
// __start's return addresses, so the game sees the link register it would.
constexpr uint32_t kAfterOSInit = 0x80004170u, kAfterInitUser = 0x800041A8u, kAfterMain = 0x800041B4u;

// Once guest threads exist on host threads, the process ends without
// running static destructors under them.
[[noreturn]] void end(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

// QILIN_RUN_SECONDS: end the run after that long, with the counters (exit
// status 3), for runs that no longer stop on their own.
void start_time_limit() {
    const char* v = std::getenv("QILIN_RUN_SECONDS");
    if (!v || !*v) return;
    const long seconds = std::strtol(v, nullptr, 10);
    if (seconds <= 0) return;
    std::thread([seconds] {
        // The game's time: a pause for the settings menu doesn't count.
        const uint64_t until = rt::time_base() + uint64_t(seconds) * rt::kTimeBaseHz;
        while (rt::time_base() < until) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // The report runs in the game, at its next safe point (as an
        // interrupt), so it reads the threads and counters while nothing
        // changes them.
        rt::post_interrupt([seconds](rt::Context&) {
            std::fprintf(stderr, "qilin_run: time limit (%ld s) reached\n", seconds);
            rt::report_stats();
            rt::report_threads();
            end(3);
        });
        // A game stuck where interrupts can't reach it still ends; its
        // counters may be a moment stale then.
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::fprintf(stderr, "qilin_run: time limit (%ld s) reached, and the game reached no safe point for 5 s\n", seconds);
        rt::report_stats();
        end(3);
    }).detach();
}

[[noreturn]] void run_game(rt::Context& c);

// Calls a guest function as a `bl` at return_to - 4 would.
void call_from(rt::Context& c, uint32_t fn, uint32_t return_to) {
    c.lr = return_to;
    rt::call(c, fn, return_to - 4);
    if (c.ret != return_to) rt::bad_return(fn, c.ret);
}

}  // namespace

int main(int argc, char** argv) {
    // UTF-8 on Windows too (not the ANSI code page): a disc folder may have any name.
    const qilin::CommandLine command_line(argc, argv);
    argc = command_line.count();
    argv = command_line.values();
#ifndef QILIN_HEADLESS_ONLY
    // qilin_run --controllers: what SDL sees, and each button as it's
    // pressed, for 20 s: to check a controller without the game.
    if (argc == 2 && std::strcmp(argv[1], "--controllers") == 0) return rt::host::test_controllers(20) ? 0 : 2;
#endif
    if (argc != 2) {
        std::fprintf(stderr, "usage: qilin_run <extracted disc folder | main.dol>\n"
                             "       qilin_run --controllers   (lists the controllers and shows their buttons as pressed)\n");
        return 2;
    }
    rt::bot::at_end = rt::report_threads;  // a bot's run ends with where the threads are
    if (const std::string log = rt::log_to_file_without_console(); !log.empty())
        std::fprintf(stderr, "qilin_run: the log of this run (no console): %s\n", log.c_str());
    // The extracted disc's folder (Dolphin: Extract Entire Disc) gives the
    // game and its files; main.dol alone runs until the game reads the disc.
    const std::filesystem::path arg = native_path(argv[1]);
    const bool folder = std::filesystem::is_directory(arg);
    const std::filesystem::path dol_path = folder ? arg / "sys" / "main.dol" : arg;
    const auto dol = Dol::load(dol_path);
    if (!dol) {
        std::fprintf(stderr, "qilin_run: %s: %s\n", utf8_path(dol_path).c_str(), dol.error().c_str());
        return 2;
    }
    if (folder && !rt::open_disc(arg)) return 2;
    const KnownBuild* build = known_build(dol->sha256);
    if (!build) {
        std::fprintf(stderr, "qilin_run: %s is not the supported build (SHA-256 %s)\n", argv[1], dol->sha256.c_str());
        return 2;
    }
    rt::init_nand(build->disc_id);
    rt::fine_timers();
    rt::start_sampling();
    if (!rt::map_guest_memory()) {
        std::fprintf(stderr, "qilin_run: cannot map the guest's memory (4 GB of address space, 88 MB of RAM)\n");
        return 2;
    }
    std::memcpy(rt::g_mem + dol->base(), dol->image().data(), dol->image().size());
    if (!rt::bind_natives()) return 2;  // after loading: some natives check the code they replace
#ifdef QILIN_CHECKED_MEMORY
    // Bring-up: lifted code may use MEM1, low memory included (the lifted OS
    // keeps its globals there, the current thread among them), and MEM2.
    // The uncached mirrors and the hardware registers stop, so each one
    // lifted code really uses is found and then provided on purpose.
    rt::g_regions[0] = {rt::kMem1, rt::kMem1 + rt::kMem1Bytes};
    rt::g_regions[1] = {rt::kMem2, rt::kMem2 + rt::kMem2Bytes};
    rt::g_region_count = 2;
#endif
    std::fprintf(stderr, "qilin_run: %zu of %zu replaced functions have native code\n", rt::natives_bound(),
                 rt::g_native_count);

    rt::Context c;
    c.r[1] = kStackTop - 8;
    rt::poke32(c.r[1], 0xFFFFFFFFu);
    rt::poke32(c.r[1] + 4, 0xFFFFFFFFu);
    c.r[2] = kSmallData2;
    c.r[13] = kSmallData;
    c.spr[kHid2] = kHid2Lsqe | kHid2Pse;
    // With a window, the game runs on a thread of its own and the main thread
    // keeps the window (host.hpp). The boot thread's context is adopted by
    // whichever host thread runs it.
    if (rt::host::windowed()) {
#ifndef QILIN_HEADLESS_ONLY
        const bool ok = rt::host::run([&c] { run_game(c); });
        if (!ok) std::fprintf(stderr, "qilin_run: QILIN_HEADLESS=1 runs without a window\n");
        return 2;
#endif
    }
    // Not on the main thread either: its stack is whatever the linker or
    // the shell gave (1 MB with MSVC). run_game ends the process itself.
    rt::start_guest_thread([&c] { run_game(c); }, true);
    return 2;
}

namespace {

void run_game(rt::Context& c) {
    rt::game_thread_started();
    rt::set_thread_context(&c);
    if (!rt::init_threads(c)) end(2);
    start_time_limit();
    try {
        call_from(c, rt::require_function("OSInit"), kAfterOSInit);
        rt::adopt_boot_thread(&c);
        call_from(c, rt::require_function("__init_user"), kAfterInitUser);
        c.r[3] = 0;  // argc, argv: none (the disc's bi2 passes no arguments)
        c.r[4] = 0;
        call_from(c, rt::require_function("main"), kAfterMain);
        // __start branches to exit without linking: it returns nowhere.
        c.lr = kAfterMain;
        rt::call(c, rt::require_function("exit"), kAfterMain);
        std::fprintf(stderr, "qilin_run: stopped: exit returned\n");
        end(1);
    } catch (const rt::Stop& s) {
        rt::report_stop(s);
        end(1);
    } catch (const rt::Exit& e) {
        end(e.code);
    } catch (const rt::ThreadEnd&) {
        // The boot thread exited; the game goes on in its other threads,
        // and one of them ends it.
        rt::wait_forever();
    }
}

}  // namespace
