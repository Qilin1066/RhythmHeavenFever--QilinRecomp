// revolution/dvd, native: the game's files from an extracted disc
// (docs/runtime.md). The disc's own file table (sys/fst.bin) gives the
// entry numbers and directories, so they are the console's; the contents
// come from files/. Reads complete at once; an asynchronous read's callback
// arrives as an interrupt, after the call has returned, as on the console.
// Runtime code: no std::format.
#include "dvd.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coherence.hpp"
#include "interrupts.hpp"
#include "native.hpp"
#include "path.hpp"

namespace qilin::rt {

namespace {

// DVDFileInfo, as the game uses it (a 60-byte structure, the length at
// +0x34): a command block, then the file's place and the user's callback.
constexpr uint32_t kCbState = 0x0C, kCbTransferred = 0x20, kStart = 0x30, kLength = 0x34, kCallback = 0x38;
// Command block states and results (the SDK's values).
constexpr uint32_t kStateEnd = 0, kStateBusy = 1, kStateCanceled = 10;
constexpr int32_t kResultCanceled = -3, kResultFatal = -1;

struct Entry {
    bool dir = false;
    std::string name;  // the last component
    std::string path;  // under files/, with '/'
    uint32_t disc_offset = 0, size = 0;  // files
    uint32_t parent = 0, next = 0;       // directories: parent, one past the last child
};

struct Disc {
    bool open = false;
    uint32_t fst_size = 0;
    std::filesystem::path root;
    std::vector<Entry> entries;
    std::map<uint32_t, uint32_t> by_offset;  // a file's disc offset -> its entry (non-empty files)
} disc;

uint32_t be32(const std::vector<uint8_t>& b, size_t o) {
    return uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3];
}

char lower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }
bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

std::string guest_string(const Context& c, uint32_t addr) { return guest_cstring(addr, 1024, c.lr - 4, "DVD: a path"); }

[[noreturn]] void no_disc(uint32_t pc) {
    std::fprintf(stderr, "qilin_run: the game reads the disc: run qilin_run on the extracted disc's folder "
                         "(Dolphin: Extract Entire Disc), not on main.dol alone\n");
    throw Stop{Stop::Unsupported, pc, 0};
}

// The SDK's lookup: from the root for an absolute path (the current
// directory is always the root here: nothing the game lifts changes it);
// "." and ".." as usual; names compared without case.
int32_t entry_of(const std::string& path) {
    uint32_t dir = 0;
    size_t i = 0;
    while (i <= path.size()) {
        const size_t slash = path.find('/', i);
        const std::string part = path.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        const bool last = slash == std::string::npos;
        i = last ? path.size() + 1 : slash + 1;
        if (part.empty() || part == ".") {
            if (last) return static_cast<int32_t>(dir);
            continue;
        }
        if (part == "..") {
            dir = disc.entries[dir].parent;
            if (last) return static_cast<int32_t>(dir);
            continue;
        }
        uint32_t found = 0;
        for (uint32_t k = dir + 1; k < disc.entries[dir].next;) {
            if (same_name(disc.entries[k].name, part)) {
                found = k;
                break;
            }
            k = disc.entries[k].dir ? disc.entries[k].next : k + 1;
        }
        if (!found) return -1;
        if (last) return static_cast<int32_t>(found);
        if (!disc.entries[found].dir) return -1;  // a file used as a directory
        dir = found;
    }
    return -1;
}

// QILIN_DVD_TRACE=1: each file opened and each read, on stderr (what the
// game loaded last, when it waits for something).
bool tracing() {
    static const bool on = [] {
        const char* v = std::getenv("QILIN_DVD_TRACE");
        return v && *v && *v != '0';
    }();
    return on;
}

bool fast_open(int32_t entry, uint32_t fi) {
    if (entry < 0 || static_cast<size_t>(entry) >= disc.entries.size() || disc.entries[entry].dir) return false;
    const Entry& e = disc.entries[entry];
    if (tracing()) std::fprintf(stderr, "[dvd %.3f] open %s (%u bytes) -> %08X\n", double(time_base()) / kTimeBaseHz, e.path.c_str(), e.size, fi);
    poke32(fi + kStart, e.disc_offset);
    poke32(fi + kLength, e.size);
    poke32(fi + kCallback, 0);
    poke32(fi + kCbState, kStateEnd);
    ++stats().files_opened;
    return true;
}

