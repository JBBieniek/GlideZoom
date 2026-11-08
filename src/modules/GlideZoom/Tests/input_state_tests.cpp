#include <stdexcept>
#include "../GlideZoom/input_state.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace glide::input;
std::uint64_t assertions = 0, explored = 0;

void Check(bool result, const char* message) {
    ++assertions;
    if (!result) {
        std::cerr << "FAIL: " << message << '\n';
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}

State Ready(bool control = false, bool mouse = false) {
    State state;
    state.Configure({control, mouse, 400, 30000});
    state.SetEnabled(true);
    return state;
}

void Chord(State& state, std::uint64_t now = 1) {
    Check(!state.Key(vk::LeftControl, true, now).consume, "left Ctrl always forwarded");
    Check(!state.Key(vk::LeftAlt, true, now + 1).consume, "left Alt always forwarded");
}

void SpaceDrag(State& state, std::uint64_t now = 1) {
    Chord(state, now);
    const auto d = state.Key(vk::Space, true, now + 2);
    Check(d.begin && d.consume && state.Active() == Gesture::Space, "direct drag starts");
}

void ControlDrag(State& state) {
    Check(!state.Key(vk::LeftControl, true, 1).consume, "first Ctrl down forwarded");
    Check(!state.Key(vk::LeftControl, false, 30).consume, "first Ctrl up forwarded");
    const auto d = state.Key(vk::LeftControl, true, 100);
    Check(d.begin && !d.consume && state.Active() == Gesture::Control,
          "second Ctrl starts while preserving its modifier down");
}

void MouseDrag(State& state) {
    Chord(state);
    Check(state.ButtonEvent(Button::Left, true, {10, 20}, 10).consume,
          "opt-in first modified click reserved");
    Check(state.ButtonEvent(Button::Left, false, {10, 20}, 50).consume,
          "first modified click release paired");
    const auto d = state.ButtonEvent(Button::Left, true, {11, 20}, 100);
    Check(d.begin && d.consume && state.Active() == Gesture::Mouse, "second click starts drag");
}

void Scenarios() {
    {
        State state;
        for (std::uint32_t key = 0; key < 256; ++key) {
            Check(!state.Key(key, true, 0).consume, "disabled key down passes");
            Check(!state.Key(key, false, 1).consume, "disabled key up passes");
        }
        for (int button = 0; button < 5; ++button) {
            Check(!state.ButtonEvent(static_cast<Button>(button), true, {}, 2).consume,
                  "disabled button down passes");
            Check(!state.ButtonEvent(static_cast<Button>(button), false, {}, 3).consume,
                  "disabled button up passes");
        }
        Check(!state.Wheel(true, 4).consume && !state.Move({}, 4).consume,
              "disabled wheel and motion pass");
        Check(state.Invariant(), "disabled invariant");
    }
    {
        auto state = Ready();
        state.Key(vk::LeftControl, true, 1);
        state.Key(vk::LeftControl, false, 20);
        Check(!state.Key(vk::LeftControl, true, 100).begin, "double Ctrl off by default");
        state.Key(vk::LeftAlt, true, 101);
        Check(!state.ButtonEvent(Button::Left, true, {}, 102).consume,
              "modified click off by default");
        state.ButtonEvent(Button::Left, false, {}, 103);
        Check(!state.ButtonEvent(Button::Left, true, {}, 200).begin,
              "modified double click off by default");
    }
    {
        auto state = Ready(true);
        ControlDrag(state);
        const auto d = state.Key('C', true, 101);
        Check(d.end && d.cancel && !d.consume, "Ctrl+C cancels drag before forwarding C");
        Check(state.Down(vk::LeftControl), "Ctrl remains visible and tracked for copy");
        Check(!state.Key('C', false, 102).consume &&
              !state.Key(vk::LeftControl, false, 103).consume, "copy releases forwarded");
        Check(!state.Move({}, 104).consume, "unrelated input releases mouse motion");
    }
    {
        auto state = Ready(true);
        state.Key(vk::LeftControl, true, 1);
        for (int i = 0; i < 10; ++i) {
            Check(!state.Key(vk::LeftControl, true, 2 + i).begin, "Ctrl repeat cannot double tap");
        }
        state.Key(vk::LeftControl, false, 300);
        Check(!state.Key(vk::LeftControl, true, 350).begin, "long first hold is not a tap");
        state.Key(vk::LeftControl, false, 360);
        state.Key('C', true, 361);
        state.Key('C', false, 362);
        Check(!state.Key(vk::LeftControl, true, 370).begin, "ordinary key invalidates double tap");
        state.Key(vk::LeftControl, false, 371);
        state.Wheel(false, 372);
        Check(!state.Key(vk::LeftControl, true, 380).begin, "wheel invalidates double tap");
    }
    {
        auto state = Ready();
        Check(!state.Key(vk::Space, true, 1).consume, "ordinary Space down forwarded");
        Chord(state, 2);
        const auto repeat = state.Key(vk::Space, true, 4);
        Check(!repeat.consume && !repeat.begin, "held foreground Space cannot be converted");
        Check(!state.Key(vk::Space, false, 5).consume, "forwarded Space up stays forwarded");
        const auto fresh = state.Key(vk::Space, true, 6);
        Check(fresh.begin && fresh.consume, "fresh direct Space chord activates");
        Check(state.Move({4, 5}, 7).consume, "active gesture owns motion");
        const auto up = state.Key(vk::Space, false, 8);
        Check(up.consume && up.end && !up.cancel, "release commits direct gesture");
        Check(state.Invariant(), "direct gesture invariant");
    }
    {
        for (int button = 0; button < 5; ++button) {
            auto state = Ready(true, true);
            const auto b = static_cast<Button>(button);
            state.ButtonEvent(b, true, {}, 0);
            Chord(state);
            Check(!state.Key(vk::Space, true, 5).consume,
                  "foreground mouse drag prevents Space interception");
            Check(!state.Wheel(true, 6).consume, "foreground mouse drag prevents wheel interception");
            Check(!state.ButtonEvent(b, false, {}, 7).consume,
                  "forwarded button down never has a swallowed up");
        }
        auto state = Ready();
        Chord(state);
        state.Key(vk::LeftShift, true, 4);
        Check(!state.Key(vk::Space, true, 5).consume, "extra modifier prevents exact chord");
    }
    {
        for (int operation = 0; operation < 4; ++operation) {
            auto state = Ready();
            SpaceDrag(state);
            Decision d;
            if (operation == 0) d = state.Cancel();
            if (operation == 1) d = state.SetEnabled(false);
            if (operation == 2) d = state.Configure({true, true, 400, 30000});
            if (operation == 3) d = state.Expire(30003);
            Check(d.end && d.cancel, "boundary cancels active gesture");
            Check(!state.Move({}, 30004).consume, "boundary immediately releases motion");
            Check(state.Key(vk::Space, true, 30005).consume, "reserved repeat remains paired");
            Check(state.Key(vk::Space, false, 30006).consume, "reserved up survives boundary");
            Check(!state.Key(vk::Space, false, 30007).consume, "up consumption is exactly once");
            Check(state.Invariant(), "boundary leaves consistent state");
        }
    }
    {
        auto state = Ready();
        SpaceDrag(state);
        const auto d = state.Key(vk::LeftControl, false, 5);
        Check(d.end && d.cancel && !d.consume, "modifier release cancels and passes through");
        Check(state.Key(vk::Space, false, 6).consume, "Space up paired after modifier release");
        state.Key(vk::LeftAlt, false, 7);
        SpaceDrag(state, 10);
        const auto reset = state.Key(vk::Escape, true, 15);
        Check(reset.reset && reset.end && reset.cancel && reset.consume, "emergency reset cancels");
        const auto repeat = state.Key(vk::Escape, true, 16);
        Check(repeat.consume && !repeat.reset, "emergency reset ignores autorepeat");
        state.SetEnabled(false);
        Check(state.Key(vk::Escape, false, 17).consume, "emergency up paired while paused");
        Check(state.Key(vk::Space, false, 18).consume, "original Space up remains paired");
    }
    {
        auto state = Ready(true, true);
        MouseDrag(state);
        auto d = state.Key(vk::LeftAlt, false, 101);
        Check(d.end && d.cancel && !d.consume, "mouse gesture cancelled by modifier up");
        Check(state.ButtonEvent(Button::Left, false, {}, 102).consume,
              "reserved click up survives lost chord");
        state.SessionReset();
        MouseDrag(state);
        d = state.ButtonEvent(Button::Right, true, {}, 101);
        Check(d.end && d.cancel && !d.consume, "unrelated mouse button cancels and passes");
        state.SetEnabled(false);
        Check(state.ButtonEvent(Button::Left, false, {}, 102).consume,
              "mouse up retained when disabled");
        Check(!state.ButtonEvent(Button::Right, false, {}, 103).consume,
              "unrelated button up never swallowed");
    }
    {
        auto state = Ready(true, true);
        Chord(state);
        state.ButtonEvent(Button::Left, true, {}, 10);
        state.Move({100, 100}, 11);
        state.ButtonEvent(Button::Left, false, {100, 100}, 12);
        Check(!state.ButtonEvent(Button::Left, true, {100, 100}, 20).begin,
              "dragging first mouse tap invalidates recognition");
        state.Cancel();
        state.ButtonEvent(Button::Left, false, {}, 21);
        Check(!state.ButtonEvent(Button::Left, true, {}, 22).begin,
              "cancel clears pending recognizer");
    }
    {
        auto state = Ready(true, true);
        SpaceDrag(state, 100);
        Check(state.Expire(30101).end == false, "deadline does not fire early");
        Check(state.Expire(30102).end, "deadline fires exactly at limit");
        Check(!state.Key(vk::Space, true, 30103).begin, "timeout cannot restart held gesture");
        Check(!state.Move({}, 30104).consume, "timeout cannot trap cursor");
        state.Key(vk::Space, false, 30105);
        Check(state.Key(vk::Space, true, 30106).begin, "fresh release and press can rearm");
        Check(state.Expire(1).cancel, "backwards clock cancels safely");
    }
    {
        auto state = Ready(true, true);
        SpaceDrag(state);
        const auto d = state.ReconcileModifier(vk::LeftControl, false);
        Check(d.end && d.cancel, "missed modifier release detected by polling");
        state.ReconcileModifier(vk::Space, false);
        Check(state.Down(vk::Space) && state.SwallowedKey(vk::Space),
              "polling cannot erase reserved Space release");
        state.Key(vk::Space, false, 10);
        state.ReconcileModifier(vk::LeftControl, true);
        Check(state.Active() == Gesture::None, "modifier polling never activates gesture");
        state.Key(vk::Space, true, 11);
        Check(state.ReconcileModifier(vk::RightWin, true).cancel,
              "unexpected modifier polling cancels exact chord");
    }
    {
        auto state = Ready(true, true);
        MouseDrag(state);
        const auto d = state.SessionReset();
        Check(d.end && d.cancel && !state.SwallowedButton(Button::Left) &&
              !state.Down(vk::LeftControl) && !state.ButtonDown(Button::Left),
              "session boundary clears old-desktop ledger");
        Check(!state.ButtonEvent(Button::Left, false, {}, 101).consume,
              "new desktop release has no old owner");
        Check(!state.Key(1000, true, 102).consume, "invalid key is transparent");
        Check(!state.ButtonEvent(static_cast<Button>(99), true, {}, 103).consume,
              "invalid button is transparent");
        Check(!state.ReconcileModifier(1000, true).end, "invalid modifier is harmless");
        Check(state.Invariant(), "session reset invariant");
    }
    {
        auto state = Ready();
        SpaceDrag(state);
        state.SetEnabled(false);
        state.SetEnabled(true);
        Check(!state.Key(vk::Space, true, 10).begin, "resume does not activate held key");
        state.Key(vk::Space, false, 11);
        Check(state.Key(vk::Space, true, 12).begin, "fresh gesture works after resume");
        const auto d = state.Move({std::numeric_limits<double>::quiet_NaN(), 0}, 13);
        Check(d.end && d.cancel && !d.consume, "invalid motion fails open");
        Check(state.Invariant(), "invalid motion state valid");
    }
    {
        auto state = Ready();
        Chord(state);
        Check(state.Wheel(true, 3).consume, "configured chord wheel consumed");
        Check(!state.Wheel(false, 4).consume, "unconfigured chord wheel forwarded");
        state.Key('A', true, 5);
        Check(!state.Wheel(true, 6).consume, "ordinary held key prevents wheel interception");
    }
    {
        // A hook fault can occur after deciding to consume but before returning
        // to Windows. Roll back that event, disable, then process the physical
        // event again so the ledger describes what Windows actually received.
        auto state = Ready();
        Chord(state);
        auto before = state;
        Check(state.Key(vk::Space, true, 3).consume, "fault setup: reserved down decision");
        state = before;
        state.SetEnabled(false);
        Check(!state.Key(vk::Space, true, 3).consume &&
              !state.Key(vk::Space, false, 4).consume, "fault on fresh down forwards complete pair");
        state.SetEnabled(true);
        state.Key(vk::Space, true, 5);
        before = state;
        Check(state.Key(vk::Space, false, 6).consume, "fault setup: reserved release decision");
        state = before;
        state.SetEnabled(false);
        Check(state.Key(vk::Space, false, 6).consume && !state.SwallowedKey(vk::Space),
              "fault on owed up consumes once and clears ledger");
        Check(!state.Key(vk::Space, true, 7).consume, "fresh down after recovered up passes");

        auto mouse = Ready(false, true);
        Chord(mouse);
        mouse.ButtonEvent(Button::Left, true, {}, 3);
        before = mouse;
        mouse.ButtonEvent(Button::Left, false, {}, 4);
        mouse = before;
        mouse.SetEnabled(false);
        Check(mouse.ButtonEvent(Button::Left, false, {}, 4).consume &&
              !mouse.SwallowedButton(Button::Left), "fault on mouse up clears reserved tail");
        Check(!mouse.ButtonEvent(Button::Left, true, {}, 5).consume,
              "fresh click after fault passes");
    }
}

// An independent observer records what the foreground application saw. Its
// ownership rule does not inspect State internals: a physical DOWN determines
// every repeat and UP until that pair ends. SessionReset starts a fresh stream.
struct Model {
    State state = Ready(true, true);
    std::array<bool, 6> held{};
    std::array<bool, 6> consumed{};
    std::array<bool, 2> buttonHeld{};
    std::array<bool, 2> buttonConsumed{};
    bool enabled = true;
    std::uint64_t now = 0;
};

constexpr std::array<std::uint32_t, 6> keys{
    vk::LeftControl, vk::LeftAlt, vk::Space, vk::Escape, 'C', vk::LeftShift};

void Apply(Model& model, unsigned action) {
    model.now += 10;
    Decision d;
    if (action < keys.size()) {
        const bool down = !model.held[action];
        d = model.state.Key(keys[action], down, model.now);
        if (down) model.consumed[action] = d.consume;
        else Check(d.consume == model.consumed[action], "model: key release matches its down");
        model.held[action] = down;
        if (action == 0 || action == 1 || action == 5) {
            Check(!d.consume, "model: modifier never intercepted");
        }
    } else if (action < 8) {
        const unsigned index = action - 6;
        const bool down = !model.buttonHeld[index];
        d = model.state.ButtonEvent(static_cast<Button>(index), down, {}, model.now);
        if (down) model.buttonConsumed[index] = d.consume;
        else Check(d.consume == model.buttonConsumed[index], "model: button release matches down");
        model.buttonHeld[index] = down;
    } else if (action == 8) {
        for (unsigned i = 0; i < keys.size(); ++i) {
            if (model.held[i]) {
                const auto repeat = model.state.Key(keys[i], true, model.now);
                Check(repeat.consume == model.consumed[i] && !repeat.begin && !repeat.reset,
                      "model: repeat preserves ownership without activation");
            }
        }
    } else if (action == 9) {
        d = model.state.Cancel();
    } else if (action == 10) {
        model.enabled = !model.enabled;
        d = model.state.SetEnabled(model.enabled);
    } else if (action == 11) {
        d = model.state.Wheel(true, model.now);
    } else if (action == 12) {
        model.now += 30000;
        d = model.state.Expire(model.now);
        Check(model.state.Active() == Gesture::None, "model: deadline always releases gesture");
    } else if (action == 13) {
        d = model.state.Configure({true, true, 400, 30000});
    } else {
        d = model.state.SessionReset();
        model.held.fill(false);
        model.consumed.fill(false);
        model.buttonHeld.fill(false);
        model.buttonConsumed.fill(false);
    }
    Check(!d.cancel || d.end, "model: cancellation always has an end effect");
    Check(!d.begin || model.enabled, "model: disabled state cannot start gestures");
    Check(model.state.Invariant(), "model: state invariant after every transition");
    const auto motion = model.state.Move({}, model.now);
    Check(motion.consume == (model.state.Active() != Gesture::None),
          "model: only live gestures intercept cursor motion");
    ++explored;
}

void Enumerate(const Model& model, unsigned depth) {
    if (!depth) return;
    for (unsigned action = 0; action < 15; ++action) {
        Model next = model;
        Apply(next, action);
        Enumerate(next, depth - 1);
    }
}

void ModelTests() {
    Enumerate(Model{}, 5); // all 15-action words through length five
    // Reach each gesture explicitly, then exhaustively vary its outgoing edges.
    for (const auto& prefix : {std::array<unsigned, 6>{0, 0, 0, 8, 8, 8},
                               std::array<unsigned, 6>{0, 1, 2, 8, 8, 8},
                               std::array<unsigned, 6>{0, 1, 6, 6, 6, 8}}) {
        Model model;
        for (unsigned action : prefix) Apply(model, action);
        Check(model.state.Active() != Gesture::None, "model prefix reaches live gesture");
        Enumerate(model, 4);
    }
    // Longer deterministic adversarial streams supplement bounded enumeration.
    std::uint32_t random = 0x97D43A11;
    Model model;
    for (unsigned i = 0; i < 100000; ++i) {
        random = random * 1664525u + 1013904223u;
        Apply(model, random % 15);
    }
}
} // namespace

int Run_input_state() {
    Scenarios();
    ModelTests();
    std::cout << "Input state: " << assertions << " assertions; " << explored
              << " event transitions checked.\n";
    return 0;
}
