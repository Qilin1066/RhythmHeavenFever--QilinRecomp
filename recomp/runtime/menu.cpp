// The settings menu (menu.hpp). Runtime code: no std::format.
#include "menu.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "gx_vulkan.hpp"
#include "host.hpp"
#include "native.hpp"
#include "path.hpp"
#include "pause.hpp"
#include "settings.hpp"
#include "threads.hpp"

#ifndef QILIN_VERSION
#define QILIN_VERSION "?"
#endif

namespace qilin::rt {

namespace {

constexpr uint64_t kHintMs = 8000;     // how long the hint shows at the start
constexpr uint64_t kCaptureMs = 6000;  // how long a control waits to be pressed
constexpr const char* kHint = "F1 or Esc (controller: Back + Start): settings";

// ImGui's frame is built on the main thread and drawn by whichever thread
// records the window's frames (the renderer's thread, or the main thread's
// while the game is parked): the lock keeps the two apart.
std::mutex g_draw_lock;
bool g_has_draw = false;  // under g_draw_lock: ImGui's draw data is a whole frame

// ImGui's Vulkan backend loads its functions through the renderer's loader.
PFN_vkGetInstanceProcAddr g_gipa = nullptr;
VkInstance g_instance = VK_NULL_HANDLE;
PFN_vkVoidFunction load(const char* name, void*) { return g_gipa(g_instance, name); }

void check(VkResult r) {
    if (r < 0) std::fprintf(stderr, "qilin_run: settings menu: Vulkan error %d\n", int(r));
}

constexpr const char* kLanguages[] = {"Japanese", "English", "German", "French", "Spanish", "Italian"};

std::string display_name(SDL_DisplayID id) {
    const char* n = SDL_GetDisplayName(id);
    return n ? n : "";
}

// Centres the window on a display; full screen moves with it.
void move_to_display(SDL_Window* w, SDL_DisplayID id) {
    const bool full = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
    if (full) SDL_SetWindowFullscreen(w, false);
    const int at = int(SDL_WINDOWPOS_CENTERED_DISPLAY(id));
    SDL_SetWindowPosition(w, at, at);
    if (full) SDL_SetWindowFullscreen(w, true);
}

uint32_t lowest_bit(uint32_t v) { return v & (~v + 1); }

// The mouse and the pad's navigation are the control's while one is being set.
void menu_navigation(bool on) {
    ImGuiIO& io = ImGui::GetIO();
    if (on) io.ConfigFlags = (io.ConfigFlags | ImGuiConfigFlags_NavEnableGamepad) & ~ImGuiConfigFlags_NoMouse;
    else io.ConfigFlags = (io.ConfigFlags & ~ImGuiConfigFlags_NavEnableGamepad) | ImGuiConfigFlags_NoMouse;
}

}  // namespace

uint32_t chosen_display() {
    const std::string& want = settings().monitor;
    if (!want.empty()) {
        int n = 0;
        if (SDL_DisplayID* ids = SDL_GetDisplays(&n)) {
            SDL_DisplayID found = 0;
            for (int i = 0; i < n && !found; ++i)
                if (display_name(ids[i]) == want) found = ids[i];
            SDL_free(ids);
            if (found) return found;
        }
        std::fprintf(stderr, "qilin_run: the settings' display \"%s\" isn't connected; using the main one\n", want.c_str());
    }
    return SDL_GetPrimaryDisplay();
}

Menu::Menu(SDL_Window* window) : window_(window) {
    gx::OverlayTarget t{};
    if (!gx::overlay_target(t)) {
        std::fprintf(stderr, "qilin_run: the window's images can't be drawn into: no settings menu\n");
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the settings file is the only file
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    // 1.25x, as the other recomps have it, times the display's own scale.
    float dpi = SDL_GetWindowDisplayScale(window);
    if (!(dpi > 0.0f)) dpi = 1.0f;
    ImGui::GetStyle().ScaleAllSizes(1.25f * dpi);
    ImGui::GetStyle().FontScaleMain = 1.25f;
    ImGui::GetStyle().FontScaleDpi = dpi;

    sdl_backend_ = ImGui_ImplSDL3_InitForVulkan(window);
    // Every controller works the menu, not only the first one SDL lists
    // (which can be a virtual pad, or a wheel's base).
    if (sdl_backend_) ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    g_gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(t.get_instance_proc_addr);
    g_instance = static_cast<VkInstance>(t.instance);
    if (!sdl_backend_ || !ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, load)) {
        std::fprintf(stderr, "qilin_run: the settings menu can't start (its SDL or Vulkan side)\n");
        return;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = g_instance;
    ii.PhysicalDevice = static_cast<VkPhysicalDevice>(t.physical_device);
    ii.Device = static_cast<VkDevice>(t.device);
    ii.QueueFamily = t.queue_family;
    ii.Queue = static_cast<VkQueue>(t.queue);
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
    ii.MinImageCount = std::max(2u, t.images);
    ii.ImageCount = ii.MinImageCount;
    ii.UseDynamicRendering = true;
    static VkFormat format;  // the backend keeps a pointer to it
    format = static_cast<VkFormat>(t.format);
    VkPipelineRenderingCreateInfoKHR& pr = ii.PipelineInfoMain.PipelineRenderingCreateInfo;
    pr = {};
    pr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    pr.colorAttachmentCount = 1, pr.pColorAttachmentFormats = &format;
    ii.CheckVkResultFn = check;
    vulkan_backend_ = ImGui_ImplVulkan_Init(&ii);
    if (!vulkan_backend_) {
        std::fprintf(stderr, "qilin_run: the settings menu can't start (its Vulkan side)\n");
        return;
    }
    gx::set_overlay([](void* cb) {
        std::lock_guard l(g_draw_lock);
        if (g_has_draw) ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<VkCommandBuffer>(cb));
    });
    ok_ = true;
    hint_ = true;
    hint_until_ = SDL_GetTicks() + kHintMs;
    std::fprintf(stderr, "qilin_run: %s\n", kHint);
}

Menu::~Menu() {
    gx::set_overlay(nullptr);
    if (vulkan_backend_) ImGui_ImplVulkan_Shutdown();
    if (sdl_backend_) ImGui_ImplSDL3_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

void Menu::save() {
    settings().fullscreen = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    if (!save_settings()) std::fprintf(stderr, "qilin_run: the settings can't be saved to %s\n", qilin::utf8_path(settings_path()).c_str());
}

bool Menu::handle(const SDL_Event& e) {
    if (!ok_) return false;
    // Looking at the picture: any key or button brings the menu back.
    if (peek_ && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)) {
        peek_ = false;
        return true;
    }
    // Setting a control: the key pressed (controller buttons and clicks are read in tick).
    if (capture_ >= 0 && e.type == SDL_EVENT_KEY_DOWN) {
        if (!e.key.repeat) {
            const char* name = SDL_GetScancodeName(e.key.scancode);
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                capture_ = -1, wait_release_ = true, note_ = "Cancelled: unchanged.";
            } else if (name && *name && !key_reserved(name)) {
                bind(capture_, Binding::kKey, 0, name);
            }
        }
        return true;
    }
    const bool shown = open_ || opening_;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.key == SDLK_F11 || (e.key.key == SDLK_RETURN && (e.key.mod & SDL_KMOD_ALT))) return false;  // full screen: the host's
        if (!e.key.repeat && (e.key.key == SDLK_F1 || e.key.key == SDLK_ESCAPE)) {
            toggle();
            return true;
        }
        if (!e.key.repeat && e.key.key == SDLK_F3) {  // the frame times, kept in the settings
            settings().performance_overlay = !settings().performance_overlay;
            if (!open_) save();
            return true;
        }
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
        if (capture_ >= 0) break;
        // Back and Start together (alone they're the remote's - and +);
        // with the menu open, Start alone closes it too.
        SDL_Gamepad* pad = SDL_GetGamepadFromID(e.gbutton.which);
        const bool back = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
        const bool start = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
        if ((back && start) || (open_ && e.gbutton.button == SDL_GAMEPAD_BUTTON_START)) {
            toggle();
            return true;
        }
        break;
    }
    default: break;
    }
    if (open_) ImGui_ImplSDL3_ProcessEvent(&e);
    if (!shown) return false;
    switch (e.type) {  // the game doesn't see input while the menu is up
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: return true;
    default: return false;
    }
}