// Reads a file's bytes into guest memory. The file is found as the SDK
// finds it, by the disc offset its open stored in the file info. Bytes past
// the file's end (the game rounds lengths up to 32) are zero: on the
// console they would be whatever follows on the disc.
bool read(uint32_t fi, uint32_t addr, uint32_t length, uint32_t offset) {
    const auto it = disc.by_offset.find(peek32(fi + kStart));
    if (it == disc.by_offset.end()) {
        std::fprintf(stderr, "qilin_run: a read from %08X, which no file of the disc starts at\n", peek32(fi + kStart));
        return false;
    }
    const Entry& e = disc.entries[it->second];
    if (tracing())
        std::fprintf(stderr, "[dvd %.3f] read %s at %u, %u bytes -> %08X (block %08X)\n", double(time_base()) / kTimeBaseHz,
                     e.path.c_str(), offset, length, addr, fi);
    if (!guest_range_ok(addr, length)) {
        std::fprintf(stderr, "qilin_run: a read of %u bytes to %08X, outside guest memory\n", length, addr);
        return false;
    }
    std::memset(g_mem + addr, 0, length);
    note_guest_write(addr, length);  // DMA: the GPU sees it without a flush
    if (offset >= e.size) return true;
    const uint32_t n = std::min(length, e.size - offset);
    std::ifstream f(disc.root / "files" / std::filesystem::path(e.path), std::ios::binary);
    if (!f.seekg(offset) || !f.read(reinterpret_cast<char*>(g_mem + addr), n)) {
        std::fprintf(stderr, "qilin_run: cannot read %s (%u bytes at %u)\n", e.path.c_str(), n, offset);
        return false;
    }
    stats().bytes_read += n;
    return true;
}

// Asynchronous requests not yet completed, by command block, each with its
// request's number: a completion finds its own request still there, or
// does nothing (it was cancelled and finished, or the block reused).
std::map<uint32_t, uint64_t> g_pending;
std::set<uint32_t> g_cancelled;  // cancelled asynchronously: the completion reports it
uint64_t g_next_request = 1;

// The drive's time. An asynchronous read finishes after the time the
// console's drive would take: one request at a time, each an access time
// plus its bytes at the drive's rate. The game depends on it: its file
// manager (CFileManager) frees a file slot when a read completes, but an
// archive keeps the slot's address until it has been decompressed, a frame
// or more later; a read on the reused slot that completes before then is
// matched to the older archive, and the newer one is never marked read. With
// reads finishing in 1 ms that happened at the first remix (four games'
// archives on five slots): a black screen for good (found 2026-11-21).
// APPROX: no measurement of the Wii's drive is at hand; the figures are of
// the order of a DVD drive's (megabytes a second, milliseconds an access),
// and leave a read of a large archive well past a frame, as on the console.
// Synchronous reads still return at once (their thread would wait either way).
constexpr uint64_t kDriveAccess = kTimeBaseHz / 100;            // 10 ms a request
constexpr uint64_t kDriveBytesPerSecond = 8 * 1024 * 1024;      // 8 MB/s
uint64_t g_drive_free = 0;                                       // when the drive finishes what it has (time base)

uint64_t drive_time(uint32_t bytes) {
    const uint64_t now = time_base();
    const uint64_t start = std::max(now, g_drive_free);
    g_drive_free = start + kDriveAccess + uint64_t(bytes) * kTimeBaseHz / kDriveBytesPerSecond;
    return g_drive_free - now;
}

