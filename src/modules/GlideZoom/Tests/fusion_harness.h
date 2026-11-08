#pragma once

#include "../GlideZoom/input_state.h"
#include "../GlideZoom/input_transport.h"
#include "../GlideZoom/runtime_state.h"
#include "../GlideZoom/settings.h"
#include "../GlideZoom/zoom_session.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Portable composition harness, not the native App or input::Host. The real
// recognizer, queue, lifecycle gate, session, settings codec and zoom math are
// composed below. Hooks, desktop observations, UI message scheduling, native
// setters/getters and a completed settings-save result are scripted adapters.
// Each method is one scheduler step; only explicit backend boundaries interleave.
namespace fusion {
using glide::input::Button;
using glide::input::Gesture;
using glide::runtime::Phase;
using glide::session::Result;
using glide::session::Snapshot;

inline void Require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

inline const char* Name(Phase phase) {
    switch (phase) {
    case Phase::Starting: return "starting";
    case Phase::Ready: return "ready";
    case Phase::Paused: return "paused";
    case Phase::Suspended: return "suspended";
    case Phase::Faulted: return "faulted";
    case Phase::Stopping: return "stopping";
    }
    return "invalid";
}

enum class Fixture { Idle, Wheel, Space, Control, Mouse };
enum class Schedule { Queued, BeginConsumed, FullyConsumed };
enum class Interruption {
    None, Reset, Pause, SettingsValid, SettingsInvalid, SaveFailure,
    Focus, DesktopUnknown, DesktopOther, GuardianLoss, DisplayChange, UiStale
};
enum class Fault {
    None, Read1, Read2, Write1NoEffect, Write1FalseAfterEffect,
    ForeignBeforeRead1, ForeignAfterWrite1
};
enum class Desktop { Current, Unknown, Other };
inline constexpr std::array<const char*, 5> FixtureNames{
    "idle", "wheel", "space_hold", "double_ctrl_hold", "modified_mouse_hold"};
inline constexpr std::array<const char*, 3> ScheduleNames{
    "queued", "begin_consumed", "fully_consumed"};
inline constexpr std::array<const char*, 12> InterruptionNames{
    "none", "reset", "pause", "settings_valid", "settings_invalid", "save_failure",
    "focus_change", "desktop_unknown", "desktop_other", "guardian_loss",
    "display_change", "ui_stale"};
inline constexpr std::array<const char*, 7> FaultNames{
    "none", "read_1", "read_2", "write_1_no_effect", "write_1_false_after_effect",
    "foreign_before_read_1", "foreign_after_write_1"};

struct Trace {
    std::vector<std::string> entries;
    void Add(std::string entry) { entries.push_back(std::move(entry)); }
};

struct Journal {
    Trace& trace;
    bool armed = false;
    glide::session::RecoveryRecord record{};
    unsigned publications = 0, clears = 0;

    void Publish(const glide::session::RecoveryRecord& next) {
        Require(glide::session::Valid(next), "journal contains an invalid recovery pair");
        record = next;
        armed = true;
        ++publications;
        trace.Add("journal.publish");
    }
    void Clear() {
        armed = false;
        ++clears;
        trace.Add("journal.clear");
    }
};

enum class Boundary { Read, BeforeWrite, AfterWrite };
enum class Effect { FailRead, RejectWrite, FalseAfterEffect, Foreign };
struct Injection {
    Boundary boundary;
    unsigned ordinal;
    Effect effect;
    bool fired = false;
};

// Native calls are deliberately independent. A setter returning false may
// still have taken effect. Foreign writes can occur at named read/write cuts.
struct Backend {
    Journal& journal;
    Trace& trace;
    Snapshot state{};
    std::vector<Injection> script;
    unsigned reads = 0, writes = 0, mutations = 0;
    unsigned foreignWrites = 0, writesAfterForeign = 0;
    bool available = true, foreignActive = false, allowForeignOverwrite = false;
    Snapshot lastForeign{};

