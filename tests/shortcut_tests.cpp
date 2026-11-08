#include <stdexcept>
#include "../src/input_state.h"
#include "../src/control_channel.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
using namespace glide::input;
std::uint64_t assertions = 0, transitions = 0, steps = 0, timerPolls = 0, timerSteps = 0;
std::string context;

void Check(bool condition, const char* message) {
    ++assertions;
    if (!condition) {
        std::cerr << "FAIL: " << message << " [" << context << "]\n";
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}

bool IsStep(std::uint32_t key) { return key == vk::Equal || key == vk::Minus || key == vk::Add || key == vk::Subtract; }
int Direction(std::uint32_t key) { return key == vk::Equal || key == vk::Add ? 1 : -1; }
bool Modifier(std::uint32_t key) {
    return key == vk::Shift || key == vk::Control || key == vk::Alt ||
           key == vk::LeftWin || key == vk::RightWin ||
           (key >= vk::LeftShift && key <= vk::RightAlt);
}

// Independent physical DOWN/UP ledger. Capturing the initial press creates a
// release obligation; all later repeats and the one UP must have the same fate.
// Step eligibility is modeled separately from that release obligation.
struct Harness {
    State state;
    std::array<bool, 256> down{}, owned{};
    std::array<bool, 5> buttons{}, ownedButtons{};
    bool enabled = true, starts = true;
    std::uint32_t repeatKey = 0;
    std::uint64_t repeatStarted = 0, repeatObserved = 0, repeatDeadline = 0;
    std::uint64_t now = 1;

    Harness(bool control = false, bool mouse = false) {
        state.Configure({control, mouse, 400, 30000});
        state.SetEnabled(true);
    }
    bool NoButtons() const {
        for (bool held : buttons) if (held) return false;
        return true;
    }
    bool Exact(std::uint32_t key) const {
        unsigned count = 0;
        for (std::size_t index = 0; index < down.size(); ++index) {
            if (key == vk::Equal && (index == vk::LeftShift || index == vk::RightShift)) continue;
            if (down[index]) ++count;
        }
        return count == 3 && down[vk::LeftWin] && down[vk::LeftAlt] && down[key];
    }
    void Observe(std::uint64_t time) {
        if (repeatKey && (time < repeatObserved || time - repeatStarted >= 30000)) repeatKey = 0;
        if (repeatKey) repeatObserved = time;
    }
    Decision Key(std::uint32_t key, bool pressed) {
        if (key >= down.size()) std::abort();
        ++transitions;
        const bool previous = down[key], obligation = owned[key];
        const bool fresh = pressed && !previous;
        Observe(now);
        down[key] = pressed;
        if (repeatKey && !Exact(repeatKey)) repeatKey = 0;
        const bool capture = fresh && IsStep(key) && enabled && starts && Exact(key) && NoButtons();
        int expectedStep = 0;
        if (capture) expectedStep = Direction(key);
        const auto pressedAt = now;
        const auto decision = state.Key(key, pressed, now++);
        Check(decision.step == expectedStep, "step action matches independent chord/hold authority");
        if (IsStep(key)) Check(decision.consume == (obligation || capture), "step capture and repeat ownership");
        if (!fresh) Check(decision.consume == obligation, "repeat/UP matches its initial DOWN");
        if (Modifier(key)) Check(!decision.consume && !decision.step, "all modifier events are forwarded");
        if (fresh) owned[key] = decision.consume;
        if (!pressed) owned[key] = false;
        if (capture) {
            repeatKey = key;
            repeatStarted = repeatObserved = pressedAt;
            repeatDeadline = pressedAt + 250;
        }
        if (decision.step) ++steps;
        Check(state.Down(key) == down[key] && state.SwallowedKey(key) == owned[key], "physical and release ledgers agree");
        Check(state.Invariant(), "production input invariant after keyboard event");
        return decision;
    }
    void ButtonInput(Button button, bool pressed) {
        ++transitions;
        const auto index = static_cast<std::size_t>(button);
        if (index >= buttons.size()) std::abort();
        const bool fresh = pressed && !buttons[index], obligation = ownedButtons[index];
        Observe(now);
        buttons[index] = pressed;
        if (!NoButtons()) repeatKey = 0;
        const auto decision = state.ButtonEvent(button, pressed, {10, 10}, now++);
        Check(!decision.step, "buttons never generate keyboard zoom steps");
        if (!fresh) Check(decision.consume == obligation, "mouse repeat/UP preserves initial ownership");
        if (fresh) ownedButtons[index] = decision.consume;
        if (!pressed) ownedButtons[index] = false;
        Check(state.ButtonDown(button) == buttons[index] && state.SwallowedButton(button) == ownedButtons[index],
            "button ledger agrees");
        Check(state.Invariant(), "production input invariant after button event");
    }
    Decision PollAt(std::uint64_t time) {
        ++transitions;
        ++timerPolls;
        Observe(time);
        int expected = 0;
        if (repeatKey && time >= repeatDeadline) {
            if (starts) expected = Direction(repeatKey);
            do { repeatDeadline += 33; } while (repeatDeadline <= time);
        }
        const auto decision = state.Expire(time);
        Check(decision.step == expected, "timer repeats match independent deadline schedule");
        Check(!decision.consume && !decision.begin && !decision.reset, "timer does not consume physical input or start a gesture");
        if (decision.step) { ++steps; ++timerSteps; }
        Check(state.Invariant(), "timer preserves input invariant");
        now = time + 1;
        return decision;
    }
    void Ledger() {
        for (std::size_t key = 0; key < down.size(); ++key) {
            Check(state.Down(static_cast<std::uint32_t>(key)) == down[key], "boundary preserves tracked physical keys");
            Check(state.SwallowedKey(static_cast<std::uint32_t>(key)) == owned[key], "boundary preserves reserved key releases");
        }
        for (std::size_t button = 0; button < buttons.size(); ++button) {
            Check(state.ButtonDown(static_cast<Button>(button)) == buttons[button], "boundary preserves physical buttons");
            Check(state.SwallowedButton(static_cast<Button>(button)) == ownedButtons[button], "boundary preserves button releases");
        }
        Check(state.Invariant(), "production invariant at boundary");
    }
    void Cancel() { state.Cancel(); repeatKey = 0; Ledger(); }
    void Enable(bool value) {
        if (value != enabled) repeatKey = 0;
        enabled = value;
        state.SetEnabled(value);
        Ledger();
    }
    void Starts(bool value) { starts = value; state.SetStartsAllowed(value); Ledger(); }
    void Configure(bool control, bool mouse) {
        state.Configure({control, mouse, 400, 30000}); repeatKey = 0; Ledger();
    }
    void ResetSession() {
        state.SessionReset(); repeatKey = 0;
        down.fill(false); owned.fill(false); buttons.fill(false); ownedButtons.fill(false);
        Ledger();
    }
    void Reconcile(std::uint32_t key, bool value) {
        if (Modifier(key)) {
            down[key] = value;
            if (repeatKey && !Exact(repeatKey)) repeatKey = 0;
        }
        const auto decision = state.ReconcileModifier(key, value);
        Check(!decision.step && !decision.consume, "modifier polling never synthesizes a step or consumes input");
        Ledger();
    }
    void Wheel() { state.Wheel(false, now++); repeatKey = 0; Ledger(); }
    void Chord(bool reverseOrder = false) {
        Key(reverseOrder ? vk::LeftAlt : vk::LeftWin, true);
        Key(reverseOrder ? vk::LeftWin : vk::LeftAlt, true);
    }
};

void DeterministicCases() {
    context = "exact chords, typematic suppression and timer repeats";
    for (bool control : {false, true}) for (bool mouse : {false, true})
        for (bool reverseOrder : {false, true}) for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
            Harness h(control, mouse);
            h.Chord(reverseOrder);
            Check(h.Key(key, true).step == (Direction(key)), "fresh physical key steps without Shift");
            const auto started = h.repeatStarted;
            for (unsigned repeat = 0; repeat < 20; ++repeat)
                Check(h.Key(key, true).step == 0, "OS typematic does not drive zoom cadence");
            Check(!h.PollAt(started + 249).step, "hold delay does not expire early");
            Check(h.PollAt(started + 250).step != 0, "hold timer steps at initial deadline");
            Check(!h.PollAt(started + 250).step, "duplicate timer service does not step twice");
            Check(!h.PollAt(started + 282).step, "repeat interval does not expire early");
            Check(h.PollAt(started + 283).step != 0, "hold repeats at next interval");
            Check(h.Key(key, false).consume, "reserved release consumed");
            Check(!h.Key(key, false).consume, "duplicate UP not consumed");
            h.Key(vk::LeftAlt, false); h.Key(vk::LeftWin, false);
        }

    context = "all extra keys reject fresh step";
    for (std::uint32_t blocker = 0; blocker < 256; ++blocker) {
        if (blocker == vk::LeftWin || blocker == vk::LeftAlt) continue;
        for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
            if (blocker == key || (key == vk::Equal && (blocker == vk::LeftShift || blocker == vk::RightShift))) continue;
            Harness h;
            h.Key(blocker, true);
            h.Chord();
            Check(!h.Key(key, true).consume, "unrelated held key blocks new shortcut");
            h.Key(blocker, false);
            Check(!h.Key(key, true).consume, "forwarded trigger cannot be captured after blocker disappears");
            h.Key(key, false);
            Check(h.Key(key, true).step != 0, "new trigger press after blocker release can step");
            h.Key(key, false);
        }
    }

    context = "right-side and incomplete modifiers";
    for (auto win : {vk::LeftWin, vk::RightWin}) for (auto alt : {vk::LeftAlt, vk::RightAlt, vk::Alt})
        for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
            Harness h;
            h.Key(win, true); h.Key(alt, true);
            const auto result = h.Key(key, true);
            Check((result.step != 0) == (win == vk::LeftWin && alt == vk::LeftAlt), "exact left-side chord only");
            h.Key(key, false);
        }

    context = "mouse buttons and opposing trigger";
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) for (int button = 0; button < 5; ++button) {
        Harness h;
        h.ButtonInput(static_cast<Button>(button), true);
        h.Chord();
        Check(!h.Key(key, true).consume, "foreground mouse drag blocks fresh keyboard shortcut");
        h.ButtonInput(static_cast<Button>(button), false);
        Check(!h.Key(key, true).consume, "released button cannot convert forwarded trigger repeat");
        h.Key(key, false);
        Check(h.Key(key, true).step != 0, "fresh shortcut after button is released");
        h.ButtonInput(static_cast<Button>(button), true);
        Check(h.Key(key, true).consume && h.Key(key, true).step == 0, "owned repeat remains consumed but does not act during mouse drag");
        h.ButtonInput(static_cast<Button>(button), false);
        h.Key(key, false);
    }

    context = "cancellation does not erase release ownership or rearm on repeats";
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) for (int interruption = 0; interruption < 8; ++interruption) {
        Harness h;
        h.Chord(); h.Key(key, true);
        switch (interruption) {
        case 0: h.Cancel(); break;
        case 1: h.Enable(false); h.Enable(true); break;
        case 2: h.Configure(true, true); break;
        case 3: h.Key(vk::LeftAlt, false); h.Key(vk::LeftAlt, true); break;
        case 4: h.Key(vk::LeftWin, false); h.Key(vk::LeftWin, true); break;
        case 5: h.Reconcile(vk::LeftAlt, false); h.Reconcile(vk::LeftAlt, true); break;
        case 6: h.Key('A', true); h.Key('A', false); break;
        case 7: h.Wheel(); break;
        }
        const auto repeat = h.Key(key, true);
        Check(repeat.consume && repeat.step == 0, "cancelled shortcut cannot resume until fresh press");
        Check(h.Key(key, false).consume, "cancelled shortcut still owns its UP");
        Check(h.Key(key, true).step != 0, "fresh press rearms after cancellation");
        h.Key(key, false);
    }

    context = "disabled/unreadable start and initially forwarded keys";
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) for (int gate = 0; gate < 3; ++gate) {
        Harness h;
        if (gate == 0) h.Enable(false);
        if (gate == 1) h.Starts(false);
        if (gate != 2) h.Chord();
        Check(!h.Key(key, true).consume, "unavailable authority forwards initial trigger");
        if (gate == 0) h.Enable(true);
        if (gate == 1) h.Starts(true);
        if (gate == 2) h.Chord();
        Check(!h.Key(key, true).consume && !h.Key(key, true).step, "enabling or adding chord cannot seize a held key");
        Check(!h.Key(key, false).consume, "forwarded initial DOWN keeps forwarded UP");
        Check(h.Key(key, true).step != 0, "fresh press after authority becomes valid");
        h.Key(key, false);
    }

    context = "overlap blocks steps temporarily but retains a committed hold";
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
        Harness h;
        h.Chord(); h.Key(key, true);
        h.Starts(false);
        Check(h.Key(key, true).consume && h.Key(key, true).step == 0, "uncertain publication cannot issue a new step");
        h.Starts(true);
        Check(h.Key(key, true).step == 0, "coherent publication cannot turn typematic into a step");
        Check(h.PollAt(h.repeatStarted + 250).step != 0, "coherent publication resumes committed timer hold");
        h.Key(key, false);
        h.Chord(); h.Key(key, true);
        h.ResetSession();
        Check(!h.Key(key, false).consume, "real desktop boundary clears old release obligations");
        h.Key(vk::LeftAlt, false); h.Key(vk::LeftWin, false);
    }
}

