// The settings file (settings.hpp). Runtime code: no std::format.
#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>

#include "input.hpp"
#include "path.hpp"

namespace qilin::rt {

std::filesystem::path user_data_base() {
#if defined(_WIN32)
    // The wide variable: the narrow one is in the ANSI code page, which may
    // not hold the user's folder name.
    if (const wchar_t* p = _wgetenv(L"LOCALAPPDATA"); p && *p) return p;
#elif defined(__APPLE__)
    if (const char* p = std::getenv("HOME"); p && *p) return std::filesystem::path(p) / "Library" / "Application Support";
#else
    if (const char* p = std::getenv("XDG_DATA_HOME"); p && *p) return p;
    if (const char* p = std::getenv("HOME"); p && *p) return std::filesystem::path(p) / ".local" / "share";
#endif
    return {};
}

// ---- controls ----
namespace {

constexpr const char* kControlNames[kControlCount] = {"a", "b", "one", "two", "plus", "minus", "home", "up", "down", "left", "right"};
constexpr const char* kControlLabels[kControlCount] = {"A", "B (the trigger)", "1", "2", "+", "-", "Home",
                                                       "D-pad up", "D-pad down", "D-pad left", "D-pad right"};
struct PadName {
    const char* name;   // in the file
    const char* label;  // in the menu
    uint32_t bit;
};
constexpr PadName kPadNames[] = {
    {"Up", "Pad Up", kPadUp},       {"Down", "Pad Down", kPadDown},   {"Left", "Pad Left", kPadLeft},
    {"Right", "Pad Right", kPadRight}, {"A", "Pad A", kPadA},         {"B", "Pad B", kPadB},
    {"X", "Pad X", kPadX},          {"Y", "Pad Y", kPadY},            {"Start", "Pad Start", kPadStart},
    {"Back", "Pad Back", kPadBack}, {"Guide", "Pad Guide", kPadGuide}, {"LB", "Pad LB", kPadLB},
    {"RB", "Pad RB", kPadRB},       {"LT", "Pad LT", kPadLT},         {"RT", "Pad RT", kPadRT},
    {"L3", "Pad L3", kPadL3},       {"R3", "Pad R3", kPadR3},
};

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
Binding pad(uint32_t bit) { return {Binding::kPad, bit, {}}; }
Binding key(const char* name) { return {Binding::kKey, 0, name}; }

}  // namespace

const char* control_name(Control c) { return kControlNames[c]; }
const char* control_label(Control c) { return kControlLabels[c]; }
uint16_t control_button(Control c) {
    static constexpr uint16_t kButtons[kControlCount] = {kButtonA,    kButtonB,     kButton1,  kButton2,
                                                         kButtonPlus, kButtonMinus, kButtonHome, kButtonUp,
                                                         kButtonDown, kButtonLeft,  kButtonRight};
    return kButtons[c];
}

const char* pad_button_name(uint32_t bit) {
    for (const PadName& p : kPadNames)
        if (p.bit == bit) return p.name;
    return nullptr;
}

bool key_reserved(const std::string& name) {
    const std::string l = lower(name);
    return l == "escape" || l == "f1" || l == "f3" || l == "f11";
}

std::string binding_text(const Binding& b) {
    switch (b.kind) {
    case Binding::kMouse: return "mouse";
    case Binding::kKey: return "key:" + b.key;
    case Binding::kPad:
        if (const char* n = pad_button_name(b.pad)) return std::string("pad:") + n;
        break;
    }
    return "?";
}

std::string binding_label(const Binding& b) {
    switch (b.kind) {
    case Binding::kMouse: return "Mouse";
    case Binding::kKey: return b.key;
    case Binding::kPad:
        for (const PadName& p : kPadNames)
            if (p.bit == b.pad) return p.label;
        break;
    }
    return "?";
}

// The owner's layout (2026-11-20): A and B on their letters and in pairs for
// two hands, right A and left B as on the controller's shoulders.
Controls default_controls() {
    Controls c;
    c[kCtlA] = {pad(kPadA), pad(kPadRB), key("A"), key("Right Shift"), key("Right"), key("Space")};
    c[kCtlB] = {pad(kPadB), pad(kPadLB), pad(kPadRT), key("B"), key("Left Shift"), key("Left")};
    c[kCtlOne] = {pad(kPadX), key("1")};
    c[kCtlTwo] = {pad(kPadY), key("2")};
    c[kCtlPlus] = {pad(kPadStart), key("Return")};
    c[kCtlMinus] = {pad(kPadBack), key("Backspace"), key("Tab")};
    c[kCtlHome] = {pad(kPadGuide), key("H")};
    c[kCtlUp] = {pad(kPadUp), key("Up"), key("I")};
    c[kCtlDown] = {pad(kPadDown), key("Down"), key("K")};
    c[kCtlLeft] = {pad(kPadLeft), key("J")};
    c[kCtlRight] = {pad(kPadRight), key("L")};
    return c;
}

namespace {
// "pad:RB", "key:Space" or "mouse"; false with the reason.
bool parse_binding(const std::string& text, Binding& b, std::string& why) {
    const std::string l = lower(text);
    if (l == "mouse") {
        b = {Binding::kMouse, 0, {}};
        return true;
    }
    const size_t colon = text.find(':');
    if (colon == std::string::npos) {
        why = "'" + text + "' isn't an input: write pad:<button>, key:<key> or mouse";
        return false;
    }
    const std::string kind = lower(text.substr(0, colon)), name = text.substr(colon + 1);
    if (kind == "pad") {
        for (const PadName& p : kPadNames)
            if (lower(name) == lower(p.name)) {
                b = pad(p.bit);
                return true;
            }
        why = "'" + name + "' isn't a pad button (Up Down Left Right A B X Y Start Back Guide LB RB LT RT L3 R3)";
        return false;
    }
    if (kind == "key") {
        if (name.empty()) {
            why = "key: needs a key's name";
            return false;
        }
        if (key_reserved(name)) {
            why = "key:" + name + " is kept for the program (Escape and F1: the menu, F3: frame times, F11: full screen)";
            return false;
        }
        b = {Binding::kKey, 0, name};
        return true;
    }
    why = "'" + kind + "' isn't a kind of input (pad, key, mouse)";
    return false;
}
}  // namespace

// The presets: what each sets before the file's own keys.
bool apply_preset(Settings& s, const std::string& name) {
    if (name == "original") {  // the console's picture: 1x, its filtering, vsync
        s.internal_scale = 1, s.anisotropy = 1, s.vsync = true;
    } else if (name == "low-end") {  // a Raspberry Pi 4-class board: the console's work, nothing more
        s.internal_scale = 1, s.anisotropy = 1, s.vsync = true;
    } else if (name == "desktop") {  // a desktop GPU: 3x (1920x1584 frame buffer), 16x anisotropic
        s.internal_scale = 3, s.anisotropy = 16, s.vsync = true;
    } else {
        return false;
    }
    s.preset = name;
    return true;
}

// The file, with its comments, for the values in `s` (the first run's
// default file, and what the menu saves).
std::string settings_text(const Settings& s) {
    auto b = [](bool v) { return v ? "true" : "false"; };
    std::string t;
    auto line = [&](const std::string& l) { t += l + "\n"; };
    line("# Beat the Beat: Rhythm Paradise - settings");
    line("#");
    line("# The settings menu (F1 or Esc in the game) changes these and saves them here.");
    line("# A preset sets the enhancements; any key below it overrides one.");
    line("# Delete this file to get the defaults back.");
    line("");
    line("[display]");
    line("# original: the console's picture. low-end: for a Raspberry Pi 4-class board.");
    line("# desktop: 3x internal resolution, 16x anisotropic filtering.");
    line("preset = " + s.preset);
    line("# Borderless fullscreen at the desktop's resolution (F11 or Alt+Enter switch it).");
    line(std::string("fullscreen = ") + b(s.fullscreen));
    line("# The display, by name (empty: the main one).");
    line("monitor = " + s.monitor);
    line("# The picture's size: integer (whole multiples, the sharpest), fit (keeping its shape), stretch (the whole window).");
    line(std::string("scaling = ") + scaling_name(s.scaling));
    line("# Smooth (filtered) at sizes that aren't whole multiples; false: sharp.");
    line(std::string("smooth = ") + b(s.smooth));
    line("# plain, or crt: a CRT television's raster lines and phosphor stripes (best at whole multiples or a large window).");
    line(std::string("look = ") + (s.look == Look::crt ? "crt" : "plain"));
    line("# CRT: how dark the gaps between the raster lines are, and the phosphor stripes' strength (0-100).");
    line("scanlines = " + std::to_string(s.scanlines));
    line("mask = " + std::to_string(s.mask));
    line("# The frame rate, its 1% and 0.1% lows and the frame times in a corner (F3 switches it).");
    line(std::string("performance_overlay = ") + b(s.performance_overlay));
    line("# The frame buffer at 1-8x the console's 640x528 (more is sharper, and more work for the GPU);");
    line("# 0: from the screen's height (3x on a 1440-line screen).");
    line("internal_scale = " + std::to_string(s.internal_scale));
    line("# Texture filtering: 1 (the console's), 2, 4, 8 or 16.");
    line("anisotropy = " + std::to_string(s.anisotropy));
    line("# Present at the display's refresh (no tearing). false: the lowest latency.");
    line(std::string("vsync = ") + b(s.vsync));
    line("# With vsync: when the display's refresh is within 0.5% of the game's 59.94 Hz (or 50 Hz),");
    line("# or a whole multiple of it, run the game at the display's rate, so no frame is shown twice");
    line("# (a hitch every 17 s at 60 Hz otherwise). The game and its sound run up to 0.5% faster.");
    line(std::string("lock_to_display = ") + b(s.lock_to_display));
    line("# With vsync: if the desktop's refresh can't show the game's frames evenly (75 Hz, 144 Hz),");
    line("# fullscreen switches the display to the same resolution at 60 or 120 Hz. false: the desktop's mode.");
    line(std::string("match_refresh = ") + b(s.match_refresh));
    line("# A variable-refresh display (G-Sync, FreeSync, VRR) with it turned on in the graphics driver:");
    line("# the display refreshes when each frame arrives, so any refresh rate shows the game evenly");
    line("# (a 144 Hz desktop as it is, with no mode switch). Overrides the two above. Only turn it on");
    line("# if the display and driver have it on: on a fixed-refresh display it shows frames unevenly.");
    line(std::string("variable_refresh = ") + b(s.variable_refresh));
    line("# The graphics device, by name (empty: the best one).");
    line("gpu = " + s.gpu);
    line("");
    line("[audio]");
    line("# Milliseconds of sound queued ahead of the speakers, held there by resampling a little.");
    line("# Lower is less lag between a press and its sound (5-200); a value below what the sound device can keep up is raised to it (said at start). Raise it if the sound crackles.");
    line("latency_ms = " + std::to_string(s.audio_latency_ms));
    line("# Windows: take the sound device for the game alone (exclusive mode), for its shortest periods");
    line("# (3 ms on a USB interface, where sharing gives 10): about 10 ms less lag. Other programs' sound");
    line("# waits while the game runs. Windows' volume and mute still apply. false: share the device.");
    line(std::string("exclusive = ") + b(s.exclusive_audio));
    line("");
    line("[controls]");
    line("# The Wii Remote's rumble on the controller.");
    line(std::string("rumble = ") + b(s.rumble));
    line("# What presses each of the Wii Remote's buttons, held upright: any of the inputs listed.");
    line("# pad:<button> (Up Down Left Right A B X Y Start Back Guide LB RB LT RT L3 R3, by an Xbox pad's");
    line("# names), key:<key> (as SDL names keys: Space, Right Shift, Return, A) or mouse (its left button).");
    line("# Escape, F1, F3 and F11 are kept for the program. The settings menu sets these by pressing them.");
    for (unsigned c = 0; c < kControlCount; ++c) {
        std::string l = std::string(control_name(Control(c))) + " =";
        for (size_t i = 0; i < s.controls[c].size(); ++i) l += (i ? ", " : " ") + binding_text(s.controls[c][i]);
        line(l);
    }
    line("");
    line("[console]");
    line("# What the game reads as the Wii's own settings (read when the game starts).");
    line("# 0 Japanese, 1 English, 2 German, 3 French, 4 Spanish, 5 Italian (this disc: 1-5).");
    line("language = " + std::to_string(s.language));
    line("# 60 Hz (EuRGB60); false: PAL 50 Hz.");
    line(std::string("sixty_hz = ") + b(s.sixty_hz));
    line("# 16:9; false: 4:3.");
    line(std::string("widescreen = ") + b(s.widescreen));
    line(std::string("progressive = ") + b(s.progressive));
    line("# 0 mono, 1 stereo, 2 surround (the game encodes Dolby Pro Logic II, for a receiver that decodes it).");
    line("sound_mode = " + std::to_string(s.sound_mode));
    line(std::string("sensor_bar_above = ") + b(s.sensor_bar_above));
    line("# Pointer sensitivity, 1-5.");
    line("dpd_sensitivity = " + std::to_string(s.dpd_sensitivity));
    return t;
}

namespace {

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// A control by the file's name; kControlCount if it isn't one.
unsigned control_of(const std::string& name) {
    for (unsigned c = 0; c < kControlCount; ++c)
        if (name == kControlNames[c]) return c;
    return kControlCount;
}

Settings load() {
    Settings s;
    const std::filesystem::path path = settings_path();
    if (path.empty()) return s;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream(path, std::ios::binary) << settings_text(Settings{});
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) return s;
    std::string error;
    unsigned line = 0;
    if (!parse_settings(f, s, error, line)) {
        std::fprintf(stderr, "qilin_run: %s, line %u: %s\n", qilin::utf8_path(path).c_str(), line, error.c_str());
        std::fflush(stderr);
        std::_Exit(2);
    }
    return s;
}
}  // namespace