    Backend(Journal& journalValue, Trace& traceValue) : journal(journalValue), trace(traceValue) {}

    static Snapshot Foreign() {
        return {{5.0f, 700, 120}, {true, {700, 120, 1084, 336}, {0, 0, 1920, 1080}}};
    }
    void Arm(std::vector<Injection> injections) {
        script = std::move(injections);
        reads = writes = mutations = 0;
        trace.Add("fault.arm");
    }
    void Arm(Fault fault) {
        std::vector<Injection> injections;
        switch (fault) {
        case Fault::None: break;
        case Fault::Read1: injections.push_back({Boundary::Read, 1, Effect::FailRead}); break;
        case Fault::Read2: injections.push_back({Boundary::Read, 2, Effect::FailRead}); break;
        case Fault::Write1NoEffect:
            injections.push_back({Boundary::BeforeWrite, 1, Effect::RejectWrite}); break;
        case Fault::Write1FalseAfterEffect:
            injections.push_back({Boundary::AfterWrite, 1, Effect::FalseAfterEffect}); break;
        case Fault::ForeignBeforeRead1:
            injections.push_back({Boundary::Read, 1, Effect::Foreign}); break;
        case Fault::ForeignAfterWrite1:
            injections.push_back({Boundary::AfterWrite, 1, Effect::Foreign}); break;
        }
        Arm(std::move(injections));
    }
    bool Reached() const {
        for (const auto& injection : script) if (injection.fired) return true;
        return false;
    }
    unsigned ReachedCount() const {
        unsigned count = 0;
        for (const auto& injection : script) if (injection.fired) ++count;
        return count;
    }
    void Disarm() { script.clear(); }
    void ExternalWrite() {
        state = lastForeign = Foreign();
        foreignActive = true;
        ++foreignWrites;
        trace.Add("external.magnifier.write");
    }
    bool At(Boundary boundary, unsigned ordinal, Effect effect) {
        bool found = false;
        for (auto& injection : script) {
            if (!injection.fired && injection.boundary == boundary &&
                injection.ordinal == ordinal && injection.effect == effect) {
                injection.fired = found = true;
                trace.Add("fault.fire boundary=" + std::to_string(static_cast<int>(boundary)) +
                          " ordinal=" + std::to_string(ordinal) +
                          " effect=" + std::to_string(static_cast<int>(effect)));
            }
        }
        return found;
    }
    bool Read(Snapshot& result) {
        Require(available, "native read while desktop is unavailable");
        ++reads;
        trace.Add("native.read " + std::to_string(reads));
        if (At(Boundary::Read, reads, Effect::Foreign)) ExternalWrite();
        if (At(Boundary::Read, reads, Effect::FailRead)) return false;
        result = state;
        return true;
    }
    bool Write(Snapshot next, const char* component) {
        Require(available, "native write while desktop is unavailable");
        Require(glide::session::Valid(next), "invalid native setter argument");
        Require(journal.armed && glide::session::Valid(journal.record),
                "native setter has no valid published recovery record");
        Require(journal.record.before == state && journal.record.after == next,
                "journal did not record the actual before/after pair before setter");
        ++writes;
        trace.Add(std::string("native.write.") + component + " " + std::to_string(writes));
        if (At(Boundary::BeforeWrite, writes, Effect::Foreign)) ExternalWrite();
        if (foreignActive) {
            ++writesAfterForeign;
            Require(allowForeignOverwrite, "application wrote after observable foreign ownership");
        }
        if (At(Boundary::BeforeWrite, writes, Effect::RejectWrite)) return false;
        // A real setter changes only its own component, including in the
        // intentionally demonstrated final-read/setter race.
        if (component[0] == 'v') state.visual = next.visual;
        else state.input = next.input;
        ++mutations;
        if (At(Boundary::AfterWrite, writes, Effect::Foreign)) ExternalWrite();
        return !At(Boundary::AfterWrite, writes, Effect::FalseAfterEffect);
    }
    bool SetVisual(glide::session::Visual value) {
        Snapshot next = state;
        next.visual = value;
        return Write(next, "visual");
    }
    bool SetInput(glide::session::Input value) {
        Snapshot next = state;
        next.input = value;
        return Write(next, "input");
    }
};

enum class Kind { Begin, End, Reset, Wheel, Delta };
struct Command {
    Kind kind = Kind::Begin;
    Gesture gesture = Gesture::None;
    glide::Vec2 anchor{800, 450};
    double amount = 0, travel = 0;
    std::uint64_t epoch = 0;
    bool cancel = false;
};

inline Snapshot Desired(glide::Transform transform, glide::Rect desktop) {
    if (transform.zoom == 1) return {};
    const auto x = static_cast<std::int32_t>(std::lround(transform.offset.x));
    const auto y = static_cast<std::int32_t>(std::lround(transform.offset.y));
    const auto zoom = static_cast<float>(transform.zoom);
    return {{zoom, x, y}, {true,
        {static_cast<std::int32_t>(std::lround(x + desktop.left / zoom)),
         static_cast<std::int32_t>(std::lround(y + desktop.top / zoom)),
         static_cast<std::int32_t>(std::lround(x + desktop.right / zoom)),
         static_cast<std::int32_t>(std::lround(y + desktop.bottom / zoom))},
        {static_cast<std::int32_t>(desktop.left), static_cast<std::int32_t>(desktop.top),
         static_cast<std::int32_t>(desktop.right), static_cast<std::int32_t>(desktop.bottom)}}};
}

class Harness {
public:
    Trace trace;
    Journal journal{trace};
    Backend backend{journal, trace};
    glide::session::Session session;
    glide::runtime::State runtime;
    glide::input::State input;
    glide::input::SpscQueue<Command, 16> commands;
    glide::settings::Values settings{};
    glide::Rect desktop{0, 0, 1920, 1080};
    glide::Transform transform{};
    Desktop context = Desktop::Current;
    bool guardian = true, queueFault = false, uiFresh = true;
    bool refreshPending = false, saveAttempted = false, saveCommitted = false;
    unsigned refreshes = 0, accepted = 0, dropped = 0, queued = 0;
    std::uint64_t now = 100, hostEpoch = 0;
    double target = 1, dragStart = 1, pixels = 0, travel = 0;
    bool dragging = false;
    glide::Vec2 anchor{800, 450};
    std::array<bool, 256> physicalKeys{}, owedKeys{};
    std::array<bool, 5> physicalButtons{}, owedButtons{};

