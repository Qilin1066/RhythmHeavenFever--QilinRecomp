// The host side on SDL3 (host.hpp): the window and its events, audio out,
// and the keyboard and controllers as the Wii Remote. It follows the other
// Qilin recomps: the window is named after the game and opens at the largest
// whole multiple of the picture that fits the display; F11 or Alt+Enter
// switch borderless fullscreen; any controller SDL knows is read through its
// standard layout; the keyboard drives player 1 too, so no controller is
// needed. Runtime code: no std::format.
#include "host.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "bot.hpp"
#include "console.hpp"
#if defined(_WIN32)
#include "wasapi.hpp"
#endif
#include "gx_vulkan.hpp"
#include "input.hpp"
#include "menu.hpp"
#include "native.hpp"
#include "settings.hpp"

namespace qilin::rt::host {

namespace {

constexpr const char* kGameName = "Beat the Beat: Rhythm Paradise";

SDL_Window* g_window = nullptr;
// Opened by the main thread (open_audio, again later if the device couldn't
// be opened at start), used by the game's (play_audio).
std::atomic<SDL_AudioStream*> g_audio{nullptr};
std::atomic<int> g_floor_ms{0};  // the least latency the open device can be held at (open_audio)
std::atomic<double> g_audio_ms{0};    // the sound queued, smoothed (the F3 readout)
std::atomic<double> g_display_hz{0};  // the window's display's refresh now (the F3 readout)
#if defined(_WIN32)
std::atomic<bool> g_exclusive{false};  // g_audio feeds wasapi.cpp's exclusive output, not an SDL device
#endif
std::vector<SDL_Gamepad*> g_pads;  // every connected gamepad: any of them plays the Wii Remote
std::atomic<uint16_t> g_keys{0}, g_pad_buttons{0};
std::atomic<bool> g_rumble{false};
uint32_t g_wake_event = 0;  // an SDL user event: something for the main thread to do

constexpr uint32_t kSampleRate = 32000;
constexpr int kBytesPerMs = kSampleRate * 4 / 1000;  // 16-bit stereo
// Audio more than this beyond the target (a burst after a stall: the
// periodic interrupts catching up, a display mode switch) is dropped,
// counted, so the lag between the game and its sound snaps back at once: a
// skip once is better than seconds of extra lag in a rhythm game (the
// resampling takes about 6 s to drain 30 ms). The target or 10 ms, whichever
// is more (40 ms before: a burst of 48 ms took the average from 7 to 11).
int max_extra_bytes(int target) { return std::max(target, 10 * kBytesPerMs); }

// The audio clock. The game's DMA makes 32 kHz by its own clock (the time
// base); the sound card plays by its crystal. Left alone, the queue between
// them wanders (any lag from 0 to the cap, a different one each session) and
// then clicks as frames are dropped or run out. Instead it is held at the
// target (settings: audio latency_ms): the queue is filled to the target
// before playing starts, then SDL resamples by a ratio that follows the
// queue's average depth (at most 0.5% from the game's speed: inaudible).
// Running dry fills it back to the target with silence at once. Touched only
// from the DMA interrupt (one guest thread at a time) and open_audio.
struct AudioClock {
    SDL_AudioStream* stream = nullptr;  // the output this is for (a new one starts again)
    bool primed = false;  // the device is playing (the queue reached the target once)
    double depth = 0;     // the queue's depth, smoothed (bytes)
    unsigned frames = 0;  // since the ratio was last set
} g_clock;

bool hidden();

// The Wii Remote's buttons, from the settings' bindings (settings.hpp's
// Controls): each key, controller button and the mouse's left button presses
// the controls it's bound to. Compiled into tables when the settings change
// (rebuild_controls); the main thread reads and updates them.
struct CompiledControls {
    std::vector<std::pair<SDL_Scancode, uint16_t>> keys;  // key -> Wii buttons
    std::array<uint16_t, 32> pad{};                       // PadButton bit index -> Wii buttons
} g_controls;
std::atomic<uint32_t> g_pad_bits{0};  // the controllers' buttons held (PadButton bits), the mouse's as kPadClick
bool g_mouse_held = false;

void rebuild_controls() {
    CompiledControls c;
    const Controls& from = settings().controls;
    for (unsigned i = 0; i < kControlCount; ++i) {
        const uint16_t button = control_button(Control(i));
        for (const Binding& b : from[i]) {
            if (b.kind == Binding::kKey) {
                const SDL_Scancode code = SDL_GetScancodeFromName(b.key.c_str());
                if (code == SDL_SCANCODE_UNKNOWN)
                    std::fprintf(stderr, "qilin_run: controls: SDL doesn't know a key named \"%s\" (%s): it does nothing\n",
                                 b.key.c_str(), control_name(Control(i)));
                else c.keys.emplace_back(code, button);
            } else {
                const uint32_t bit = b.kind == Binding::kMouse ? uint32_t(kPadClick) : b.pad;
                if (bit) c.pad[std::countr_zero(bit)] |= button;
            }
        }
    }
    g_controls = std::move(c);
}

void update_keys() {
    const bool* state = SDL_GetKeyboardState(nullptr);
    uint16_t held = 0;
    for (const auto& [code, button] : g_controls.keys)
        if (state[code]) held |= button;
    g_keys.store(held, std::memory_order_relaxed);
}

// A controller's buttons, by an Xbox pad's names; the left stick counts as
// the d-pad past half way, as do the triggers as buttons.
//
// Every gamepad SDL knows is opened and any of them plays (their buttons
// add up), as the other recomps do it (Zelda64Recomp reads all pads,
// UnleashedRecomp the one last touched). Taking only the first one listed
// went wrong on computers with other game devices: a virtual pad or a wheel
// listed first took the place and the real controller was never read.
uint32_t pad_bits(SDL_Gamepad* pad) {
    uint32_t held = 0;
    auto button = [pad](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
    auto axis = [pad](SDL_GamepadAxis a) { return int(SDL_GetGamepadAxis(pad, a)); };
    const int x = axis(SDL_GAMEPAD_AXIS_LEFTX), y = axis(SDL_GAMEPAD_AXIS_LEFTY);
    if (button(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -16000) held |= kPadUp;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 16000) held |= kPadDown;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -16000) held |= kPadLeft;
    if (button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 16000) held |= kPadRight;
    if (button(SDL_GAMEPAD_BUTTON_SOUTH)) held |= kPadA;
    if (button(SDL_GAMEPAD_BUTTON_EAST)) held |= kPadB;
    if (button(SDL_GAMEPAD_BUTTON_WEST)) held |= kPadX;
    if (button(SDL_GAMEPAD_BUTTON_NORTH)) held |= kPadY;
    if (button(SDL_GAMEPAD_BUTTON_START)) held |= kPadStart;
    if (button(SDL_GAMEPAD_BUTTON_BACK)) held |= kPadBack;
    if (button(SDL_GAMEPAD_BUTTON_GUIDE)) held |= kPadGuide;
    if (button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER)) held |= kPadLB;
    if (button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) held |= kPadRB;
    if (axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) held |= kPadLT;
    if (axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) held |= kPadRT;
    if (button(SDL_GAMEPAD_BUTTON_LEFT_STICK)) held |= kPadL3;
    if (button(SDL_GAMEPAD_BUTTON_RIGHT_STICK)) held |= kPadR3;
    return held;
}

void update_pad() {
    uint32_t bits = g_mouse_held ? uint32_t(kPadClick) : 0u;
    for (SDL_Gamepad* pad : g_pads) bits |= pad_bits(pad);
    g_pad_bits.store(bits, std::memory_order_relaxed);
    uint16_t held = 0;
    for (uint32_t rest = bits; rest; rest &= rest - 1) held |= g_controls.pad[std::countr_zero(rest)];
    g_pad_buttons.store(held, std::memory_order_relaxed);
}
const char* pad_type(SDL_Gamepad* pad) {
    const char* s = SDL_GetGamepadStringForType(SDL_GetGamepadType(pad));
    return s ? s : "unknown";
}

void attach_pad(SDL_JoystickID id) {
    for (SDL_Gamepad* pad : g_pads)
        if (SDL_GetGamepadID(pad) == id) return;  // already open
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) {
        std::fprintf(stderr, "qilin_run: controller: a gamepad can't be opened (%s)\n", SDL_GetError());
        return;
    }
    g_pads.push_back(pad);
    std::fprintf(stderr, "qilin_run: controller: %s (%s, %04X:%04X) plays the Wii Remote\n", SDL_GetGamepadName(pad), pad_type(pad),
                 SDL_GetGamepadVendor(pad), SDL_GetGamepadProduct(pad));
    update_pad();
}
void detach_pad(SDL_JoystickID id) {
    for (auto it = g_pads.begin(); it != g_pads.end(); ++it)
        if (SDL_GetGamepadID(*it) == id) {
            std::fprintf(stderr, "qilin_run: controller: %s disconnected\n", SDL_GetGamepadName(*it));
            SDL_CloseGamepad(*it);
            g_pads.erase(it);
            break;
        }
    update_pad();
}
// A game device SDL sees but has no gamepad layout for (a wheel, a flight
// stick): said once, so it's known why it does nothing.
void note_joystick(SDL_JoystickID id) {
    if (SDL_IsGamepad(id)) return;
    const char* name = SDL_GetJoystickNameForID(id);
    std::fprintf(stderr, "qilin_run: controller: %s isn't a gamepad SDL knows the layout of: not used\n", name ? name : "a game device");
}