bool parse_settings(std::istream& in, Settings& s, std::string& error, unsigned& line_number) {
    std::string line, section;
    for (unsigned n = 1; std::getline(in, line); ++n) {
        line_number = n;
        auto bad = [&](const std::string& what) {
            error = what;
            return false;
        };
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') return bad("a section is written [name]");
            section = trim(line.substr(1, line.size() - 2));
            if (section != "display" && section != "audio" && section != "controls" && section != "console")
                return bad("no section [" + section + "]");
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return bad("a setting is written key = value");
        const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        bool ok = true;
        auto boolean = [&](bool& out) {
            if (value == "true") out = true;
            else if (value == "false") out = false;
            else ok = bad(key + " is true or false");
        };
        auto number = [&](uint32_t lo, uint32_t hi) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(value.c_str(), &end, 10);
            if (value.empty() || *end || value.front() == '-' || v < lo || v > hi) {
                ok = bad(key + " is a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
                return lo;
            }
            return static_cast<uint32_t>(v);
        };
        if (section == "display" && key == "preset") {
            if (!apply_preset(s, value)) return bad("the presets are original, low-end and desktop");
        } else if (section == "display" && key == "fullscreen") {
            boolean(s.fullscreen);
        } else if (section == "display" && key == "internal_scale") {
            s.internal_scale = number(0, 8);
        } else if (section == "display" && key == "look") {
            if (value == "plain") s.look = Look::plain;
            else if (value == "crt") s.look = Look::crt;
            else return bad("look is plain or crt");
        } else if (section == "display" && key == "scanlines") {
            s.scanlines = number(0, 100);
        } else if (section == "display" && key == "mask") {
            s.mask = number(0, 100);
        } else if (section == "display" && key == "performance_overlay") {
            boolean(s.performance_overlay);
        } else if (section == "display" && key == "anisotropy") {
            s.anisotropy = number(1, 16);
            if (ok && (s.anisotropy & (s.anisotropy - 1))) return bad("anisotropy is 1, 2, 4, 8 or 16");
        } else if (section == "display" && key == "monitor") {
            s.monitor = value;
        } else if (section == "display" && key == "gpu") {
            s.gpu = value;
        } else if (section == "display" && key == "scaling") {
            if (value == "integer") s.scaling = Scaling::integer;
            else if (value == "fit") s.scaling = Scaling::fit;
            else if (value == "stretch") s.scaling = Scaling::stretch;
            else return bad("scaling is integer, fit or stretch");
        } else if (section == "display" && key == "smooth") {
            boolean(s.smooth);
        } else if (section == "controls" && key == "rumble") {
            boolean(s.rumble);
        } else if (section == "controls" && control_of(key) < kControlCount) {
            // A comma-separated list; empty: nothing presses it.
            auto& list = s.controls[control_of(key)];
            list.clear();
            for (size_t at = 0; at <= value.size();) {
                const size_t comma = std::min(value.find(',', at), value.size());
                const std::string item = trim(value.substr(at, comma - at));
                at = comma + 1;
                if (item.empty()) {
                    if (comma == value.size()) break;
                    return bad("an empty input in " + key + "'s list");
                }
                Binding bd;
                std::string why;
                if (!parse_binding(item, bd, why)) return bad(why);
                if (std::find(list.begin(), list.end(), bd) == list.end()) list.push_back(bd);
            }
        } else if (section == "display" && key == "vsync") {
            boolean(s.vsync);
        } else if (section == "display" && key == "lock_to_display") {
            boolean(s.lock_to_display);
        } else if (section == "display" && key == "match_refresh") {
            boolean(s.match_refresh);
        } else if (section == "display" && key == "variable_refresh") {
            boolean(s.variable_refresh);
        } else if (section == "audio" && key == "exclusive") {
            boolean(s.exclusive_audio);
        } else if (section == "audio" && key == "latency_ms") {
            s.audio_latency_ms = number(5, 200);  // raised to what the device can keep up (host.cpp)
        } else if (section == "console" && key == "language") {
            s.language = static_cast<uint8_t>(number(0, 6));
        } else if (section == "console" && key == "sixty_hz") {
            boolean(s.sixty_hz);
        } else if (section == "console" && key == "widescreen") {
            boolean(s.widescreen);
        } else if (section == "console" && key == "progressive") {
            boolean(s.progressive);
        } else if (section == "console" && key == "sound_mode") {
            s.sound_mode = static_cast<uint8_t>(number(0, 2));
        } else if (section == "console" && key == "sensor_bar_above") {
            boolean(s.sensor_bar_above);
        } else if (section == "console" && key == "dpd_sensitivity") {
            s.dpd_sensitivity = static_cast<uint8_t>(number(1, 5));
        } else {
            return bad("no setting " + (section.empty() ? key : "[" + section + "] " + key));
        }
        if (!ok) return false;
    }
    return true;
}
std::filesystem::path game_data_dir() {
    static const std::filesystem::path dir = [] {
        const std::filesystem::path base = user_data_base();
        if (base.empty()) return base;
        const std::filesystem::path now = base / "qilin" / "beatthebeat", before = base / "BeatTheBeat-QilinComp";
        // Up to v0.16.0 the folder was named after the project, which was
        // then called BeatTheBeat-QilinComp (the old name stays here: it's
        // the folder's). What's there (saves above all) moves to the new
        // place once.
        std::error_code ec;
        if (std::filesystem::is_directory(before, ec) && !std::filesystem::exists(now, ec)) {
            std::filesystem::create_directories(now.parent_path(), ec);
            std::filesystem::rename(before, now, ec);
            if (ec) std::fprintf(stderr, "qilin_run: can't move %s to %s (%s): your saves are still in the old place\n",
                                 qilin::utf8_path(before).c_str(), qilin::utf8_path(now).c_str(), ec.message().c_str());
            else std::fprintf(stderr, "qilin_run: moved %s to %s\n", qilin::utf8_path(before).c_str(), qilin::utf8_path(now).c_str());
        }
        return now;
    }();
    return dir;
}

std::filesystem::path settings_path() {
    if (const char* p = std::getenv("QILIN_SETTINGS"); p && *p) return p;
    const std::filesystem::path dir = game_data_dir();
    return dir.empty() ? dir : dir / "beatthebeat.ini";
}

Settings& settings() {
    static Settings s = load();
    return s;
}

bool save_settings() {
    const std::filesystem::path path = settings_path();
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::filesystem::path tmp = std::filesystem::path(path) += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << settings_text(settings());
        if (!f) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    return !ec;
}

const char* scaling_name(Scaling s) {
    switch (s) {
    case Scaling::integer: return "integer";
    case Scaling::stretch: return "stretch";
    default: return "fit";
    }
}

}  // namespace qilin::rt
