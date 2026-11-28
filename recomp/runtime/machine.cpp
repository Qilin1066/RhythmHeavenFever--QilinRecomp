#include "machine.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "path.hpp"
#include "settings.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <avrt.h>
#include <process.h>
#include <dwmapi.h>
#include <timeapi.h>

#include <chrono>
#else
#include <pthread.h>
#include <sys/mman.h>
#endif

namespace qilin::rt {

void fine_timers() {
#if defined(_WIN32)
    // Windows rounds short waits up to its 15.6 ms tick by default; the
    // audio clock needs 3 ms (the setting lasts until the process exits).
    timeBeginPeriod(1);
    // Windows 11 throttles a process whose window is hidden, minimised or
    // covered: slower cores (EcoQoS) and its timer request ignored. The
    // game's clock keeps running whether it's seen or not, so the audio
    // would break up. Opt out of both.
    PROCESS_POWER_THROTTLING_STATE p{};
    p.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    p.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    p.StateMask = 0;  // neither
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &p, sizeof p);  // older Windows: refused, harmless
#endif
}

bool display_vblank(uint64_t& vblank_ns, uint64_t& period_ns) {
#if defined(_WIN32)
    // The compositor's timing: the last vblank and the refresh period, on
    // the performance counter. Read with the counter's present value and the
    // steady clock's, so the vblank lands on the steady clock however the
    // C++ library scales the counter.
    DWM_TIMING_INFO ti{};
    ti.cbSize = sizeof ti;
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &ti)) || ti.qpcRefreshPeriod == 0) return false;
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    const auto steady = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    const double ns_per_tick = 1e9 / double(freq.QuadPart);
    const double ago = double(int64_t(now.QuadPart) - int64_t(ti.qpcVBlank)) * ns_per_tick;  // negative: a vblank ahead
    vblank_ns = uint64_t(int64_t(steady) - int64_t(ago));
    period_ns = uint64_t(double(ti.qpcRefreshPeriod) * ns_per_tick);
    return period_ns > 0;
#else
    (void)vblank_ns, (void)period_ns;
    return false;
#endif
}

std::string log_to_file_without_console() {
#if defined(_WIN32)
    // Started from a shortcut or Explorer there is no console: stderr goes
    // nowhere, and a stuck or stopped game leaves nothing to read. Then the
    // log goes to qilin_run.log in the game's data folder (a new one each run).
    const HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return {};
    const std::filesystem::path dir = game_data_dir();
    if (dir.empty()) return {};
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path path = dir / "qilin_run.log";
    if (!_wfreopen(path.c_str(), L"w", stderr)) return {};
    std::setvbuf(stderr, nullptr, _IONBF, 0);  // every line reaches the file, a hang or a crash included
    return qilin::utf8_path(path);
#else
    return {};
#endif
}

void game_thread_started() {
#if defined(_WIN32)
    // The multimedia scheduler's "Games" class: the game's threads aren't
    // held back behind background work, so the retrace and the audio DMA
    // run when due. Each guest thread has its own host thread, so each asks.
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Games", &task);  // without the service: no change
#endif
}

namespace {
#if defined(_WIN32)
unsigned __stdcall guest_thread_entry(void* arg) {
#else
void* guest_thread_entry(void* arg) {
#endif
    const std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(arg));
    (*fn)();
    return {};
}
}  // namespace

void start_guest_thread(std::function<void()> fn, bool wait) {
    auto arg = std::make_unique<std::function<void()>>(std::move(fn));
#if defined(_WIN32)
    // The size as the reservation, as the linker's /STACK (--stack) sets one.
    const uintptr_t h = _beginthreadex(nullptr, unsigned(kGuestStackBytes), guest_thread_entry, arg.get(),
                                       STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) throw std::system_error(errno, std::generic_category(), "cannot start a guest thread");
    arg.release();  // the thread owns it now
    if (wait) WaitForSingleObject(reinterpret_cast<HANDLE>(h), INFINITE);
    CloseHandle(reinterpret_cast<HANDLE>(h));
#else
    pthread_attr_t attr;
    if (const int e = pthread_attr_init(&attr)) throw std::system_error(e, std::generic_category(), "pthread_attr_init");
    int e = pthread_attr_setstacksize(&attr, kGuestStackBytes);
    pthread_t t{};
    if (!e) e = pthread_create(&t, &attr, guest_thread_entry, arg.get());
    pthread_attr_destroy(&attr);
    if (e) throw std::system_error(e, std::generic_category(), "cannot start a guest thread");
    arg.release();  // the thread owns it now
    if (wait) pthread_join(t, nullptr);
    else pthread_detach(t);
#endif
}

bool map_guest_memory() {
    constexpr uint64_t kSpace = 1ull << 32;
#if defined(_WIN32)
    // Windows has no overcommit: reserve the space, commit only the RAM.
    auto* base = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSpace, MEM_RESERVE, PAGE_NOACCESS));
    if (!base) return false;
    if (!VirtualAlloc(base + kMem1, kMem1Bytes, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(base + kMem2, kMem2Bytes, MEM_COMMIT, PAGE_READWRITE))
        return false;
#else
    void* p = mmap(nullptr, kSpace, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return false;
    auto* base = static_cast<uint8_t*>(p);
    if (mprotect(base + kMem1, kMem1Bytes, PROT_READ | PROT_WRITE) != 0 ||
        mprotect(base + kMem2, kMem2Bytes, PROT_READ | PROT_WRITE) != 0)
        return false;
#endif
    g_mem = base;
    return true;
}

namespace {
uint32_t g_scratch_next = 0, g_scratch_end = 0;
}

void set_scratch(uint32_t lo, uint32_t hi) { g_scratch_next = lo, g_scratch_end = hi; }

uint32_t guest_alloc(uint32_t bytes, uint32_t align) {
    const uint32_t at = (g_scratch_next + align - 1) & ~(align - 1);
    if (!g_scratch_end || at + bytes > g_scratch_end) {
        std::fprintf(stderr, "qilin_run: the runtime's guest scratch (%u bytes) is used up\n", kScratchBytes);
        throw Stop{Stop::Unsupported, 0, bytes};
    }
    g_scratch_next = at + bytes;
    std::memset(g_mem + at, 0, bytes);
    return at;
}

}  // namespace qilin::rt