// The gamepads already connected, from SDL's list (not only its "added"
// events, which the Ghoulies recomp found it couldn't rely on at start-up).
// attach_pad skips one that is open already.
void attach_connected() {
    int n = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        for (int i = 0; i < n; ++i) attach_pad(ids[i]);
        SDL_free(ids);
    }
    if (SDL_JoystickID* ids = SDL_GetJoysticks(&n)) {
        for (int i = 0; i < n; ++i) note_joystick(ids[i]);
        SDL_free(ids);
    }
}

void apply_rumble() {
    const bool on = g_rumble && settings().rumble;
    for (SDL_Gamepad* pad : g_pads) SDL_RumbleGamepad(pad, on ? 0xA000 : 0, on ? 0xA000 : 0, on ? 60000 : 0);
}
// Opens the default output. `retry`: a later attempt after the device
// couldn't be opened (said only when it works). When Windows can't open the
// device at all (a USB interface's driver stuck: WASAPI's "can't initialize
// audio client", 0x8889000F, found 2026-11-20), every program is silent;
// the main loop tries again every 2 s, so sound comes when the device does.
// The least the queue can be held at for a device: the device takes a whole
// period at each read, so just before one the queue must hold a period, and
// the game's frames come 3 ms at a time with a little jitter; the queue's
// average is about half a period above its low point. So a period plus 4 ms
// (14 ms with a 10 ms period, 7 with a 3 ms one). A lower setting is raised
// to it, and said.
void set_audio_floor(double period_ms) {
    const int floor_ms = int(std::ceil(period_ms)) + 4;
    g_floor_ms.store(floor_ms, std::memory_order_relaxed);
    if (int(settings().audio_latency_ms) < floor_ms)
        std::fprintf(stderr, "qilin_run: audio: %u ms is below what this device can keep up (%d ms); holding at %d ms\n",
                     settings().audio_latency_ms, floor_ms, floor_ms);
}