void PrintedPlusAliases() {
    context = "printed plus accepts either Shift without capturing modifier releases";
    for (unsigned shiftMask = 1; shiftMask <= 3; ++shiftMask) {
        for (bool shiftFirst : {false, true}) {
            for (unsigned interruption = 0; interruption < 10; ++interruption) {
                Harness h;
                const auto shift = [&](bool pressed) {
                    if (shiftMask & 1u) Check(!h.Key(vk::LeftShift, pressed).consume, "left Shift always forwarded");
                    if (shiftMask & 2u) Check(!h.Key(vk::RightShift, pressed).consume, "right Shift always forwarded");
                };
                if (shiftFirst) shift(true);
                h.Chord();
                if (!shiftFirst) shift(true);
                Check(h.Key(vk::Equal, true).step == 1, "printed plus issues a positive step");
                const auto started = h.repeatStarted;
                Check(h.PollAt(started + 250).step == 1, "printed plus repeats on the same hold cadence");
                Check(h.Key(vk::Equal, true).consume, "printed-plus typematic stays reserved");
                switch (interruption) {
                case 0: h.Cancel(); break;
                case 1: h.Enable(false); h.Enable(true); break;
                case 2: h.Configure(true, true); break;
                case 3: h.Key(vk::LeftControl, true); h.Key(vk::LeftControl, false); break;
                case 4: h.Key(vk::RightAlt, true); h.Key(vk::RightAlt, false); break;
                case 5: h.Key(vk::RightWin, true); h.Key(vk::RightWin, false); break;
                case 6: h.Key('A', true); h.Key('A', false); break;
                case 7: h.Reconcile(vk::LeftAlt, false); h.Reconcile(vk::LeftAlt, true); break;
                case 8: h.ButtonInput(Button::Left, true); h.ButtonInput(Button::Left, false); break;
                case 9: h.Wheel(); break;
                }
                Check(!h.PollAt(started + 283).step, "cancelled printed plus cannot replay a held step");
                Check(h.Key(vk::Equal, true).consume, "cancelled printed-plus repeats retain their release obligation");
                Check(h.Key(vk::Equal, false).consume, "printed-plus release consumed exactly once");
                Check(!h.Key(vk::Equal, false).consume, "duplicate printed-plus release is forwarded");
                Check(h.Key(vk::Equal, true).step == 1, "a fresh printed-plus press rearms");
                h.Key(vk::Equal, false);
                shift(false);
                h.Key(vk::LeftAlt, false);
                h.Key(vk::LeftWin, false);
                h.Ledger();
            }
        }
    }

    context = "Shift only modifies OEM plus, never minus or numpad aliases";
    for (auto key : {vk::Minus, vk::Add, vk::Subtract}) {
        for (auto shift : {vk::LeftShift, vk::RightShift}) {
            Harness h;
            h.Chord();
            h.Key(shift, true);
            Check(!h.Key(key, true).consume, "Shift with another trigger passes its initial DOWN");
            h.Key(shift, false);
            Check(!h.Key(key, true).consume, "removing Shift cannot seize a forwarded numpad/minus hold");
            Check(!h.Key(key, false).consume, "forwarded numpad/minus DOWN keeps its UP");
            Check(h.Key(key, true).step == Direction(key), "fresh exact numpad/minus chord works after Shift release");
            h.Key(key, false);
        }
    }

    context = "switching between equals and printed plus preserves one physical hold";
    Harness h;
    h.Chord();
    h.Key(vk::Equal, true);
    const auto start = h.repeatStarted;
    h.Key(vk::LeftShift, true);
    Check(h.PollAt(start + 250).step == 1, "adding an allowed alias modifier does not lose the held key");
    h.Key(vk::LeftShift, false);
    Check(h.PollAt(start + 283).step == 1, "releasing Shift returns to the exact equals alias");
    Check(h.Key(vk::Equal, false).consume, "alias change retains exactly one owed trigger release");
}

