#include <stdexcept>
#include "../GlideZoom/pointer_adapter.h"
#include "../GlideZoom/input_state.h"
#include "../GlideZoom/monitor_geometry.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using glide::Vec2;
using glide::Rect;
using namespace glide::pointer;
std::uint64_t assertions = 0, matrixCases = 0, nativeMoves = 0, edgeCases = 0;
void Check(bool condition, const char* message) {
    ++assertions;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
void Near(double actual, double expected, const char* message) {
    if (!(std::isfinite(actual) && std::abs(actual - expected) < 1e-8))
        std::cerr << "actual=" << actual << " expected=" << expected << '\n';
    Check(std::isfinite(actual) && std::abs(actual - expected) < 1e-8, message);
}
View Enabled(double zoom = 4, Rect bounds = {0, 0, 1920, 1080}) {
    return {true, true, bounds, zoom, 1};
}

struct Native {
    Vec2 cursor{100, 100};
    Adapter* adapter = nullptr;
    View view = Enabled();
    std::uint64_t now = 0;
    bool ready = true, reachable = true, writeOk = true, wrongReadback = false;
    bool reenter = false, earlyHostBypass = false, pauseDuringWrite = false, pauseDuringReady = false;
    unsigned failReadAt = 0, pauseReadAt = 0, reads = 0, writes = 0, checks = 0, reentries = 0;
    bool Ready(const View&) noexcept {
        ++checks;
        if (pauseDuringReady) adapter->Pause();
        return ready;
    }
    bool Read(Vec2& value) noexcept {
        ++reads;
        if (failReadAt == reads) return false;
        value = cursor;
        if (pauseReadAt == reads) adapter->Pause();
        return true;
    }
    bool CanPlace(Vec2) noexcept { return reachable; }
    bool Warp(Vec2 value) noexcept {
        ++writes;
        if (!writeOk) return false;
        cursor = value;
        if (reenter) {
            ++reentries;
            Check(adapter && adapter->InWarp(), "native write is covered by reentrancy guard");
            if (earlyHostBypass) adapter->ObserveWarp(value);
            else Check(!adapter->Move(value, true, now, view, true, false, *this),
                       "reentrant owned callback passes without another native call");
        }
        if (pauseDuringWrite) adapter->Pause();
        if (wrongReadback) cursor.x += 10;
        return true;
    }
};
struct Driver {
    Adapter adapter;
    Native native;
    View view = Enabled();
    std::uint64_t now = 1;
    explicit Driver(bool synchronousEcho = true) {
        native.adapter = &adapter;
        native.reenter = synchronousEcho;
    }
    bool Send(Vec2 candidate, bool alreadyPositioned = false, bool injected = false,
              bool permitted = true, bool gesture = false) {
        ++nativeMoves;
        native.now = now;
        native.view = view;
        if (alreadyPositioned) native.cursor = candidate;
        const bool consumed = adapter.Move(candidate, injected, now, view, permitted, gesture, native);
        if (!consumed) native.cursor = candidate; // Native event is allowed through.
        ++now;
        return consumed;
    }
    void Seed(Vec2 point = {100, 100}) {
        native.cursor = point;
        Check(!Send(point), "first accepted move only establishes native baseline");
    }
};

void GeometryAndBoundaryMatrix() {
    const std::array<Rect, 4> displays{{{0, 0, 1920, 1080}, {-1920, -1080, 0, 0},
                                     {1920, 200, 4480, 1640}, {-1920, -1080, 4480, 1640}}};
    for (Rect bounds : displays)
        for (double zoom : {1.0, 1.001, 1.05, 2.0, 4.0, 16.0, 64.0})
            for (Vec2 delta : {Vec2{-128, -64}, Vec2{-1, 0}, Vec2{0, 1}, Vec2{3, -7},
                               Vec2{128, 64}, Vec2{0, 0}}) {
                ++matrixCases;
                const Vec2 start{(bounds.left + bounds.right) / 2,
                                 (bounds.top + bounds.bottom) / 2};
                const Vec2 next = Advance(start, delta, bounds, zoom);
                Near(next.x, start.x + delta.x / zoom, "inside movement gets 1/zoom gain in x");
                Near(next.y, start.y + delta.y / zoom, "inside movement gets 1/zoom gain in y");
                Step step;
                Check(Propose(start, {start.x + delta.x, start.y + delta.y}, {}, Enabled(zoom, bounds), step),
                      "valid processed-pixel movement creates bounded integer proposal");
                Near(step.point.x + step.remainder.x, next.x, "integer x plus double residual preserves target");
                Near(step.point.y + step.remainder.y, next.y, "integer y plus double residual preserves target");
            }
    const Rect monitor{0, 0, 100, 100};
    // At 4x, exit costs 40 native pixels; the remaining 10 pixels move normally
    // on the neighboring monitor. No clamping traps the pointer at the edge.
    Near(Advance({90, 50}, {50, 0}, monitor, 4).x, 110, "outbound motion pays slow inside and ordinary outside budget");
    Near(Advance({110, 50}, {-50, 0}, monitor, 4).x, 90, "inbound motion is ordinary to edge then slow inside");
    Near(Advance({-10, 50}, {500, 0}, monitor, 4).x, 190, "one event crossing both edges accounts for each segment");
    Near(Advance({110, 50}, {-500, 0}, monitor, 4).x, -90, "reverse crossing both edges is symmetric");
    const auto diagonal = Advance({90, 90}, {50, 50}, monitor, 4);
    Near(diagonal.x, 110, "corner exit preserves vector x");
    Near(diagonal.y, 110, "corner exit preserves vector y");
    Near(Advance({100, 10}, {0, 20}, monitor, 4).y, 30, "right edge belongs to neighboring monitor");
    Near(Advance({0, 10}, {0, 20}, monitor, 4).y, 15, "left edge belongs to selected monitor");
    Near(Advance({-10, -10}, {5, 5}, monitor, 4).x, -5, "motion missing selected monitor is unchanged");
    Near(Advance({-10, 100}, {200, 0}, monitor, 4).x, 190, "tangential motion below monitor is unchanged");
    Near(Advance({100, 50}, {-4, 0}, monitor, 4).x, 99, "entry from exact right edge slows as it enters");
    Near(Advance({0, 50}, {-4, 0}, monitor, 4).x, -4, "exit from exact left edge never sticks");
}

void FractionalAndGainSemantics() {
    for (double zoom : {1.001, 1.05, 2.0, 4.0, 16.0, 64.0})
        for (int direction : {-1, 1}) {
            Vec2 current{20000, 20000}, remainder{};
            const auto view = Enabled(zoom, {0, 0, 40000, 40000});
            for (unsigned i = 1; i <= 4096; ++i) {
                ++matrixCases;
                Step step;
                Check(Propose(current, {current.x + direction, current.y}, remainder, view, step),
                      "tiny native pixels are retained across long sequence");
                current = step.point; remainder = step.remainder;
                Near(current.x + remainder.x, 20000 + direction * static_cast<double>(i) / zoom,
                     "long sequence cannot lose fractional processed displacement");
                Check(std::abs(remainder.x) <= 0.500001 && remainder.y == 0,
                      "fractional state stays bounded without artificial orthogonal movement");
            }
        }
    // The layer has one explicit velocity contract. Click routing is separate;
    // it is not an undocumented detector for gain from another mouse utility.
    Step alreadyMapped, notMapped;
    Check(Propose({100, 100}, {104, 100}, {}, Enabled(4), alreadyMapped), "already slowed native movement accepted");
    Check(Propose({100, 100}, {116, 100}, {}, Enabled(4), notMapped), "unscaled native movement accepted");
    Near(alreadyMapped.point.x, 101, "processed Windows 4px becomes one source pixel at 4x");
    Near(notMapped.point.x, 104, "processed Windows 16px becomes four source pixels at 4x");
    Check(!Same(alreadyMapped.point, notMapped.point), "one normalization preserves different accelerated Windows input speeds");
    Step outside;
    Check(Propose({2200, 100}, {2208, 111}, {0.3, -0.3}, Enabled(4), outside), "other monitor remains routable");
    Check(Same(outside.point, {2208, 111}) && Same(outside.remainder, {}), "other monitor moves exactly normally and forgets residual");
}

void NativeOrderingAndReentryMatrix() {
    for (bool alreadyPositioned : {false, true})
        for (bool synchronousEcho : {false, true})
            for (bool earlyBypass : {false, true}) {
                ++matrixCases;
                Driver d(synchronousEcho); d.native.earlyHostBypass = earlyBypass;
                d.Seed();
                Check(d.Send({116, 108}, alreadyPositioned), "ordinary correction consumes only processed native move");
                Check(Same(d.native.cursor, {104, 102}) && d.native.writes == 1,
                      "both modeled coordinate observations request one bounded normalization write");
                Check(!d.adapter.Faulted() && !d.adapter.InWarp(), "write readback leaves no stuck guard or fault");
                if (synchronousEcho) Check(d.native.reentries == 1, "synchronous echo was passed exactly once");
                else {
                    const auto before = d.native.writes;
                    Check(!d.Send({104, 102}, true, true), "asynchronous possible owned injected echo passes through");
                    Check(d.native.writes == before && !d.adapter.Faulted(), "owned echo never recursively scales or warps");
                }
                Check(d.Send({120, 110}, alreadyPositioned), "following real motion remains eligible after echo");
                Check(Same(d.native.cursor, {108, 104}), "successive extra gain uses actual accepted baseline");
            }
    Driver fractional;
    fractional.Seed();
    Check(fractional.Send({101, 100}) && fractional.native.writes == 0,
          "one quarter pixel consumes proposed move without writing unchanged cursor");
    Near(fractional.adapter.Remainder().x, 0.25, "quarter pixel survives until later motion");
    Check(!fractional.Send({101, 100}) && fractional.native.writes == 0,
          "half pixel equals native candidate and needs no cursor setter");
    // The half-pixel step rounds to 101: the original candidate passes, not consumes.
}

void GatesAndFailures() {
    for (unsigned condition = 0; condition < 5; ++condition) {
        ++matrixCases;
        Driver d; d.Seed();
        if (condition == 0) d.view.requested = false;
        if (condition == 1) d.view.active = false;
        if (condition == 2) d.view.zoom = 1;
        const bool injected = condition == 3, gesture = condition == 4;
        const auto reads = d.native.reads, writes = d.native.writes;
        Check(!d.Send({116, 100}, false, injected, true, gesture), "disabled, idle, unity, foreign injected or held gesture passes");
        Check(d.native.reads == reads && d.native.writes == writes && !d.adapter.Faulted(),
              "ineligible conditions do not touch cursor and do not latch a false failure");
    }
    for (unsigned failure = 0; failure < 4; ++failure) {
        ++matrixCases;
        Driver d; d.Seed();
        if (failure == 0) d.view.bounds.right = std::numeric_limits<double>::quiet_NaN();
        if (failure == 1) d.native.writeOk = false;
        if (failure == 2) d.native.failReadAt = d.native.reads + 2;
        if (failure == 3) d.native.wrongReadback = true;
        Check(!d.Send({116, 100}), "uncertain write or invalid geometry lets original event through");
        Check(d.adapter.Faulted() && !d.adapter.InWarp(), "uncertain write or invalid geometry latches gain off with guard released");
        Check(Same(d.adapter.Remainder(), {}), "failed proposal does not commit fractional motion state");
        const auto writes = d.native.writes, reads = d.native.reads;
        d.adapter.Pause(); d.view = Enabled(); d.native.ready = d.native.writeOk = true;
        d.native.wrongReadback = false; d.native.failReadAt = 0;
        Check(!d.Send({124, 104}) && !d.Send({132, 108}), "faulted session passes all later ordinary events");
        Check(d.native.writes == writes && d.native.reads == reads && d.adapter.Faulted(),
              "pause and checkbox cycling never clear failure or attempt cursor restoration");
    }
    Driver huge; huge.Seed();
    Check(huge.Send({2000, 100}) && !huge.adapter.Faulted(),
          "fast large-monitor movement does not disable compensation at an arbitrary 512px cutoff");
    Check(Same(huge.native.cursor, {575, 100}), "1900 processed pixels get measured quarter-speed gain at 4x");
    Driver clock; clock.Seed(); clock.now = 0;
    Check(!clock.Send({116, 100}) && clock.adapter.FailureReason() == Failure::Clock,
          "backward monotonic time cannot extend an input lease");
    Driver idle; idle.Seed(); idle.now += 251;
    Check(idle.Send({116, 100}) && !idle.adapter.Faulted() && idle.native.writes == 1,
          "first move after idle uses fresh pre-event sample and receives normal gain");
    Check(Same(idle.native.cursor, {104, 100}), "idle resume cannot cause one unscaled speed burst");
    Driver changed; changed.Seed(); changed.view.epoch++;
    Check(changed.Send({116, 100}) && !changed.adapter.Faulted(), "epoch change starts with verified current baseline");
    changed.view.bounds = {-1920, 0, 0, 1080};
    Check(changed.Send({-100, 100}) && !changed.adapter.Faulted(), "monitor/topology scope change discards old position");
    Check(Same(changed.native.cursor, {-25, 100}), "new scope applies exact ray gain only inside its monitor");

    for (unsigned condition = 0; condition < 4; ++condition) {
        ++matrixCases;
        Driver d; d.Seed();
        const auto writes = d.native.writes;
        const bool permitted = condition != 0;
        if (condition == 1) d.native.ready = false;
        if (condition == 2) d.native.failReadAt = d.native.reads + 1;
        if (condition == 3) d.native.cursor = {500, 500};
        Check(!d.Send({116, 100}, false, false, permitted), "temporary context or changed owner passes original motion");
        Check(!d.adapter.Faulted() && d.native.writes == writes && Same(d.adapter.Remainder(), {}),
              "temporary state cannot permanently disable gain or restore a stale cursor");
        d.native.ready = true; d.native.failReadAt = 0;
        Check(d.Send({132, 100}), "next verified context resumes automatically");
        Check(Same(d.native.cursor, {120, 100}), "resumed gain uses actual current cursor and no discarded history");
    }
    Driver gap; gap.Seed(); gap.native.reachable = false;
    Check(!gap.Send({116, 100}) && gap.native.writes == 0 && !gap.adapter.Faulted(),
          "target in physical monitor gap passes original event without a setter or permanent failure");
    gap.native.reachable = true;
    Check(gap.Send({132, 100}) && Same(gap.native.cursor, {120, 100}),
          "leaving a physical monitor gap resumes measured gain immediately");
}

void EchoAndPressureFailures() {
    Driver canceled(false); canceled.Seed(); canceled.native.pauseDuringWrite = true;
    Check(!canceled.Send({117, 100}), "reentrant cancellation during cursor write passes original event");
    Check(!canceled.adapter.Faulted() && Same(canceled.adapter.Remainder(), {}),
          "completed old native write cannot resurrect canceled fractional state");
    Check(canceled.adapter.BypassEcho({104, 100}, canceled.now),
          "canceled write retains only pending echo bypass for Host gesture safety");
    canceled.native.pauseDuringWrite = false;
    Check(canceled.Send({121, 100}) && canceled.native.writes == 2,
          "first native move after nested cancellation uses a fresh measured gain");
    Check(Same(canceled.native.cursor, {118, 100}), "canceled write history does not alter next displacement");
    Driver early(false); early.Seed();
    Check(early.Send({116, 100}), "host early echo-bypass fixture has an owned warp");
    const auto earlyWrites = early.native.writes;
    Check(early.adapter.BypassEcho({104, 100}, early.now),
          "possible async echo can bypass Host gesture dispatch before it is interpreted as drag");
    Check(!early.adapter.BypassEcho({104, 100}, early.now), "possible echo is recognized once");
    Check(early.native.writes == earlyWrites && !early.adapter.Faulted(),
          "early echo path only passes input and never writes cursor");
    Driver paused(false); paused.Seed();
    Check(paused.Send({116, 100}), "async echo pause fixture has a pending native callback");
    paused.adapter.Pause();
    Check(paused.adapter.BypassEcho({104, 100}, paused.now),
          "gesture/reset/publication pause cannot forget an already requested async setter echo");
    Check(!paused.adapter.Faulted() && Same(paused.adapter.Remainder(), {}),
          "pause preserves only echo bypass, never old gain or fractional state");
    Driver delayed(false); delayed.Seed();
    Check(delayed.Send({116, 100}), "first untagged setter target recorded");
    Check(delayed.Send({120, 100}), "second untagged setter target recorded");
    const auto writes = delayed.native.writes;
    Check(!delayed.Send({104, 100}, true, true) && !delayed.adapter.Faulted(),
          "possible delayed old setter echo passes and reseeds instead of disabling gain forever");
    Check(delayed.native.writes == writes, "old echo cannot create an injection feedback loop");
    Check(delayed.Send({120, 100}) && Same(delayed.native.cursor, {108, 100}),
          "movement following delayed echo resumes at normalized speed");
    Driver pressure(false); pressure.Seed(); pressure.now = 2;
    for (unsigned i = 0; i < 1024; ++i) {
        const Vec2 native{pressure.native.cursor.x + 4, pressure.native.cursor.y};
        pressure.now = 2;
        Check(pressure.Send(native), "finite possible-echo ledger accepts bounded write pressure");
    }
    pressure.now = 2;
    Check(!pressure.Send({pressure.native.cursor.x + 4, 100}) &&
          !pressure.adapter.Faulted(),
          "unbounded callback pressure passes motion without allocating or disabling gain forever");
    pressure.now += 65;
    const auto resume = pressure.native.cursor;
    Check(pressure.Send({resume.x + 4, resume.y}) && !pressure.adapter.Faulted(),
          "expired callback pressure automatically resumes normal speed");
    Driver expiry(false); expiry.Seed();
    Check(expiry.Send({116, 100}), "possible echo lease starts");
    expiry.now += 65;
    Check(expiry.Send({120, 100}) && !expiry.adapter.Faulted(), "readback-verified write does not require an undocumented echo to arrive");
}

// Exercise the actual adapter, not just Propose. An independent path integral
// computes source displacement from the supplied Windows-processed movement.
// A displayed source displacement is multiplied by zoom, so the visual path
// recovers the original speed and acceleration within cursor-pixel rounding.
void VelocityAndAccelerationRegressions() {
    const Rect bounds{-100000, -100000, 100000, 100000};
    for (double zoom : {1.001, 1.05, 2.0, 4.0, 8.0, 16.0, 64.0})
        for (double speed : {1.0, 3.0, 12.0, 48.0})
            for (unsigned path = 0; path < 4; ++path) {
                Driver d;
                d.view = Enabled(zoom, bounds);
                d.Seed({0, 0});
                Vec2 integral{}, nativeIntegral{};
                for (unsigned i = 0; i < 512; ++i) {
                    ++matrixCases;
                    // Ramps, sudden reversals, diagonals and small adjustments
                    // are real input, not a constant per-frame movement rate.
                    const double ramp = path == 0 ? 1 : 1 + (i % 7);
                    const double sign = path >= 2 && ((i / 13) & 1) ? -1 : 1;
                    const Vec2 delta{speed * ramp * sign,
                        path == 3 ? speed * (static_cast<int>(i % 5) - 2) : 0};
                    integral.x += delta.x / zoom;
                    integral.y += delta.y / zoom;
                    nativeIntegral.x += delta.x;
                    nativeIntegral.y += delta.y;
                    const Vec2 before = d.native.cursor;
                    d.Send({before.x + delta.x, before.y + delta.y}, (i & 1) != 0);
                    Check(!d.adapter.Faulted(), "normal acceleration/reversal path must keep compensation running");
                    const Vec2 remainder = d.adapter.Remainder();
                    Near(d.native.cursor.x + remainder.x, integral.x,
                         "adapter source x follows independent integral of Windows movement over zoom");
                    Near(d.native.cursor.y + remainder.y, integral.y,
                         "adapter source y follows independent integral of Windows movement over zoom");
                    Near((d.native.cursor.x + remainder.x) * zoom, nativeIntegral.x,
                         "displayed x movement keeps pre-zoom Windows speed and acceleration");
                    Near((d.native.cursor.y + remainder.y) * zoom, nativeIntegral.y,
                         "displayed y movement keeps pre-zoom Windows speed and acceleration");
                    Check(std::abs(d.native.cursor.x - integral.x) <= 0.500001 &&
                          std::abs(d.native.cursor.y - integral.y) <= 0.500001,
                          "integer source cursor stays within half source pixel of continuous target");
                }
            }

    // Zoom changes do not erase accumulated subpixel movement or cause an
    // artificial pointer relocation. Each event uses the newly published gain.
    Driver changing;
    changing.view = Enabled(1.05, bounds);
    changing.Seed({0, 0});
    Vec2 integral{};
    for (unsigned i = 0; i < 10000; ++i) {
        ++matrixCases;
        changing.view.zoom = 1.001 + static_cast<double>((i * 17) % 7000) / 1000;
        const Vec2 delta{static_cast<double>(static_cast<int>(i % 31) - 15),
                         static_cast<double>(static_cast<int>(i % 17) - 8)};
        integral.x += delta.x / changing.view.zoom;
        integral.y += delta.y / changing.view.zoom;
        const auto before = changing.native.cursor;
        changing.Send({before.x + delta.x, before.y + delta.y});
        Near(changing.native.cursor.x + changing.adapter.Remainder().x, integral.x,
             "changing zoom uses exact current gain in x without old-target movement");
        Near(changing.native.cursor.y + changing.adapter.Remainder().y, integral.y,
             "changing zoom uses exact current gain in y without old-target movement");
        Check(!changing.adapter.Faulted(), "animation telemetry cannot disable normalized motion");
    }
}

void ResumeEchoAndDeviceRateRegressions() {
    for (unsigned cancellation = 0; cancellation < 3; ++cancellation) {
        ++matrixCases;
        Driver d; d.Seed();
        if (cancellation == 0) d.native.pauseDuringReady = true;
        if (cancellation == 1) d.native.pauseReadAt = d.native.reads + 1;
        if (cancellation == 2) d.native.pauseReadAt = d.native.reads + 2;
        Check(!d.Send({117, 100}), "nested cancellation never consumes the original move");
        Check(!d.adapter.Faulted() && !d.adapter.InWarp() && Same(d.adapter.Remainder(), {}),
              "nested cancellation cannot resurrect partial gain or a write guard");
        Check(d.native.writes == (cancellation == 2 ? 1u : 0u),
              "cancellation before a write never writes; cancellation after readback never retries");
        d.native.pauseDuringReady = false; d.native.pauseReadAt = 0;
        Check(d.Send({133, 100}) && Same(d.native.cursor, {121, 100}),
              "first later movement resumes from the current position, not the canceled target");
    }

    Driver initial;
    Check(initial.Send({116, 100}) && Same(initial.native.cursor, {104, 100}),
          "very first pre-event movement after enabling is scaled");
    Driver positioned;
    Check(!positioned.Send({116, 100}, true) && Same(positioned.native.cursor, {116, 100}),
          "initial already-positioned event has no measured delta and is passed once");
    Check(positioned.Send({132, 100}, true) && Same(positioned.native.cursor, {120, 100}),
          "later already-positioned event scales from its verified accepted baseline");

    // Emulate all native setter callbacks arriving later. A fake or real event
    // at an old untagged target is conservatively passed; it cannot recursively
    // produce a new warp, erase the feature, or affect key/button pairing.
    for (bool injected : {false, true})
        for (unsigned order = 0; order < 3; ++order) {
            ++matrixCases;
            Driver d(false); d.Seed();
            std::array<Vec2, 16> targets{};
            for (unsigned i = 0; i < targets.size(); ++i) {
                Check(d.Send({d.native.cursor.x + 16, 100}), "queued setter correction accepted");
                targets[i] = d.native.cursor;
            }
            const auto writes = d.native.writes;
            for (unsigned i = 0; i < targets.size(); ++i) {
                const unsigned index = order == 0 ? i : order == 1 ? 15 - i : (i * 7) % 16;
                Check(!d.Send(targets[index], true, injected), "delayed owned coordinate passes with no physical movement");
                Check(d.native.writes == writes && !d.adapter.Faulted(),
                      "no physical input plus arbitrary echo order cannot create another cursor write");
            }
            const auto before = d.native.cursor;
            Check(d.Send({before.x + 16, 100}) && Same(d.native.cursor, {before.x + 4, 100}),
                  "normal gain returns immediately after delayed callback drain");
        }

    Driver reverse(false); reverse.Seed();
    Check(reverse.Send({116, 100}) && reverse.Send({120, 100}), "ambiguous reversal fixture writes two targets");
    const auto writes = reverse.native.writes;
    Check(!reverse.Send({104, 100}) && Same(reverse.native.cursor, {104, 100}),
          "real reversal matching untagged old target is conservatively passed, never swallowed");
    Check(!reverse.adapter.Faulted() && reverse.native.writes == writes,
          "ambiguous reversal cannot permanently leave the cursor at unscaled speed");
    Check(reverse.Send({88, 100}) && Same(reverse.native.cursor, {100, 100}),
          "next reversal event receives quarter-speed gain immediately");

    Driver fast(false);
    fast.view = Enabled(4, {-100000, -100000, 100000, 100000});
    fast.Seed({0, 0});
    for (unsigned i = 0; i < 8000; ++i) {
        ++matrixCases;
        fast.now = 2 + i / 8; // Eight events per millisecond: 8000 Hz.
        Check(fast.Send({fast.native.cursor.x + 4, 0}), "8000 Hz mouse remains scaled with delayed or absent echoes");
        Check(!fast.adapter.Faulted() && fast.native.cursor.x == static_cast<double>(i + 1),
              "high report rate cannot exhaust the normal echo lease or produce a speed burst");
    }
}

void ButtonAndGestureComposition() {
    using glide::input::Button;
    using glide::input::Gesture;
    glide::input::State gestures;
    gestures.SetEnabled(true);
    Driver d; d.Seed();
    Check(!gestures.ButtonEvent(Button::Left, true, {100, 100}, 1).consume, "ordinary drag press passes");
    Check(d.Send({116, 100}), "ordinary drag motion uses zoom-normalized gain");
    Check(!gestures.ButtonEvent(Button::Left, false, {104, 100}, 2).consume, "ordinary drag release passes unchanged");
    Check(gestures.Active() == Gesture::None, "pointer adapter never starts a zoom gesture");
    // Existing double-click-and-hold owns its own balanced release ledger.
    gestures.Configure({false, true, 400, 30000});
    gestures.Key(glide::input::vk::LeftControl, true, 5);
    gestures.Key(glide::input::vk::LeftAlt, true, 6);
    gestures.ButtonEvent(Button::Left, true, {104, 100}, 10);
    gestures.ButtonEvent(Button::Left, false, {104, 100}, 20);
    const auto second = gestures.ButtonEvent(Button::Left, true, {104, 100}, 30);
    Check(second.consume && second.begin, "gesture owns second mouse-down before pointer adapter runs");
    const auto writes = d.native.writes;
    Check(!d.Send({120, 100}, false, false, true, true), "active gesture is left for Host's anchor-freeze handling");
    Check(d.native.writes == writes, "precision layer cannot move a held zoom anchor");
    gestures.SetEnabled(false);
    const auto release = gestures.ButtonEvent(Button::Left, false, {104, 100}, 40);
    Check(release.consume, "existing owed release survives precision pause/failure and feature disable");
    Check(!gestures.ButtonEvent(Button::Left, false, {104, 100}, 41).consume,
          "owed release is consumed once and pointer adapter has no button ledger");
}

void InvalidInputMatrix() {
    const auto good = Enabled();
    Step sentinel{{777, 888}, {0.1, 0.2}};
    for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(), -1.0, 0.0, 65.0}) {
        View value = good; value.zoom = invalid;
        Step output = sentinel;
        Check(!Propose({100, 100}, {104, 100}, {}, value, output), "invalid zoom rejects proposal");
        Check(Same(output.point, sentinel.point) && Same(output.remainder, sentinel.remainder),
              "failed pure proposal leaves output untouched");
    }
    for (Vec2 point : {Vec2{std::numeric_limits<double>::quiet_NaN(), 0}, Vec2{0, 0.1},
                       Vec2{300000, 0}, Vec2{0, -300000}}) {
        Step output = sentinel;
        Check(!Propose(point, {104, 100}, {}, good, output), "invalid or noninteger native point rejected");
        Check(Same(output.point, sentinel.point), "invalid point cannot partially change output");
    }
    Step output = sentinel;
    Check(!Propose({100, 100}, {104, 100}, {0.51, 0}, good, output), "corrupted residual is rejected");
    View empty = good; empty.bounds.right = empty.bounds.left;
    Check(!Propose({100, 100}, {104, 100}, {}, empty, output), "degenerate selected monitor is rejected");
}
void EdgeAccelerationProposalMatrix() {
    const Rect bounds{0, 0, 1920, 1080};
    for (const double gain : {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0}) {
        auto view = Enabled(4, bounds);
        view.offset = {720, 405};
        view.edgePanMultiplier = gain;
        Step out;
        Check(Propose({1198, 540}, {1206, 540}, {}, view, out),
              "edge crossing proposal uses the applied camera transform");
        Near(out.point.x + out.remainder.x, 1200 + 1.25 * (gain - 1),
             "only source travel beyond the five-display-pixel edge is accelerated");
        Near(out.point.y, 540, "horizontal acceleration cannot alter vertical point");
        Near(out.remainder.y, 0, "horizontal acceleration cannot create vertical residual");
        Check(Propose({1199, 540}, {1191, 540}, {}, view, out),
              "inward reversal remains a valid compensated move");
        Near(out.point.x + out.remainder.x, 1197, "inward reversal immediately uses normal1/zoom gain");
        Check(Propose({960, 540}, {968, 540}, {}, view, out), "interior proposal remains valid");
        Near(out.point.x + out.remainder.x, 962, "visible interior never receives edge acceleration");

        auto normal = view;
        normal.edgePanMultiplier = 1;
        Step ordinary;
        for (const Vec2 outside : {Vec2{1930, 540}, Vec2{-10, 540}, Vec2{960, 1090}, Vec2{960, -10}}) {
            const Vec2 from{std::clamp(outside.x, bounds.left, bounds.right - 1),
                            std::clamp(outside.y, bounds.top, bounds.bottom - 1)};
            Check(Propose(from, outside, {}, view, out) && Propose(from, outside, {}, normal, ordinary),
                  "physical monitor crossing is valid at every multiplier");
            Check(Same(out.point, ordinary.point) && Same(out.remainder, ordinary.remainder),
                  "original cross-monitor exit retains exactly the established pointer behavior");
        }
        Check(Propose({1917, 540}, {1921, 540}, {}, view, out),
              "bonus close to outer right display edge is bounded");
        Check(out.point.x >= 1918 && out.point.x <= 1919,
              "edge bonus cannot force an integer cursor into adjacent monitor");
        Check(Propose({1919, 540}, {1927, 540}, {}, view, out) &&
              Propose({1919, 540}, {1927, 540}, {}, normal, ordinary),
              "normal following move can leave a bonus-clamped display");
        Check(Same(out.point, ordinary.point) && out.point.x >= 1920,
              "last-pixel bonus clamp does not trap subsequent outward input");
        for (const double residual : {-0.49, 0.0, 0.49}) {
            Check(Propose({1919, 540}, {1921, 540}, {residual, 0}, view, out),
                  "fractional close-to-boundary movement stays valid");
            Check(Propose({1919, 540}, {1921, 540}, {residual, 0}, normal, ordinary),
                  "fractional boundary reference stays valid");
            Check(out.point.x >= ordinary.point.x &&
                  (ordinary.point.x >= 1920 ? out.point.x == ordinary.point.x : out.point.x < 1920),
                  "rounding bonus never pulls inward or invents an integer monitor exit");
        }
        ++edgeCases;
    }

    // Defaults and center-follow publication use multiplier1. Even a different
    // camera offset cannot perturb old coordinates or fractional accumulation.
    for (const double zoom : {1.001, 1.05, 2.0, 4.0, 16.0, 64.0})
    for (const Vec2 current : {Vec2{10, 10}, Vec2{960, 540}, Vec2{1919, 1079}, Vec2{-50, 800}})
    for (const Vec2 delta : {Vec2{-100, -15}, Vec2{1, 0}, Vec2{0, 1}, Vec2{200, 80}, Vec2{0, 0}})
    for (const Vec2 residual : {Vec2{}, Vec2{-0.49, 0.49}, Vec2{0.5, -0.5}}) {
        const auto baseline = Enabled(zoom, bounds);
        auto offsetOnly = baseline;
        offsetOnly.offset = {720.125, 405.375};
        const Vec2 candidate{current.x + delta.x, current.y + delta.y};
        Step oldPath, withOffset;
        Check(Propose(current, candidate, residual, baseline, oldPath) &&
              Propose(current, candidate, residual, offsetOnly, withOffset),
              "default acceleration produces a valid legacy proposal");
        Check(Same(oldPath.point, withOffset.point) && Same(oldPath.remainder, withOffset.remainder),
              "default acceleration remains bitwise identical for point and residual");
        ++edgeCases;
    }
}

