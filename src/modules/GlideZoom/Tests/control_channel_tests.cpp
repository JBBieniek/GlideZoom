#include <stdexcept>
#include "../GlideZoom/control_channel.h"

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

namespace {
using namespace glide::input;
std::uint64_t assertions = 0;

void Check(bool ok, const char* message) {
    ++assertions;
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}

constexpr std::uint64_t EnabledOptions = DefaultControlOptions | 7;

// Fault injection at the real reader boundary: false is exactly the result
// returned by SnapshotChannel for a writer overlap. All subsequent policy and
// input decisions use the production ControlReader and State.
struct ReadAttempt {
    const ControlChannel& channel;
    bool overlap = false;
    bool Read(Snapshot& value) const noexcept { return !overlap && channel.Read(value); }
};

struct Effect {
    Decision decision;
    std::uint64_t epoch;
};

struct Harness {
    ControlChannel channel;
    ControlReader reader;
    State state;
    bool gate = true;
    bool faultOnEnd = false;
    std::vector<Effect> effects;

    Harness() {
        channel.Publish({EnabledOptions, 7, 2});
        Sync();
        effects.clear();
    }
    bool Sync(bool overlap = false) {
        return reader.Synchronize(ReadAttempt{channel, overlap}, state, [this] { return gate; },
            [this](Decision decision) {
                if (decision.end || decision.begin || decision.reset) {
                    effects.push_back({decision, reader.Epoch()});
                }
                if (decision.end && faultOnEnd) {
                    // Host::Queue may latch a fault during a cancellation.
                    gate = false;
                    state.SetEnabled(false);
                }
            });
    }
};

void Chord(State& state, std::uint64_t now = 1) {
    Check(!state.Key(vk::LeftControl, true, now).consume, "Ctrl forwards");
    Check(!state.Key(vk::LeftAlt, true, now + 1).consume, "Alt forwards");
}

void Start(State& state, Gesture gesture) {
    Decision result;
    if (gesture == Gesture::Control) {
        state.Key(vk::LeftControl, true, 1);
        state.Key(vk::LeftControl, false, 20);
        result = state.Key(vk::LeftControl, true, 50);
    } else if (gesture == Gesture::Space) {
        Chord(state);
        result = state.Key(vk::Space, true, 50);
    } else {
        Chord(state);
        state.ButtonEvent(Button::Left, true, {}, 3);
        state.ButtonEvent(Button::Left, false, {}, 20);
        result = state.ButtonEvent(Button::Left, true, {}, 50);
    }
    Check(result.begin && state.Active() == gesture, "real input state starts requested hold");
    Check(state.Invariant(), "start invariant");
}

Decision Release(State& state, Gesture gesture, std::uint64_t now = 100) {
    if (gesture == Gesture::Control) return state.Key(vk::LeftControl, false, now);
    if (gesture == Gesture::Space) return state.Key(vk::Space, false, now);
    return state.ButtonEvent(Button::Left, false, {}, now);
}

void OverlapScenarios() {
    for (auto gesture : {Gesture::Control, Gesture::Space, Gesture::Mouse}) {
        Harness h;
        Start(h.state, gesture);
        Check(!h.Sync(true), "control writer overlap observed");
        Check(h.state.Active() == gesture && h.effects.empty(),
              "in-progress publication does not end a committed hold");
        Check(h.state.Move({3, 4}, 60).consume, "existing hold retains motion during overlap");
        Check(h.state.Invariant(), "overlap retains state invariant");
        const auto release = Release(h.state, gesture);
        Check(release.end && !release.cancel, "release finishes hold normally during overlap");
        Check(release.consume == (gesture != Gesture::Control), "reserved release pairing preserved");
        Check(!Release(h.state, gesture, 101).consume, "reserved release consumed once");
        Check(h.state.Invariant(), "release invariant");
    }
    {
        Harness h;
        Chord(h.state);
        h.Sync(true);
        Check(!h.state.Key(vk::Space, true, 10).consume, "fresh Space passes during overlap");
        h.Sync();
        Check(!h.state.Key(vk::Space, true, 11).begin, "forwarded Space repeat cannot become a gesture");
        Check(!h.state.Key(vk::Space, false, 12).consume, "forwarded Space up remains forwarded");
        Check(h.state.Key(vk::Space, true, 13).begin, "fresh press works after coherent publication");
    }
    {
        Harness h;
        h.Sync(true);
        h.state.Key(vk::LeftControl, true, 1);
        h.state.Key(vk::LeftControl, false, 10);
        Check(!h.state.Key(vk::LeftControl, true, 30).begin, "double Ctrl cannot start during overlap");
        h.state.Key(vk::LeftControl, false, 40);
        h.Sync();
        Check(!h.state.Key(vk::LeftControl, true, 50).begin,
              "taps during an uncertain publication do not prime a later gesture");
    }
    {
        Harness h;
        Chord(h.state);
        h.Sync(true);
        Check(!h.state.ButtonEvent(Button::Left, true, {}, 10).consume,
              "fresh modified click passes during overlap");
        h.Sync();
        Check(!h.state.ButtonEvent(Button::Left, false, {}, 20).consume,
              "click forwarded during overlap retains its forwarded release");
        Check(!h.state.ButtonEvent(Button::Left, true, {}, 30).begin,
              "uncertain first click cannot prime a double click hold");
    }
    {
        Harness h;
        Start(h.state, Gesture::Control);
        h.Sync(true);
        const auto wheel = h.state.Wheel(true, 60);
        Check(wheel.end && wheel.cancel && !wheel.consume,
              "new wheel action passes during overlap and cancels the preceding hold");
    }
    {
        Harness h;
        Chord(h.state);
        h.Sync(true);
        const auto reset = h.state.Key(vk::Escape, true, 10);
        Check(reset.consume && reset.reset, "emergency reset remains available with committed authority");
        Check(h.state.Key(vk::Escape, false, 11).consume, "emergency reset up stays paired");
    }
    {
        ControlChannel channel;
        ControlReader reader;
        State state;
        channel.Publish({EnabledOptions, 7, 2});
        reader.Synchronize(ReadAttempt{channel, true}, state, [] { return true; }, [](Decision) {});
        Chord(state);
        Check(!state.Key(vk::Space, true, 3).consume && !state.Wheel(true, 4).consume,
              "first unreadable snapshot grants no input authority");
    }
}

void SafetyComposition() {
    for (auto gesture : {Gesture::Control, Gesture::Space, Gesture::Mouse}) {
        Harness h;
        Start(h.state, gesture);
        h.gate = false;
        h.Sync(true);
        Check(h.state.Active() == Gesture::None && h.effects.size() == 1 &&
              h.effects.front().decision.cancel, "independent safety closure cancels despite overlap");
        Check(!h.state.Move({}, 60).consume, "closed safety gate releases motion immediately");
        Check(Release(h.state, gesture).consume == (gesture != Gesture::Control),
              "safety closure retains reserved physical release");
        Check(!Release(h.state, gesture, 101).consume, "safety closure consumes release once");
        Check(h.state.Invariant(), "safety closure invariant");
    }
    {
        Harness h;
        Start(h.state, Gesture::Space);
        h.channel.Publish({EnabledOptions, 8, 3});
        h.Sync(true);
        Check(h.state.Active() == Gesture::Space && h.reader.Epoch() == 7,
              "uncommitted epoch does not enter the input state");
        h.Sync();
        Check(h.effects.size() == 1 && h.effects[0].decision.cancel && h.effects[0].epoch == 7,
              "committed new epoch emits exactly one cancellation in the old epoch");
        Check(h.reader.Epoch() == 8 && h.state.Active() == Gesture::None,
              "new epoch cancels the old hold before admitting commands");
        Check(h.state.Key(vk::Space, false, 100).consume, "epoch invalidation keeps release ledger");
        Check(h.state.Key(vk::Space, true, 101).begin, "fresh press can start in the new epoch");
    }
    {
        Harness h;
        Start(h.state, Gesture::Space);
        h.channel.Publish({EnabledOptions & ~std::uint64_t{1}, 7, 2});
        h.Sync();
        Check(h.state.Active() == Gesture::None && h.effects.size() == 1,
              "committed disabled control cancels even without an epoch change");
        h.state.Key(vk::Space, false, 100);
        Check(!h.state.Key(vk::Space, true, 101).consume, "disabled control blocks new interception");
    }
    {
        Harness h;
        Start(h.state, Gesture::Space);
        h.channel.Publish({EnabledOptions ^ 2, 7, 2});
        h.Sync();
        Check(h.state.Active() == Gesture::None && h.effects.size() == 1,
              "committed recognition change deliberately cancels the old gesture");
    }
    {
        Harness h;
        Start(h.state, Gesture::Space);
        h.faultOnEnd = true;
        h.channel.Publish({EnabledOptions, 8, 2});
        h.Sync();
        Check(!h.gate && h.state.Active() == Gesture::None,
              "queue fault during epoch cancellation closes the gate in the same synchronization");
        h.state.Key(vk::Space, false, 100);
        Check(!h.state.Key(vk::Space, true, 101).consume,
              "fault raised by cancellation cannot be re-enabled by stale gate evaluation");
    }
}

void TelemetryConcurrency() {
    Harness h;
    Start(h.state, Gesture::Space);
    Check(!h.channel.Publish({EnabledOptions, 7, 4}) && h.channel.Zoom() == 4,
          "zoom-only frame bypasses the control transaction");
    Check(!h.channel.Publish({EnabledOptions, 7, std::numeric_limits<double>::quiet_NaN()}) &&
          h.channel.Zoom() == 1, "invalid telemetry cannot poison motion scaling or control");
    h.channel.Publish({EnabledOptions, 7, 1000});
    Check(h.channel.Zoom() == 64, "telemetry upper bound");
    h.channel.Publish({EnabledOptions, 7, -1000});
    Check(h.channel.Zoom() == 1, "telemetry lower bound");

    constexpr unsigned frames = 200000;
    std::atomic<bool> done{false}, unexpectedControlWrite{false};
    std::thread writer([&] {
        for (unsigned i = 0; i < frames; ++i) {
            if (h.channel.Publish({EnabledOptions, 7, 1.0 + (i % 6300) / 100.0})) {
                unexpectedControlWrite.store(true);
            }
        }
        done.store(true);
    });
    std::uint64_t reads = 0;
    do {
        Check(h.Sync(), "animation-only writer never invalidates a coherent control read");
        Check(h.state.Active() == Gesture::Space && h.effects.empty(),
              "concurrent animation telemetry cannot cancel an active hold");
        const double zoom = h.channel.Zoom();
        Check(std::isfinite(zoom) && zoom >= 1 && zoom <= 64, "concurrent telemetry stays bounded");
        ++reads;
    } while (!done.load());
    writer.join();
    Check(!unexpectedControlWrite.load(), "all animation frames avoided control publication");
    Check(h.state.Invariant(), "concurrent telemetry leaves input state consistent");
    std::cout << "Control composition: " << frames << " animation frames, " << reads
              << " concurrent reads, " << assertions << " assertions.\n";
}
} // namespace

int Run_control_channel() {
    OverlapScenarios();
    SafetyComposition();
    TelemetryConcurrency();
    return 0;
}
