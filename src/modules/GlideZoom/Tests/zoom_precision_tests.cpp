#include <stdexcept>
#include "../GlideZoom/zoom_animation.h"
#include "../GlideZoom/touchpad_state.h"
#include "../GlideZoom/monitor_presenter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
std::uint64_t assertions = 0, gestures = 0, animations = 0, monitorFrames = 0;
std::uint64_t fractionalPackets = 0, regressions = 0;
void Check(bool value, const char* message) {
    ++assertions;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
void Near(double value, double expected, double tolerance, const char* message) {
    Check(std::isfinite(value) && std::abs(value - expected) <= tolerance, message);
}
bool Success(glide::session::Result result) {
    return result == glide::session::Result::Applied || result == glide::session::Result::Unchanged;
}
glide::touchpad::Frame Contacts(unsigned count, double y = 0.5) {
    glide::touchpad::Frame frame;
    frame.device = 1; frame.count = count;
    for (unsigned i = 0; i < count; ++i)
        frame.contacts[i] = {10 + i, 0.4 + i * 0.05, y, true, true, true};
    return frame;
}
glide::touchpad::State Holding() {
    glide::touchpad::State state;
    state.SetEnabled(true);
    state.Push(Contacts(0), 1);
    state.Push(Contacts(3), 10);
    state.Push(Contacts(0), 50);
    Check(state.Push(Contacts(3), 100).begin && state.Active(), "actual second touchdown starts held gesture");
    return state;
}

struct Backend {
    glide::session::Snapshot state{};
    unsigned visualWrites = 0, inputWrites = 0;
    bool Read(glide::session::Snapshot& output) { output = state; return true; }
    bool SetVisual(glide::session::Visual value) { ++visualWrites; state.visual = value; return true; }
    bool SetInput(glide::session::Input value) { ++inputWrites; state.input = value; return true; }
};
struct Journal {
    glide::session::RecoveryRecord record{};
    bool armed = false;
    void Publish(const glide::session::RecoveryRecord& value) { record = value; armed = true; }
    void Clear() { armed = false; }
};
struct Renderer {
    Backend& backend;
    glide::monitor::Frame prepared{};
    bool visible = false;
    unsigned preparations = 0, shows = 0;
    bool Matches(const glide::monitor::Frame& next) const {
        return visible && glide::monitor::SameFrame(prepared, next);
    }
    bool Prepare(const glide::monitor::Frame& next) { ++preparations; prepared = next; return true; }
    bool Pending() const { return false; }
    bool CanRetain(const glide::session::Snapshot&) const { return false; }
    bool Show() {
        ++shows;
        Check(backend.state.visual.zoom == 1 && backend.state.input.enabled &&
              backend.state.input.source == prepared.source && backend.state.input.destination == prepared.destination,
              "fine visual scale is shown only with matching bounded input mapping");
        visible = true; return true;
    }
    void Hide() { visible = false; }
};

glide::monitor::Frame Frame(double zoom, glide::Rect bounds = {0, 0, 1920, 1080}) {
    const glide::Vec2 center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
    glide::monitor::Frame result;
    Check(glide::monitor::MakeFrame(bounds, glide::AnchorZoom({}, zoom, center, bounds), result),
          "fine requested scale forms a valid monitor frame");
    return result;
}

void GestureAndAnimationMatrix() {
    for (double start : {1.0, 1.05, 2.0, 8.0, 15.99})
        for (double change : {-0.01, -0.0001, 0.0, 0.0001, 0.01})
            for (double sensitivity : {140.0, 220.0, 360.0})
                for (unsigned fragments : {1u, 10u, 100u}) {
                    ++gestures;
                    auto state = Holding();
                    const double requested = start + change;
                    const double expected = std::clamp(requested, 1.0, 16.0);
                    // Independently invert the logarithmic transfer function to
                    // request this exact factor through actual contact motion.
                    const double pixels = -sensitivity * std::log1p(change / start) / std::log(2.0);
                    double accumulated = 0, target = start;
                    for (unsigned i = 1; i <= fragments; ++i) {
                        const double y = 0.5 + pixels * (static_cast<double>(i) / fragments) / 1000.0;
                        const auto effect = state.Push(Contacts(3, y), 100 + i);
                        Check(state.Active() && !effect.begin && !effect.end && !effect.cancel,
                              "tiny motion remains a held gesture without a new threshold");
                        if (effect.travel > 0 && std::abs(effect.delta) < 0.001) ++fractionalPackets;
                        accumulated += effect.delta;
                        target = glide::DragZoom(start, accumulated, sensitivity, 16);
                        Check(target >= std::min(start, expected) - 1e-10 &&
                              target <= std::max(start, expected) + 1e-10,
                              "every fractional packet stays within requested bounded factor interval");
                    }
                    Near(accumulated, pixels, 2e-10, "splitting contact movement does not truncate fractional motion");
                    Near(target, expected, 3e-11, "contact-derived target resolves thousandths and ten-thousandths");
                    const double selected = glide::animation::Quantize(target, 16);
                    const auto released = state.Push(Contacts(0), 101 + fragments);
                    Check(released.end && !released.cancel && !state.Active(), "release preserves the final precise target");
                    for (double rate : {30.0, 60.0, 144.0}) {
                        ++animations;
                        double current = start;
                        unsigned frames = 0;
                        while (current != selected && frames < 2) {
                            const double next = glide::animation::Quantize(target, 16);
                            Check(rate > 0, "step output does not depend on the service rate");
                            Check(std::isfinite(next) && next >= 1 && next <= 16, "animated scale remains finite and bounded");
                            Check(next >= std::min(current, selected) - 1e-14 && next <= std::max(current, selected) + 1e-14,
                                  "animation is monotonic and cannot overshoot a precise target");
                            current = next; ++frames;
                        }
                        Check(current == selected && frames <= 1, "selected0.01x output is applied in one frame without an intermediate transition");
                        for (const auto bounds : {glide::Rect{0,0,1920,1080}, glide::Rect{-1920,0,0,1080},
                                                  glide::Rect{300,-2160,4140,0}}) {
                            ++monitorFrames;
                            const auto frame = Frame(current, bounds);
                            Near(frame.transform.zoom, selected, 1e-6, "native float factor preserves the selected0.01x output");
                            Check(frame.source.left >= bounds.left && frame.source.top >= bounds.top &&
                                  frame.source.right <= bounds.right && frame.source.bottom <= bounds.bottom,
                                  "fine scale preserves single-monitor source containment");
                            if (glide::animation::Magnified(current)) {
                                Backend backend; Journal journal; Renderer renderer{backend}; glide::session::Session owner;
                                Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result),
                                      "production presenter accepts precise native frame");
                                Check(renderer.visible && owner.OwnsState() && journal.armed && backend.visualWrites == 0,
                                      "fine monitor zoom owns only bounded input plus private renderer");
                                renderer.Hide();
                                Check(Success(owner.Reset(backend, journal)) && !owner.NeedsRecovery() && !journal.armed,
                                      "fine session restores with no retained ownership");
                            }
                        }
                    }
                }
    Check(gestures == 225 && animations == 675 && monitorFrames == 2025, "all declared precision matrix tuples were executed");
    Check(fractionalPackets > 1000, "test actually exercised motion packets below 0.001 input units");
}

