#include <stdexcept>
#include "../GlideZoom/frame_timing.h"
#include "../GlideZoom/zoom_animation.h"
#include "../GlideZoom/viewport_motion.h"
#include "../GlideZoom/touchpad_state.h"
#include "../GlideZoom/input_state.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using glide::animation::StepZoom;
using glide::animation::Quantize;
using glide::animation::QuantizeHeld;
using glide::timing::Cadence;
std::uint64_t assertions = 0, outputs = 0, directReports = 0, schedules = 0, mixedSteps = 0;
std::uint64_t multiplierCases = 0, heldSteps = 0;

void Check(bool value, const char* message) {
    ++assertions;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
void Near(double actual, double expected, double tolerance, const char* message) {
    Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
void ValidOutput(double value, double maximum = 16) {
    Check(glide::animation::ValidScale(value) && value <= maximum, "selected output remains within native and configured bounds");
    if (value != maximum)
        Near(value * 100, std::round(value * 100), 1e-8, "every selected output is on the requested0.01x grid");
}

void ImmediateOutputMatrix() {
    for (double maximum : {1.0, 1.004, 1.005, 2.0, 8.0, 16.0, 4096.0})
    for (double start : {1.0, 1.001, 1.01, 1.05, 2.0, 4.0, 16.0, 4096.0})
    for (double amount : {-1200.0, -120.0, -60.0, -1.0, -0.125, 0.0, 0.125, 1.0, 60.0, 120.0, 1200.0}) {
        ++outputs;
        const double raw = StepZoom(start, amount, maximum);
        const double expectedRaw = std::clamp(std::clamp(start, 1.0, maximum) + amount / 120 * 0.01, 1.0, maximum);
        Near(raw, expectedRaw, 1e-12, "key/wheel raw change is additive0.01 per120 units at every scale");
        const double selected = Quantize(raw, maximum);
        ValidOutput(selected, maximum);
        Check(Quantize(selected, maximum) == selected, "output quantization is idempotent and never queues intermediate values");
        for (unsigned idle = 0; idle < 5; ++idle)
            Check(Quantize(raw, maximum) == selected, "unchanged input cannot animate or settle over later frames");
    }
    Check(Quantize(1.005) == 1.01 && Quantize(1.015) == 1.02,
          "decimal half-step ties survive binary floating-point representation");
    Check(Quantize(1.005 - 2e-9) == 1 && Quantize(1.005 + 2e-9) == 1.01,
          "only negligible numerical slack is used at a midpoint");
}

void PartialWheelAndExactSteps() {
    for (double start : {1.0, 1.01, 2.0, 8.0, 15.99})
    for (double direction : {-1.0, 1.0})
    for (unsigned fragments : {1u, 2u, 10u, 120u, 1000u}) {
        double raw = start;
        for (unsigned i = 0; i < fragments; ++i)
            raw = StepZoom(raw, direction * 120 / fragments, 16);
        const double expected = std::clamp(start + direction * 0.01, 1.0, 16.0);
        Near(raw, expected, 3e-11, "splitting a notch into partial wheel reports preserves its accumulated change");
        Check(Quantize(raw, 16) == Quantize(expected, 16), "one whole notch changes exactly one output step independent of packet count");
    }
    double raw = 1;
    for (unsigned i = 0; i < 10; ++i) raw = StepZoom(raw, 1, 16);
    Check(raw > 1 && Quantize(raw, 16) == 1, "sub-step wheel remainder is kept even before visible output changes");
    for (unsigned i = 10; i < 120; ++i) raw = StepZoom(raw, 1, 16);
    Check(Quantize(raw, 16) == 1.01, "retained small wheel reports eventually produce the requested full step");
    for (unsigned i = 0; i < 120; ++i) raw = StepZoom(raw, -1, 16);
    Check(Quantize(raw, 16) == 1, "reversed partial reports return to the original output without drift");
    Check(StepZoom(StepZoom(16, 120000, 16), -120, 16) == 15.99,
          "upper clamp does not accumulate hidden overshoot that delays reversal");
    Check(StepZoom(StepZoom(1, -120000, 16), 120, 16) == 1.01,
          "lower clamp does not accumulate hidden overshoot that delays reversal");
}

void MultiplierAndHoldMatrix() {
    for (int multiplier = 1; multiplier <= 100; ++multiplier)
    for (double start : {1.0, 1.01, 2.0, 8.0, 15.99, 16.0})
    for (double direction : {-1.0, 1.0})
    for (unsigned fragments : {1u, 2u, 120u}) {
        ++multiplierCases;
        double raw = start;
        for (unsigned packet = 0; packet < fragments; ++packet)
            raw = StepZoom(raw, direction * 120 / fragments, 16, multiplier);
        const double expected = std::clamp(start + direction * multiplier / 100.0, 1.0, 16.0);
        Near(raw, expected, 1e-10, "all integer multipliers preserve partial-wheel accumulation and endpoints");
        Check(Quantize(raw, 16) == Quantize(expected, 16),
              "multiplier changes step size while preserving hundredth-grid output");
    }
    for (int multiplier : {-100, 0, 101, std::numeric_limits<int>::max()})
        Check(StepZoom(2.004, 120, 16, multiplier) == 2.004,
              "invalid multiplier cannot change an otherwise valid raw target");
    for (int multiplier : {1, 2, 5, 10, 20, 50, 100})
    for (bool reverse : {false, true}) {
        glide::input::State state;
        state.SetEnabled(true);
        state.Key(glide::input::vk::LeftWin, true, 10);
        state.Key(glide::input::vk::LeftAlt, true, 20);
        const auto trigger = reverse ? glide::input::vk::Minus : glide::input::vk::Equal;
        double selected = reverse ? 16 : 1;
        unsigned actions = 0;
        const auto apply = [&](glide::input::Decision event) {
            if (!event.step) return;
            ++actions; ++heldSteps;
            selected = Quantize(StepZoom(selected, event.step * 120.0, 16, multiplier), 16);
            ValidOutput(selected);
        };
        apply(state.Key(trigger, true, 100));
        for (std::uint64_t now = 101; now < 1100; ++now) {
            if (now % 17 == 0) apply(state.Key(trigger, true, now)); // Typematic never adds an action.
            apply(state.Expire(now));
        }
        Check(actions == 24, "one held second emits one initial and 23 elapsed repeats regardless of OS repeat storm");
        const double expected = std::clamp((reverse ? 16.0 : 1.0) +
            (reverse ? -1.0 : 1.0) * actions * multiplier / 100.0, 1.0, 16.0);
        Near(selected, Quantize(expected, 16), 1e-10, "real held input and multiplier compose to the expected bounded zoom");
        apply(state.Key(trigger, false, 1100));
        for (std::uint64_t now = 1101; now < 1200; ++now) apply(state.Expire(now));
        Check(actions == 24 && state.Invariant(), "release ends multiplier-scaled repeated zoom without a tail");
    }
}

void HeldMidpointHysteresis() {
    for (double lower : {1.0, 1.05, 2.0, 8.0, 15.99})
    for (bool fromAbove : {false, true}) {
        const double initial = Quantize(lower + (fromAbove ? 0.01 : 0));
        double selected = initial;
        for (unsigned i = 0; i < 10000; ++i) {
            const double raw = lower + 0.005 + (i & 1 ? 0.0001 : -0.0001);
            selected = QuantizeHeld(raw, selected, 16);
            Check(selected == initial, "tiny held-finger jitter at a midpoint cannot alternate output scales");
        }
        selected = QuantizeHeld(lower + 0.00600001, selected, 16);
        Check(selected == Quantize(lower + 0.01), "deliberate upward crossing changes the step immediately");
        selected = QuantizeHeld(lower + 0.00399999, selected, 16);
        Check(selected == Quantize(lower), "deliberate reversal crosses the lower boundary immediately");
    }
    Check(QuantizeHeld(1, 1.01, 16) == 1 && QuantizeHeld(16, 15.99, 16) == 16,
          "held hysteresis never delays the exact identity or configured maximum endpoints");
    Check(QuantizeHeld(1.004, 1, 1.004) == 1.004 && Quantize(1.004, 1.004) == 1.004,
          "a non-grid configured maximum remains reachable");
    Check(QuantizeHeld(1.005, 4, 16) == 1.01, "large held movement skips obsolete steps and selects the current output");
    // EndDrag reseeds the next input source from the selected step. Otherwise
    // an old held hysteresis remainder could make one key jump two grid cells.
    const double selected = QuantizeHeld(1.006, 1, 16);
    Check(Quantize(StepZoom(selected, 120, 16), 16) == selected + 0.01,
          "a fresh discrete source steps once from a finished held gesture");
}

glide::touchpad::Frame Contacts(unsigned count, double y = 0.5) {
    glide::touchpad::Frame frame;
    frame.device = 1; frame.count = count;
    for (unsigned i = 0; i < count; ++i)
        frame.contacts[i] = {10 + i, 0.4 + i * 0.05, y, true, true, true};
    return frame;
}

void ActualContactSteps() {
    for (double start : {1.0, 1.05, 2.0, 8.0})
    for (double delta : {-0.01, -0.001, 0.001, 0.01, 0.05})
    for (unsigned fragments : {1u, 10u, 100u}) {
        glide::touchpad::State contacts;
        contacts.SetEnabled(true);
        contacts.Push(Contacts(0), 1); contacts.Push(Contacts(3), 10); contacts.Push(Contacts(0), 50);
        Check(contacts.Push(Contacts(3), 100).begin, "actual tap then second touchdown begins held contact gesture");
        double selected = start, pixels = 0;
        const double motion = -220 * std::log1p(delta / start) / std::log(2.0);
        for (unsigned i = 1; i <= fragments; ++i) {
            const auto report = contacts.Push(Contacts(3, 0.5 + motion * i / fragments / 1000), 100 + i);
            pixels += report.delta;
            const double raw = glide::DragZoom(start, pixels, 220, 16);
            selected = QuantizeHeld(raw, selected, 16);
            ValidOutput(selected);
            ++directReports;
        }
        const double expectedRaw = std::clamp(start + delta, 1.0, 16.0);
        Check(selected == Quantize(expectedRaw, 16), "real fragmented contact movement reaches its selected output without an easing tail");
        const auto ended = contacts.Push(Contacts(0), 101 + fragments);
        Check(ended.end && !ended.cancel && Quantize(selected, 16) == selected,
              "release leaves the last selected factor unchanged, even when batched with its final delta");
    }
}

void InvalidAndBoundaryInputs() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    for (double invalid : {0.0, -1.0, 4096.1, std::numeric_limits<double>::max(), nan, infinity, -infinity}) {
        Check(Quantize(invalid, 16) == 1, "invalid target cannot reach native output");
        Check(QuantizeHeld(invalid, 4, 16) == 1, "invalid held target returns safely to identity");
        Check(Quantize(2, invalid) == 1 && StepZoom(2, 120, invalid) == 1,
              "invalid maximum cannot expand the native range");
        Check(StepZoom(invalid, 120, 16) == 1.01, "invalid raw current normalizes before applying a valid discrete input");
    }
    Check(StepZoom(2.004, nan, 16) == 2.004, "invalid wheel amount preserves a valid raw remainder");
    Check(StepZoom(2, infinity, 16) == 16 && StepZoom(2, -infinity, 16) == 1,
          "unbounded wheel amount saturates without an overflow");
    Check(Quantize(4096) == 4096 && QuantizeHeld(4096, 1) == 4096, "native upper endpoint remains representable");
    Check(QuantizeHeld(2, nan, 16) == 2, "invalid prior held target cannot retain corrupt interpolation state");
}

void ImmediateAnchorAndFollow() {
    for (glide::Rect bounds : {glide::Rect{0,0,1920,1080}, glide::Rect{-1920,0,0,1080},
                               glide::Rect{-1920,-2160,3840,1440}}) {
        const glide::Vec2 cursor{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        glide::viewport::Anchor anchor;
        glide::viewport::Follow follow;
        glide::Transform precise{};
        Check(anchor.Set(precise, cursor, bounds), "immediate zoom starts with a stable precise anchor");
        Check(anchor.At(Quantize(4), bounds, precise), "new step reaches its final transform in one service frame");
        const auto projected = glide::SourceToScreen(cursor, precise);
        Near(projected.x, cursor.x, 1e-8, "immediate output preserves stationary source anchor x");
        Near(projected.y, cursor.y, 1e-8, "immediate output preserves stationary source anchor y");
        glide::Transform next{};
        Check(follow.Advance(precise, cursor, bounds, true, glide::viewport::FollowMode::Centered, cursor, next),
              "follow is eligible immediately because there is no pending animation");
        Check(next.offset.x == precise.offset.x && next.offset.y == precise.offset.y,
              "one immediate step cannot invent a pointer displacement");
        const glide::Vec2 moved{cursor.x + 2, cursor.y + 1};
        Check(follow.Advance(precise, moved, bounds, true, glide::viewport::FollowMode::Centered, cursor, next),
              "first movement after a zoom step is accepted");
        Near(next.offset.x - precise.offset.x, 2, 1e-8, "post-step x follow responds without delayed settling");
        Near(next.offset.y - precise.offset.y, 1, 1e-8, "post-step y follow responds without delayed settling");
    }
}

void CadenceMatrix() {
    for (std::uint64_t frequency : {1ull, 1000ull, 10000000ull, 3579545ull, 1000000000ull})
    for (double rate : {1.0, 30.0, 59.94, 60.0, 120.0, 144.0, 165.0, 240.0, 360.0})
    for (std::uint64_t start : {0ull, 100ull, 1000000000000ull}) {
        ++schedules;
        Cadence cadence;
        Check(cadence.Configure(frequency, rate), "valid QPC frequency configures frame timing");
        Check(!cadence.Valid() && cadence.Delay100ns(start) == 0, "configuration alone cannot arm an unseeded deadline");
        Check(cadence.Reset(start) && cadence.Due(start), "reset allows a current first frame immediately");
        Check(cadence.Consume(start) && !cadence.Due(start), "one consumed frame cannot be replayed at the same timestamp");
        const auto interval = cadence.IntervalTicks();
        Check(interval >= 1 && interval <= frequency, "cadence interval stays in the representable one-second range");
        Check(!cadence.Due(start + interval - 1) && cadence.Due(start + interval),
              "frame deadline has precise inclusive boundary semantics");
        for (std::uint64_t lateSlots : {0ull, 1ull, 10ull, 1000ull}) {
            const auto now = start + (lateSlots + 2) * interval;
            Check(cadence.Due(now) && cadence.Consume(now), "late wake consumes one current frame");
            Check(!cadence.Due(now), "late wake never leaves historical frames pending");
            Check(cadence.Delay100ns(now) >= 1 && cadence.Delay100ns(now) <= 10000000,
                  "rearmed wait is positive and bounded without signed overflow");
            Check(cadence.Due(now + interval), "skipping missed slots cannot strand the next frame");
        }
        const auto rollback = start;
        Check(cadence.Due(rollback) && cadence.Consume(rollback) && !cadence.Due(rollback),
              "counter rollback rebases once without hanging or replaying frames");
        Check(cadence.Due(rollback + interval), "rollback retains a usable future cadence");
    }

    Cadence cadence;
    Check(!cadence.Configure(0, 60) && !cadence.Reset(0) && !cadence.Consume(0),
          "zero clock frequency cannot create a division or busy wait");
    Check(!cadence.Configure(std::numeric_limits<std::uint64_t>::max(), 60),
          "unsigned values outside QPC's signed range are rejected");
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()}) {
        Check(cadence.Configure(10000000, invalid) && cadence.Rate() == 60,
              "unknown display frequency falls back to a bounded 60 Hz cadence");
    }
    Check(cadence.Configure(10000000, 0.01) && cadence.Rate() == 1, "tiny reported refresh rate cannot request an indefinite wait");
    Check(cadence.Configure(10000000, 1000000) && cadence.Rate() == 240, "huge reported refresh rate cannot request a spin loop");
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    Check(cadence.Reset(maximum) && !cadence.Consume(maximum) && !cadence.Valid() && cadence.Delay100ns(maximum) == 0,
          "unrepresentable next deadline fails explicitly instead of wrapping into a render loop");

    Check(cadence.Configure(1000, 30) && cadence.IntervalTicks() == 34 && cadence.Reset(10) && cadence.Consume(10),
          "nonintegral clock cadence rounds its period upward instead of overdriving a display");
    Check(cadence.Consume(20) && !cadence.Due(43) && cadence.Due(44),
          "an early fallback wake does not consume or postpone the real deadline");
    Check(cadence.Consume(59) && !cadence.Due(77) && cadence.Due(78),
          "a late native wake preserves phase and skips only expired slots");
    Check(cadence.Delay100ns(59) == 190000, "relative due time converts remaining ticks without restarting a full period");
    Check(cadence.Configure(10000000, 144) && cadence.Reset(0) && cadence.Consume(0) &&
          cadence.Delay100ns(69400) == 45, "relative timer retains a 4.5-microsecond remainder instead of truncating it to zero");
    Check(cadence.Configure(1000, 240) && !cadence.Valid() && cadence.Reset(60) && cadence.Consume(60) &&
          !cadence.Due(64) && cadence.Due(65), "monitor refresh change replaces the previous cadence and deadline");
    const auto largeFrequency = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    Check(cadence.Configure(largeFrequency, 1) && cadence.IntervalTicks() <= largeFrequency && cadence.Reset(0) &&
          cadence.Consume(0) && cadence.Delay100ns(0) == 10000000,
          "largest signed QPC frequency converts without integer multiplication overflow");

    // Simulate a hot message queue: arbitrarily frequent wake-ups inspect Due,
    // but only display deadlines may produce frames. This is timing-policy
    // testing, not execution of MsgWaitForMultipleObjectsEx or WM_TIMER.
    Check(cadence.Configure(1000000, 144) && cadence.Reset(0), "busy-queue cadence is configured");
    unsigned rendered = 0;
    for (std::uint64_t now = 0; now < 1000000; ++now) {
        if (cadence.Due(now)) {
            ++rendered;
            Check(cadence.Consume(now) && !cadence.Due(now), "message flood cannot duplicate one cadence frame");
        }
    }
    Check(rendered == 144, "one million message wakes still render only the configured 144 frames");
}