void complete(uint32_t fi, int32_t result, uint32_t callback, uint32_t bytes) {
    const uint64_t request = g_next_request++;
    g_pending[fi] = request;
    stats().dvd_pending = g_pending.size();
    g_cancelled.erase(fi);
    post_interrupt([fi, result, callback, request](Context& c) {
        const auto it = g_pending.find(fi);
        if (tracing())
            std::fprintf(stderr, "[dvd %.3f] completion of block %08X: %s, callback %08X\n", double(time_base()) / kTimeBaseHz, fi,
                         it == g_pending.end() || it->second != request ? "nothing pending (cancelled or reused)" : "delivered",
                         callback);
        if (it == g_pending.end() || it->second != request) return;
        g_pending.erase(it);
        stats().dvd_pending = g_pending.size();
        const bool cancelled = g_cancelled.erase(fi) != 0;
        poke32(fi + kCbState, cancelled ? kStateCanceled : kStateEnd);
        if (!cancelled && result >= 0) poke32(fi + kCbTransferred, static_cast<uint32_t>(result));
        if (!callback) return;
        c.r[3] = static_cast<uint32_t>(cancelled ? kResultCanceled : result);
        c.r[4] = fi;
        call_guest(c, callback);
    }, drive_time(bytes));
}

// A synchronous cancel (DVDCancel, and DVDClose's): as the SDK's, the request
// is finished before it returns, its callback run with DVD_RESULT_CANCELED,
// so the game may free the block right after. Its queued completion then
// finds nothing to do.
void cancel_now(Context& c, uint32_t block) {
    if (!g_pending.erase(block)) return;
    stats().dvd_pending = g_pending.size();
    g_cancelled.erase(block);
    poke32(block + kCbState, kStateCanceled);
    const uint32_t callback = peek32(block + kCallback);
    if (!callback) return;
    const uint32_t r3 = c.r[3], r4 = c.r[4];
    c.r[3] = static_cast<uint32_t>(kResultCanceled), c.r[4] = block;
    call_guest(c, callback);
    c.r[3] = r3, c.r[4] = r4;
}

void cancel_later(uint32_t block) {
    if (g_pending.contains(block)) g_cancelled.insert(block);
}

// __DVDTestAlarm(alarm): whether an OS alarm is one of DVD's own (OSAlarm's
// reset handling leaves those alone). The native DVD sets no OS alarms, so
// none is.
QILIN_NATIVE(__DVDTestAlarm) { c.r[3] = 0; }

QILIN_NATIVE(DVDInit) {
    if (!disc.open) no_disc(c.lr - 4);
}

QILIN_NATIVE(DVDConvertPathToEntrynum) { c.r[3] = static_cast<uint32_t>(entry_of(guest_string(c, c.r[3]))); }

QILIN_NATIVE(DVDFastOpen) { c.r[3] = fast_open(static_cast<int32_t>(c.r[3]), c.r[4]) ? 1 : 0; }

QILIN_NATIVE(DVDOpen) {
    const std::string path = guest_string(c, c.r[3]);
    const int32_t entry = entry_of(path);
    if (entry < 0) std::fprintf(stderr, "[dvd] %s: not on the disc\n", path.c_str());  // the SDK warns too
    c.r[3] = fast_open(entry, c.r[4]) ? 1 : 0;
}

// DVDClose(fileInfo): the SDK's cancels any request still pending on it.
QILIN_NATIVE(DVDClose) {
    cancel_now(c, c.r[3]);
    c.r[3] = 1;
}

// DVDReadPrio(fileInfo, addr, length, offset, prio): the bytes read, or a
// negative result.
QILIN_NATIVE(DVDReadPrio) {
    c.r[3] = read(c.r[3], c.r[4], c.r[5], c.r[6]) ? c.r[5] : static_cast<uint32_t>(kResultFatal);
}

// DVDReadAsyncPrio(fileInfo, addr, length, offset, callback, prio): TRUE;
// the callback gets (result, fileInfo) later.
QILIN_NATIVE(DVDReadAsyncPrio) {
    const uint32_t fi = c.r[3], callback = c.r[7];
    const bool ok = read(fi, c.r[4], c.r[5], c.r[6]);
    poke32(fi + kCallback, callback);
    poke32(fi + kCbState, kStateBusy);
    complete(fi, ok ? static_cast<int32_t>(c.r[5]) : kResultFatal, callback, c.r[5]);
    c.r[3] = 1;
}