bool open_audio(bool retry = false) {
#if defined(_WIN32)
    // Exclusive mode first (settings: [audio] exclusive): the device's
    // shortest period (3 ms on a USB interface, against 10 shared).
    if (settings().exclusive_audio) {
        wasapi::Opened o;
        const char* why = "";
        if (wasapi::open(o, why)) {
            g_exclusive.store(true, std::memory_order_relaxed);
            std::fprintf(stderr, "qilin_run: audio%s: exclusive, %d Hz, %d channels, %s, a %.2f ms period\n",
                         retry ? " (the device is back)" : "", o.rate, o.channels, o.sample, o.period_ms);
            set_audio_floor(o.period_ms);
            g_audio.store(o.stream, std::memory_order_release);
            return true;
        }
        if (!retry) std::fprintf(stderr, "qilin_run: audio: no exclusive mode (%s); sharing the device\n", why);
    }
    g_exclusive.store(false, std::memory_order_relaxed);
#endif
    // A small device buffer: the latency is the queue (held at the target)
    // plus this. SDL asks WASAPI for its low-latency shared mode with it
    // (IAudioClient3), which a driver grants down to its own minimum (a
    // Scarlett Solo: 7 ms; Windows' own HD Audio driver: about 3 ms).
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "128");
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kSampleRate};
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream) {
        // A driver may refuse a buffer that small: its own, then.
        SDL_ResetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    }
    if (!stream) {
        if (!retry)
            std::fprintf(stderr, "qilin_run: no audio output (%s): the system or the driver can't open the default device; "
                                 "trying again every 2 s\n", SDL_GetError());
        return false;
    }
    // The device starts paused: it plays once the queue has reached the
    // target (play_audio).
    SDL_AudioSpec out{};
    int frames = 0;
    if (SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(stream), &out, &frames) && out.freq > 0)
        std::fprintf(stderr, "qilin_run: audio%s: %d Hz, a %d-sample device buffer (%.1f ms), %u ms queued ahead\n",
                     retry ? " (the device is back)" : "", out.freq, frames, 1000.0 * frames / out.freq, settings().audio_latency_ms);
    if (out.freq > 0 && frames > 0) set_audio_floor(1000.0 * frames / out.freq);
    g_audio.store(stream, std::memory_order_release);
    return true;
}

