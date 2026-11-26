// LEONIDA_INPUT_SCRIPT, shared by both GameInput shims: the SDL3 one (gameinput.cpp, where the script drives the
// keyboard pad) and the one built without SDL3 (gameinput_no_sdl.cpp, where it drives a scripted pad of its own).
// The parser, the press timeline, the button names and the press log lines live here, once; nothing in this file
// needs SDL. docs/gameinput.md, "Scripted input", has the format.
//
//   LEONIDA_INPUT_SCRIPT="a@70,a@85,start@120"
//
// Each entry is button@seconds[:hold], the seconds counted from the shim's first update. A press is held for `hold`
// seconds (kHoldSeconds when the entry has none), then released: a longer hold reaches a title that reads its pad
// only every few seconds, as the retail title does at world-start frame rates. Buttons: a b x y start back lb rb
// up down left right; rsup and rsdown throw the right stick fully up or down for the same time (they replay the
// stick throws the press log writes). Spaces around an entry are allowed; empty entries, unknown buttons, invalid
// times (negative, non-finite, trailing text) and invalid holds (zero, negative, non-finite, more than
// kMaxHoldSeconds) are counted as rejected.
#pragma once
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "gameinput_abi.h"

namespace rt::gameinput::script {

constexpr double kHoldSeconds = 0.3;
constexpr double kMaxHoldSeconds = 600;  // a typo such as b@10:1100 should not hold B for the whole run

// One scriptable button: its name in the script and the press log, its GameInput gamepad bit, and its SDL3
// SDL_GamepadButton number, which the "scripted press of button <n>" line has always carried (tools/stage_timeline.py
// reads it). gameinput.cpp checks the numbers against SDL's enum.
struct Button {
    const char* name;
    uint32_t bit;
    int sdl;
};
inline constexpr Button kButtons[] = {
    {"a", gi::GameInputGamepadA, 0},              {"b", gi::GameInputGamepadB, 1},
    {"x", gi::GameInputGamepadX, 2},              {"y", gi::GameInputGamepadY, 3},
    {"start", gi::GameInputGamepadMenu, 6},       {"back", gi::GameInputGamepadView, 4},
    {"lb", gi::GameInputGamepadLeftShoulder, 9},  {"rb", gi::GameInputGamepadRightShoulder, 10},
    {"up", gi::GameInputGamepadDPadUp, 11},       {"down", gi::GameInputGamepadDPadDown, 12},
    {"left", gi::GameInputGamepadDPadLeft, 13},   {"right", gi::GameInputGamepadDPadRight, 14},
};

struct Press {
    uint32_t button = 0;  // a GameInput gamepad bit, or 0 for a stick throw
    int stick = 0;        // +1 rsup, -1 rsdown (GameInput's +Y is up)
    double at = 0;        // seconds
    double hold = kHoldSeconds;
};

struct Script {
    std::vector<Press> presses;
    size_t rejected = 0;
    uint32_t named = 0;  // every button the script presses at least once
    bool empty() const { return presses.empty(); }
};

// Read a hold duration followed only by optional whitespace.
inline bool parse_hold(const char* text, double& hold) {
    char* tail;
    hold = std::strtod(text, &tail);
    if (tail == text || !std::isfinite(hold) || hold <= 0 || hold > kMaxHoldSeconds) return false;
    while (std::isspace(static_cast<unsigned char>(*tail))) ++tail;
    return *tail == '\0';
}

// The script in `text`, matched without regard to case.
inline Script parse(std::string text) {
    for (auto& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    Script out;
    if (text.empty()) return out;
    constexpr char whitespace[] = " \t\r\n\f\v";
    size_t items = 0;
    for (size_t at = 0, end; at <= text.size(); at = end + 1) {
        ++items;
        end = std::min(text.find(',', at), text.size());
        const std::string item = text.substr(at, end - at);
        const size_t first = item.find_first_not_of(whitespace);
        const size_t sep = item.find('@');
        if (sep == std::string::npos || first >= sep) continue;
        const char* time = item.c_str() + sep + 1;
        char* tail;
        const double seconds = std::strtod(time, &tail);
        if (tail == time || !std::isfinite(seconds) || seconds < 0) continue;
        double hold = kHoldSeconds;
        if (*tail == ':') {
            if (!parse_hold(tail + 1, hold)) continue;
        } else if (item.find_first_not_of(whitespace, static_cast<size_t>(tail - item.c_str())) != std::string::npos) {
            continue;
        }
        const auto is = [&](const char* name) { return item.compare(first, sep - first, name) == 0; };
        if (is("rsup") || is("rsdown")) {
            out.presses.push_back({0, is("rsup") ? 1 : -1, seconds, hold});
            continue;
        }
        for (const Button& b : kButtons) {
            if (is(b.name)) {
                out.presses.push_back({b.bit, 0, seconds, hold});
                out.named |= b.bit;
                break;
            }
        }
    }
    out.rejected = items - out.presses.size();
    return out;
}

// What the script holds `seconds` after its clock started: the buttons (GameInput bits) and the right stick throw.
// One decision per button: a later entry for the same button must not release what an earlier entry holds (it did:
// with several entries only the last one ever reached the title).
struct Held {
    uint32_t buttons = 0;
    int stick = 0;  // +1 up, -1 down, 0 at rest
};
inline Held held_at(const Script& script, double seconds) {
    Held h;
    for (const Press& p : script.presses) {
        if (seconds < p.at || seconds >= p.at + p.hold) continue;
        if (p.stick) h.stick = p.stick;
        else h.buttons |= p.button;
    }
    return h;
}

inline const Button* button_of(uint32_t bit) {
    for (const Button& b : kButtons)
        if (b.bit == bit) return &b;
    return nullptr;
}

// "[rt] gameinput: scripted press of button <n> at <t> s" for every button the script starts holding: `was` is the
// previous call's held set and is updated.
inline void log_scripted(uint32_t held, uint32_t* was, double seconds) {
    for (const Button& b : kButtons)
        if ((held & b.bit) && !(*was & b.bit))
            std::fprintf(stderr, "[rt] gameinput: scripted press of button %d at %.1f s\n", b.sdl, seconds);
    *was = held;
}

// The press log (LEONIDA_INPUT_LOG, on unless 0): every newly pressed button and every new right-stick throw in the
// script's own form, name@seconds, so a session played by hand can be replayed by joining the items with commas.
inline int stick_throw(float y) { return y > 0.6f ? 1 : y < -0.6f ? -1 : 0; }
inline void log_presses(uint32_t pressed, float right_y_was, float right_y, double seconds, const char* device) {
    for (const Button& b : kButtons)
        if (pressed & b.bit) std::fprintf(stderr, "[rt] gameinput: press %s@%.1f (%s)\n", b.name, seconds, device);
    const int was = stick_throw(right_y_was), is = stick_throw(right_y);
    if (is != was && is != 0)
        std::fprintf(stderr, "[rt] gameinput: press %s@%.1f (%s)\n", is > 0 ? "rsup" : "rsdown", seconds, device);
}

// UI navigation derived from a gamepad state, the same for every pad (VERIFY against the console's own mapping).
inline uint32_t ui_navigation(const gi::GameInputGamepadState& g) {
    using namespace gi;
    const uint32_t b = g.buttons;
    const float t = 0.5f;
    uint32_t u = 0;
    if (b & GameInputGamepadMenu) u |= GameInputUiNavigationMenu;
    if (b & GameInputGamepadView) u |= GameInputUiNavigationView;
    if (b & GameInputGamepadA) u |= GameInputUiNavigationAccept;
    if (b & GameInputGamepadB) u |= GameInputUiNavigationCancel;
    if (b & GameInputGamepadX) u |= GameInputUiNavigationContext1;
    if (b & GameInputGamepadY) u |= GameInputUiNavigationContext2;
    if (b & GameInputGamepadLeftThumbstick) u |= GameInputUiNavigationContext3;
    if (b & GameInputGamepadRightThumbstick) u |= GameInputUiNavigationContext4;
    if ((b & GameInputGamepadDPadUp) || g.leftThumbstickY > t) u |= GameInputUiNavigationUp;
    if ((b & GameInputGamepadDPadDown) || g.leftThumbstickY < -t) u |= GameInputUiNavigationDown;
    if ((b & GameInputGamepadDPadLeft) || g.leftThumbstickX < -t) u |= GameInputUiNavigationLeft;
    if ((b & GameInputGamepadDPadRight) || g.leftThumbstickX > t) u |= GameInputUiNavigationRight;
    if (b & GameInputGamepadLeftShoulder) u |= GameInputUiNavigationPageLeft;
    if (b & GameInputGamepadRightShoulder) u |= GameInputUiNavigationPageRight;
    if (g.leftTrigger > t) u |= GameInputUiNavigationPageUp;
    if (g.rightTrigger > t) u |= GameInputUiNavigationPageDown;
    if (g.rightThumbstickY > t) u |= GameInputUiNavigationScrollUp;
    if (g.rightThumbstickY < -t) u |= GameInputUiNavigationScrollDown;
    if (g.rightThumbstickX < -t) u |= GameInputUiNavigationScrollLeft;
    if (g.rightThumbstickX > t) u |= GameInputUiNavigationScrollRight;
    return u;
}

// An environment variable in lower case, of any length (a script is easily longer than a fixed buffer, and a value
// that did not fit used to read as unset). Empty when unset.
inline std::string env_lower(const char* name) {
    const DWORD need = GetEnvironmentVariableA(name, nullptr, 0);  // with the terminator
    if (need == 0) return {};
    std::string s(need, '\0');
    const DWORD n = GetEnvironmentVariableA(name, s.data(), need);
    if (n == 0 || n >= need) return {};
    s.resize(n);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace rt::gameinput::script