void MixedStateSequences() {
    std::uint64_t random = 0x87e1a94d2f607bc3ull;
    auto Next = [&]() { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; };
    double raw = 1, selected = 1;
    for (unsigned step = 0; step < 50000; ++step) {
        ++mixedSteps;
        switch (Next() % 8) {
        case 0: raw = StepZoom(raw, (Next() & 1) ? 120 : -120, 16); selected = Quantize(raw, 16); break;
        case 1: raw = StepZoom(raw, (Next() & 1) ? 1 : -1, 16); selected = Quantize(raw, 16); break;
        case 2: raw = std::clamp(raw + ((Next() & 1) ? 0.0001 : -0.0001), 1.0, 16.0);
                selected = QuantizeHeld(raw, selected, 16); break;
        case 3: raw = selected = 1; break;
        case 4: raw = selected = Quantize(1 + static_cast<double>(Next() % 150000) / 10000, 16); break;
        case 5: raw = selected; break; // release/capture/reset to a committed view
        case 6: raw = (Next() & 1) ? 1 : 16; selected = QuantizeHeld(raw, selected, 16); break;
        default: Check(Quantize(selected, 16) == selected, "idle mixed-state frame never advances a hidden transition"); break;
        }
        ValidOutput(selected);
        Check(std::isfinite(raw) && raw >= 1 && raw <= 16, "raw accumulation stays bounded through mixed input/reset sources");
    }
}
} // namespace

int Run_latency() {
    ImmediateOutputMatrix(); PartialWheelAndExactSteps(); MultiplierAndHoldMatrix(); HeldMidpointHysteresis(); ActualContactSteps();
    InvalidAndBoundaryInputs(); ImmediateAnchorAndFollow(); CadenceMatrix(); MixedStateSequences();
    std::cout << "PASS: " << assertions << " step/timing assertions; " << outputs << " immediate-output tuples; "
              << directReports << " actual contact reports; 100000 midpoint-jitter reports; " << schedules
              << " cadence tuples; 1000000 simulated message wakes; " << mixedSteps << " mixed state steps; "
              << multiplierCases << " multiplier cases; " << heldSteps << " actual multiplied hold steps\n";
    return 0;
}