// With vsync, the game's clock follows the display when they nearly agree:
// a 60 Hz display shows the game's 59.94 Hz frames with one shown twice
// every 16.7 s (a hitch in anything that scrolls), a 120 Hz one likewise
// every 8.3 s. Running the game at the display's rate (or a whole fraction
// of it) within 0.5% removes that; its sound follows the same clock (the
// audio clock resamples by the same speed). Set once at start: the window's
// display then. Not for runs that must replay alike (hidden, the bot).
double mode_hz(const SDL_DisplayMode& m) {
    return m.refresh_rate_numerator > 0 && m.refresh_rate_denominator > 0
               ? double(m.refresh_rate_numerator) / double(m.refresh_rate_denominator)
               : double(m.refresh_rate);
}
double field_hz() { return console().eurgb60 ? 60000.0 / 1001.0 : 50.0; }
// The game's speed that shows each frame for a whole number of refreshes
// (k) at `hz`, within 0.5%; 0 if there is none.
double lock_speed(double hz, int& k) {
    if (!(hz > 0)) return 0;
    // 60 Hz (EuRGB60): up to 480 Hz, each frame shown k times. PAL 50 Hz:
    // only a 50 Hz display (the owner's choice, 2026-11-20: no 100 or 150).
    const int most = console().eurgb60 ? 8 : 1;
    for (k = 1; k <= most; ++k)
        if (const double speed = hz / k / field_hz(); std::abs(speed - 1.0) <= 0.005) return speed;
    return 0;
}

// The display mode fullscreen switches to (settings: match_refresh), when
// the desktop's refresh can't carry the game's frames evenly (75 Hz, 144 Hz:
// at 75 Hz every fourth frame stays up for an extra refresh, a constant
// judder). The same resolution at a refresh that can (60 or 120 Hz), the
// highest such. F11 and Alt+Enter use it too; windowed play keeps the desktop.
SDL_DisplayMode g_fullscreen_mode{};
bool g_have_fullscreen_mode = false;
std::string g_mode_note;  // what the display does now, for the menu (main thread)

// Sets the window's fullscreen mode only if it differs: setting it while full
// screen switches the display, which comes back as a mode-change event.
void set_fullscreen_mode(const SDL_DisplayMode* want) {
    const SDL_DisplayMode* now = SDL_GetWindowFullscreenMode(g_window);
    const bool same = (!now && !want) || (now && want && now->w == want->w && now->h == want->h &&
                                          now->refresh_rate_numerator == want->refresh_rate_numerator &&
                                          now->refresh_rate_denominator == want->refresh_rate_denominator &&
                                          now->refresh_rate == want->refresh_rate);
    if (!same) SDL_SetWindowFullscreenMode(g_window, want);
}

