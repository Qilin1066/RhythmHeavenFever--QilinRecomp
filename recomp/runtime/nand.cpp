// revolution/nand (the console's flash), native: the game's save data in a
// host folder, laid out as the flash's own paths
// (<root>/title/00010000/534f4d50/data/...). The root is QILIN_NAND, or the
// platform's per-user data folder (qilin/beatthebeat/nand): never the
// repository, since saves are the game's data. Functions are added as
// bring-up reaches them, each checked against how its callers use the
// result. Runtime code: no std::format.
#include "nand.hpp"
#include "nand_io.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "coherence.hpp"
#include "interrupts.hpp"
#include "machine.hpp"
#include "native.hpp"
#include "settings.hpp"

namespace qilin::rt {

namespace {

// NAND results, as the game's callers test them (CBackupManager: 0 is fine,
// -12 means "not there yet", anything else is an error).
constexpr int32_t kOk = 0, kAccess = -1, kExists = -6, kInvalid = -8, kNoExists = -12, kUnknown = -64;
constexpr uint8_t kTypeFile = 1, kTypeDir = 2;
constexpr uint32_t kModeRead = 1, kModeWrite = 2;  // NANDOpen's access bits

// Open files, by the address of the game's NANDFileInfo (only the replaced
// NAND library looks inside it).
struct OpenFile {
    std::fstream stream;
    uint32_t mode = 0;
};
std::map<uint32_t, OpenFile> g_open;

struct Nand {
    std::string home;  // the title's data directory, as the game sees it
    std::filesystem::path root;
} nand;

std::filesystem::path default_root() {
    if (const char* p = std::getenv("QILIN_NAND"); p && *p) return p;
    const std::filesystem::path dir = game_data_dir();
    return dir.empty() ? dir : dir / "nand";
}

void poke_string(uint32_t addr, const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) poke8(addr + static_cast<uint32_t>(i), static_cast<uint8_t>(s[i]));
    poke8(addr + static_cast<uint32_t>(s.size()), 0);
}

// The flash's paths are at most 63 characters (NAND_MAX_PATH 64 with the
// NUL); a longer one is a game bug or a misread argument: stop.
std::string guest_string(const Context& c, uint32_t addr) { return guest_cstring(addr, 63, c.lr - 4, "NAND: a path"); }

[[noreturn]] void no_root(const Context& c) {
    std::fprintf(stderr, "qilin_run: NAND: no folder for the console's flash (set QILIN_NAND)\n");
    throw Stop{Stop::Unsupported, c.lr - 4, 0};
}

// A NAND path on the host: absolute as given, relative from the title's
// home directory (a game's current directory). Empty (the callers' INVALID)
// if it could leave the flash's folder: a ".." component, or one a host
// would read as a separator or a drive ('\\', ':').
std::filesystem::path host_path(const Context& c, const std::string& path) {
    if (nand.root.empty()) no_root(c);
    const std::string full = path.starts_with("/") ? path : nand.home + "/" + path;
    std::filesystem::path p = nand.root;
    size_t i = 1;
    while (i <= full.size()) {
        const size_t slash = full.find('/', i);
        const std::string part = full.substr(i, slash == std::string::npos ? std::string::npos : slash - i);
        if (part == ".." || part.find_first_of("\\:") != std::string::npos) return {};
        if (!part.empty() && part != ".") p /= part;
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return p;
}

// NANDGetHomeDir(path): the title's data directory; NAND_RESULT_OK.
QILIN_NATIVE(NANDGetHomeDir) {
    poke_string(c.r[3], nand.home);
    c.r[3] = static_cast<uint32_t>(kOk);
}

// NANDCheck(blocks, inodes, u32* answer): whether a save of that size fits;
// the answer's bits name the limits it would exceed (the game reads 5 as "no
// space" and 10 as "too many files"). A host folder has room: 0.
QILIN_NATIVE(NANDCheck) {
    poke32(c.r[5], 0);
    c.r[3] = static_cast<uint32_t>(kOk);
}

// NANDGetStatus(path, NANDStatus*): owner ID (u32), group ID (u16),
// attribute and permission (bytes). The game's one caller asks only whether
// its save exists (the result) and reads none of the fields. The host
// folder doesn't keep owners or groups (0 here); the permission is owner
// read/write, as the game creates its files.
constexpr uint8_t kPermOwnerReadWrite = 0x30;
QILIN_NATIVE(NANDGetStatus) {
    const std::filesystem::path p = host_path(c, guest_string(c, c.r[3]));
    std::error_code ec;
    if (p.empty()) {
        c.r[3] = static_cast<uint32_t>(kInvalid);
    } else if (std::filesystem::exists(p, ec)) {
        const uint32_t st = c.r[4];
        poke32(st, 0);
        poke16(st + 4, 0);
        poke8(st + 6, 0);
        poke8(st + 7, kPermOwnerReadWrite);
        c.r[3] = static_cast<uint32_t>(kOk);
    } else {
        c.r[3] = static_cast<uint32_t>(kNoExists);
    }
}

// NANDCreate(path, permission, attribute): a new empty file; EXISTS if it's
// there already. Permissions and attributes aren't kept by the host folder.
QILIN_NATIVE(NANDCreate) {
    const std::filesystem::path p = host_path(c, guest_string(c, c.r[3]));
    std::error_code ec;
    if (p.empty()) {
        c.r[3] = static_cast<uint32_t>(kInvalid);
        return;
    }
    if (std::filesystem::exists(p, ec)) {
        c.r[3] = static_cast<uint32_t>(kExists);
        return;
    }
    std::filesystem::create_directories(p.parent_path(), ec);  // the title's directory exists on a console
    std::ofstream f(p, std::ios::binary);
    c.r[3] = static_cast<uint32_t>(f ? kOk : kAccess);
}

int32_t open_at(const std::filesystem::path& p, uint32_t fi, uint32_t mode);

// NANDOpen(path, fileInfo, mode): 1 read, 2 write, 3 both. Writing keeps the
// contents, as NAND does.
QILIN_NATIVE(NANDOpen) {
    c.r[3] = static_cast<uint32_t>(open_at(host_path(c, guest_string(c, c.r[3])), c.r[4], c.r[5]));
}

OpenFile* open_file(const Context& c) {
    const auto it = g_open.find(c.r[3]);
    if (it == g_open.end()) {
        std::fprintf(stderr, "qilin_run: NAND: file info %08X isn't open\n", c.r[3]);
        throw Stop{Stop::Unsupported, c.lr - 4, c.r[3]};
    }
    return &it->second;
}

// NANDRead / NANDWrite(fileInfo, buffer, length): the bytes moved, or an error.
QILIN_NATIVE(NANDRead) {
    OpenFile* f = open_file(c);
    if (!(f->mode & kModeRead)) {
        c.r[3] = static_cast<uint32_t>(kAccess);
        return;
    }
    require_guest_range(c.r[4], c.r[5], c.lr - 4, "NANDRead's buffer");
    const int32_t result = nand_read(f->stream, reinterpret_cast<char*>(g_mem + c.r[4]), c.r[5]);
    // A hard failure may have changed part of the buffer without reporting
    // its size. Invalidate conservatively then; EOF reports an exact count.
    note_guest_write(c.r[4], result < 0 ? c.r[5] : static_cast<uint32_t>(result));
    c.r[3] = static_cast<uint32_t>(result);
}
QILIN_NATIVE(NANDWrite) {
    OpenFile* f = open_file(c);
    if (!(f->mode & kModeWrite)) {
        c.r[3] = static_cast<uint32_t>(kAccess);
        return;
    }
    require_guest_range(c.r[4], c.r[5], c.lr - 4, "NANDWrite's buffer");
    f->stream.clear();
    f->stream.write(reinterpret_cast<const char*>(g_mem + c.r[4]), c.r[5]);
    c.r[3] = f->stream ? c.r[5] : static_cast<uint32_t>(kUnknown);
}
// NANDSeek(fileInfo, offset, whence): 0 from the start, 1 from here, 2 from
// the end; the new position.
QILIN_NATIVE(NANDSeek) {
    OpenFile* f = open_file(c);
    const auto offset = static_cast<int32_t>(c.r[4]);
    const uint32_t whence = c.r[5];
    if (whence > 2) {
        c.r[3] = static_cast<uint32_t>(kInvalid);
        return;
    }
    const std::ios::seekdir dir = whence == 0 ? std::ios::beg : whence == 1 ? std::ios::cur : std::ios::end;
    // A position outside the file: host streams allow it, what the console's
    // file system answers isn't pinned down here, so it stops.
    f->stream.clear();
    const auto here = f->stream.tellg();
    f->stream.seekg(0, std::ios::end);
    const int64_t length = f->stream.tellg();
    const int64_t base = whence == 0 ? 0 : whence == 1 ? int64_t(here) : length;
    if (base + offset < 0 || base + offset > length) {
        std::fprintf(stderr, "qilin_run: NAND: a seek to %lld in a file of %lld bytes\n", static_cast<long long>(base + offset),
                     static_cast<long long>(length));
        throw Stop{Stop::Unsupported, c.lr - 4, static_cast<uint32_t>(offset)};
    }
    f->stream.seekg(here);
    f->stream.seekg(offset, dir);
    f->stream.seekp(f->stream.tellg());
    c.r[3] = f->stream ? static_cast<uint32_t>(f->stream.tellg()) : static_cast<uint32_t>(kInvalid);
}
// NANDGetLength(fileInfo, u32* length)
QILIN_NATIVE(NANDGetLength) {
    OpenFile* f = open_file(c);
    f->stream.clear();
    const auto here = f->stream.tellg();
    f->stream.seekg(0, std::ios::end);
    poke32(c.r[4], static_cast<uint32_t>(f->stream.tellg()));
    f->stream.seekg(here);
    c.r[3] = static_cast<uint32_t>(kOk);
}
QILIN_NATIVE(NANDClose) {
    OpenFile* f = open_file(c);
    f->stream.flush();
    const bool ok = static_cast<bool>(f->stream);
    g_open.erase(c.r[3]);
    c.r[3] = static_cast<uint32_t>(ok ? kOk : kUnknown);
}

// NANDMove(path, directory): the file goes into the directory, keeping its
// name (a safe save's last step, from /tmp). EXISTS if the destination is
// there already, as on the console: a game deletes the old save first.
QILIN_NATIVE(NANDMove) {
    const std::string from_path = guest_string(c, c.r[3]);
    const std::filesystem::path from = host_path(c, from_path), dir = host_path(c, guest_string(c, c.r[4]));
    std::error_code ec;
    if (from.empty() || dir.empty()) {
        c.r[3] = static_cast<uint32_t>(kInvalid);
        return;
    }
    if (!std::filesystem::exists(from, ec) || !std::filesystem::is_directory(dir, ec)) {
        c.r[3] = static_cast<uint32_t>(kNoExists);
        return;
    }
    const std::filesystem::path to = dir / from.filename();
    if (std::filesystem::exists(to, ec)) {
        c.r[3] = static_cast<uint32_t>(kExists);
        return;
    }
    std::filesystem::rename(from, to, ec);
    c.r[3] = static_cast<uint32_t>(ec ? kAccess : kOk);
}

// ---- asynchronous requests ----
// The request is accepted at once (NAND_RESULT_OK) and its result comes
// through callback(result, block) as an interrupt, after the call returns,
// as on the console. The work itself is done here at once.
void complete(uint32_t callback, int32_t result, uint32_t block) {
    post_interrupt([callback, result, block](Context& ctx) {
        ctx.r[3] = static_cast<uint32_t>(result);
        ctx.r[4] = block;
        call_guest(ctx, callback);
    }, kIoLatency);
}

// Opens `path` for `fi` (the work of NANDOpen); the NAND result.
int32_t open_at(const std::filesystem::path& p, uint32_t fi, uint32_t mode) {
    std::error_code ec;
    if (p.empty() || mode == 0 || mode > 3) return kInvalid;
    if (!std::filesystem::is_regular_file(p, ec)) return kNoExists;
    if (g_open.contains(fi)) return kInvalid;  // a file info opened twice
    OpenFile& f = g_open[fi];
    std::ios::openmode m = std::ios::binary;
    if (mode & kModeRead) m |= std::ios::in;
    if (mode & kModeWrite) m |= std::ios::out | std::ios::in;
    f.stream.open(p, m);
    f.mode = mode;
    if (!f.stream) {
        g_open.erase(fi);
        return kAccess;
    }
    return kOk;
}

// NANDPrivateSafeOpenAsync(path, fileInfo, mode, buffer, bufferSize,
// callback, block). "Private": a path outside the title's directory (the
// system's shared files, the Mii database among them; the host folder has
// them only if the owner copied in their own). A safe open for reading is a
// plain open; for writing the SDK works on a copy in /tmp and commits it at
// close, which isn't modelled yet.
QILIN_NATIVE(NANDPrivateSafeOpenAsync) {
    const uint32_t mode = c.r[5];
    if (mode & kModeWrite) {
        std::fprintf(stderr, "qilin_run: NAND: a safe open for writing isn't modelled yet\n");
        throw Stop{Stop::Unsupported, c.lr - 4, mode};
    }
    const std::filesystem::path p = host_path(c, guest_string(c, c.r[3]));
    complete(c.r[8], open_at(p, c.r[4], mode), c.r[9]);
    c.r[3] = static_cast<uint32_t>(kOk);
}

// The other asynchronous requests: the synchronous call's work and result
// (arguments first, then callback and block), through the callback. The
// Mii library (RFL) reads its database this way.
template <void (*Sync)(Context&)>
void async_of(Context& c, unsigned callback_reg) {
    const uint32_t callback = c.r[callback_reg], block = c.r[callback_reg + 1];
    Sync(c);
    complete(callback, static_cast<int32_t>(c.r[3]), block);
    c.r[3] = static_cast<uint32_t>(kOk);
}
QILIN_NATIVE(NANDReadAsync) { async_of<qilin_native_NANDRead>(c, 6); }            // (info, buffer, length, cb, block)
QILIN_NATIVE(NANDSeekAsync) { async_of<qilin_native_NANDSeek>(c, 6); }            // (info, offset, whence, cb, block)
QILIN_NATIVE(NANDGetLengthAsync) { async_of<qilin_native_NANDGetLength>(c, 5); }  // (info, u32* length, cb, block)
// A safe close commits a copy written in /tmp; files are only safe-opened for
// reading here (NANDPrivateSafeOpenAsync), so it's a close.
QILIN_NATIVE(NANDSafeCloseAsync) { async_of<qilin_native_NANDClose>(c, 4); }  // (info, cb, block)
// "Private" versions take any path in the flash, not only the title's.
QILIN_NATIVE(NANDPrivateOpen) { qilin_native_NANDOpen(c); }
QILIN_NATIVE(NANDPrivateCreateAsync) { async_of<qilin_native_NANDCreate>(c, 6); }  // (path, perm, attr, cb, block)
// NANDDelete(path): a file, or a directory with what's in it; NOEXISTS if
// it isn't there.
QILIN_NATIVE(NANDDelete) {
    const std::filesystem::path p = host_path(c, guest_string(c, c.r[3]));
    std::error_code ec;
    if (p.empty()) c.r[3] = static_cast<uint32_t>(kInvalid);
    else if (!std::filesystem::exists(p, ec)) c.r[3] = static_cast<uint32_t>(kNoExists);
    else c.r[3] = static_cast<uint32_t>(std::filesystem::remove_all(p, ec) && !ec ? kOk : kAccess);
}

// NANDGetType(path, u8* type): file 1, directory 2; NOEXISTS if neither.
QILIN_NATIVE(NANDGetType) {
    const std::filesystem::path p = host_path(c, guest_string(c, c.r[3]));
    std::error_code ec;
    if (p.empty()) c.r[3] = static_cast<uint32_t>(kInvalid);
    else if (std::filesystem::is_directory(p, ec)) poke8(c.r[4], kTypeDir), c.r[3] = kOk;
    else if (std::filesystem::is_regular_file(p, ec)) poke8(c.r[4], kTypeFile), c.r[3] = kOk;
    else c.r[3] = static_cast<uint32_t>(kNoExists);
}

}  // namespace

void init_nand(std::string_view disc_id) {
    // The title ID's low half is the game code's four characters, in hex.
    char code[9] = {};
    for (size_t i = 0; i < 4 && i < disc_id.size(); ++i)
        std::snprintf(code + 2 * i, 3, "%02x", static_cast<unsigned char>(disc_id[i]));
    nand.home = std::string("/title/00010000/") + code + "/data";
    nand.root = default_root();
    // A console has the title's data directory and /tmp before the game
    // runs (the system makes them); so does the host folder.
    if (!nand.root.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(nand.root / "title" / "00010000" / code / "data", ec);
        std::filesystem::create_directories(nand.root / "tmp", ec);
    }
}

}  // namespace qilin::rt