void AcceleratedNativeTransactions() {
    for (unsigned gain = 1; gain <= 10; ++gain)
    for (unsigned condition = 0; condition < 13; ++condition)
    for (const bool alreadyPositioned : {false, true}) {
        Driver d(condition != 1);
        d.view = Enabled(16);
        d.view.offset = {900, 506.25};
        d.view.edgePanMultiplier = gain;
        d.Seed({1020, 540});
        switch (condition) {
        case 2: d.native.wrongReadback = true; break;
        case 3: d.native.writeOk = false; break;
        case 4: d.native.failReadAt = d.native.reads + 2; break;
        case 5: d.native.pauseDuringWrite = true; break;
        case 6: d.native.ready = false; break;
        case 7: d.native.reachable = false; break;
        case 11: d.view.requested = false; break;
        case 12: d.view.active = false; break;
        default: break;
        }
        const bool consumed = d.Send({1052, 540}, alreadyPositioned,
                                    condition == 8, condition != 9, condition == 10);
        if (condition <= 1) {
            Check(consumed && d.native.writes == 1 && !d.adapter.Faulted(),
                  "accelerated edge move uses one successful verified native transaction");
            Near(d.native.cursor.x, 1020 + 2 * gain,
                 "native cursor receives requested edge multiplier after normal zoom compensation");
            if (condition == 1) {
                const auto writes = d.native.writes;
                Check(!d.Send(d.native.cursor, true, true), "accelerated owned asynchronous echo passes");
                Check(d.native.writes == writes && !d.adapter.Faulted(),
                      "accelerated owned echo cannot recursively multiply or rewrite movement");
            }
        } else if (condition <= 4) {
            Check(!consumed && d.native.writes == 1 && d.adapter.Faulted(),
                  "accelerated write/readback failure still fails open and latches the feature off");
            Check(Same(d.adapter.Remainder(), {}), "accelerated native failure cannot commit a fractional remainder");
        } else if (condition == 5) {
            Check(!consumed && d.native.writes == 1 && !d.adapter.Faulted(),
                  "reentrant cancellation during accelerated write cannot resurrect or consume its proposal");
            Check(Same(d.adapter.Remainder(), {}), "reentrant accelerated cancellation retains no partial movement");
        } else {
            Check(!consumed && d.native.writes == 0 && !d.adapter.Faulted(),
                  "edge acceleration obeys desktop, gap, injection, permission, gesture and settings gates");
        }
        Check(!d.adapter.InWarp(), "accelerated transaction always releases native recursion guard");
        ++edgeCases;
    }
}