void choose_fullscreen_mode() {
    g_have_fullscreen_mode = false;
    const bool wanted = !hidden() && settings().vsync && settings().match_refresh && !settings().variable_refresh;
    if (!wanted) {
        set_fullscreen_mode(nullptr);  // the desktop's mode
        return;
    }
    const SDL_DisplayID display = SDL_GetDisplayForWindow(g_window);
    const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
    int k = 0;
    if (!desktop || lock_speed(mode_hz(*desktop), k) != 0) {  // the desktop's own refresh suits
        set_fullscreen_mode(nullptr);
        return;
    }
    int n = 0;
    SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &n);
    if (!modes) return;
    const SDL_DisplayMode* best = nullptr;
    for (int i = 0; i < n; ++i) {
        const SDL_DisplayMode* m = modes[i];
        if (m->w != desktop->w || m->h != desktop->h || m->pixel_density != desktop->pixel_density) continue;
        if (lock_speed(mode_hz(*m), k) != 0 && (!best || mode_hz(*m) > mode_hz(*best))) best = m;
    }
    if (best) g_fullscreen_mode = *best, g_have_fullscreen_mode = true;
    SDL_free(modes);
    set_fullscreen_mode(g_have_fullscreen_mode ? &g_fullscreen_mode : nullptr);
}

// The game's clock and the display, made to agree for the display as it is
// now: at start, and again whenever full screen is switched, the display's
// mode changes or the menu's refresh settings do. With vsync, a refresh
// within 0.5% of the game's 59.94 Hz (or 50 Hz) times 1-8 locks the clock to
// it (each frame shown that many times) and the vblank lock steers the
// presents; otherwise the clock runs at its own rate. A variable-refresh
// display follows the frames: nothing is locked or switched. Not for runs
// that must replay alike (hidden, the bot).
void apply_display_mode() {
    if (hidden() || bot::active()) return;
    choose_fullscreen_mode();
    const bool full = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN) != 0;
    double hz = 0;
    if (full && g_have_fullscreen_mode) hz = mode_hz(g_fullscreen_mode);
    else if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_window))) hz = mode_hz(*m);
    g_display_hz.store(hz, std::memory_order_relaxed);
    char note[256];
    int k = 0;
    const double speed = settings().vsync && settings().lock_to_display && !settings().variable_refresh ? lock_speed(hz, k) : 0;
    if (speed != 0) {
        set_time_base_speed(speed);
        gx::lock_frames_to_vblank(speed);
        std::snprintf(note, sizeof note, "%.3f Hz: the game runs at %.3f Hz (%+.2f%%), each frame shown %d time%s, locked to the vblanks.",
                      hz, hz / k, (speed - 1.0) * 100.0, k, k == 1 ? "" : "s");
    } else {
        gx::lock_frames_to_vblank(0);  // off
        set_time_base_speed(1.0);
        if (settings().variable_refresh)
            std::snprintf(note, sizeof note, "Variable refresh: the display (up to %.0f Hz) follows the game's %.2f Hz frames "
                                             "(it must be on in the graphics driver).", hz, field_hz());
        else if (!settings().vsync)
            std::snprintf(note, sizeof note, "No vsync: frames are shown as soon as they're drawn (tearing).");
        else
            std::snprintf(note, sizeof note, "%.3f Hz: not a whole multiple of the game's %.2f Hz, so frames are shown unevenly%s.", hz,
                          field_hz(), full ? "" : g_have_fullscreen_mode ? " (full screen switches to a mode that suits)" : "");
    }
    if (g_mode_note != note) std::fprintf(stderr, "qilin_run: display %s\n", note);
    g_mode_note = note;
}
// QILIN_HIDDEN=1: the renderer runs as with a window, but the window isn't
// shown and nothing is played (scripted runs on a computer someone is
// using, with QILIN_FRAME_DUMP to see the frames).
bool hidden() {
    static const bool h = [] {
        const char* v = std::getenv("QILIN_HIDDEN");
        return v && *v && *v != '0';
    }();
    return h;
}