void PredicatesAndInvalidInputs() {
    using namespace glide::animation;
    const double firstFloat = static_cast<double>(std::nextafter(1.0f, 2.0f));
    const double subFloat = std::nextafter(1.0, 2.0);
    Check(!Magnified(1) && !Magnified(subFloat) && Magnified(firstFloat), "magnification identity follows native representability");
    for (double value : {1.0000001, 1.000001, 1.00001, 1.0001, 1.001}) {
        Check(Magnified(value), "old activation and toggle deadbands do not hide native magnification");
        Check(!RestoreIdentity(value, value), "visible precise zoom cannot be mistaken for identity restoration");
    }
    Check(RestoreIdentity(1, 1) && RestoreIdentity(subFloat, subFloat), "native-identity sub-ULP factors permit cleanup");
    Check(!RestoreIdentity(firstFloat, 1) && !RestoreIdentity(1, firstFloat), "one still-magnified side prevents early restoration");
    double previous = 1;
    for (unsigned i = 1; i <= 15000; ++i) {
        const double requested = 1.0 + i / 1000.0;
        const double native = static_cast<double>(static_cast<float>(requested));
        Check(NativeScaleChanged(requested, previous), "every adjacent thousandth from1x through16x is distinct natively");
        Check(!NativeScaleChanged(requested, native), "same native factor is not dirtied by double-to-float representation alone");
        Near(native, requested, 1e-6, "float conversion error is far below one thousandth throughout app range");
        previous = requested;
    }
    for (double value : {0.0, -1.0, 4096.1, std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                         -std::numeric_limits<double>::infinity()}) {
        Check(!ValidScale(value) && !Magnified(value), "invalid native scales are never cast as magnified values");
        Check(NativeScaleChanged(value, 1) && NativeScaleChanged(1, value), "invalid scale reaches caller validation rather than looking unchanged");
        Check(!RestoreIdentity(value, 1) && !RestoreIdentity(1, value), "invalid scale cannot authorize identity restoration");
    }
    Check(ValidScale(1) && ValidScale(4096), "native valid-scale boundaries remain inclusive");
    Check(Quantize(1.0001) == 1 && Quantize(8.001) == 8,
          "sub-step raw targets quantize without creating intermediate animated values");
    Check(Quantize(2.01) == 2.01, "one full output step is immediate and exact");
    Check(Quantize(std::numeric_limits<double>::quiet_NaN()) == 1,
          "invalid raw target normalizes to identity before quantization");
    Check(QuantizeHeld(1.005, 1) == 1 && QuantizeHeld(1.007, 1) == 1.01,
          "held midpoint hysteresis is bounded and switches immediately after crossing");
    ++regressions;
}