void Menu::toggle() {
    if (open_ || opening_) {
        close();
        return;
    }
    opening_ = true;
    request_pause(true);  // the game parks at the end of its frame; tick() opens then
}

void Menu::close() {
    if (open_) {
        save();
        gx::apply_display_settings();  // the game is still parked: the renderer is ours
    }
    open_ = opening_ = peek_ = false;
    capture_ = -1;
    wait_release_ = false;
    menu_navigation(true);
    {
        std::lock_guard l(g_draw_lock);
        g_has_draw = false;
    }
    request_pause(false);
}

// An input pressed while setting `control`: it presses that control now,
// and is taken from any other it pressed.
void Menu::bind(int control, unsigned kind, uint32_t pad, const std::string& key) {
    Binding b;
    b.kind = Binding::Kind(kind), b.pad = pad, b.key = key;
    Controls& all = settings().controls;
    note_.clear();
    for (unsigned c = 0; c < kControlCount; ++c) {
        const auto it = std::find(all[c].begin(), all[c].end(), b);
        if (it == all[c].end()) continue;
        if (int(c) != control) note_ = binding_label(b) + " moved here from " + control_label(Control(c)) + ".";
        all[c].erase(it);
    }
    all[control].push_back(b);
    host::controls_changed();
    capture_ = -1;
    wait_release_ = true;
}

