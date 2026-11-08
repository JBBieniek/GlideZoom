#include <stdexcept>
#include "../GlideZoom/foreground_context.h"
#include "../GlideZoom/control_channel.h"
#include "../GlideZoom/pointer_adapter.h"
#include "../GlideZoom/touchpad_state.h"
#include "../GlideZoom/frame_stall_policy.h"
#include "../GlideZoom/gpu_frame_policy.h"
#include "../GlideZoom/cursor_visibility.h"
#include "../GlideZoom/zoom_animation.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace glide::input;
std::uint64_t assertions = 0, scenarios = 0, focusTransitions = 0, pointerWrites = 0;
const char* context = "";

void Check(bool value, const char* text) {
    ++assertions;
    if (!value) {
        std::cerr << "FAIL [" << context << "]: " << text << '\n';
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}

void Near(double actual, double expected, const char* text) {
    Check(std::isfinite(actual) && std::abs(actual - expected) < 1e-9, text);
}

constexpr std::array<std::uintptr_t, 5> windows{
    0, 1, 42, 0xFFFFu, std::numeric_limits<std::uintptr_t>::max()};

struct Controls {
    State state;
    ControlChannel channel;
    ControlReader reader;
    ForegroundContext foreground;
    glide::pointer::Adapter pointer;
    std::uint64_t options = DefaultControlOptions | 1;
    std::uint64_t epoch = 1;
    bool gate = true;
    std::uintptr_t activeWindow = 0;

    Controls() { Publish(); Sync(); }
    void Publish() { channel.Publish({options, epoch, 2}); }
    void Sync() {
        foreground.Observe(activeWindow, pointer);
        reader.Synchronize(channel, state, [&] { return gate; }, [](Decision) {});
        Check(state.Invariant(), "control synchronization preserves input invariant");
    }
    void Focus(std::uintptr_t window) {
        if (window != activeWindow) ++focusTransitions;
        activeWindow = window;
        Sync();
    }
    Decision Key(std::uint32_t key, bool down, std::uint64_t now) {
        Sync();
        const auto effect = state.Key(key, down, now);
        Check(state.Invariant(), "physical event preserves input invariant");
        return effect;
    }
    void Hold(std::uint32_t key) {
        Key(vk::LeftWin, true, 1);
        Key(vk::LeftAlt, true, 2);
        const auto first = Key(key, true, 3);
        Check(first.consume && first.step == (key == vk::Equal ? 1 : -1),
              "fresh global shortcut starts under any foreground identity");
    }
};

void OrdinaryActivation() {
    context = "held shortcut across application activation";
    for (auto initial : windows) for (auto next : windows)
        for (auto trigger : {vk::Equal, vk::Minus}) {
            ++scenarios;
            Controls controls;
            controls.Focus(initial);
            controls.Hold(trigger);
            controls.Focus(next);
            Check(controls.state.SwallowedKey(trigger), "focus preserves owed physical release");
            Check(!controls.state.Expire(252).step, "focus does not bring repeat deadline forward");
            for (unsigned repeat = 0; repeat != 12; ++repeat) {
                controls.Focus(windows[repeat % windows.size()]);
                const auto now = 253 + repeat * 33;
                Check(controls.state.Expire(now).step == (trigger == vk::Equal ? 1 : -1),
                      "live held repeat survives focus changes and transient null HWND");
                Check(!controls.state.Expire(now).step, "focus cannot replay a consumed timer slot");
            }
            const auto release = controls.Key(trigger, false, 650);
            Check(release.consume && !release.step, "matching release remains consumed once");
            Check(!controls.state.Expire(800).step, "release stops future repeat after focus churn");
            controls.Focus(next);
            Check(controls.Key(trigger, true, 801).step == (trigger == vk::Equal ? 1 : -1),
                  "new global gesture also starts with another window still foreground");
        }

    context = "modifier wheel across application activation";
    for (auto initial : windows) for (auto next : windows) {
        ++scenarios;
        Controls controls;
        controls.Focus(initial);
        controls.Key(vk::LeftControl, true, 1);
        controls.Key(vk::LeftAlt, true, 2);
        controls.Focus(next);
        Check(controls.state.CanZoom() && controls.state.Wheel(true, 3).consume,
              "focus does not disable modifier wheel readiness");
        controls.Key('A', true, 4);
        controls.Focus(initial);
        Check(!controls.state.Wheel(true, 5).consume, "foreground typing still blocks wheel interception");
        controls.Key('A', false, 6);
        Check(controls.state.Wheel(true, 7).consume, "wheel resumes after real unrelated key release");
        Check(!controls.state.Wheel(false, 8).consume, "ordinary scrolling still passes through");
    }
}

void MouseAndControlTaps() {
    context = "double tap and hold survives focus transitions";
    for (auto initial : windows) for (auto next : windows) {
        ++scenarios;
        Controls controls;
        controls.options |= 2 | 4;
        controls.Publish();
        controls.Focus(initial);
        controls.Key(vk::LeftControl, true, 10);
        controls.Key(vk::LeftControl, false, 20);
        controls.Focus(next);
        Check(controls.Key(vk::LeftControl, true, 40).begin,
              "foreground change preserves first control tap");
        controls.Focus(0);
        Check(controls.state.Move({100, 100}, 50).consume,
              "owned control hold survives transient activation gap");
        controls.Focus(initial);
        const auto end = controls.Key(vk::LeftControl, false, 60);
        Check(end.end && !end.cancel && !end.consume, "physical control release ends hold normally");

        controls.Key(vk::LeftControl, true, 1000);
        controls.Key(vk::LeftAlt, true, 1001);
        Check(controls.state.ButtonEvent(Button::Left, true, {100, 100}, 1010).consume,
              "optional mouse first tap owns down");
        Check(controls.state.ButtonEvent(Button::Left, false, {100, 100}, 1020).consume,
              "optional mouse first tap owns up");
        controls.Focus(next);
        const auto begin = controls.state.ButtonEvent(Button::Left, true, {100, 100}, 1040);
        Check(begin.begin && begin.consume, "foreground change preserves first modified mouse tap");
        controls.Focus(initial);
        Check(controls.state.Move({101, 103}, 1050).consume, "modified mouse hold stays globally owned");
        const auto mouseEnd = controls.state.ButtonEvent(Button::Left, false, {101, 103}, 1060);
        Check(mouseEnd.end && !mouseEnd.cancel && mouseEnd.consume,
              "modified mouse hold releases without leaking button up");
        Check(controls.state.Invariant(), "tap sequence invariant");
    }
}

glide::touchpad::Frame Contacts(unsigned count, double y, std::uintptr_t device) {
    glide::touchpad::Frame frame;
    frame.device = device;
    frame.count = count;
    for (unsigned i = 0; i < count; ++i)
        frame.contacts[i] = {i + 1, 0.4 + 0.05 * i, y, true, true, true};
    return frame;
}

void TouchpadActivation() {
    context = "three finger tap hold across all foreground phases";
    for (auto initial : windows) for (auto next : windows) for (unsigned phase = 0; phase < 5; ++phase) {
        ++scenarios;
        Controls controls;
        controls.Focus(initial);
        glide::touchpad::State touchpad;
        touchpad.SetEnabled(true);
        touchpad.Push(Contacts(0, 0.5, 7), 1);
        if (phase == 0) controls.Focus(next);
        touchpad.Push(Contacts(1, 0.5, 7), 10);
        touchpad.Push(Contacts(3, 0.5, 7), 20);
        if (phase == 1) controls.Focus(next);
        touchpad.Push(Contacts(0, 0.5, 7), 30);
        if (phase == 2) controls.Focus(next);
        touchpad.Push(Contacts(1, 0.5, 7), 40);
        if (phase == 3) controls.Focus(next);
        const auto begin = touchpad.Push(Contacts(3, 0.5, 7), 50);
        Check(begin.begin && touchpad.Active(), "focus cannot lose first tap or partial second landing");
        if (phase == 4) controls.Focus(next);
        for (unsigned packet = 0; packet < 12; ++packet) {
            controls.Focus(windows[packet % windows.size()]);
            const auto effect = touchpad.Push(Contacts(3, 0.501 + packet * 0.001, 7), 60 + packet * 10);
            Check(!effect.begin && !effect.end && touchpad.Active(), "focus churn preserves owned contact hold");
            Near(effect.delta, 1.0, "focus introduces no lost or synthetic touchpad displacement");
            Check(touchpad.Invariant(), "touchpad invariant across activation");
        }
        const auto end = touchpad.Push(Contacts(0, 0.512, 7), 190);
        Check(end.end && !end.cancel && !touchpad.Active(), "physical contact release ends normally");
    }
}

void SafetyBoundariesStillCancel() {
    context = "foreground changes cannot override actual input safety boundaries";
    for (auto trigger : {vk::Equal, vk::Minus}) for (unsigned interruption = 0; interruption != 8; ++interruption) {
        ++scenarios;
        Controls controls;
        controls.Focus(1);
        controls.Hold(trigger);
        controls.Focus(42);
        switch (interruption) {
        case 0: controls.gate = false; break; // Unavailable or different input desktop.
        case 1: controls.gate = false; break; // Stale UI heartbeat / stopping host.
        case 2: controls.options &= ~std::uint64_t{1}; break; // Explicit pause.
        case 3: ++controls.epoch; break; // Reset, settings or session owner epoch.
        case 4: controls.options |= 2; break; // Configuration changes.
        case 5: controls.state.ReconcileModifier(vk::LeftAlt, false); break;
        case 6: controls.state.Cancel(); break;
        case 7: controls.state.SessionReset(); break; // Genuine desktop boundary forgets old releases.
        }
        controls.Publish();
        controls.Focus(0);
        Check(!controls.state.Expire(253).step, "real interruption still cancels held repeat");
        controls.gate = true;
        controls.options |= 1;
        controls.Publish();
        controls.Focus(1);
        controls.state.ReconcileModifier(vk::LeftAlt, true);
        Check(!controls.state.Expire(500).step, "focus or safety recovery cannot restart old hold");
        const auto release = controls.Key(trigger, false, 510);
        Check(release.consume == (interruption != 7), "ordinary boundaries preserve releases; real session reset clears them");
        controls.Key(vk::LeftWin, true, 520);
        controls.Key(vk::LeftAlt, true, 521);
        controls.Focus(42);
        Check(controls.Key(trigger, true, 522).step == (trigger == vk::Equal ? 1 : -1),
              "fresh chord recovers globally after actual safety boundary");
    }

    context = "touchpad safety is independent of foreground identity";
    for (unsigned interruption = 0; interruption != 5; ++interruption) {
        ++scenarios;
        Controls controls;
        glide::touchpad::State touchpad;
        touchpad.SetEnabled(true);
        touchpad.Push(Contacts(0, 0.5, 7), 1);
        touchpad.Push(Contacts(3, 0.5, 7), 10);
        touchpad.Push(Contacts(0, 0.5, 7), 20);
        Check(touchpad.Push(Contacts(3, 0.5, 7), 30).begin, "touchpad owned before safety interruption");
        controls.Focus(42);
        glide::touchpad::Effect end;
        switch (interruption) {
        case 0: end = touchpad.SetEnabled(false); break; // Heartbeat, desktop, pause or other held input.
        case 1: end = touchpad.Cancel(); break; // Explicit reset / keyboard action.
        case 2: end = touchpad.Configure({500, 30000}); break;
        case 3: end = touchpad.Expire(500); break;
        case 4: end = touchpad.Push(Contacts(3, 0.5, 8), 40); break;
        }
        controls.Focus(0);
        Check(end.end && end.cancel && !touchpad.Active(), "real safety boundary cancels contact hold");
        touchpad.SetEnabled(true);
        controls.Focus(1);
        Check(!touchpad.Push(Contacts(3, 0.5, 7), 501).begin,
              "activation and gate recovery cannot resurrect contacts still down");
        Check(touchpad.Invariant(), "cancelled touchpad invariant");
    }
}

struct NativePointer {
    glide::Vec2 cursor{100, 100};
    glide::pointer::Adapter* pointer = nullptr;
    ForegroundContext* context = nullptr;
    bool changeDuringReady = false, changeDuringWarp = false;
    std::uintptr_t nextWindow = 42;
    unsigned writes = 0;
    bool Ready(const glide::pointer::View&) noexcept {
        if (changeDuringReady) context->Observe(nextWindow, *pointer);
        return true;
    }
    bool Read(glide::Vec2& result) noexcept { result = cursor; return true; }
    bool CanPlace(glide::Vec2) noexcept { return true; }
    bool Warp(glide::Vec2 result) noexcept {
        ++writes;
        ++pointerWrites;
        cursor = result;
        pointer->ObserveWarp(result);
        if (changeDuringWarp) context->Observe(nextWindow, *pointer);
        return true;
    }
};

void PointerReseedingAndReentry() {
    context = "pointer history belongs to foreground; global holds do not";
    for (auto oldWindow : windows) for (auto newWindow : windows) {
        ++scenarios;
        ForegroundContext foreground;
        glide::pointer::Adapter pointer;
        NativePointer native;
        native.pointer = &pointer;
        native.context = &foreground;
        const glide::pointer::View view{true, true, {0, 0, 1920, 1080}, 4, 1};
        foreground.Observe(oldWindow, pointer);
        Check(pointer.Move({101, 100}, false, 1, view, true, false, native), "quarter-pixel move is retained");
        Near(pointer.Remainder().x, 0.25, "initial fractional movement retained");
        foreground.Observe(oldWindow, pointer);
        Near(pointer.Remainder().x, 0.25, "unchanged foreground never repeatedly clears motion");
        foreground.Observe(newWindow, pointer);
        Near(pointer.Remainder().x, oldWindow == newWindow ? 0.25 : 0.0,
             "only changed foreground resets fractional motion");
        if (oldWindow != newWindow) {
            native.cursor = {500, 500}; // A newly active app deliberately positioned its cursor.
            Check(pointer.Move({504, 500}, false, 2, view, true, false, native), "first new-app motion uses fresh native baseline");
            Near(native.cursor.x, 501, "new-app pointer is never pulled to previous app position");
            Near(native.cursor.y, 500, "foreground transition does not invent perpendicular motion");
            Check(native.writes == 1 && !pointer.Faulted(), "activation reseeds without disabling pointer scaling");
        }
    }

    context = "focus transition during a native pointer operation";
    for (bool duringWarp : {false, true}) {
        ++scenarios;
        Controls controls;
        controls.Focus(1);
        controls.Hold(vk::Equal);
        NativePointer native;
        native.pointer = &controls.pointer;
        native.context = &controls.foreground;
        native.changeDuringReady = !duringWarp;
        native.changeDuringWarp = duringWarp;
        const glide::pointer::View view{true, true, {0, 0, 1920, 1080}, 4, 1};
        Check(!controls.pointer.Move({116, 100}, false, 4, view, true, false, native),
              "activation reentry revokes stale pointer commit");
        Check(native.writes == (duringWarp ? 1u : 0u), "no retry or extra pointer write after activation reentry");
        Check(!controls.pointer.Faulted(), "ordinary activation is not a permanent pointer fault");
        Check(controls.state.Expire(253).step == 1, "pointer reentry does not cancel globally owned zoom hold");
        Check(controls.state.SwallowedKey(vk::Equal), "pointer reentry preserves physical release ownership");
    }
}

enum class PacketKind { Reset, Step, Delta };
struct Packet {
    PacketKind kind = PacketKind::Step;
    std::uint64_t epoch = 0;
    double amount = 0;
};

struct RecoveryJournal {
    bool cursorArmed = false;
    void Publish(const glide::session::RecoveryRecord&) noexcept {}
    void Clear() noexcept {}
    bool ArmCursorRecovery() noexcept { cursorArmed = true; return true; }
    void ClearCursorRecovery() noexcept { cursorArmed = false; }
};

struct SharedState {
    glide::session::Snapshot current{};
    bool showing = true;
    bool Read(glide::session::Snapshot& value) noexcept { value = current; return true; }
    bool SetInput(glide::session::Input value) noexcept { current.input = value; return true; }
    bool SetVisual(glide::session::Visual value) noexcept { current.visual = value; return true; }
    bool OnInputDesktop() const noexcept { return true; }
    bool CursorShowing(bool& value) const noexcept { value = showing; return true; }
    bool ShowSystemCursor(bool value) noexcept { showing = value; return true; }
};

// Compose the real queue, ownership and epoch policies. Native presentation is
// deliberately absent: the startup exception must keep all shared state at
// identity until a complete capture can be presented by the actual adapter.
struct Warmup {
    glide::runtime::State lifecycle;
    Controls controls;
    glide::gpu::FrameGate capture;
    glide::session::Session session;
    glide::cursor::Lease cursor;
    SharedState shared;
    RecoveryJournal journal;
    SpscQueue<Packet, 256> queue;
    double target = 2.0;
    bool monitorMode = true, visible = false;
    unsigned acceptedMotion = 0, acceptedResets = 0;

    Warmup() {
        Check(lifecycle.PrepareEnable() && lifecycle.Activate(), "warmup starts under a ready runtime");
        Check(capture.Begin(1920, 1080, 0) != 0, "warmup owns one initial capture generation");
        Configure();
    }
    void Configure() {
        controls.epoch = lifecycle.Epoch();
        controls.options = (controls.options & ~std::uint64_t{1}) | (lifecycle.Ready() ? 1u : 0u);
        controls.Publish();
        controls.Sync();
    }
    void CancelInput() { lifecycle.Cancel(); Configure(); }
    void Reset() {
        target = 1;
        lifecycle.RequestRestore();
        CancelInput();
        capture.Close();
        visible = false;
        const auto restored = session.Reset(shared, journal);
        Check(restored == glide::session::Result::Applied || restored == glide::session::Result::Unchanged,
              "stale-frame reset conditionally restores actual Session ownership");
        Check(cursor.Release(shared, journal), "stale-frame reset restores the actual cursor lease");
        lifecycle.RestoreComplete();
        Configure();
    }
    glide::timing::StallContext Context(std::uint64_t now = 500, bool resetRequested = false) {
        return {lifecycle.Ready(), monitorMode,
                capture.State(now) == glide::gpu::FrameState::Pending,
                session.OwnsState(), visible, cursor.NeedsRecovery(), resetRequested};
    }
    glide::timing::StallAction Stall(bool resetRequested = false) {
        const auto action = glide::timing::DecideStall(Context(500, resetRequested));
        if (action == glide::timing::StallAction::PreserveInitialTargetAndDiscardInput) CancelInput();
        else Reset();
        return action;
    }
    void Push(PacketKind kind, std::uint64_t epoch, double amount = 120) {
        Check(queue.Push({kind, epoch, amount}), "bounded owned input queue accepts fixture packet");
    }
    void Drain() {
        Packet packet;
        for (unsigned count = 0; count != 256 && queue.Pop(packet); ++count) {
            const bool reset = packet.kind == PacketKind::Reset;
            if (!glide::timing::AcceptQueuedInput(reset, packet.epoch, lifecycle)) continue;
            if (reset) { ++acceptedResets; Reset(); }
            else {
                ++acceptedMotion;
                target = packet.kind == PacketKind::Step
                    ? glide::animation::StepZoom(target, packet.amount, 16)
                    : glide::DragZoom(target, packet.amount, 500, 16);
            }
        }
    }
};

void StalledWarmupAndQueuedReset() {
    context = "stalled first frame drops stale input without losing an owned reset";
    constexpr std::array<std::array<PacketKind, 3>, 6> orders{{
        {PacketKind::Reset, PacketKind::Step, PacketKind::Delta},
        {PacketKind::Reset, PacketKind::Delta, PacketKind::Step},
        {PacketKind::Step, PacketKind::Reset, PacketKind::Delta},
        {PacketKind::Step, PacketKind::Delta, PacketKind::Reset},
        {PacketKind::Delta, PacketKind::Reset, PacketKind::Step},
        {PacketKind::Delta, PacketKind::Step, PacketKind::Reset}
    }};
    for (const auto& order : orders) {
        ++scenarios;
        Warmup warmup;
        warmup.controls.Focus(42);
        warmup.controls.Hold(vk::Equal);
        warmup.controls.Key(vk::LeftControl, true, 10);
        const auto reset = warmup.controls.Key(vk::Escape, true, 11);
        Check(reset.reset && reset.consume, "reset packet originates from an actually owned global shortcut");
        const auto oldEpoch = warmup.lifecycle.Epoch();
        for (auto kind : order) warmup.Push(kind, oldEpoch);
        Check(warmup.Stall() == glide::timing::StallAction::PreserveInitialTargetAndDiscardInput,
              "unapplied initial capture may preserve one existing target while dropping stale input");
        Check(warmup.target == 2 && warmup.lifecycle.Epoch() != oldEpoch,
              "startup exception preserves target but advances input cancellation epoch");
        Check(glide::session::IsIdentity(warmup.shared.current) && !warmup.cursor.NeedsRecovery() &&
              warmup.shared.showing && !warmup.visible,
              "waiting never installs transformed input or hides the ordinary cursor");
        warmup.Drain();
        Check(warmup.acceptedResets == 1 && warmup.acceptedMotion == 0 && warmup.target == 1,
              "old reset wins in every packet order; old steps and deltas cannot revive zoom");
        Check(warmup.capture.State(501) == glide::gpu::FrameState::Closed,
              "queued old reset also closes the pending capture generation");
        Check(!warmup.controls.state.Expire(600).step,
              "cancelled physical hold cannot generate a future step after reset");
        Check(warmup.controls.Key(vk::Escape, false, 610).consume,
              "accepted old reset retains its matching physical release obligation");
        Check(!warmup.controls.Key(vk::Escape, false, 611).consume,
              "reset key release is still consumed exactly once");
    }

    context = "stalled first frame retains only the requested target";
    ++scenarios;
    Warmup warmup;
    warmup.controls.Hold(vk::Equal);
    const auto oldEpoch = warmup.lifecycle.Epoch();
    warmup.Push(PacketKind::Step, oldEpoch);
    warmup.Push(PacketKind::Delta, oldEpoch);
    Check(warmup.Stall() == glide::timing::StallAction::PreserveInitialTargetAndDiscardInput,
          "first-frame waiting is the only preserve-target path");
    warmup.Drain();
    Check(warmup.target == 2 && warmup.acceptedMotion == 0,
          "discarded queued motion cannot extend an initial target");
    Check(!warmup.controls.state.Expire(500).step && !warmup.controls.state.Expire(1000).step,
          "input cancellation ends native held repeat instead of resuming old input after warmup");
    Check(warmup.controls.Key(vk::Equal, false, 1001).consume,
          "discarding startup input keeps owned releases paired");
    const auto fresh = warmup.controls.Key(vk::Equal, true, 1002);
    Check(fresh.step == 1, "a new physical command works in the new startup epoch");
    warmup.Push(PacketKind::Step, warmup.lifecycle.Epoch(), 120.0 * fresh.step);
    warmup.Drain();
    Near(warmup.target, 2.01, "only fresh input may change the retained pending target");
    Check(warmup.capture.State(1999) == glide::gpu::FrameState::Pending &&
          warmup.capture.State(2000) == glide::gpu::FrameState::TimedOut,
          "discarding stale input does not extend the first-frame timeout");
    Check(glide::timing::DecideStall(warmup.Context(2000)) == glide::timing::StallAction::Reset,
          "an expired capture cannot keep using the startup exception");
}

void StalledStateMatrix() {
    context = "mixed applied and pending states cannot broaden startup exception";
    for (unsigned mask = 0; mask != 128; ++mask) {
        ++scenarios;
        Warmup warmup;
        if (!(mask & 1)) { warmup.lifecycle.Pause(); warmup.Configure(); }
        warmup.monitorMode = (mask & 2) != 0;
        if (!(mask & 4))
            Check(warmup.capture.Accept(warmup.capture.Generation(), 1920, 1080),
                  "matrix includes an already complete capture, not just startup wait");
        if (mask & 8) {
            const glide::session::Snapshot mapped{{1, 0, 0}, {true, {0, 0, 960, 540}, {0, 0, 1920, 1080}}};
            Check(warmup.session.Apply(mapped, warmup.shared, warmup.journal) == glide::session::Result::Applied,
                  "matrix establishes real owned input mapping");
        }
        warmup.visible = (mask & 16) != 0;
        if (mask & 32)
            Check(warmup.cursor.Update(true, warmup.shared, warmup.journal) && warmup.cursor.NeedsRecovery(),
                  "matrix acquires a real cursor recovery lease");
        const auto oldEpoch = warmup.lifecycle.Epoch();
        warmup.Push(PacketKind::Step, oldEpoch);
        const auto decision = warmup.Stall((mask & 64) != 0);
        Check((decision == glide::timing::StallAction::PreserveInitialTargetAndDiscardInput) == (mask == 7),
              "exactly one mixed-state row qualifies for initial-target preservation");
        warmup.Drain();
        Check(warmup.acceptedMotion == 0 && warmup.target == (mask == 7 ? 2.0 : 1.0),
              "stale motion cannot survive any stalled-state action");
        Check(!warmup.session.OwnsState() && glide::session::IsIdentity(warmup.shared.current) &&
              !warmup.cursor.NeedsRecovery() && !warmup.journal.cursorArmed && warmup.shared.showing,
              "a stalled applied or uncertain view never retains its input/cursor ownership");
        Check(!warmup.visible, "stalled visible overlay is closed before accepting fresh input");
    }

    context = "reset acceptance does not grant motion authority while paused";
    ++scenarios;
    Warmup paused;
    paused.lifecycle.Pause();
    paused.Configure();
    const auto epoch = paused.lifecycle.Epoch();
    Check(!glide::timing::AcceptQueuedInput(false, epoch, paused.lifecycle),
          "even matching-epoch movement remains rejected while paused");
    Check(glide::timing::AcceptQueuedInput(true, epoch - 1, paused.lifecycle),
          "an owned older reset remains conservative and available while paused");
    paused.Push(PacketKind::Reset, epoch - 1);
    paused.Push(PacketKind::Step, epoch);
    paused.Drain();
    Check(paused.target == 1 && paused.acceptedResets == 1 && !paused.acceptedMotion &&
          !paused.lifecycle.Ready(), "reset clears zoom without resuming paused controls");
}
} // namespace

int Run_input_context() {
    OrdinaryActivation();
    MouseAndControlTaps();
    TouchpadActivation();
    SafetyBoundariesStillCancel();
    PointerReseedingAndReentry();
    StalledWarmupAndQueuedReset();
    StalledStateMatrix();
    std::cout << "PASS: " << assertions << " input-context assertions; " << scenarios
              << " scenarios; " << focusTransitions << " foreground transitions; "
              << pointerWrites << " mocked pointer writes; production foreground/control/input/touchpad/pointer policies\n";
    return 0;
}