void EdgeInvalidInputs() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const Step sentinel{{12, 34}, {0.2, -0.3}};
    for (const double gain : {nan, inf, -inf, 0.0, -1.0, 0.99, 10.01, 1e300}) {
        auto view = Enabled(4);
        view.edgePanMultiplier = gain;
        auto output = sentinel;
        Check(!Propose({1200, 540}, {1216, 540}, {}, view, output),
              "corrupt edge multiplier cannot create pointer proposal");
        Check(Same(output.point, sentinel.point) && Same(output.remainder, sentinel.remainder),
              "corrupt multiplier cannot partially publish pointer or fractional state");
        ++edgeCases;
    }
    for (const Vec2 offset : {Vec2{nan, 0}, Vec2{0, inf}, Vec2{262145, 0}, Vec2{0, -262145}}) {
        auto view = Enabled(4);
        view.offset = offset;
        view.edgePanMultiplier = 4;
        auto output = sentinel;
        Check(!Propose({1200, 540}, {1216, 540}, {}, view, output),
              "corrupt applied camera transform cannot steer the pointer");
        Check(Same(output.point, sentinel.point) && Same(output.remainder, sentinel.remainder),
              "corrupt camera transform cannot partially publish movement state");
        ++edgeCases;
    }
}

void AcceleratedQuantizedFrameFusion() {
    const std::array<Rect, 4> displays{{{0, 0, 1920, 1080}, {-1920, -1080, 0, 0},
                                     {1920, -720, 4480, 720}, {-3072, -100, -1152, 980}}};
    for (const auto bounds : displays)
    for (const double zoom : {1.01, 1.05, 3.17, 16.0})
    for (const double gain : {1.0, 3.0, 10.0})
    for (const double direction : {-1.0, 1.0})
    for (const unsigned axes : {1u, 2u, 3u})
    for (const double nativePixels : {1.0, 3.0, 32.0})
    for (const unsigned renderEvery : {1u, 4u, 15u}) {
        const Vec2 center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        auto precise = glide::AnchorZoom({}, zoom, center, bounds);
        const Vec2 border{(axes & 1) ? (direction > 0 ? bounds.right - 5 : bounds.left + 5) : center.x,
                          (axes & 2) ? (direction > 0 ? bounds.bottom - 5 : bounds.top + 5) : center.y};
        auto current = glide::ScreenToSource(border, precise);
        const auto insideInteger = [&](double value) { return direction > 0 ? std::floor(value) : std::ceil(value); };
        current = {insideInteger(current.x), insideInteger(current.y)};
        Vec2 remainder{};
        glide::viewport::Follow following;
        Check(following.Rebase(current, bounds), "quantized fusion seeds actual integer source pointer");
        glide::monitor::Frame frame;
        Check(glide::monitor::MakeFrame(bounds, precise, frame), "quantized fusion establishes initial GPU/input geometry");
        auto view = Enabled(frame.transform.zoom, bounds);
        view.offset = frame.transform.offset;
        view.edgePanMultiplier = gain;
        bool leftMonitor = false;
        for (unsigned event = 1; event <= 128; ++event) {
            const Vec2 candidate{current.x + ((axes & 1) ? direction * nativePixels : 0),
                                 current.y + ((axes & 2) ? direction * nativePixels : 0)};
            Step proposed;
            Check(Propose(current, candidate, remainder, view, proposed),
                  "integer/residual hook proposal survives fractional applied zoom and camera offset");
            Check(direction * (proposed.point.x - current.x) >= 0 &&
                  direction * (proposed.point.y - current.y) >= 0,
                  "quantized accelerated pointer never moves opposite the real monotone input");
            Check(std::abs(proposed.remainder.x) <= 0.500001 &&
                  std::abs(proposed.remainder.y) <= 0.500001,
                  "mixed accelerated input/render trace retains bounded fractional state");
            current = proposed.point;
            remainder = proposed.remainder;
            if (!Contains(bounds, current)) { leftMonitor = true; break; }
            if (event % renderEvery != 0 && event != 128) continue;
            const auto before = precise;
            Check(following.Advance(precise, current, bounds, true, glide::viewport::FollowMode::Edges, center, precise),
                  "following consumes accepted integer pointer while retaining precise camera state");
            Check(direction * (precise.offset.x - before.offset.x) >= -1e-8 &&
                  direction * (precise.offset.y - before.offset.y) >= -1e-8,
                  "camera never reverses while pointer motion remains outward across quantized frame feedback");
            Check(glide::monitor::MakeFrame(bounds, precise, frame),
                  "accelerated precise camera can produce validated native presentation geometry");
            // Only presented, quantized values return to the hook. Feeding the
            // ideal camera here would miss the earlier quantization feedback risk.
            view.zoom = frame.transform.zoom;
            view.offset = frame.transform.offset;
            const auto clamped = glide::ClampOffset(precise.offset, precise.zoom, bounds);
            Near(precise.offset.x, clamped.x, "accelerated quantized camera remains horizontally bounded");
            Near(precise.offset.y, clamped.y, "accelerated quantized camera remains vertically bounded");
        }
        if (!leftMonitor) {
            const auto stopped = precise;
            for (unsigned idle = 0; idle < 32; ++idle) {
                Step proposal;
                Check(Propose(current, current, remainder, view, proposal), "stopped quantized pointer proposal is valid");
                Check(Same(proposal.point, current) && Same(proposal.remainder, remainder),
                      "stationary input cannot turn fractional remainder into a synthetic move");
                Check(following.Advance(precise, current, bounds, true, glide::viewport::FollowMode::Edges, center, precise),
                      "stationary quantized camera update is valid");
                Check(precise.zoom == stopped.zoom && Same(precise.offset, stopped.offset),
                      "stationary quantized feedback cannot drift or replay edge acceleration");
            }
            const Vec2 inward{current.x - ((axes & 1) ? direction * nativePixels : 0),
                              current.y - ((axes & 2) ? direction * nativePixels : 0)};
            auto normal = view;
            normal.edgePanMultiplier = 1;
            Step accelerated, ordinary;
            Check(Propose(current, inward, remainder, view, accelerated) &&
                  Propose(current, inward, remainder, normal, ordinary),
                  "inward reversal is valid after real quantized accelerated trace");
            Check(Same(accelerated.point, ordinary.point) && Same(accelerated.remainder, ordinary.remainder),
                  "first inward reversal after quantized feedback has exactly ordinary pointer speed");
            Check(following.Advance(precise, accelerated.point, bounds, true,
                                    glide::viewport::FollowMode::Edges, center, precise),
                  "camera accepts first inward reversal after real quantized accelerated trace");
            Check(Same(precise.offset, stopped.offset),
                  "inward reversal immediately releases the camera even after fractional frame feedback");
        }
        ++edgeCases;
    }
}