// The window: the largest whole multiple of the picture (640x480 at 4:3,
// 854x480 at 16:9) that fits nine tenths of the settings' display's usable
// area, centred on it.
SDL_Window* open_window() {
    const uint32_t width = console().widescreen ? 854 : 640, height = 480;
    uint32_t scale = 1;
    SDL_Rect usable{};
    const SDL_DisplayID display = chosen_display();
    if (SDL_GetDisplayUsableBounds(display, &usable))
        while (width * (scale + 1) <= uint32_t(usable.w) * 9 / 10 && height * (scale + 1) <= uint32_t(usable.h) * 9 / 10) ++scale;
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (hidden()) flags |= SDL_WINDOW_HIDDEN;
    else if (settings().fullscreen) flags |= SDL_WINDOW_FULLSCREEN;  // borderless, at the desktop's mode
    SDL_Window* w = SDL_CreateWindow(kGameName, int(width * scale), int(height * scale), flags);
    if (w) {
        SDL_SetWindowMinimumSize(w, 160, 120);
        const int at = int(SDL_WINDOWPOS_CENTERED_DISPLAY(display));
        SDL_SetWindowPosition(w, at, at);
    }
    return w;
}

}  // namespace

bool windowed() {
    static const bool w = [] {
        const char* v = std::getenv("QILIN_HEADLESS");
        return !(v && *v && *v != '0');
    }();
    return w;
}

uint16_t keyboard_buttons() {
    return g_keys.load(std::memory_order_relaxed) | g_pad_buttons.load(std::memory_order_relaxed);
}

void play_audio(const int16_t* lr, uint32_t frames) {
    SDL_AudioStream* const audio = g_audio.load(std::memory_order_acquire);
    if (!audio) return;
    if (g_clock.stream != audio) g_clock = AudioClock{audio};  // a new output (opened again): fill it first
    Stats& st = stats();
    const int target_ms = std::max(int(settings().audio_latency_ms), g_floor_ms.load(std::memory_order_relaxed));
    const int target = target_ms * kBytesPerMs, bytes = int(frames * 4);
    int queued = SDL_GetAudioStreamQueued(audio);
    if (queued < 0) return;  // the device is gone
    if (queued > target + max_extra_bytes(target)) {
        ++st.audio_frames_dropped;
        return;
    }
    if (g_clock.primed && queued == 0) {
        // Ran dry (a stall, or the pause for the menu): silence up to the
        // target at once, so the lag is the target again, not whatever the
        // queue grows back to.
        ++st.audio_underruns;
        static const std::vector<uint8_t> silence(size_t(200) * kBytesPerMs, 0);  // the most a target can be
        const int fill = std::max(target - bytes, 0);
        SDL_PutAudioStreamData(audio, silence.data(), fill & ~3);
        queued = fill & ~3;
    }
    SDL_PutAudioStreamData(audio, lr, bytes);
    queued += bytes;
    if (!g_clock.primed) {
        if (queued < target) return;
        g_clock.primed = true, g_clock.depth = queued, g_clock.frames = 0;
#if defined(_WIN32)
        if (g_exclusive.load(std::memory_order_relaxed)) {
            wasapi::start();
            return;
        }
#endif
        SDL_ResumeAudioStreamDevice(audio);
        return;
    }
    st.audio_queue_sum += uint64_t(queued), ++st.audio_queue_count;
    st.audio_queue_min = std::min<uint64_t>(st.audio_queue_min, uint64_t(queued));
    st.audio_queue_max = std::max<uint64_t>(st.audio_queue_max, uint64_t(queued));
    // The depth over about 200 ms (64 frames of 3 ms): the device takes the
    // queue in its own bursts, so one reading says little.
    g_clock.depth += (queued - g_clock.depth) / 64.0;
    g_audio_ms.store(g_clock.depth / kBytesPerMs, std::memory_order_relaxed);  // for the F3 readout
    if (++g_clock.frames < 16) return;  // the ratio every 48 ms
    g_clock.frames = 0;
    // Proportional: 10 ms too deep plays 1/3% faster at a 30 ms target, which
    // takes it back in about 3 s. The clocks' own difference (a few in ten
    // thousand) leaves a steady error of about a millisecond.
    const double error = (g_clock.depth - target) / target;
    const double ratio = time_base_speed() * (1.0 + std::clamp(error * 0.01, -0.005, 0.005));
    st.audio_ratio = ratio;
    SDL_SetAudioStreamFrequencyRatio(audio, float(ratio));
}

