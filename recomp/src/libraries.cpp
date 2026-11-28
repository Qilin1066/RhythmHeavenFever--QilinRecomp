#include "libraries.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <fstream>
#include <stdexcept>

#include "path.hpp"

namespace qilin {

namespace {

using enum Plan;

// docs/code-analysis.md, "Libraries in the PAL build", in address order.
constexpr Library kLibraries[] = {
    {0x80004000, 0x800043C4, "init/start", Replace},  // the runtime starts the game
    {0x800043C4, 0x80004744, "init/runtime", Lift},   // memcpy, __fill_mem, memset
    {0x80004744, 0x80006720, "init/trk", Drop},       // debugger vectors, __init_data's tables
    // The game's last three functions (0x800DA6A4-0x800DA6BC, each returning
    // a constant) sit just before base: port_symbols.py once put base's
    // first names on them (fixed in v0.13.0 by checking names against code).
    {0x80006F00, 0x800DA6C0, "game", Lift},
    {0x800DA6C0, 0x800DA8E0, "base", Replace},
    {0x800DA8E0, 0x800E81FC, "os", Replace},
    {0x800E8200, 0x800E8CD8, "mtx", Lift},
    {0x800E8CE0, 0x800F1A28, "dvd", Replace},
    {0x800F1A30, 0x800F646C, "vi", Replace},
    {0x800F6470, 0x800F7CDC, "pad", Replace},
    {0x800F7CE0, 0x800F828C, "ai", Replace},
    {0x800F8290, 0x800FB5D0, "ax", Replace},
    {0x800FB5D0, 0x800FC714, "axfx", Lift},
    {0x800FC720, 0x800FD268, "dsp", Replace},
    {0x800FD270, 0x80104EF0, "gx", Replace},
    {0x80104EF0, 0x80107694, "runtime", Lift},
    {0x80107694, 0x80112124, "msl", Lift},
    {0x80112130, 0x80113EF8, "exi", Replace},
    {0x80113F00, 0x801155D8, "si", Replace},
    {0x801155D8, 0x8011A6A0, "trk", Drop},
    {0x8011A6A0, 0x8011B160, "ndevexi2a", Drop},
    {0x8011B160, 0x8011C6D0, "mem", Lift},
    {0x8011C6D0, 0x8011CA50, "euart", Replace},
    {0x8011CA50, 0x8011E1F0, "fs", Replace},
    {0x8011E1F0, 0x8012067C, "ipc", Replace},
    {0x80120680, 0x801244A8, "nand", Replace},
    {0x801244B0, 0x801265D4, "sc", Replace},
    {0x801265D4, 0x801268AC, "wenc", Lift},
    {0x801268B0, 0x80127288, "arc", Lift},
    {0x80127290, 0x80127890, "cx", Lift},
    {0x80127890, 0x8013EBE8, "wpad", Replace},
    {0x8013EBF0, 0x8016C3C4, "bte", Replace},
    {0x8016C3D0, 0x8016D91C, "usb", Replace},
    {0x8016D920, 0x80175494, "kpad", Lift},
    {0x801754A0, 0x801757D8, "tpl", Lift},
    {0x801757E0, 0x80175E00, "esp", Replace},
    {0x80175E00, 0x801995B8, "hbm", Lift},
    {0x801995C0, 0x8019B628, "nw4r/db", Lift},
    {0x8019B630, 0x8019CFEC, "nw4r/g3d", Lift},
    {0x8019CFF0, 0x801A9140, "nw4r/lyt", Lift},
    {0x801A9140, 0x801A9684, "nw4r/math", Lift},
    {0x801A9690, 0x801C966C, "nw4r/snd", Lift},
    {0x801C9670, 0x801D4CE4, "nw4r/ut", Lift},
    {0x801D4CE4, 0x801EFBF0, "gameutil", Lift},
    {0x801EFBF0, 0x801F968C, "rfl", Lift},
};

// Start-up after OSInit, from the end of the OS library (__ppc_eabi_init):
// __init_user runs the static constructors through __init_cpp, and exit runs
// the destructors before halting. The runtime calls them as __start does.
//
// GX's light objects: these functions only fill in or read back the
// GXLightObj the caller passes (colour, position, direction, the attenuation
// maths). Nothing reaches the GPU until GXLoadLightObjImm, which is native.
constexpr std::string_view kLiftAnyway[] = {
    "__init_user",
    "__init_cpp",
    "exit",
    "GXInitLightAttn",
    "GXInitLightSpot",
    "GXInitLightDistAttn",
    "GXInitLightPos",
    "GXGetLightPos",
    "GXInitLightDir",
    "GXGetLightDir",
    "GXInitLightColor",
    // GX's object functions: they fill in or read the texture and TLUT
    // objects the caller passes, in the SDK's own layout, and touch neither
    // the hardware nor GX's state. GXLoadTexObj and GXLoadTlut, native,
    // decode what they leave.
    "GXInitTexObj",
    "GXInitTexObjCI",
    "GXInitTexObjLOD",
    "GXInitTexObjWrapMode",
    "GXInitTexObjTlut",
    "GXInitTexObjFilter",
    "GXInitTexObjUserData",
    "GXGetTexObjUserData",
    "GXGetTexObjAll",
    "GXGetTexObjData",
    "GXGetTexObjWidth",
    "GXGetTexObjHeight",
    "GXGetTexObjFmt",
    "GXGetTexObjWrapS",
    "GXGetTexObjWrapT",
    "GXGetTexObjMipMap",
    "GXGetTexObjLODAll",
    "GXGetTexObjTlut",
    "GXInitTlutObj",
    "GXGetTexBufferSize",
    "__GetImageTileCount",
    // GX arithmetic and compositions of other GX calls: the Y scale for a
    // frame buffer height, overscan, and GXSetTevDirect (GXSetTevIndirect
    // with nothing indirect).
    "GXGetYScaleFactor",
    "GXAdjustForOverscan",
    "GXSetTevDirect",
    // The OS's debug reports: they format through MSL's vprintf (lifted) and
    // write through MSL's console, to the debug UART (native).
    "OSReport",
    "OSVReport",
    // ... after which MSL's console also calls the debugger stub's console
    // write on every console. On retail it reads the stub's "use serial I/O"
    // byte, which stays 0 because the stub never starts, and returns. Lifted
    // as such; if the byte were ever set, its next call isn't lifted and stops.
    "__TRK_write_console",
    "GetUseSerialIO",
    // NAND's save banner: it fills in the banner the game passes (its title
    // and comment, with MSL's wide-string functions). Writing it is native.
    "NANDInitBanner",
    // A NAND command block's user data: a store and a load at its start.
    "NANDSetUserData",
    "NANDGetUserData",
    // WPAD's work memory size: a constant of the Bluetooth stack, reached by
    // two tail calls. The game makes a heap of it for WPAD; lifted, its heap
    // layout is the console's.
    "WPADGetWorkMemorySize",
    "WUDGetAllocatedMemSize",
    "__ntd_get_allocated_mem_size",
    // GXInit's defaults: __GXInitGX calls GX's setters (native) with the
    // SDK's own default arguments. It reads only constants and the SDK's
    // default render mode, and touches no hardware.
    "__GXInitGX",
    // OSContext.c: these only clear or read the context structures and the
    // current-context pointer in low memory. Saving and loading a context
    // (the switch itself) stays native, and so does OSInitContext, which
    // tells the runtime a context is being reused for a new thread.
    "OSClearContext",
    "OSGetCurrentContext",
    "OSGetStackPointer",
};

// The OS's software (docs/runtime.md, v0.13.0): threads, mutexes and message
// queues are bookkeeping on guest structures, over the interrupt enable and
// the thread switch, which stay native. Lifted, the SDK's own scheduler
// decides which thread runs, with its priorities and queues exactly.
constexpr std::string_view kLiftUnits[] = {
    "revolution/os/OSThread.c",
    "revolution/os/OSMutex.c",
    // The OS's heap allocator: lists in arena memory, no hardware.
    "revolution/os/OSAlloc.c",
    "revolution/os/OSMessage.c",
    // The time base (mftb, which lifted code reads) and conversions of it.
    "revolution/os/OSTime.c",
    // CRC-32 (the game checksums its save with it): arithmetic.
    "revolution/os/OSCrc.c",
    // Alarms: the queue, periodic alarms and the decrementer callback that
    // fires them. Under them, native: the decrementer (PPCMtdec), the
    // exception table, and OSLoadContext returning to the interrupted code.
    "revolution/os/OSAlarm.c",
    // AX, whole: it is CPU code (voices and their priorities, the setters,
    // PB sync, aux callbacks, each frame's command list) with no hardware
    // access. Under it, native: AI's DMA and the DSP, which carries out AX's
    // command lists (audio.cpp).
    "revolution/ax/AX.c",
    "revolution/ax/AXAlloc.c",
    "revolution/ax/AXAux.c",
    "revolution/ax/AXCL.c",
    "revolution/ax/AXOut.c",
    "revolution/ax/AXSPB.c",
    "revolution/ax/AXVPB.c",
    "revolution/ax/AXProf.c",
};

// Native although their unit is lifted. SelectThread switches threads: lifted,
// its idle loop would spin forever, waiting for an interrupt the runtime
// could never deliver inside it.
constexpr std::string_view kNativeAnyway[] = {
    "SelectThread",
};

}  // namespace

std::span<const Library> libraries() { return kLibraries; }

const Library* library_at(uint32_t addr) {
    const auto it = std::ranges::upper_bound(kLibraries, addr, {}, &Library::lo);
    if (it == std::begin(kLibraries)) return nullptr;
    const Library& l = *(it - 1);
    return addr < l.hi ? &l : nullptr;
}

std::span<const std::string_view> lift_anyway_names() { return kLiftAnyway; }

bool lift_anyway(std::string_view name) { return std::ranges::find(kLiftAnyway, name) != std::end(kLiftAnyway); }
std::span<const std::string_view> lift_unit_names() { return kLiftUnits; }
bool lift_unit(std::string_view unit) { return std::ranges::find(kLiftUnits, unit) != std::end(kLiftUnits); }
std::span<const std::string_view> native_anyway_names() { return kNativeAnyway; }
bool native_anyway(std::string_view name) { return std::ranges::find(kNativeAnyway, name) != std::end(kNativeAnyway); }

std::optional<Plan> runtime_plan(uint32_t addr, std::string_view name, std::string_view unit) {
    const Library* lib = library_at(addr);
    if (!lib) return std::nullopt;
    if (native_anyway(name)) return Plan::Replace;
    if (lib->plan == Plan::Lift || lift_anyway(name) || lift_unit(unit)) return Plan::Lift;
    return lib->plan;
}

std::map<uint32_t, SymbolInfo> load_symbols(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error(std::format("cannot open {}", utf8_path(path)));
    std::map<uint32_t, SymbolInfo> names;
    for (std::string line; std::getline(f, line);) {
        // name = .section:0xADDRESS; // ... unit:path
        const size_t eq = line.find(" = ."), colon = line.find(":0x", eq == std::string::npos ? 0 : eq);
        if (eq == std::string::npos || eq == 0 || colon == std::string::npos || colon + 11 > line.size()) continue;
        uint32_t addr = 0;
        const char* first = line.data() + colon + 3;
        const auto [end, ec] = std::from_chars(first, first + 8, addr, 16);
        if (ec != std::errc{} || end != first + 8) continue;
        std::string unit;
        if (const size_t u = line.find(" unit:"); u != std::string::npos) {
            unit = line.substr(u + 6);
            unit = unit.substr(0, unit.find_first_of(" \r"));
        }
        names.emplace(addr, SymbolInfo{line.substr(0, eq), std::move(unit)});
    }
    if (names.empty()) throw std::runtime_error(std::format("{}: no symbols (expected \"name = .text:0x80001234; ...\")", utf8_path(path)));
    return names;
}

}  // namespace qilin