// Model only App's scheduling/dirty guards. The numeric policy, frame builder,
// presenter, Session and recovery journal below are actual production code;
// this is explicitly not execution of the Windows App/message loop.
struct Loop {
    bool monitor = true;
    double precise = 1, applied = 1, target = 1;
    Backend backend;
    Journal journal;
    Renderer renderer{backend};
    glide::session::Session owner;
    unsigned steps = 0, restores = 0;
    void Tick(double /*dt*/) {
        using namespace glide::animation;
        target = Quantize(target, 16);
        if (!Magnified(applied) && !Magnified(target) && !owner.NeedsRecovery() && !renderer.visible) return;
        ++steps;
        const double next = target; // No interpolation, including on a zero-time frame.
        precise = next; // Never replace this accumulator with the native float.
        if (!monitor && !NativeScaleChanged(next, applied) && !RestoreIdentity(target, next)) return;
        if (RestoreIdentity(target, next)) {
            renderer.Hide();
            Check(Success(owner.Reset(backend, journal)), "owed identity mapping restores through shared production session");
            precise = applied = target = 1; ++restores;
        } else if (Magnified(next)) {
            Check(Success(glide::monitor::Present(Frame(next), renderer, owner, backend, journal).result),
                  "modeled scheduling presents only native-magnified frames");
            applied = renderer.prepared.transform.zoom;
        }
    }
};

void RestorationAndNativeRefresh() {
    using namespace glide::animation;
    {
        ++regressions;
        Loop loop;
        loop.target = static_cast<double>(std::nextafter(1.0f, 2.0f));
        loop.Tick(0.001);
        Check(loop.precise == 1 && loop.target == 1, "raw input below a visible step leaves identity immediately");
        Check(!loop.owner.NeedsRecovery() && !loop.renderer.visible && loop.backend.inputWrites == 0,
              "initial native-identity frame never acquires a private overlay or input mapping");
        loop.target = 1; loop.Tick(0.001);
        Check(!loop.owner.NeedsRecovery() && !loop.renderer.visible && loop.backend.inputWrites == 0,
              "reversal before any native magnification leaves no recovery obligation");
    }
    for (bool monitor : {false, true}) for (bool subFloat : {false, true}) {
        ++regressions;
        Loop loop; loop.monitor = monitor;
        const glide::session::Rect rectangle{0,0,1920,1080};
        Check(Success(loop.owner.Apply({{}, {true, rectangle, rectangle}}, loop.backend, loop.journal)),
              "fixture represents an already-owned enabled identity input mapping");
        loop.renderer.visible = monitor;
        loop.target = subFloat ? std::nextafter(1.0, 2.0) : 1;
        loop.Tick(0.001);
        Check(loop.restores == 1 && !loop.owner.NeedsRecovery() && !loop.journal.armed && !loop.renderer.visible,
              "outer ownership and inner identity guards clean up even when all native zoom values equal1");
        Check(glide::session::IsIdentity(loop.backend.state), "owned identity mapping is disabled in both scopes");
    }
    {
        ++regressions;
        Loop loop; loop.target = 16;
        loop.Tick(std::numeric_limits<double>::infinity());
        Check(loop.owner.OwnsState() && loop.renderer.visible, "large zoom establishes recovery fixture");
        loop.target = 1;
        unsigned ticks = 0;
        while ((loop.owner.NeedsRecovery() || loop.renderer.visible) && ticks < 2000) { loop.Tick(0.001); ++ticks; }
        Check(ticks == 1 && loop.precise == 1 && !loop.owner.NeedsRecovery() && !loop.renderer.visible,
              "16x to1x releases owned state in the first serviced frame");
    }
    {
        ++regressions;
        Loop loop; loop.target = 1.01;
        for (unsigned i = 0; i < 2000 && loop.precise != loop.target; ++i) loop.Tick(0.001);
        Check(loop.precise == loop.target && loop.applied > 1,
              "a selected0.01x step is applied without any intermediate output");
    }
    {
        ++regressions;
        Backend backend; Journal journal; Renderer renderer{backend}; glide::session::Session owner;
        const auto first = Frame(8.01), second = Frame(8.011);
        Check(first.source == second.source && first.destination == second.destination,
              "fixture has identical integer mapping for distinct fine native scales");
        Check(Success(glide::monitor::Present(first, renderer, owner, backend, journal).result), "first mapped frame presents");
        const auto writes = backend.inputWrites;
        Check(glide::monitor::Present(second, renderer, owner, backend, journal).result == glide::session::Result::Unchanged,
              "shared input mapping may remain unchanged at a new fine scale");
        Check(renderer.preparations == 2 && renderer.shows == 2 && backend.inputWrites == writes &&
              NativeScaleChanged(renderer.prepared.transform.zoom, first.transform.zoom),
              "window renderer still updates scale when integer input rectangles do not change");
        renderer.Hide(); Check(Success(owner.Reset(backend, journal)), "fine unchanged-mapping fixture cleans up");
    }
    {
        ++regressions;
        auto touchpad = Holding();
        double cumulative = 0;
        Loop loop; loop.target = 2; loop.Tick(std::numeric_limits<double>::infinity());
        const auto down = touchpad.Push(Contacts(3, 0.72), 110);
        cumulative += down.delta;
        loop.target = glide::DragZoom(2, cumulative, 220, 16);
        while ((loop.owner.NeedsRecovery() || loop.renderer.visible) && loop.steps < 3000) loop.Tick(0.001);
        Check(touchpad.Active() && loop.precise == 1 && !loop.owner.NeedsRecovery(),
              "restoring1x does not cancel the still-held contact recognizer");
        const double up = 220 * std::log1p(0.01) / std::log(2.0) / 1000.0;
        cumulative += touchpad.Push(Contacts(3, 0.72 - up), 120).delta;
        loop.target = glide::DragZoom(2, cumulative, 220, 16);
        Near(loop.target, 1.01, 1e-12, "same hold reverses from1x to1.01 without a new tap");
        loop.Tick(std::numeric_limits<double>::infinity());
        Check(touchpad.Active() && loop.owner.OwnsState() && loop.renderer.visible, "same held gesture can reacquire its fine view");
    }
}