    explicit Harness(double zoom = 1) {
        // Enable optional gestures explicitly for their fixture rows.
        settings.keyboardGesture = settings.mouseGesture = true;
        input.Configure({true, true, 400, 30000});
        Require(runtime.PrepareEnable() && runtime.Activate(), "fixture enable failed");
        Sync();
        target = zoom;
        Frame(10);
        Check();
        trace.Add("fixture.zoom=" + std::to_string(zoom));
    }

    bool Push(Command command) {
        if (queueFault) return false;
        command.epoch = hostEpoch;
        if (!commands.Push(command)) {
            trace.Add("queue.full -> host fault");
            queueFault = true;
            input.SetEnabled(false); // Same nonrecursive fault contract as Host.
            return false;
        }
        ++queued;
        trace.Add("queue.push kind=" + std::to_string(static_cast<int>(command.kind)) +
                  " epoch=" + std::to_string(command.epoch));
        return true;
    }
    void Emit(glide::input::Decision decision) {
        if (decision.end) {
            Command command;
            command.kind = Kind::End;
            command.cancel = decision.cancel;
            Push(command);
        }
        if (decision.begin) {
            Command command;
            command.kind = Kind::Begin;
            command.gesture = input.Active();
            Push(command);
        }
        if (decision.reset) {
            Command command;
            command.kind = Kind::Reset;
            Push(command);
        }
    }
    void Sync() {
        if (hostEpoch != runtime.Epoch()) {
            Emit(input.Cancel()); // Cancellation is stamped with the old epoch.
            hostEpoch = runtime.Epoch();
        }
        Emit(input.SetEnabled(runtime.Ready() && context == Desktop::Current &&
                              guardian && !queueFault && uiFresh));
        Check();
    }
    void Reconfigure() {
        Emit(input.Configure({settings.keyboardGesture, settings.mouseGesture, 400, 30000}));
        Sync();
    }
    bool Key(std::uint32_t key, bool down) {
        Require(key < physicalKeys.size(), "invalid test key");
        const bool wasDown = physicalKeys[key], owed = owedKeys[key];
        const auto decision = input.Key(key, down, ++now);
        if (wasDown || !down)
            Require(decision.consume == owed, "key repeat/up disagrees with delivered DOWN");
        else if (decision.consume)
            Require(key == glide::input::vk::Space || key == glide::input::vk::Escape,
                    "a modifier or ordinary key was consumed");
        physicalKeys[key] = down;
        owedKeys[key] = down && ((wasDown && owed) || (!wasDown && decision.consume));
        trace.Add("key " + std::to_string(key) + (down ? " down" : " up") +
                  (decision.consume ? " reserved" : " forwarded"));
        Emit(decision);
        Check();
        return decision.consume;
    }
    bool ButtonEvent(Button button, bool down) {
        const auto index = static_cast<std::size_t>(button);
        const bool wasDown = physicalButtons[index], owed = owedButtons[index];
        const auto decision = input.ButtonEvent(button, down, {800, 450}, ++now);
        if (wasDown || !down)
            Require(decision.consume == owed, "button repeat/up disagrees with delivered DOWN");
        else if (decision.consume)
            Require(button == Button::Left, "unreserved button was consumed");
        physicalButtons[index] = down;
        owedButtons[index] = down && ((wasDown && owed) || (!wasDown && decision.consume));
        trace.Add("button " + std::to_string(index) + (down ? " down" : " up") +
                  (decision.consume ? " reserved" : " forwarded"));
        Emit(decision);
        Check();
        return decision.consume;
    }
    void Delta(double amount) {
        const auto decision = input.Move({800, 450 + amount}, ++now);
        Emit(decision);
        if (decision.consume && input.Active() != Gesture::None) {
            Command command;
            command.kind = Kind::Delta;
            command.amount = amount;
            command.travel = std::abs(amount);
            Push(command);
        }
        Check();
    }
    void Wheel(double amount = 120) {
        const auto decision = input.Wheel(true, ++now);
        Emit(decision);
        if (decision.consume) {
            Command command;
            command.kind = Kind::Wheel;
            command.amount = amount;
            Push(command);
        }
        Check();
    }
    void Start(Fixture fixture) {
        trace.Add(std::string("fixture.input=") + FixtureNames[static_cast<unsigned>(fixture)]);
        using namespace glide::input;
        switch (fixture) {
        case Fixture::Idle: return;
        case Fixture::Wheel:
            Key(vk::LeftControl, true);
            Key(vk::LeftAlt, true);
            Wheel();
            return;
        case Fixture::Space:
            Key(vk::LeftControl, true);
            Key(vk::LeftAlt, true);
            Require(Key(vk::Space, true), "Space fixture did not reserve DOWN");
            Require(input.Active() == Gesture::Space, "Space fixture did not begin");
            break;
        case Fixture::Control:
            Key(vk::LeftControl, true);
            Key(vk::LeftControl, false);
            Key(vk::LeftControl, true);
            Require(input.Active() == Gesture::Control, "Ctrl fixture did not begin");
            break;
        case Fixture::Mouse:
            Key(vk::LeftControl, true);
            Key(vk::LeftAlt, true);
            Require(ButtonEvent(Button::Left, true), "first modified click not reserved");
            Require(ButtonEvent(Button::Left, false), "first modified release not reserved");
            Require(ButtonEvent(Button::Left, true), "second modified click not reserved");
            Require(input.Active() == Gesture::Mouse, "mouse fixture did not begin");
            break;
        }
        Delta(-90);
    }
    void Drain(unsigned maximum = std::numeric_limits<unsigned>::max()) {
        Command command;
        while (maximum-- != 0 && commands.Pop(command)) {
            if (!runtime.Accept(command.epoch)) {
                ++dropped;
                trace.Add("queue.drop stale-or-disabled");
                continue;
            }
            ++accepted;
            trace.Add("queue.accept kind=" + std::to_string(static_cast<int>(command.kind)));
            switch (command.kind) {
            case Kind::Begin:
                if (!dragging) {
                    dragging = true;
                    dragStart = transform.zoom;
                    pixels = travel = 0;
                    anchor = command.anchor;
                    target = transform.zoom;
                }
                break;
            case Kind::End:
                if (dragging && !command.cancel && travel < 4)
                    target = target > 1.001 || transform.zoom > 1.001 ? 1 : 2;
                dragging = false;
                break;
            case Kind::Reset: Reset(); break;
            case Kind::Wheel:
                target = glide::WheelZoom(target, settings.reverse ? -command.amount : command.amount,
                                          settings.WheelFactor(), settings.maxZoom);
                break;
            case Kind::Delta:
                if (dragging && std::isfinite(command.amount) &&
                    std::isfinite(command.travel) && command.travel >= 0) {
                    pixels = std::clamp(pixels + command.amount, -1000000.0, 1000000.0);
                    travel = std::min(1000000.0, travel + std::max(std::abs(command.amount), command.travel));
                    target = glide::DragZoom(dragStart, settings.reverse ? -pixels : pixels,
                                            settings.DragDistance(), settings.maxZoom);
                }
                break;
            }
        }
        Check();
    }
    void CancelInput() {
        runtime.Cancel();
        dragging = false;
        pixels = travel = 0;
        Sync();
    }
    bool RestoreOwned() {
        target = 1;
        if (session.OwnsState()) {
            if (context != Desktop::Current) {
                runtime.RequestRestore();
                CancelInput();
                trace.Add("restore.deferred");
                return false;
            }
            const auto result = session.Reset(backend, journal);
            if (result == Result::Conflict) {
                runtime.RestoreConflict();
                CancelInput();
            } else if (result != Result::Applied && result != Result::Unchanged) {
                runtime.Fault();
                CancelInput();
                trace.Add("restore.failed -> faulted");
                return false;
            }
        }
        runtime.RestoreComplete();
        transform = {};
        Sync();
        return true;
    }
    bool Reset() {
        trace.Add("app.reset");
        runtime.RequestRestore();
        CancelInput();
        return RestoreOwned();
    }
    void Pause() {
        runtime.Pause();
        Reset();
    }
    void Suspend() {
        runtime.Suspend();
        Reset();
    }
    void FaultNow() {
        runtime.Fault();
        Reset();
        Sync();
    }
    void SessionFailure(Result result) {
        trace.Add("session.failure=" + std::to_string(static_cast<int>(result)));
        runtime.Pause();
        Reset();
    }
    bool Enable() {
        if (!runtime.PrepareEnable()) return false;
        CancelInput();
        if (!guardian || queueFault) return false;
        if (context != Desktop::Current) {
            runtime.Suspend(true);
            Sync();
            return false;
        }
        if (runtime.NeedsRestore() && !RestoreOwned()) return false;
        if (refreshPending) Refresh();
        Snapshot observed;
        if (!backend.Read(observed) || !glide::session::Valid(observed) ||
            !glide::session::IsIdentity(observed)) return false;
        if (!runtime.Activate()) return false;
        CancelInput();
        return true;
    }
    void Frame(double dt = 0.05) {
        if (!runtime.Ready()) return;
        if (!guardian || queueFault) { FaultNow(); return; }
        if (context != Desktop::Current) { Suspend(); return; }
        if (transform.zoom <= 1.0001 && target <= 1.0001) return;
        double zoom = glide::SmoothZoom(transform.zoom, target, dt, 0.075);
        if (std::abs(zoom - target) < 0.0005) zoom = target;
        if (target <= 1.000001 && zoom < 1.0005) {
            if (dragging) RestoreOwned();
            else Reset();
            return;
        }
        const auto next = glide::AnchorZoom(transform, zoom, anchor, desktop);
        const Snapshot desired = Desired(next, desktop);
        const auto result = session.Apply(desired, backend, journal);
        if (result != Result::Applied && result != Result::Unchanged) {
            SessionFailure(result);
            return;
        }
        transform = {desired.visual.zoom,
                     {static_cast<double>(desired.visual.x), static_cast<double>(desired.visual.y)}};
        Sync();
        Check();
    }
    void SetDesktop(Desktop value) {
        context = value;
        backend.available = value == Desktop::Current;
        trace.Add("desktop=" + std::to_string(static_cast<int>(value)));
        if (value == Desktop::Other) {
            Emit(input.SessionReset());
            physicalKeys.fill(false);
            owedKeys.fill(false);
            physicalButtons.fill(false);
            owedButtons.fill(false);
        }
        Sync();
    }
    void Refresh() {
        Require(context == Desktop::Current, "refreshed geometry on unavailable desktop");
        Require(!runtime.NeedsRestore(), "refreshed geometry before deferred restoration");
        desktop = {-1280, 0, 1920, 1080};
        refreshPending = false;
        ++refreshes;
        trace.Add("display.refresh");
    }
    void DisplayChange() {
        refreshPending = true;
        Suspend();
    }
    void Save(bool valid, bool succeeds) {
        const auto old = settings;
        auto candidate = old;
        candidate.maxZoom = 16;
        candidate.sensitivity = 2;
        candidate.reverse = true;
        candidate.keyboardGesture = candidate.mouseGesture = false;
        auto parsed = old;
        const auto bytes = valid ? glide::settings::Serialize(candidate) :
                                  glide::settings::Serialize(candidate) + "MaximumZoom=2\n";
        if (!glide::settings::Parse(bytes, parsed)) {
            Require(parsed == old, "failed settings parse partially modified live candidate");
            trace.Add("settings.rejected");
            return;
        }
        const bool resume = runtime.Ready();
        runtime.Pause();
        if (!Reset()) return;
        Require(!session.OwnsState() && !runtime.Ready() && input.Active() == Gesture::None,
                "synchronous save began while zoom or fresh interception remained active");
        saveAttempted = true;
        trace.Add(succeeds ? "settings.save.completed" : "settings.save.failed");
        if (succeeds) { settings = parsed; saveCommitted = true; }
        else Require(settings == old, "save failure modified active configuration");
        Reconfigure();
        if (resume) Enable();
    }
    void Interrupt(Interruption interruption) {
        trace.Add(std::string("interrupt=") + InterruptionNames[static_cast<unsigned>(interruption)]);
        switch (interruption) {
        case Interruption::None: break;
        case Interruption::Reset: Reset(); break;
        case Interruption::Pause: Pause(); break;
        case Interruption::SettingsValid: Save(true, true); break;
        case Interruption::SettingsInvalid: Save(false, true); break;
        case Interruption::SaveFailure: Save(true, false); break;
        case Interruption::Focus: Emit(input.Cancel()); break;
        case Interruption::DesktopUnknown: SetDesktop(Desktop::Unknown); Suspend(); break;
        case Interruption::DesktopOther: SetDesktop(Desktop::Other); Suspend(); break;
        case Interruption::GuardianLoss: guardian = false; Sync(); FaultNow(); break;
        case Interruption::DisplayChange: DisplayChange(); break;
        case Interruption::UiStale: uiFresh = false; Sync(); break;
        }
        Check();
    }
    void Tick() {
        trace.Add("app.tick");
        if (runtime.Terminal()) return;
        if (!guardian || queueFault) { FaultNow(); return; }
        if (context != Desktop::Current) { Suspend(); return; }
        if (runtime.NeedsRestore() && !RestoreOwned()) return;
        if (runtime.Current() == Phase::Suspended) {
            if (refreshPending) Refresh();
            if (runtime.ReturnFromSuspend()) Enable();
        }
        if (!uiFresh) {
            Reset();
            uiFresh = true;
            Sync();
        }
        if (runtime.Ready()) {
            if (session.OwnsState()) {
                const auto result = session.Check(backend, journal);
                if (result != Result::Unchanged) SessionFailure(result);
            } else {
                Snapshot observed;
                if (!backend.Read(observed)) SessionFailure(Result::ReadFailure);
                else if (!glide::session::Valid(observed) || !glide::session::IsIdentity(observed))
                    SessionFailure(Result::Conflict);
            }
        }
        Drain();
        Frame();
        Check();
    }
    void Check() const {
        Require(input.Invariant(), "input state invariant failed in composition");
        Require(glide::settings::Valid(settings), "live settings are invalid");
        Require(glide::session::Valid(backend.state), "native model state is invalid");
        Require(std::isfinite(target) && target >= 1 && target <= settings.maxZoom,
                "target is outside configured finite range");
        Require(!runtime.NeedsRestore() || !runtime.Ready(), "pending restore accepted input");
        for (unsigned key = 0; key < owedKeys.size(); ++key) {
            Require(input.SwallowedKey(key) == owedKeys[key], "key release ledger diverged from delivered events");
            Require(input.Down(key) == physicalKeys[key], "key physical ledger diverged");
        }
        for (unsigned button = 0; button < owedButtons.size(); ++button) {
            Require(input.SwallowedButton(static_cast<Button>(button)) == owedButtons[button],
                    "button release ledger diverged from delivered events");
            Require(input.ButtonDown(static_cast<Button>(button)) == physicalButtons[button],
                    "button physical ledger diverged");
        }
        if (journal.armed) Require(glide::session::Valid(journal.record), "armed recovery record invalid");
        if (runtime.Terminal()) Require(!runtime.Accept(runtime.Epoch()), "terminal phase accepted command");
    }
    void ReleaseAll() {
        for (unsigned button = 0; button < physicalButtons.size(); ++button)
            if (physicalButtons[button]) ButtonEvent(static_cast<Button>(button), false);
        // Release reserved keys before modifiers to cover their actual UP paths.
        for (auto key : {glide::input::vk::Space, glide::input::vk::Escape})
            if (physicalKeys[key]) Key(key, false);
        for (unsigned key = 0; key < physicalKeys.size(); ++key)
            if (physicalKeys[key]) Key(key, false);
        for (bool owed : owedKeys) Require(!owed, "cleanup left an owed key release");
        for (bool owed : owedButtons) Require(!owed, "cleanup left an owed button release");
    }
    void Cleanup() {
        // Cleanup is a separate phase: faults are disarmed only after the row's
        // reached/unreached result and counters were captured by the runner.
        backend.Disarm();
        runtime.Stop();
        Sync();
        if (context != Desktop::Current) SetDesktop(Desktop::Current);
        Reset();
        ReleaseAll();
        Drain();
        Require(!session.OwnsState() && !journal.armed && !runtime.NeedsRestore(),
                "cleanup did not release recoverable owned state");
        if (backend.foreignActive)
            Require(backend.state == backend.lastForeign && backend.writesAfterForeign == 0,
                    "cleanup altered another magnifier's state");
        else Require(glide::session::IsIdentity(backend.state), "cleanup did not reach baseline");
        Check();
    }
};
} // namespace fusion