void ControlComposition() {
    context = "real ControlReader epoch and independent safety gates";
    struct ReadAttempt {
        const ControlChannel& channel;
        bool overlap;
        bool Read(Snapshot& value) const noexcept { return !overlap && channel.Read(value); }
    };
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) for (int interruption = 0; interruption < 3; ++interruption) {
        ControlChannel channel;
        ControlReader reader;
        State state;
        bool safe = true;
        auto sync = [&](bool overlap) {
            return reader.Synchronize(ReadAttempt{channel, overlap}, state,
                [&] { return safe; }, [](Decision) {});
        };
        channel.Publish({DefaultControlOptions | 1, 10, 1});
        Check(sync(false), "initial coherent controls");
        Check(!state.Key(vk::LeftWin, true, 1).consume, "Win forwarded through real control policy");
        Check(!state.Key(vk::LeftAlt, true, 2).consume, "Alt forwarded through real control policy");
        Check(state.Key(key, true, 3).step != 0, "shortcut starts under real control policy");
        if (interruption == 0) {
            channel.Publish({DefaultControlOptions | 1, 11, 2});
            Check(sync(false), "changed epoch committed");
        } else if (interruption == 1) {
            safe = false; sync(true); safe = true; sync(false);
        } else {
            channel.Publish({DefaultControlOptions, 11, 1}); sync(false);
            channel.Publish({DefaultControlOptions | 1, 12, 1}); sync(false);
        }
        const auto repeat = state.Key(key, true, 4);
        Check(repeat.consume && !repeat.step, "epoch/safety/pause cancellation cannot restart held shortcut");
        Check(state.Key(key, false, 5).consume, "release still paired after control cancellation");
        Check(state.Key(key, true, 6).step != 0, "fresh press after coherent safe controls works");
        Check(state.Key(key, false, 7).consume && state.Invariant(), "composition finishes consistently");
    }
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
        ControlChannel channel;
        ControlReader reader;
        State state;
        bool safe = true;
        auto sync = [&](bool overlap) {
            return reader.Synchronize(ReadAttempt{channel, overlap}, state,
                [&] { return safe; }, [](Decision) {});
        };
        channel.Publish({DefaultControlOptions | 1, 10, 1});
        Check(sync(false), "timer composition controls start coherent");
        state.Key(vk::LeftWin, true, 1); state.Key(vk::LeftAlt, true, 2);
        Check(state.Key(key, true, 3).step != 0, "timer composition initial physical step");
        for (unsigned slot = 0; slot < 20; ++slot) {
            Check(!channel.Publish({DefaultControlOptions | 1, 10, 1.01 + slot * 0.01}),
                  "changing zoom telemetry does not publish new controls or cancel hold");
            Check(sync(false), "zoom telemetry does not make control read unavailable");
            Check(state.Expire(253 + 33 * slot).step == (Direction(key)),
                  "real control channel preserves timer hold across applied zoom updates");
        }
        Check(!sync(true), "overlapping control publication is observable");
        Check(!state.Expire(913).step, "unreadable controls suppress a due held step");
        Check(sync(false), "coherent publication returns");
        Check(!state.Expire(914).step, "returning controls cannot replay suppressed step");
        Check(state.Expire(946).step != 0, "next live slot resumes under committed controls");
        safe = false;
        sync(true);
        Check(!state.Expire(979).step, "stale heartbeat gate cancels even during control overlap");
        safe = true;
        sync(false);
        Check(!state.Expire(1012).step, "heartbeat recovery requires a fresh physical key");
        Check(state.Key(key, false, 1013).consume && state.Invariant(),
              "heartbeat gate retained the matching release obligation");
    }
}

