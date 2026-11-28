// Guest threads on host threads (docs/runtime.md). The SDK's thread, mutex
// and message code is lifted; this is the switch beneath it.
//
// SelectThread is native, following the supported build's own code step by
// step: the same run queues, priorities, reschedule flag and switch
// callback. Only its context save and load are different: each guest thread
// runs on its own host thread, which keeps the guest registers in its own
// rt::Context, and a switch hands a run token from one host thread to the
// next. One guest thread runs at a time, as on the Wii's single core.
//
// SelectThread's globals (the run queues, the bitmask of non-empty queues,
// the reschedule flag, the switch callback, the idle context) are read out
// of its own instructions at start-up, and the runtime refuses to start if
// they aren't where the supported build has them. Runtime code: no
// std::format.
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#include "interrupts.hpp"
#include "machine.hpp"
#include "native.hpp"
#include "threads.hpp"

namespace qilin::rt {

namespace {

// OSThread, as the supported build's thread code uses it: the saved context
// first, then the scheduler's fields.
constexpr uint32_t kCtxCr = 128, kCtxLr = 132, kCtxCtr = 136, kCtxXer = 140, kCtxFpr = 144, kCtxSrr0 = 408,
                   kCtxSrr1 = 412, kCtxMode = 416, kCtxState = 418, kCtxGqr = 420, kCtxPs1 = 456;
constexpr uint32_t kState = 712, kPriority = 720, kQueue = 732, kNext = 736, kPrev = 740;
constexpr uint16_t kReady = 1, kRunning = 2, kMoribund = 8;
constexpr uint16_t kCtxStateExc = 2;  // context state bit SelectThread tests before saving
constexpr uint32_t kSrr1Fp = 0x2000;  // MSR[FP] in a saved SRR1
constexpr uint32_t kMsrEe = 0x8000;   // MSR[EE]: interrupts enabled
// Low memory, as the OS keeps it.
constexpr uint32_t kCurrentContextPhys = 0x800000C0u, kCurrentContext = 0x800000D4u, kFpuContext = 0x800000D8u,
                   kCurrentThread = 0x800000E4u;

struct Globals {
    uint32_t select = 0;      // SelectThread itself
    uint32_t reschedule = 0;  // > 0: switching is off (OSDisableScheduler)
    uint32_t hint = 0;        // a thread became ready: reschedule when allowed
    uint32_t bits = 0;        // bit 31 - p: run queue p isn't empty
    uint32_t callback = 0;    // the switch callback (old, new)
    uint32_t run_queue = 0;   // 32 x {head, tail}, by priority
    uint32_t idle = 0;        // the context the idle loop runs on
} g;

uint32_t ld(uint32_t a) { return peek32(a); }

// ---- the run token ----
struct Host {
    uint32_t gen = 0;              // the context generation this host thread runs
    const Context* ctx = nullptr;  // its registers, for report_threads
};
std::mutex g_lock;
// Each guest thread's host thread waits on its own condition variable, so a
// switch wakes only the thread that runs next (one shared one woke every
// waiting thread on every switch, only for all but one to sleep again).
// std::map keeps each variable where it is.
std::map<uint32_t, std::condition_variable> g_turn;
uint32_t g_running = 0;                // the guest thread whose host thread may run
std::map<uint32_t, uint32_t> g_gen;    // guest thread -> context generation (OSInitContext bumps it)
std::map<uint32_t, Host> g_hosts;      // guest thread -> its host thread
Context g_template;                    // special registers new threads start with
thread_local uint32_t t_self = 0, t_gen = 0;

// Waits until this host thread's guest thread is chosen again. A host
// thread whose context was re-initialised for a new thread ends instead.
void wait_turn() {
    std::unique_lock l(g_lock);
    g_turn[t_self].wait(l, [] { return g_gen[t_self] != t_gen || g_running == t_self; });
    if (g_gen[t_self] != t_gen) throw ThreadEnd{};
}

[[noreturn]] void end_process(int code) {
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

// A new guest thread: its registers from the context OSCreateThread filled
// in, then its entry point; when that returns, OSExitThread, as the link
// register OSCreateThread set sends it there.
void host_main(uint32_t t, uint32_t gen) {
    t_self = t, t_gen = gen;
    game_thread_started();
    try {
        wait_turn();
        Context c = g_template;
        set_thread_context(&c);
        {
            std::lock_guard l(g_lock);
            if (auto it = g_hosts.find(t); it != g_hosts.end()) it->second.ctx = &c;
        }
        for (unsigned i = 0; i < 32; ++i) {
            c.r[i] = ld(t + 4 * i);
            c.f[i] = dbl(uint64_t(ld(t + kCtxFpr + 8 * i)) << 32 | ld(t + kCtxFpr + 8 * i + 4));
            c.ps1[i] = dbl(uint64_t(ld(t + kCtxPs1 + 8 * i)) << 32 | ld(t + kCtxPs1 + 8 * i + 4));
        }
        for (unsigned i = 0; i < 8; ++i) c.spr[912 + i] = ld(t + kCtxGqr + 4 * i);
        c.cr = ld(t + kCtxCr), c.lr = ld(t + kCtxLr), c.ctr = ld(t + kCtxCtr), c.xer = ld(t + kCtxXer);
        // The thread starts with the interrupt enable its saved MSR gives it.
        set_interrupts_enabled((ld(t + kCtxSrr1) & kMsrEe) != 0);
        const uint32_t entry = ld(t + kCtxSrr0), exit_to = c.lr;
        call(c, entry, 0);
        if (c.ret != exit_to) bad_return(entry, c.ret);
        call(c, exit_to, 0);  // OSExitThread, with the entry's result in r3
        throw Stop{Stop::Unsupported, exit_to, t};  // OSExitThread came back
    } catch (const ThreadEnd&) {
        // Its context (a local here) is gone: reports mustn't read it.
        std::lock_guard l(g_lock);
        if (auto it = g_hosts.find(t); it != g_hosts.end() && it->second.gen == gen) it->second.ctx = nullptr;
        return;
    } catch (const Stop& s) {
        report_stop(s);
        end_process(1);
    } catch (const Exit& e) {
        end_process(e.code);
    }
}

bool dead(uint32_t t) {
    const uint16_t state = peek16(t + kState);
    return state == 0 || state == kMoribund;
}

// OSLoadContext's part: run `next`, and come back when this thread is chosen
// again (as OSSaveContext returning 1 in the original).
void switch_to(uint32_t next) {
    if (next == t_self) return;
    ++stats().switches;
    bool self_dead;
    {
        std::lock_guard l(g_lock);
        // Whether this thread is ending is read before the run token goes:
        // after, the next thread may already be rewriting its OSThread.
        self_dead = dead(t_self);
        if (self_dead) g_hosts.erase(t_self);
        const uint32_t gen = g_gen[next];
        auto it = g_hosts.find(next);
        if (it == g_hosts.end() || it->second.gen != gen) {
            g_hosts[next] = Host{gen};
            ++stats().threads_started;
            start_guest_thread([next, gen] { host_main(next, gen); });
        }
        g_running = next;
        // Under the lock: the variable's map node is stable, and the next
        // thread can't miss it (it tests g_running under this lock first).
        g_turn[next].notify_one();
    }
    if (self_dead) throw ThreadEnd{};
    wait_turn();
}

// Calls a lifted function from native code, keeping the caller's link
// register (lifted functions return to it through c.ret).
void guest_call(Context& c, uint32_t fn, uint32_t r3, uint32_t r4) {
    c.r[3] = r3, c.r[4] = r4;
    call_guest(c, fn);
}

void set_current_context(uint32_t ctx) {
    poke32(kCurrentContext, ctx);
    poke32(kCurrentContextPhys, ctx & 0x3FFFFFFFu);
    const uint32_t srr1 = ld(ctx + kCtxSrr1);
    poke32(ctx + kCtxSrr1, ld(kFpuContext) == ctx ? srr1 | kSrr1Fp : srr1 & ~kSrr1Fp);
}

void enqueue_tail(uint32_t t) {
    const uint32_t queue = g.run_queue + 8 * ld(t + kPriority);
    poke32(t + kQueue, queue);
    const uint32_t tail = ld(queue + 4);
    if (tail) poke32(tail + kNext, t);
    else poke32(queue, t);
    poke32(t + kPrev, tail);
    poke32(t + kNext, 0);
    poke32(queue + 4, t);
    poke32(g.bits, ld(g.bits) | 1u << (31 - ld(t + kPriority)));
    poke32(g.hint, 1);
}

uint32_t select_thread(Context& c, bool yield) {
    if (static_cast<int32_t>(ld(g.reschedule)) > 0) return 0;
    const uint32_t cur = ld(kCurrentThread);
    if (ld(kCurrentContext) != cur) return 0;  // in an interrupt handler's context
    if (cur) {
        if (peek16(cur + kState) == kRunning) {
            if (!yield) {
                const auto best = static_cast<int32_t>(std::countl_zero(ld(g.bits)));
                if (static_cast<int32_t>(ld(cur + kPriority)) <= best) return 0;
            }
            poke16(cur + kState, kReady);
            enqueue_tail(cur);
        }
        // The original saves the context unless the thread is inside an
        // exception, in which case it never comes back here. Interrupts
        // aren't delivered that way by this runtime.
        if (peek16(cur + kCtxState) & kCtxStateExc) throw Stop{Stop::Unsupported, g.select, cur};
    }
    if (ld(g.bits) == 0) {
        // Idle, as the original: no current thread, the idle context, and
        // interrupts on until one makes a thread ready. If nothing could
        // ever do that, the game is stuck: stop and say so.
        guest_call(c, ld(g.callback), cur, 0);
        poke32(kCurrentThread, 0);
        set_current_context(g.idle);
        set_interrupts_enabled(true);
        while (ld(g.bits) == 0)
            if (!wait_and_deliver(c)) throw Stop{Stop::Idle, g.select, cur};
        set_interrupts_enabled(false);
        static uint32_t clear = 0;
        if (!clear) clear = require_function("OSClearContext");
        guest_call(c, clear, g.idle, 0);
    }
    poke32(g.hint, 0);
    const auto p = static_cast<uint32_t>(std::countl_zero(ld(g.bits)));
    const uint32_t queue = g.run_queue + 8 * p, next = ld(queue), after = ld(next + kNext);
    if (after) poke32(after + kPrev, 0);
    else poke32(queue + 4, 0);
    poke32(queue, after);
    if (!after) poke32(g.bits, ld(g.bits) & ~(1u << (31 - p)));
    poke32(next + kQueue, 0);
    poke16(next + kState, kRunning);
    guest_call(c, ld(g.callback), ld(kCurrentThread), next);
    poke32(kCurrentThread, next);
    set_current_context(next);
    switch_to(next);
    return 0;
}

// ---- SelectThread's globals, from its instructions ----
// D-form: opcode, rD/rS, rA, signed 16-bit displacement.
bool dform(uint32_t w, unsigned op, unsigned rd, unsigned ra, int32_t& d) {
    if (w >> 26 != op || ((w >> 21) & 31) != rd || ((w >> 16) & 31) != ra) return false;
    d = static_cast<int16_t>(w & 0xFFFF);
    return true;
}
constexpr unsigned kLwz = 32, kStw = 36, kAddi = 14, kAddis = 15;

}  // namespace

bool init_threads(const Context& boot) {
    g_template = boot;
    g.select = symbol_address("SelectThread");
    if (!g.select) {
        std::fprintf(stderr, "qilin_run: no function named SelectThread\n");
        return false;
    }
    const uint32_t r13 = boot.r[13];
    auto word = [](uint32_t off) { return ld(g.select + off); };
    int32_t d[8];
    const bool ok = dform(word(0x18), kLwz, 0, 13, d[0])          // lwz r0, Reschedule(r13)
                    && dform(word(0x64), kLwz, 3, 13, d[1])       // lwz r3, RunQueueBits(r13)
                    && dform(word(0x88), kAddis, 3, 0, d[2])      // lis r3, RunQueue@ha
                    && dform(word(0x90), kAddi, 3, 3, d[3])       // addi r3, r3, RunQueue@l
                    && dform(word(0xE8), kStw, 3, 13, d[4])       // stw r3, RunQueueHint(r13)
                    && dform(word(0x11C), kLwz, 12, 13, d[5])     // lwz r12, SwitchThreadCallback(r13)
                    && dform(word(0x138), kAddis, 3, 0, d[6])     // lis r3, IdleContext@ha
                    && dform(word(0x140), kAddi, 3, 3, d[7]);     // addi r3, r3, IdleContext@l
    if (!ok) {
        std::fprintf(stderr, "qilin_run: SelectThread (%08X) isn't the supported build's\n", g.select);
        return false;
    }
    g.reschedule = r13 + static_cast<uint32_t>(d[0]);
    g.bits = r13 + static_cast<uint32_t>(d[1]);
    g.run_queue = static_cast<uint32_t>(d[2]) * 0x10000u + static_cast<uint32_t>(d[3]);
    g.hint = r13 + static_cast<uint32_t>(d[4]);
    g.callback = r13 + static_cast<uint32_t>(d[5]);
    g.idle = static_cast<uint32_t>(d[6]) * 0x10000u + static_cast<uint32_t>(d[7]);
    return true;
}

void adopt_boot_thread(const Context* c) {
    std::lock_guard l(g_lock);
    t_self = ld(kCurrentThread);
    t_gen = g_gen[t_self];
    g_hosts[t_self] = Host{t_gen, c};
    g_running = t_self;
}

void report_threads() {
    std::lock_guard l(g_lock);
    std::fprintf(stderr, "qilin_run: scheduler: current thread %08X, run queue bits %08X, reschedule %d, hint %u\n",
                 ld(kCurrentThread), ld(g.bits), static_cast<int32_t>(ld(g.reschedule)), ld(g.hint));
    for (const auto& [t, h] : g_hosts) {
        std::fprintf(stderr, "qilin_run: guest thread %08X: state %u, priority %u, queue %08X%s\n", t, peek16(t + kState),
                     ld(t + kPriority), ld(t + kQueue), t == g_running ? " (its host thread has the run token; stack as last synced)" : "");
        if (h.ctx) report_stack(*h.ctx);
    }
}

void wait_forever() {
    static std::condition_variable never;  // nothing notifies it
    std::unique_lock l(g_lock);
    for (;;) never.wait(l);
}

QILIN_NATIVE(SelectThread) { c.r[3] = select_thread(c, c.r[3] != 0); }

QILIN_NATIVE(OSSetCurrentContext) { set_current_context(c.r[3]); }

// On the console it loads a context and never returns. Lifted code calls it
// only to end an interrupt handler (the decrementer callback): that unwinds
// to the delivery, which puts the interrupted registers back. Anywhere else
// it would be a context switch this runtime doesn't make: stop.
QILIN_NATIVE(OSLoadContext) {
    if (!delivering()) throw Stop{Stop::Unsupported, c.lr - 4, c.r[3]};
    throw Resume{};
}

// OSInitContext(context, entry, stack), as the supported build's: the entry
// in SRR0, the stack, the small-data bases, everything else cleared, then
// OSClearContext. It also starts a new generation of the context, so a host
// thread still waiting on the old one ends instead of resuming.
QILIN_NATIVE(OSInitContext) {
    const uint32_t ctx = c.r[3];
    poke32(ctx + kCtxSrr0, c.r[4]);
    poke32(ctx + 4, c.r[5]);
    poke32(ctx + kCtxSrr1, 0x9032u);
    poke32(ctx + kCtxCr, 0);
    poke32(ctx + kCtxXer, 0);
    poke32(ctx + 8, c.r[2]);
    poke32(ctx + 52, c.r[13]);
    for (unsigned r = 3; r < 32; ++r)
        if (r != 13) poke32(ctx + 4 * r, 0);
    for (unsigned i = 0; i < 8; ++i) poke32(ctx + kCtxGqr + 4 * i, 0);
    poke16(ctx + kCtxMode, 0);
    poke16(ctx + kCtxState, 0);
    if (ld(kFpuContext) == ctx) poke32(kFpuContext, 0);
    {
        // A host thread still waiting on the old generation wakes and ends.
        std::lock_guard l(g_lock);
        ++g_gen[ctx];
        g_turn[ctx].notify_all();
    }
}

}  // namespace qilin::rt