// The drive: busy while a request waits for its completion, else idle.
QILIN_NATIVE(DVDGetDriveStatus) { c.r[3] = g_pending.empty() ? kStateEnd : kStateBusy; }

// DVDCancel(block): 0 once cancelled, the request finished (cancel_now).
QILIN_NATIVE(DVDCancel) {
    cancel_now(c, c.r[3]);
    c.r[3] = 0;
}
// DVDCancelAsync(block, callback): the pending completion reports the
// cancel, then the cancel's own callback runs.
QILIN_NATIVE(DVDCancelAsync) {
    const uint32_t block = c.r[3], callback = c.r[4];
    cancel_later(block);
    if (callback)
        post_interrupt([block, callback](Context& ctx) {
            ctx.r[3] = 0;
            ctx.r[4] = block;
            call_guest(ctx, callback);
        }, kIoLatency);
    c.r[3] = 1;
}
QILIN_NATIVE(DVDCancelAllAsync) {
    const uint32_t callback = c.r[3];
    for (const auto& pending : g_pending) g_cancelled.insert(pending.first);
    if (callback)
        post_interrupt([callback](Context& ctx) {
            ctx.r[3] = 0;
            ctx.r[4] = 0;
            call_guest(ctx, callback);
        }, kIoLatency);
    c.r[3] = 1;
}

}  // namespace

bool open_disc(const std::filesystem::path& root) {
    std::ifstream f(root / "sys" / "fst.bin", std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "qilin_run: %s has no sys/fst.bin: not an extracted disc\n", qilin::utf8_path(root).c_str());
        return false;
    }
    const std::vector<uint8_t> b{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (b.size() < 12 || (be32(b, 0) >> 24) != 1) {
        std::fprintf(stderr, "qilin_run: sys/fst.bin doesn't start with the root directory\n");
        return false;
    }
    const uint32_t count = be32(b, 8);
    const size_t strings = size_t(count) * 12;
    if (count == 0 || strings > b.size()) {
        std::fprintf(stderr, "qilin_run: sys/fst.bin is malformed (%u entries)\n", count);
        return false;
    }
    disc.entries.assign(count, Entry{});
    disc.entries[0] = Entry{true, "", "", 0, 0, 0, count};
    // Directories end where their `next` says; a stack of the open ones
    // gives each entry its path.
    std::vector<uint32_t> open{0};
    for (uint32_t i = 1; i < count; ++i) {
        while (open.size() > 1 && i >= disc.entries[open.back()].next) open.pop_back();
        const uint32_t w0 = be32(b, i * 12u), w1 = be32(b, i * 12u + 4), w2 = be32(b, i * 12u + 8);
        Entry& e = disc.entries[i];
        e.dir = (w0 >> 24) == 1;
        const size_t name = strings + (w0 & 0xFFFFFF);
        if (name >= b.size()) {
            std::fprintf(stderr, "qilin_run: sys/fst.bin: entry %u's name is outside the file\n", i);
            return false;
        }
        for (size_t k = name; k < b.size() && b[k]; ++k) e.name += static_cast<char>(b[k]);
        const std::string& parent_path = disc.entries[open.back()].path;
        e.path = parent_path.empty() ? e.name : parent_path + "/" + e.name;
        if (e.dir) {
            e.parent = w1, e.next = w2;
            if (w2 <= i || w2 > count) {
                std::fprintf(stderr, "qilin_run: sys/fst.bin: directory %u ends at %u\n", i, w2);
                return false;
            }
            open.push_back(i);
        } else {
            e.disc_offset = w1, e.size = w2;
            if (e.size && !disc.by_offset.emplace(e.disc_offset, i).second) {
                std::fprintf(stderr, "qilin_run: sys/fst.bin: files %u and %u start at the same place\n",
                             disc.by_offset[e.disc_offset], i);
                return false;
            }
        }
    }
    disc.root = root;
    disc.fst_size = static_cast<uint32_t>(b.size());
    disc.open = true;
    return true;
}

uint32_t disc_fst_size() { return disc.open ? disc.fst_size : 0; }

}  // namespace qilin::rt
