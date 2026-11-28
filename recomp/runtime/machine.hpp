// The guest machine as the runtime sets it up: the 4 GB guest address space
// with MEM1 and MEM2 committed, and the state start-up leaves behind.
// Runtime code: no std::format.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "guest.hpp"

namespace qilin::rt {

inline constexpr uint32_t kMem1 = 0x80000000u, kMem1Bytes = 24u << 20;
inline constexpr uint32_t kMem2 = 0x90000000u, kMem2Bytes = 64u << 20;

// Reserves the guest address space and commits MEM1 and MEM2, zeroed. Sets
// g_mem; false if the host refuses.
bool map_guest_memory();

// Host timers fine enough for the audio clock's 3 ms (Windows: 1 ms
// resolution; elsewhere the default already is).
void fine_timers();
// Called on each host thread that runs guest code as it starts: on Windows,
// the multimedia scheduler's "Games" class, so background work doesn't
// delay the retrace and the audio DMA.
void game_thread_started();
// The host stack of every thread that runs guest code: lifted calls nest as
// deeply as the guest's, so the systems' defaults (512 KB for a secondary
// thread on macOS, 1 MB with MSVC, 8 MB on Linux) aren't left to decide it.
// 8 MB: what the Windows player kit links for its main thread, and Linux's.
inline constexpr size_t kGuestStackBytes = size_t(8) << 20;
// Runs `fn` on a new host thread with kGuestStackBytes of stack (std::thread
// can't set one): detached, or waited for when `wait`. Throws
// std::system_error if the system refuses, as std::thread does.
void start_guest_thread(std::function<void()> fn, bool wait = false);
// Windows, started without a console (a shortcut): the log (stderr) goes to
// qilin_run.log in the game's data folder; its path, or empty if not.
std::string log_to_file_without_console();
// The display's last vblank (on the steady clock, in nanoseconds) and its
// refresh period, where the platform tells them (Windows: the compositor's
// timing); false elsewhere.
bool display_vblank(uint64_t& vblank_ns, uint64_t& period_ns);

// Guest memory for the runtime's own objects that the game's code works on
// (GX's FIFO object, VI's thread queue): a block OSInit keeps back from the
// top of the MEM1 arena. Zeroed; never freed. Stops when it runs out.
inline constexpr uint32_t kScratchBytes = 64u << 10;
void set_scratch(uint32_t lo, uint32_t hi);
uint32_t guest_alloc(uint32_t bytes, uint32_t align);

// Thrown by the native code that ends the game (PPCHalt after exit's
// destructors): not an error.
struct Exit {
    int code;
};

}  // namespace qilin::rt