void Menu::tick() {
    if (!ok_) return;
    if (opening_ && game_parked()) {
        opening_ = false, open_ = true, hint_ = false;
        ImGui::GetIO().ClearInputKeys();
    }
    if (open_ && capture_ >= 0) {
        // A controller's button or a click pressed since it began sets the control.
        const uint32_t held = host::pad_bits_held();
        const uint32_t fresh = held & ~capture_held_;
        capture_held_ &= held;  // let go, a button counts again
        if (fresh) {
            const uint32_t bit = lowest_bit(fresh);
            if (bit == kPadClick) bind(capture_, Binding::kMouse, 0, {});
            else bind(capture_, Binding::kPad, bit, {});
        } else if (SDL_GetTicks() >= capture_until_) {
            capture_ = -1, wait_release_ = true;
            note_ = "Nothing was pressed: unchanged.";
        }
    }
    // The mouse and the pad's navigation come back once the press that set a control is let go.
    if (wait_release_ && host::pad_bits_held() == 0) {
        menu_navigation(true);
        wait_release_ = false;
    }
    if (open_) {
        {
            std::lock_guard l(g_draw_lock);
            ImGui_ImplVulkan_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            if (!peek_) draw_menu();
            if (peek_ && settings().performance_overlay) build_frame_times();
            ImGui::Render();
            g_has_draw = true;
        }
        if (close_asked_ || quit_) {
            close_asked_ = false;
            if (quit_) save();
            else close();
            return;
        }
        gx::present_paused();
        return;
    }
    if (hint_ && SDL_GetTicks() >= hint_until_) hint_ = false;
    std::lock_guard l(g_draw_lock);
    if (!hint_ && !settings().performance_overlay) {
        g_has_draw = false;
        return;
    }
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (hint_) build_hint();
    if (settings().performance_overlay) build_frame_times();
    ImGui::Render();
    g_has_draw = true;
}

bool Menu::busy() const { return open_ || opening_ || hint_ || settings().performance_overlay; }

void Menu::build_hint() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 12.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad, vp->WorkPos.y + vp->WorkSize.y - pad), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##hint", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(kHint);
    ImGui::End();
}

// F3: how evenly frames reach the screen, top left. The lows are the frame
// rates of the slowest 1% and 0.1% of frames over the last 20 s.
void Menu::build_frame_times() {
    const gx::FrameTimeSummary s = gx::frame_time_summary();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8.0f, vp->WorkPos.y + 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##frame times", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (s.frames == 0) {
        ImGui::TextUnformatted("frame times: waiting for frames");
    } else {
        ImGui::Text("%.1f fps  %.2f ms", s.fps, s.average_ms);
        ImGui::Text("1%% low %.1f  0.1%% low %.1f", s.low1_fps, s.low01_fps);
        ImGui::Text("worst %.1f ms", s.worst_ms);
        // The game draws 60 frames a second, as on the Wii; a faster display
        // shows each one more than once, with less wait before it's seen.
        const double hz = host::display_hz();
        if (hz > 0) {
            const double k = hz / 60.0;
            if (settings().variable_refresh) ImGui::Text("display up to %.0f Hz: variable refresh", hz);
            else if (std::fabs(k - std::round(k)) * 60.0 > 0.3) ImGui::Text("display %.1f Hz: not a multiple of 60 (uneven)", hz);
            else if (std::round(k) >= 2) ImGui::Text("display %.0f Hz: each frame shown %d times", hz, int(std::round(k)));
            else ImGui::Text("display %.0f Hz", hz);
        }
        const std::vector<float> recent = gx::recent_frame_times(240);
        ImGui::PlotLines("##ft", recent.data(), int(recent.size()), 0, nullptr, 0.0f, 50.0f, ImVec2(240.0f, 48.0f));
    }
    if (const double slack = gx::vblank_slack_ms(); slack > 0) ImGui::Text("presented %.1f ms before the vblank", slack);
    ImGui::Text("sound: %.1f ms queued", host::audio_queued_ms());
    ImGui::End();
}