void EdgeResidualAndFrameCadenceRegression() {
    const std::array<Rect, 4> displays{{{0, 0, 1920, 1080}, {-1920, -1080, 0, 0},
                                     {1920, -720, 4480, 720}, {-3072, -100, -1152, 980}}};
    for (const auto bounds : displays)
    for (const double zoom : {1.01, 1.05, 3.17, 4.0, 16.0})
    for (const double gain : {2.0, 3.0, 10.0})
    for (const double direction : {-1.0, 1.0}) {
        const Vec2 center{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        const double border = direction > 0 ? bounds.right - 5 : bounds.left + 5;
        auto initial = glide::AnchorZoom({}, zoom, center, bounds);
        const double minimumOffset = bounds.left - bounds.left / zoom;
        const double maximumOffset = bounds.right - bounds.right / zoom;
        initial.offset.x = minimumOffset + (maximumOffset - minimumOffset) * (direction > 0 ? 0.25 : 0.75);
        Vec2 start = glide::ScreenToSource({border, center.y}, initial);
        start = {std::round(start.x), std::round(start.y)};
        initial.offset.x = start.x - border / zoom;
        const double room = direction > 0 ? bounds.right - 1 - start.x : start.x - bounds.left;
        const double cameraRoom = direction > 0 ? maximumOffset - initial.offset.x
                                                : initial.offset.x - minimumOffset;
        const unsigned total = static_cast<unsigned>(std::max(1.0,
            std::min(240.0, std::floor(std::min(room, cameraRoom) * zoom / gain * 0.5))));
        for (const unsigned packetSize : {1u, 2u, 4u, 8u, 16u})
        for (const unsigned renderEvery : {1u, 4u, 10000u}) {
            auto precise = initial;
            auto current = start;
            Vec2 remainder{};
            glide::viewport::Follow following;
            Check(following.Rebase(current, bounds), "cadence regression seeds integer cursor at ideal edge");
            glide::monitor::Frame frame;
            Check(glide::monitor::MakeFrame(bounds, precise, frame), "cadence regression quantizes initial edge frame");
            auto view = Enabled(frame.transform.zoom, bounds);
            view.offset = frame.transform.offset;
            view.edgePanMultiplier = gain;
            const double expected = direction * total * gain / view.zoom;
            unsigned processed = 0, packets = 0;
            while (processed < total) {
                const auto amount = std::min(packetSize, total - processed);
                Step out;
                Check(Propose(current, {current.x + direction * amount, current.y}, remainder, view, out),
                      "cadence regression computes actual rounded pointer proposal");
                current = out.point;
                remainder = out.remainder;
                processed += amount;
                ++packets;
                Check(Contains(bounds, current), "cadence regression excludes intentional outer-monitor exit limits");
                if (packets % renderEvery != 0 && processed != total) continue;
                Check(following.Advance(precise, current, bounds, true,
                                        glide::viewport::FollowMode::Edges, center, precise),
                      "cadence regression updates retained precise camera from accepted integer cursor");
                Check(glide::monitor::MakeFrame(bounds, precise, frame),
                      "cadence regression reconstructs real quantized presentation feedback");
                view.zoom = frame.transform.zoom;
                view.offset = frame.transform.offset;
                const auto visible = glide::SourceToScreen(current, frame.transform);
                Check(std::abs(visible.x - border) <= 1.001,
                      "accelerated cursor stays at five-pixel edge within existing one-output-pixel presentation rounding");
            }
            Near(current.x + remainder.x - start.x, expected,
                 "post-gain residual and frame quantization cannot reduce edge multiplier for tiny packets");
            Near(precise.offset.x - initial.offset.x, current.x - start.x,
                 "camera follows accepted integer source movement independently of packet and render cadence");
            ++edgeCases;
        }
    }
}
} // namespace

int Run_pointer_motion() {
    GeometryAndBoundaryMatrix();
    FractionalAndGainSemantics();
    NativeOrderingAndReentryMatrix();
    GatesAndFailures();
    EchoAndPressureFailures();
    VelocityAndAccelerationRegressions();
    ResumeEchoAndDeviceRateRegressions();
    ButtonAndGestureComposition();
    InvalidInputMatrix();
    EdgeAccelerationProposalMatrix();
    AcceleratedNativeTransactions();
    EdgeInvalidInputs();
    AcceleratedQuantizedFrameFusion();
    EdgeResidualAndFrameCadenceRegression();
    std::cout << "Zoom-scaled pointer: " << assertions << " assertions; " << matrixCases
              << " geometry/fraction/failure matrix cases; " << nativeMoves
              << " mocked native moves; " << edgeCases
              << " edge acceleration/transaction cases. Native driver/Windows feel remains an acceptance check.\n";
    return 0;
}