uint32_t pad_bits_held() { return g_pad_bits.load(std::memory_order_relaxed); }
void controls_changed() {
    rebuild_controls();
    update_keys();
    update_pad();
}
double audio_queued_ms() { return g_audio_ms.load(std::memory_order_relaxed); }
double display_hz() { return g_display_hz.load(std::memory_order_relaxed); }
const std::string& mode_note() { return g_mode_note; }
void display_settings_changed() { apply_display_mode(); }

void rumble(bool on) {
    if (g_rumble.exchange(on) == on || !g_wake_event) return;
    SDL_Event e{};
    e.type = g_wake_event;  // the main thread applies it (SDL_PushEvent is thread-safe)
    SDL_PushEvent(&e);
}

// qilin_run --controllers: every game device SDL sees, then each change of
// the Wii Remote's buttons the controllers make, for `seconds`. The same
// code paths as the game's (attach_pad, update_pad), with no game.
bool test_controllers(unsigned seconds) {
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        std::fprintf(stderr, "qilin_run: SDL can't start: %s\n", SDL_GetError());
        return false;
    }
    std::fprintf(stderr, "qilin_run: SDL %d.%d.%d; looking for controllers for %u s (press buttons)\n", SDL_MAJOR_VERSION,
                 SDL_MINOR_VERSION, SDL_MICRO_VERSION, seconds);
    attach_connected();
    static constexpr struct {
        uint16_t bit;
        const char* name;
    } kNames[] = {{kButtonA, "A"},       {kButtonB, "B"},         {kButton1, "1"},       {kButton2, "2"},
                  {kButtonPlus, "+"},    {kButtonMinus, "-"},     {kButtonHome, "Home"}, {kButtonUp, "Up"},
                  {kButtonDown, "Down"}, {kButtonLeft, "Left"},   {kButtonRight, "Right"}};
    rebuild_controls();  // the settings' bindings, as in the game
    const uint64_t end = SDL_GetTicks() + 1000ull * seconds;
    uint16_t last = 0;
    unsigned changes = 0;
    while (SDL_GetTicks() < end) {
        SDL_Event e;
        if (!SDL_WaitEventTimeout(&e, 50)) continue;
        switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED: attach_pad(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: detach_pad(e.gdevice.which); break;
        case SDL_EVENT_JOYSTICK_ADDED: note_joystick(e.jdevice.which); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: update_pad(); break;
        default: break;
        }
        const uint16_t now = g_pad_buttons.load(std::memory_order_relaxed);
        if (now != last) {
            ++changes;
            std::fprintf(stderr, "qilin_run: the Wii Remote's buttons held:");
            for (const auto& n : kNames)
                if (now & n.bit) std::fprintf(stderr, " %s", n.name);
            std::fprintf(stderr, "%s\n", now ? "" : " (none)");
            last = now;
        }
    }
    std::fprintf(stderr, "qilin_run: %zu gamepad(s) open, %u button change(s) seen\n", g_pads.size(), changes);
    SDL_Quit();
    return true;
}