void Menu::draw_menu() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted("Beat the Beat: Rhythm Paradise - settings (the game is paused)");
    // Close and Quit, top right on every page.
    const ImGuiStyle& st = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Close (Esc)").x + ImGui::CalcTextSize("Quit the game").x + 4 * st.FramePadding.x + st.ItemSpacing.x;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttons + ImGui::GetCursorPosX());
    if (ImGui::Button("Close (Esc)")) close_asked_ = true;  // tick() closes it, outside the draw lock
    ImGui::SameLine();
    if (ImGui::Button("Quit the game")) quit_ = true;
    ImGui::Separator();
    if (ImGui::BeginTabBar("pages")) {
        if (ImGui::BeginTabItem("Controls")) {
            controls_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            display_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
            game_page();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void Menu::controls_page() {
    Settings& s = settings();
    ImGui::SeparatorText("Controllers");
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (n == 0) ImGui::TextDisabled("None connected: the keyboard and mouse play the Wii Remote.");
    for (int i = 0; i < n; ++i) {
        const char* name = SDL_GetGamepadNameForID(ids[i]);
        ImGui::BulletText("%s", name ? name : "A controller");  // each one plays the Wii Remote
    }
    SDL_free(ids);
    ImGui::Checkbox("Rumble", &s.rumble);

    ImGui::SeparatorText("The Wii Remote's buttons (held upright)");
    if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthStretch, 0.22f);
        ImGui::TableSetupColumn("Pressed by (click one to remove it)", ImGuiTableColumnFlags_WidthStretch, 0.63f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableHeadersRow();
        for (unsigned c = 0; c < kControlCount; ++c) {
            ImGui::PushID(int(c));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(control_label(Control(c)));
            ImGui::TableNextColumn();
            if (capture_ == int(c)) {
                const uint64_t now = SDL_GetTicks();
                const uint64_t left = capture_until_ > now ? (capture_until_ - now + 999) / 1000 : 0;
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Press a key, a controller button or click... (Esc: cancel, %u s)",
                                   unsigned(left));
            } else {
                auto& list = s.controls[c];
                if (list.empty()) ImGui::TextDisabled("unused");
                for (size_t i = 0; i < list.size(); ++i) {
                    ImGui::PushID(int(i));
                    if (i) ImGui::SameLine();
                    if (ImGui::SmallButton(binding_label(list[i]).c_str())) {
                        list.erase(list.begin() + std::ptrdiff_t(i));
                        host::controls_changed();
                        note_.clear();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(capture_ >= 0);
            if (ImGui::SmallButton("Add")) {
                capture_ = int(c);
                capture_until_ = SDL_GetTicks() + kCaptureMs;
                capture_held_ = host::pad_bits_held();
                note_.clear();
                menu_navigation(false);  // the mouse and the pad's navigation are the control's to take until it's set
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!note_.empty()) ImGui::TextUnformatted(note_.c_str());
    if (ImGui::Button("Back to the defaults")) {
        s.controls = default_controls();
        host::controls_changed();
        note_.clear();
    }
    ImGui::TextDisabled("An input added to one button moves there from another. Escape, F1, F3 and F11 are kept for the program.");
    ImGui::TextDisabled("F1 or Esc (controller: Back + Start): this menu. F3: frame times. F11 or Alt+Enter: full screen.");
}

void Menu::display_page() {
    Settings& s = settings();
    ImGui::SeparatorText("Window");
    bool full = (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN) != 0;
    if (ImGui::Checkbox("Full screen (F11 or Alt+Enter)", &full)) {
        SDL_SetWindowFullscreen(window_, full);
        s.fullscreen = full;
    }
    {
        int n = 0;
        SDL_DisplayID* ids = SDL_GetDisplays(&n);
        const std::string current = s.monitor.empty() ? "The main display" : s.monitor;
        if (ImGui::BeginCombo("Display", current.c_str())) {
            if (ImGui::Selectable("The main display", s.monitor.empty())) {
                s.monitor.clear();
                move_to_display(window_, SDL_GetPrimaryDisplay());
            }
            for (int i = 0; i < n; ++i) {
                const std::string name = display_name(ids[i]);
                ImGui::PushID(i);
                if (ImGui::Selectable(name.c_str(), name == s.monitor)) {
                    s.monitor = name;
                    move_to_display(window_, ids[i]);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        SDL_free(ids);
    }

    ImGui::SeparatorText("Picture");
    int mode = int(s.scaling);
    ImGui::RadioButton("Whole multiples (sharpest)", &mode, int(Scaling::integer));
    ImGui::SameLine();
    ImGui::RadioButton("Fit", &mode, int(Scaling::fit));
    ImGui::SameLine();
    ImGui::RadioButton("Fill the window", &mode, int(Scaling::stretch));
    s.scaling = Scaling(mode);
    int look = int(s.look);
    ImGui::RadioButton("Plain", &look, int(Look::plain));
    ImGui::SameLine();
    ImGui::RadioButton("CRT television", &look, int(Look::crt));
    s.look = Look(look);
    if (s.look == Look::crt) {
        int scan = int(s.scanlines), mask = int(s.mask);
        if (ImGui::SliderInt("Scanlines", &scan, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s.scanlines = uint32_t(scan);
        if (ImGui::SliderInt("Phosphor stripes", &mask, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s.mask = uint32_t(mask);
        ImGui::TextDisabled("The raster is the console's own lines: it shows best at Whole multiples, or a large window.");
    } else {
        ImGui::Checkbox("Smooth", &s.smooth);
        ImGui::SameLine();
    }
    ImGui::Checkbox("Vsync", &s.vsync);
    ImGui::Checkbox("Frame times (F3)", &s.performance_overlay);
    if (ImGui::Button("Look at the picture")) peek_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(any key or button comes back)");

    ImGui::SeparatorText("Refresh rate");
    const bool was_lock = s.lock_to_display, was_match = s.match_refresh, was_vrr = s.variable_refresh, was_vsync = s.vsync;
    ImGui::Checkbox("Variable refresh (G-Sync, FreeSync, VRR: on in the driver too)", &s.variable_refresh);
    ImGui::BeginDisabled(s.variable_refresh || !s.vsync);
    ImGui::Checkbox("Lock the game to the display's refresh", &s.lock_to_display);
    ImGui::Checkbox("Full screen at a multiple of 60 Hz (120 Hz on a 144 Hz monitor)", &s.match_refresh);
    ImGui::EndDisabled();
    if (was_lock != s.lock_to_display || was_match != s.match_refresh || was_vrr != s.variable_refresh || was_vsync != s.vsync)
        host::display_settings_changed();  // the mode and the lock follow at once
    if (!host::mode_note().empty()) ImGui::TextWrapped("%s", host::mode_note().c_str());

    ImGui::SeparatorText("Sound");
    int latency = int(s.audio_latency_ms);
    if (ImGui::SliderInt("Sound latency", &latency, 5, 200, "%d ms", ImGuiSliderFlags_AlwaysClamp))
        s.audio_latency_ms = uint32_t(latency);
    ImGui::TextDisabled("Lower: less lag between a press and its sound (held no lower than the device can keep up). Raise it if the sound crackles.");
#if defined(_WIN32)
    ImGui::Checkbox("Exclusive sound: the device for the game alone, less lag (from the next start)", &s.exclusive_audio);
#endif

    ImGui::SeparatorText("Enhancements");
    static constexpr const char* kPresets[] = {"original", "low-end", "desktop"};
    if (ImGui::BeginCombo("Preset", s.preset.c_str())) {
        for (const char* p : kPresets)
            if (ImGui::Selectable(p, s.preset == p)) apply_preset(s, p);
        ImGui::EndCombo();
    }
    int scale = int(s.internal_scale);
    char label[96];
    if (scale == 0) std::snprintf(label, sizeof label, "Auto (the screen's height: %ux now)", gx::internal_scale());
    else std::snprintf(label, sizeof label, "%dx (%dx%d)", scale, 640 * scale, 528 * scale);
    if (ImGui::SliderInt("Internal resolution", &scale, 0, 8, label, ImGuiSliderFlags_AlwaysClamp)) s.internal_scale = uint32_t(scale);
    ImGui::TextDisabled("The GPU's work grows with the square: 8x is 64 times the pixels of 1x, 4x 16 times.");
    const float limit = gx::anisotropy_limit();
    ImGui::BeginDisabled(limit <= 1.0f);
    static constexpr uint32_t kAniso[] = {1, 2, 4, 8, 16};
    std::snprintf(label, sizeof label, "%ux", s.anisotropy);
    if (ImGui::BeginCombo("Anisotropic filtering", label)) {
        for (uint32_t a : kAniso) {
            if (float(a) > limit && a > 1) continue;
            std::snprintf(label, sizeof label, a == 1 ? "%ux (the console's)" : "%ux", a);
            if (ImGui::Selectable(label, s.anisotropy == a)) s.anisotropy = a;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (limit <= 1.0f) ImGui::TextDisabled("This GPU has no anisotropic filtering.");

    ImGui::SeparatorText("Graphics device");
    const std::string in_use = gx::gpu_in_use();
    const std::string chosen = s.gpu.empty() ? "The best one" : s.gpu;
    if (ImGui::BeginCombo("Device", chosen.c_str())) {
        if (ImGui::Selectable("The best one", s.gpu.empty())) s.gpu.clear();
        for (const std::string& name : gx::gpu_names())
            if (ImGui::Selectable(name.c_str(), name == s.gpu)) s.gpu = name;
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("In use: %s. A change takes effect the next time the game starts.", in_use.c_str());
    ImGui::TextDisabled("The internal resolution and filtering take effect when this menu closes.");
}

void Menu::game_page() {
    Settings& s = settings();
    ImGui::SeparatorText("The Wii's own settings (read the next time the game starts)");
    int lang = std::clamp(int(s.language), 1, 5);  // this disc has English, German, French, Spanish and Italian
    if (ImGui::BeginCombo("Language", kLanguages[lang])) {
        for (int i = 1; i <= 5; ++i)
            if (ImGui::Selectable(kLanguages[i], i == lang)) s.language = uint8_t(i);
        ImGui::EndCombo();
    }
    ImGui::Checkbox("60 Hz (EuRGB60; off: PAL 50 Hz)", &s.sixty_hz);
    ImGui::Checkbox("Widescreen (16:9; off: 4:3)", &s.widescreen);
    ImGui::Checkbox("Progressive scan", &s.progressive);
    // The game plays surround as Dolby Pro Logic II (dpl2.hpp), for a
    // receiver or TV that decodes it.
    static constexpr const char* kSound[] = {"Mono", "Stereo", "Surround (Dolby Pro Logic II)"};
    int sound = std::clamp(int(s.sound_mode), 0, 2);
    if (ImGui::Combo("Sound", &sound, kSound, 3)) s.sound_mode = uint8_t(sound);
    ImGui::Checkbox("Sensor bar above the TV", &s.sensor_bar_above);
    int dpd = std::clamp(int(s.dpd_sensitivity), 1, 5);
    if (ImGui::SliderInt("Pointer sensitivity", &dpd, 1, 5, "%d", ImGuiSliderFlags_AlwaysClamp)) s.dpd_sensitivity = uint8_t(dpd);

    ImGui::SeparatorText("If the game is stuck");
    // The game is parked while the menu is open: every guest thread is still,
    // so their state can be read from here.
    if (ImGui::Button("Write a report to the log")) {
        std::fprintf(stderr, "qilin_run: a report asked for from the settings menu\n");
        report_stats();
        report_threads();
        std::fflush(stderr);
        report_note_ = "Written: what each of the game's threads waits on, and what it has read and played.";
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(for a black screen or a freeze: then quit, and send the log)");
    if (!report_note_.empty()) ImGui::TextDisabled("%s", report_note_.c_str());
    ImGui::TextWrapped("The log: %s", qilin::utf8_path(game_data_dir() / "qilin_run.log").c_str());

    ImGui::SeparatorText("About");
    ImGui::TextWrapped("Beat the Beat: Rhythm Paradise QilinRecomp %s: a static recompilation, the game's own code compiled for this computer.",
                       QILIN_VERSION);
    ImGui::TextWrapped("Graphics: Vulkan. Window, sound and input: SDL3 (zlib). Menu: Dear ImGui (MIT).");
    ImGui::TextWrapped("Settings file: %s", qilin::utf8_path(settings_path()).c_str());
    ImGui::TextWrapped("Saves and the pipeline cache: %s", qilin::utf8_path(game_data_dir()).c_str());
}

}  // namespace qilin::rt