void DirectionTimingAndDrift() {
    for (double sensitivity : {140.0,220.0,360.0}) {
        ++regressions;
        auto state = Holding();
        const double distance = -sensitivity * std::log1p(0.001 / 2) / std::log(2.0) / 1000;
        const auto effect = state.Push(Contacts(3, 0.5 + distance), 110);
        Near(glide::DragZoom(2, effect.delta, sensitivity, 16), 2.001, 1e-12, "upward micro-movement has normal direction");
        Near(glide::DragZoom(2, -effect.delta, sensitivity, 16), 4.0 / 2.001, 1e-12,
             "reverse preference changes direction without quantizing delta");
    }
    {
        ++regressions;
        double reference = 0;
        for (unsigned rate : {30u,60u,144u}) {
                    double current = 2;
            for (unsigned tick = 0; tick < rate / 2; ++tick) current = glide::animation::Quantize(2.01);
            if (!reference) reference = current;
            Near(current, reference, 2e-12, "same half-second interval yields the same exact target across rates");
        }
    }
    {
        ++regressions;
        auto state = Holding();
        const double distance = -220 * std::log1p(0.001 / 2) / std::log(2.0) / 1000;
        double cumulative = 0;
        for (unsigned cycle = 0; cycle < 5000; ++cycle) {
            cumulative += state.Push(Contacts(3, 0.5 + distance), 101 + cycle * 2).delta;
            Near(glide::DragZoom(2, cumulative, 220, 16), 2.001, 1e-12, "long repeated outward fine movement retains target");
            cumulative += state.Push(Contacts(3), 102 + cycle * 2).delta;
            Near(cumulative, 0, 1e-12, "opposite fractional displacement does not accrue long-term drift");
            Near(glide::DragZoom(2, cumulative, 220, 16), 2, 1e-12, "returning fingers to origin returns precise starting factor");
            Check(state.Active(), "drift test uses one real held gesture without accidental cancellation");
        }
    }
}
}

int Run_zoom_precision() {
    GestureAndAnimationMatrix(); PredicatesAndInvalidInputs(); RestorationAndNativeRefresh(); DirectionTimingAndDrift();
    std::cout << "PASS: " << assertions << " zoom precision assertions; " << gestures << " actual contact gesture paths; "
              << animations << " immediate-step matrix cases; " << monitorFrames << " monitor frames; " << fractionalPackets
              << " motion packets below 0.001 input units; " << regressions << " targeted regressions; 5000 drift cycles\n";
    return 0;
}
