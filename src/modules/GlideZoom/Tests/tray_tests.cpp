#include <stdexcept>
#include "../GlideZoom/tray_state.h"
#include "../GlideZoom/runtime_state.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
std::uint64_t assertions = 0, matrix = 0, transitions = 0;
void Check(bool condition, const char* message) {
    ++assertions;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
using glide::tray::Action;
using glide::tray::State;
using glide::tray::Visibility;

struct Shell {
    bool add = true, version = true, refresh = false;
    unsigned adds = 0, versions = 0, refreshes = 0;
    std::string calls;
    bool Add() noexcept { ++adds; calls += 'A'; return add; }
    bool Refresh() noexcept { ++refreshes; calls += 'M'; return refresh; }
    bool Version4() noexcept { ++versions; calls += 'V'; return version; }
};
std::uintptr_t Pack(std::uint16_t id, std::uint16_t message) {
    return (static_cast<std::uintptr_t>(id) << 16) | message;
}
Action ExpectedEvent(bool modern, unsigned message) {
    switch (message) {
    case 0x0202: case 0x0203: case 0x0400: case 0x0401: return Action::Restore;
    case 0x007b: return modern ? Action::Menu : Action::None;
    case 0x0205: return modern ? Action::None : Action::Menu;
    default: return Action::None;
    }
}
void Registrations() {
    for (bool add : {false, true}) for (bool version : {false, true}) for (bool refresh : {false, true}) {
        ++matrix;
        State state;
        Shell shell; shell.add = add; shell.version = version; shell.refresh = refresh;
        const bool available = add || refresh;
        Check(!state.Registered() && !state.Version4() && !state.Stopping(), "initial state owns no tray icon");
        Check(state.RequestVisibility(true) == Visibility::Show, "no tray never hides the window");
        Check(glide::tray::Register(state, shell) == available, "registration accepts only new or positively refreshed owned icon");
        Check(shell.calls == (add ? "AV" : refresh ? "AMV" : "AM"), "version negotiation follows successful Add or exact-icon refresh");
        Check(state.Registered() == available && state.Version4() == (available && version), "only successful version negotiation selects packed callbacks");
        Check(state.RequestVisibility(true) == (available ? Visibility::Hide : Visibility::Show), "hide requires a registered icon");
        Check(state.RequestVisibility(false) == Visibility::Show, "restore never toggles hidden");
        if (available) {
            const auto calls = shell.calls;
            Check(glide::tray::Register(state, shell), "repeat registration remains available");
            Check(shell.calls == calls, "repeat registration does not duplicate icon or renegotiate");
        }
        state.Lost();
        Check(!state.Registered() && !state.Version4(), "Explorer loss invalidates presence and callback format together");
        Check(state.RequestVisibility(true) == Visibility::Show, "Explorer loss exposes fallback visibility");
        shell.add = true; shell.version = !version; shell.calls.clear();
        Check(glide::tray::Register(state, shell) && shell.calls == "AV", "Explorer recreation always negotiates version again");
        Check(state.Version4() == !version, "old callback format does not leak into new icon");
        state.Stop();
        const auto calls = shell.calls;
        Check(!glide::tray::Register(state, shell) && shell.calls == calls, "stopping forbids native re-registration");
        Check(state.Registered(), "stopping retains known icon identity for native deletion");
        Check(state.RequestVisibility(true) == Visibility::None && state.RequestVisibility(false) == Visibility::None,
              "stopping cannot hide or reopen window");
        state.Lost(); state.Installed(true);
        Check(!state.Registered() && !state.Version4(), "late successful callback cannot resurrect terminal state");
        Check(state.Invariant(), "registration transitions preserve invariant");
    }
}
void Callbacks() {
    // Enumerate every 16-bit event, not only the recognized messages. Unknown
    // mouse/tooltip/balloon notifications must never activate the application.
    for (bool modern : {false, true}) {
        State state; state.Installed(modern);
        for (unsigned message = 0; message <= 0xffff; ++message) {
            ++matrix;
            const auto expected = ExpectedEvent(modern, message);
            const auto packed = modern ? Pack(1, static_cast<std::uint16_t>(message)) : message;
            const auto wParam = modern ? std::uintptr_t{0xfff0ff80} : 1u;
            Check(state.Callback(wParam, packed) == expected, "only defined messages dispatch for current callback format");
            const auto other = modern ? Pack(2, static_cast<std::uint16_t>(message)) : message;
            Check(state.Callback(modern ? wParam : 2u, other) == Action::None, "another icon ID cannot dispatch");
        }
        for (const auto id : {std::uintptr_t{0}, std::uintptr_t{2}, std::uintptr_t{0x10001},
                              std::numeric_limits<std::uintptr_t>::max()}) {
            if (!modern) Check(state.Callback(id, 0x0202) == Action::None, "legacy IDs are compared without truncation");
        }
        for (const auto coordinate : {std::uintptr_t{0}, std::uintptr_t{1}, std::uintptr_t{0x80007fff},
                                      std::uintptr_t{0xffffffff}, std::numeric_limits<std::uintptr_t>::max()}) {
            if (modern) Check(state.Callback(coordinate, Pack(1, 0x0401)) == Action::Restore,
                              "v4 coordinates are never mistaken for icon identity");
        }
        for (unsigned id = 0; id <= 0xffff; ++id) {
            ++matrix;
            const auto selection = modern ? Pack(static_cast<std::uint16_t>(id), 0x0202) : 0x0202;
            const auto menu = modern ? Pack(static_cast<std::uint16_t>(id), 0x007b) : 0x0205;
            Check(state.Callback(modern ? 0 : id, selection) == (id == 1 ? Action::Restore : Action::None),
                  "all encoded IDs discriminate restore target");
            Check(state.Callback(modern ? 0 : id, menu) == (id == 1 ? Action::Menu : Action::None),
                  "all encoded IDs discriminate context target");
        }
        Check(state.Callback(modern ? 0 : 1, modern ? Pack(1, 0x0202) : 0x0202) == Action::Restore,
              "single release restores without waiting for a double click");
        Check(state.Callback(modern ? 0 : 1, modern ? Pack(1, 0x0401) : 0x0401) == Action::Restore,
              "keyboard activation restores");
        Check(state.Callback(modern ? 0 : 1, modern ? Pack(1, 0x0205) : 0x007b) == Action::None,
              "modern and legacy menu messages cannot both open duplicate menus");
        state.Stop();
        Check(state.Callback(modern ? 0 : 1, modern ? Pack(1, 0x0202) : 0x0202) == Action::None,
              "queued left click cannot restore a closing app");
        Check(!state.BeginMenu(), "queued menu cannot reopen during shutdown");
        state.Lost();
        Check(state.Callback(1, 0x0202) == Action::None, "missing icon never accepts legacy stale activation");
    }
}
void MenusAndPresentation() {
    State state;
    Check(!state.BeginMenu(), "menu requires known tray presence");
    state.Installed(true);
    Check(state.BeginMenu() && state.MenuOpen(), "first menu opens");
    Check(!state.BeginMenu(), "reentrant right-click does not nest popup");
    state.Lost();
    Check(state.MenuOpen(), "Explorer loss preserves active native menu obligation");
    state.Installed(false);
    Check(!state.BeginMenu(), "recreated icon cannot nest old popup while it unwinds");
    state.EndMenu();
    Check(state.BeginMenu(), "menu can reopen after old popup has ended");
    state.Stop();
    Check(!state.BeginMenu(), "shutdown cannot nest popup");
    state.EndMenu(); state.EndMenu();
    Check(!state.MenuOpen() && state.Invariant(), "popup cleanup is idempotent");

    // Visibility is intentionally independent of zoom state. Compose the real
    // policies to guard against implementing restore as Resume/Reset/Activate.
    for (unsigned phase = 0; phase < 5; ++phase) for (bool registered : {false, true}) {
        ++matrix;
        glide::runtime::State runtime;
        runtime.PrepareEnable(); runtime.Activate();
        if (phase == 1) runtime.Pause();
        if (phase == 2) runtime.Suspend();
        if (phase == 3) runtime.RequestRestore();
        if (phase == 4) runtime.Fault();
        const auto oldPhase = runtime.Current();
        const auto epoch = runtime.Epoch();
        const auto pending = runtime.NeedsRestore();
        State presentation;
        if (registered) presentation.Installed(true);
        for (unsigned repeat = 0; repeat < 100; ++repeat) {
            Check(presentation.RequestVisibility(true) == (registered ? Visibility::Hide : Visibility::Show),
                  "minimize and close use same tray-presence gate");
            Check(presentation.RequestVisibility(false) == Visibility::Show, "repeated left clicks only restore");
            Check(runtime.Current() == oldPhase && runtime.Epoch() == epoch && runtime.NeedsRestore() == pending,
                  "window visibility does not resume, cancel, or restore zoom ownership");
        }
    }
}
void MixedSequences() {
    // Independent reference: presence is a capability obtained from a successful
    // shell add, lost by Explorer loss, and never regained after Stop. The menu
    // has a separate lifetime and can outlive that capability briefly.
    for (std::uint32_t seed = 1; seed <= 100; ++seed) {
        State state;
        Shell shell;
        bool present = false, modern = false, stopped = false, menu = false;
        auto random = seed * 2654435761u;
        for (unsigned index = 0; index < 1000; ++index) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            const unsigned action = random % 10;
            ++transitions;
            switch (action) {
            case 0: case 1: case 2: {
                shell.add = action != 2; shell.version = action == 0;
                const bool expected = !stopped && (present || shell.add);
                Check(glide::tray::Register(state, shell) == expected, "mixed registration outcome follows capability ownership");
                if (!stopped && !present && shell.add) { present = true; modern = shell.version; }
                break;
            }
            case 3: state.Lost(); present = modern = false; break;
            case 4:
                Check(state.RequestVisibility(true) == (stopped ? Visibility::None : present ? Visibility::Hide : Visibility::Show),
                      "mixed minimize cannot strand unavailable tray");
                break;
            case 5:
                Check(state.RequestVisibility(false) == (stopped ? Visibility::None : Visibility::Show),
                      "mixed restores are idempotent and cannot revive stopping");
                break;
            case 6: {
                const bool expected = present && !stopped && !menu;
                Check(state.BeginMenu() == expected, "mixed popup entry respects active menu lifetime");
                if (expected) menu = true;
                break;
            }
            case 7: state.EndMenu(); menu = false; break;
            case 8:
                // Keep most sequences alive long enough to exercise repeated
                // Explorer loss/recreation; stop dominates the final quarter.
                if (index >= 750) { state.Stop(); stopped = true; }
                break;
            case 9:
                Check(state.Callback(modern ? 0 : 1, modern ? Pack(1, 0x0202) : 0x0202) ==
                      (present && !stopped ? Action::Restore : Action::None), "mixed click uses committed callback format");
                break;
            }
            Check(state.Registered() == present && state.Version4() == modern && state.Stopping() == stopped,
                  "mixed state agrees with independent presence/format/stop oracle");
            Check(state.MenuOpen() == menu && state.Invariant(), "mixed popup lifetime and state invariant preserved");
        }
    }
}
}

int Run_tray() {
    Registrations(); Callbacks(); MenusAndPresentation(); MixedSequences();
    std::cout << "PASS: " << assertions << " tray assertions; " << matrix
              << " callback/registration/visibility matrix cases; " << transitions
              << " mixed transitions; version4 and legacy callbacks, registration faults, Explorer loss, menu reentrancy and shutdown\n";
    return 0;
}