bool run(const std::function<void()>& game) {
    SDL_SetHint(SDL_HINT_APP_NAME, kGameName);
    // Controllers are read whether or not the window has the focus (SDL drops
    // their input in the background otherwise: a click elsewhere, an overlay,
    // and the controller seemed dead).
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "qilin_run: SDL can't start: %s\n", SDL_GetError());
        return false;
    }
    g_wake_event = SDL_RegisterEvents(1);
    g_window = open_window();
    if (!g_window) {
        std::fprintf(stderr, "qilin_run: no window: %s\n", SDL_GetError());
        return false;
    }
    if (!gx::init_vulkan(g_window)) return false;
    if (!hidden()) open_audio();
    rebuild_controls();
    attach_connected();
    std::fprintf(stderr,
                 "qilin_run: settings: %s (preset %s, %ux internal resolution, %ux anisotropic filtering)\n"
                 "qilin_run: controls as the settings file has them (the menu's Controls page sets them); "
                 "F1 or Esc: settings, F3: frame times, F11 or Alt+Enter: full screen\n",
                 settings_path().string().c_str(), settings().preset.c_str(), gx::internal_scale(), settings().anisotropy);

    apply_display_mode();  // before the game's threads: the clock's speed is set here first
    g_report_extra = gx::vblank_report;
    std::thread(game).detach();
    Menu menu(g_window);
    // QILIN_MENU_TOGGLE=<seconds>,...: the menu opened or closed at those
    // times after start (scripted runs: the pause, the overlay).
    std::vector<uint64_t> toggles;
    if (const char* v = std::getenv("QILIN_MENU_TOGGLE"))
        for (const char* p = v; *p;) {
            char* end = nullptr;
            const double s = std::strtod(p, &end);
            if (end == p) {
                std::fprintf(stderr, "qilin_run: QILIN_MENU_TOGGLE is seconds, comma-separated\n");
                return false;
            }
            toggles.push_back(SDL_GetTicks() + uint64_t(s * 1000));
            p = *end == ',' ? end + 1 : end;
        }
    auto quit = [] {
        std::fflush(stderr);
        std::_Exit(0);
    };
    for (;;) {
        SDL_Event e;
        // The menu draws a frame each pass while it's up (vsync paces it).
        bool got = SDL_WaitEventTimeout(&e, menu.busy() ? 10 : 100);
        const bool was_shown = menu.is_open();
        while (got) {
            bool taken = false;
            try {  // F1 or Esc closing the menu applies the settings
                taken = menu.handle(e);
            } catch (const Stop& s) {
                report_stop(s);
                std::fflush(stderr);
                std::_Exit(1);
            }
            if (taken) {
                got = SDL_PollEvent(&e);
                continue;
            }
            switch (e.type) {
            case SDL_EVENT_QUIT:
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                if (menu.is_open()) save_settings();
                quit();
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!e.key.repeat && (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT)))) {
                    const bool full = !(SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN);
                    SDL_SetWindowFullscreen(g_window, full);
                    // Kept for the next start. With the game running, only
                    // the main thread writes settings, and the game doesn't
                    // read this one; the open menu saves when it closes.
                    settings().fullscreen = full;
                    if (!menu.is_open()) save_settings();
                    break;  // Alt+Enter isn't the remote's +
                }
                update_keys();
                break;
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_WINDOW_FOCUS_LOST: update_keys(); break;
            case SDL_EVENT_GAMEPAD_ADDED: attach_pad(e.gdevice.which); break;
            case SDL_EVENT_JOYSTICK_ADDED: note_joystick(e.jdevice.which); break;
            case SDL_EVENT_GAMEPAD_REMOVED: detach_pad(e.gdevice.which); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: update_pad(); break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_LEFT) {
                    g_mouse_held = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
                    update_pad();
                }
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: gx::window_resized(); break;
            // Full screen switched (F11, the menu) or the display changed:
            // the game's clock and the vblank lock follow the refresh now.
            case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
            case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
            case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
            case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: apply_display_mode(); break;
            default:
                if (e.type == g_wake_event) apply_rumble();
                break;
            }
            got = SDL_PollEvent(&e);
        }
        try {  // the renderer can stop on this thread too while the menu has it
            if (!toggles.empty() && SDL_GetTicks() >= toggles.front()) {
                toggles.erase(toggles.begin());
                menu.toggle();
            }
            menu.tick();
        } catch (const Stop& s) {
            report_stop(s);
            std::fflush(stderr);
            std::_Exit(1);
        }
        if (menu.quit_requested()) quit();
#if defined(_WIN32)
        // The exclusive device went away (unplugged, the default changed):
        // let go of it (its stream is left, the game's thread may be in it)
        // and open the output again below.
        if (g_exclusive.load(std::memory_order_relaxed) && wasapi::lost()) {
            std::fprintf(stderr, "qilin_run: audio: the device went away; opening the output again\n");
            g_exclusive.store(false, std::memory_order_relaxed);
            g_audio.store(nullptr, std::memory_order_release);
        }
#endif
        // No sound device yet (it couldn't be opened): try again every 2 s.
        if (!hidden() && !g_audio.load(std::memory_order_relaxed)) {
            static uint64_t next_try = 0;
            if (const uint64_t now = SDL_GetTicks(); now >= next_try) {
                if (next_try) open_audio(true);
                next_try = now + 2000;
            }
        }
        if (was_shown != menu.is_open()) {  // opened or closed: what's held now is the game's again
            update_keys();
            update_pad();
            apply_rumble();
        }
    }
}
}  // namespace qilin::rt::host