void HeldRepeats() {
    context = "repeat cadence is independent of physical event frequency";
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
        for (const std::uint64_t interval : {1, 8, 16, 33, 50, 100, 250}) {
            Harness h;
            h.Chord(); h.Key(key, true);
            const auto start = h.repeatStarted;
            for (std::uint64_t elapsed = 0; elapsed < 30000; elapsed += interval) {
                h.PollAt(start + elapsed);
                // A typematic storm at the same wall time neither adds a
                // second step nor postpones the next timer deadline.
                h.now = start + elapsed;
                const auto repeated = h.Key(key, true);
                Check(repeated.consume && !repeated.step, "native typematic remains paired without duplicating timer steps");
            }
            Check(!h.PollAt(start + 30000).step, "maximum hold time expires before repeating");
            Check(!h.PollAt(start + 30033).step, "expired hold cannot restart on timer");
            Check(h.Key(key, false).consume, "deadline retains paired key release");
            Check(h.Key(key, true).step != 0, "new press rearms after hold timeout");
            h.Key(key, false);
        }
    }

    context = "stall skips repeat backlog";
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
        Harness h;
        h.Chord(); h.Key(key, true);
        const auto start = h.repeatStarted;
        Check(h.PollAt(start + 5000).step != 0, "stalled service emits at most the current repeat");
        for (unsigned count = 0; count < 100; ++count)
            Check(!h.PollAt(start + 5000).step, "same stalled timestamp cannot drain obsolete repeats");
        Check(!h.PollAt(start + 5001).step, "skipped intervals do not produce immediate catch-up");
        Check(h.PollAt(start + 5002).step != 0, "next live deadline remains on cadence");
        h.Key(key, false);
    }

    context = "release at repeat deadline has cancellation priority";
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
        for (unsigned boundary = 0; boundary < 12; ++boundary) {
            Harness h;
            h.Chord(); h.Key(key, true);
            const auto deadline = h.repeatStarted + 250;
            h.now = deadline;
            switch (boundary) {
            case 0: Check(!h.Key(key, false).step, "trigger UP cannot emit due repeat"); break;
            case 1: Check(!h.Key(vk::LeftWin, false).step, "Win UP cannot emit due repeat"); break;
            case 2: Check(!h.Key(vk::LeftAlt, false).step, "Alt UP cannot emit due repeat"); break;
            case 3: h.Key('A', true); h.Key('A', false); break;
            case 4: h.ButtonInput(Button::Left, true); h.ButtonInput(Button::Left, false); break;
            case 5: h.Cancel(); break;
            case 6: h.Enable(false); h.Enable(true); break;
            case 7: h.Configure(true, true); break;
            case 8: h.Reconcile(vk::LeftAlt, false); h.Reconcile(vk::LeftAlt, true); break;
            case 9: h.Wheel(); break;
            case 10: h.ResetSession(); break;
            case 11: h.Key(key == vk::Equal ? vk::Minus : vk::Equal, true); break;
            }
            Check(!h.PollAt(deadline + 33).step, "cancelled or changed chord emits no delayed step");
            h.Key(key, false);
            h.ResetSession();
        }
    }

    context = "every extra key cancels an existing held shortcut";
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
        for (std::uint32_t extra = 0; extra < 256; ++extra) {
            if (extra == key || extra == vk::LeftWin || extra == vk::LeftAlt ||
                (key == vk::Equal && (extra == vk::LeftShift || extra == vk::RightShift))) continue;
            Harness h;
            h.Chord(); h.Key(key, true);
            const auto start = h.repeatStarted;
            Check(!h.Key(extra, true).step, "unrelated key does not emit an expiring repeat");
            h.Key(extra, false);
            Check(!h.PollAt(start + 250).step, "removing extra key cannot rearm old hold");
            Check(h.Key(key, false).consume, "cancelled hold retains only its owned UP");
        }
        for (unsigned button = 0; button < 5; ++button) {
            Harness h;
            h.Chord(); h.Key(key, true);
            const auto start = h.repeatStarted;
            h.ButtonInput(static_cast<Button>(button), true);
            h.ButtonInput(static_cast<Button>(button), false);
            Check(!h.PollAt(start + 250).step, "mouse-button cancellation requires a fresh key press");
            h.Key(key, false);
        }
    }

    context = "overlapping publication skips elapsed slots without catch-up";
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
        Harness h;
        h.Chord(); h.Key(key, true);
        const auto start = h.repeatStarted;
        h.Starts(false);
        Check(!h.PollAt(start + 250).step, "unreadable controls suppress due repeat");
        h.Starts(true);
        Check(!h.PollAt(start + 251).step, "coherent controls do not replay suppressed repeat");
        Check(h.PollAt(start + 283).step != 0, "live next slot resumes a still-authorized hold");
        h.PollAt(start + 300);
        Check(!h.PollAt(start + 299).step, "backwards observed time cancels the hold");
        Check(!h.PollAt(start + 500).step, "clock recovery does not rearm cancelled hold");
        h.Key(key, false);
    }

    context = "uint64 boundary arithmetic and short configured deadlines";
    for (auto key : {vk::Equal, vk::Minus, vk::Add, vk::Subtract}) {
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        State state;
        state.SetEnabled(true);
        state.Key(vk::LeftWin, true, maximum - 1000);
        state.Key(vk::LeftAlt, true, maximum - 1000);
        Check(state.Key(key, true, maximum - 1000).step != 0, "near-limit fresh hold starts");
        Check(!state.Expire(maximum - 751).step, "near-limit initial delay holds");
        Check(state.Expire(maximum - 750).step != 0, "near-limit timer can repeat without absolute deadline overflow");
        Check(state.Expire(maximum).step != 0, "last representable timestamp emits at most current slot");
        Check(!state.Expire(maximum).step && !state.Expire(0).step, "duplicate or wrapped timestamp does not repeat");
        Check(!state.Expire(1000).step, "clock restart requires a fresh physical press");
        Check(state.Key(key, false, 1001).consume && state.Invariant(), "overflow boundary preserves release pairing");
        state.SessionReset();
        state.Configure({false, false, 400, 1});
        state.Key(vk::LeftWin, true, 0); state.Key(vk::LeftAlt, true, 0);
        Check(state.Key(key, true, 0).step != 0, "one-millisecond limit permits the initial press");
        Check(!state.Expire(1).step && !state.Expire(250).step, "configured hold deadline beats first repeat");
        Check(state.Key(key, false, 251).consume && state.Invariant(), "short deadline retains exactly paired release");
    }
}

std::uint32_t Random(std::uint32_t& value) {
    value ^= value << 13; value ^= value >> 17; value ^= value << 5;
    return value;
}
void MixedSequences() {
    constexpr std::array<std::uint32_t, 18> keys{
        vk::LeftWin, vk::LeftAlt, vk::RightWin, vk::RightAlt, vk::LeftShift,
        vk::LeftControl, vk::RightControl, vk::RightShift, vk::Equal, vk::Minus,
        vk::Space, vk::Escape, 'A', 'Z', vk::Alt, vk::Control, vk::Add, vk::Subtract};
    for (std::uint32_t seed = 1; seed <= 100; ++seed) {
        Harness h(true, true);
        std::uint32_t random = seed;
        for (unsigned i = 0; i < 1000; ++i) {
            context = "mixed seed=" + std::to_string(seed) + " event=" + std::to_string(i);
            if (i % 100 == 0) {
                h.ResetSession(); h.Enable(true); h.Starts(true);
                h.Chord((i / 100) % 2 != 0);
                h.Key((i / 100) % 2 ? vk::Minus : vk::Equal, true);
                h.PollAt(h.repeatStarted + 249);
                Check(h.PollAt(h.repeatStarted + 250).step != 0, "mixed run reaches a held timer step before interruptions");
            }
            const auto action = Random(random) % 21;
            const auto key = keys[Random(random) % keys.size()];
            if (action < 9) h.Key(key, action < 5);
            else if (action < 12) h.ButtonInput(static_cast<Button>(Random(random) % 5), action != 11);
            else if (action == 12) h.Cancel();
            else if (action == 13) h.Enable((Random(random) & 1) != 0);
            else if (action == 14) h.Starts((Random(random) & 1) != 0);
            else if (action == 15) h.Configure((Random(random) & 1) != 0, (Random(random) & 1) != 0);
            else if (action == 16) h.Reconcile(key, (Random(random) & 1) != 0);
            else if (action == 17) h.Wheel();
            else if (action == 18) h.ResetSession();
            else if (action == 19) h.PollAt(h.now + Random(random) % 300);
            else {
                h.Observe(h.now);
                const auto motion = h.state.Move({static_cast<double>(i), static_cast<double>(seed)}, h.now++);
                Check(!motion.step, "pointer movement itself never issues a keyboard repeat");
            }
        }
        for (auto key : keys) h.Key(key, false);
        for (int button = 0; button < 5; ++button) h.ButtonInput(static_cast<Button>(button), false);
        h.Ledger();
    }
    Check(steps > 1000, "mixed runs actually reach step actions, not only rejected starts");
}
}

int main() {
    DeterministicCases();
    PrintedPlusAliases();
    ControlComposition();
    HeldRepeats();
    MixedSequences();
    std::cout << "PASS: " << assertions << " shortcut assertions; " << transitions
              << " input transitions; " << steps << " reached step actions; " << timerPolls
              << " timer polls; " << timerSteps << " timer steps; 100 mixed 1000-event sequences\n";
    return 0;
}
