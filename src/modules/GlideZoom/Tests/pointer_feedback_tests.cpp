#include <stdexcept>
#include "../GlideZoom/pointer_adapter.h"
#include "../GlideZoom/pointer_feedback.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using glide::Vec2;
using glide::Rect;
using glide::Transform;
using namespace glide::pointer;
std::uint64_t assertions = 0, scenarios = 0, displayedMoves = 0, transactionRows = 0;

void Check(bool value, const char* message) {
    ++assertions;
    if (!value) {
        std::cerr << "FAIL pointer feedback: " << message << " [scenario=" << scenarios << "]\n";
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}
bool Near(double a, double b) { return std::abs(a - b) < 1e-8; }

struct Native {
    Adapter* adapter = nullptr;
    Vec2 cursor{960, 540};
    bool ready = true, reachable = true, writeOkay = true, wrongReadback = false;
    bool pauseReady = false, pauseWrite = false, synchronousEcho = true;
    bool invalidateReady = false, invalidateWrite = false;
    unsigned reads = 0, writes = 0, failReadAt = 0, pauseReadAt = 0, invalidInsideWarp = 0;
    bool Ready(const View&) noexcept {
        if (pauseReady) adapter->Pause();
        if (invalidateReady) adapter->InvalidateFeedback();
        return ready;
    }
    bool Read(Vec2& output) noexcept {
        ++reads;
        if (failReadAt == reads) return false;
        output = cursor;
        if (pauseReadAt == reads) adapter->Pause();
        return true;
    }
    bool CanPlace(Vec2) noexcept { return reachable; }
    bool Warp(Vec2 point) noexcept {
        ++writes;
        if (adapter->InWarp() && !adapter->FeedbackSnapshot().valid) ++invalidInsideWarp;
        if (!writeOkay) return false;
        cursor = point;
        if (synchronousEcho) adapter->ObserveWarp(point);
        if (pauseWrite) adapter->Pause();
        if (invalidateWrite) adapter->InvalidateFeedback();
        if (wrongReadback) cursor.x += 1;
        return true;
    }
};

struct Driver {
    Adapter adapter;
    Native native{};
    FeedbackChannel channel;
    View view{true, true, {0, 0, 1920, 1080}, 4, 7, {720, 405}, 1};
    std::uint64_t now = 1;
    Driver() { native.adapter = &adapter; }
    bool Send(Vec2 candidate, bool injected = false, bool permitted = true, bool gesture = false) {
        const bool consumed = adapter.Move(candidate, injected, now++, view, permitted, gesture, native);
        // Publication precedes return from the hook. An allowed native event
        // may not yet have committed its integer point; the resolver must
        // reject it until the independently read physical cursor matches.
        channel.Publish(adapter.FeedbackSnapshot());
        if (!consumed) native.cursor = candidate;
        return consumed;
    }
    void Seed() { Send(native.cursor); }
};

void ProportionalInteriorDisplay() {
    for (const double zoom : {1.01, 1.05, 2.0, 4.0, 8.0, 16.0})
        for (int direction : {-1, 1})
            for (unsigned axes : {1u, 2u, 3u})
                for (bool negative : {false, true}) {
                    ++scenarios;
                    Driver driver;
                    if (negative) {
                        driver.view.bounds = {-1920, -1080, 0, 0};
                        driver.native.cursor = {-960, -540};
                    }
                    const Vec2 start = driver.native.cursor;
                    driver.view.zoom = zoom;
                    driver.view.offset = {start.x * (1 - 1 / zoom), start.y * (1 - 1 / zoom)};
                    driver.Seed();
                    const Transform applied{zoom, driver.view.offset};
                    const auto startScreen = glide::SourceToScreen(start, applied);
                    const Vec2 delta{(axes & 1) ? static_cast<double>(direction) : 0,
                                     (axes & 2) ? static_cast<double>(direction) : 0};
                    for (unsigned step = 1; step <= 128; ++step) {
                        ++displayedMoves;
                        const auto before = driver.native.cursor;
                        driver.Send({before.x + delta.x, before.y + delta.y});
                        const auto writes = driver.native.writes;
                        Feedback feedback;
                        Check(driver.channel.Read(feedback) && feedback.valid,
                              "every committed ordinary move provides a coherent fractional cursor sample");
                        Vec2 precise{};
                        Check(ResolveCursor(feedback, driver.native.cursor, applied, driver.view.bounds,
                                            driver.view.epoch, true, precise),
                              "ordinary interior samples resolve against exactly applied geometry");
                        const auto screen = glide::SourceToScreen(precise, applied);
                        Check(Near(screen.x, startScreen.x + step * delta.x) &&
                              Near(screen.y, startScreen.y + step * delta.y),
                              "one processed pixel produces one displayed pixel at fractional, 4x and 16x zoom");
                        Check(driver.native.writes == writes && !driver.adapter.Faulted(),
                              "render feedback never writes native cursor or changes compensation safety");
                        Check(std::abs(precise.x - driver.native.cursor.x) <= 0.500001 &&
                              std::abs(precise.y - driver.native.cursor.y) <= 0.500001,
                              "drawn precision remains within the nearest source pixel used by native clicks");
                    }
                }
}

void CandidateAndNativeTransactionBoundaries() {
    ++scenarios; ++transactionRows;
    Driver proposed;
    proposed.Seed();
    proposed.Send({961, 540}); // 0.25 source pixel, consumed without a write.
    const auto before = proposed.native.cursor;
    const bool consumed = proposed.adapter.Move({961, 540}, false, proposed.now++, proposed.view,
                                                 true, false, proposed.native);
    Check(!consumed && Same(before, proposed.native.cursor),
          "half-pixel proposal is allowed through before Windows commits its integer point");
    Feedback feedback = proposed.adapter.FeedbackSnapshot();
    Vec2 output{123, 456};
    const Transform transform{4, proposed.view.offset};
    Check(feedback.valid && !ResolveCursor(feedback, before, transform, proposed.view.bounds,
                                           proposed.view.epoch, true, output) && Same(output, {123, 456}),
          "pre-commit integer candidate is never rendered over a different physical cursor");
    proposed.native.cursor = {961, 540};
    Check(ResolveCursor(feedback, proposed.native.cursor, transform, proposed.view.bounds,
                        proposed.view.epoch, true, output) && Near(output.x, 960.5),
          "same sample becomes usable only after its physical position actually commits");

    // Each row reaches a real Adapter native/cancellation boundary. None may
    // publish the tentative fraction after an uncertain or cancelled write.
    for (unsigned fault = 0; fault != 12; ++fault) {
        ++scenarios; ++transactionRows;
        Driver driver;
        driver.Seed();
        driver.Send({961, 540});
        Check(driver.adapter.FeedbackSnapshot().valid, "establish fractional feedback before boundary fault");
        switch (fault) {
        case 0: driver.native.ready = false; break;
        case 1: driver.native.reachable = false; break;
        case 2: driver.native.writeOkay = false; break;
        case 3: driver.native.wrongReadback = true; break;
        case 4: driver.native.failReadAt = driver.native.reads + 1; break;
        case 5: driver.native.failReadAt = driver.native.reads + 2; break;
        case 6: driver.native.pauseReady = true; break;
        case 7: driver.native.pauseWrite = true; break;
        case 8: driver.native.pauseReadAt = driver.native.reads + 1; break;
        case 9: driver.native.pauseReadAt = driver.native.reads + 2; break;
        case 10: driver.native.cursor = {1200, 600}; break;
        case 11: driver.view.bounds.right = driver.view.bounds.left; break;
        }
        driver.Send({976, 548});
        Check(!driver.adapter.FeedbackSnapshot().valid && !driver.adapter.InWarp(),
              "failed, cancelled, foreign or invalid native transaction cannot expose a residual");
        Check(driver.channel.Read(feedback) && !feedback.valid,
              "publishing a failed transaction invalidates previously usable render feedback");
        Check(driver.native.invalidInsideWarp == driver.native.writes,
              "all observed native setter callbacks see invalid provisional feedback");
    }
    for (unsigned gate = 0; gate != 7; ++gate) {
        ++scenarios; ++transactionRows;
        Driver driver;
        driver.Seed();
        driver.Send({961, 540});
        if (gate == 0) driver.view.requested = false;
        if (gate == 1) driver.view.active = false;
        if (gate == 2) driver.view.zoom = 1;
        if (gate == 6) driver.adapter.Pause();
        else driver.Send({976, 548}, gate == 3, gate != 4, gate == 5);
        Check(!driver.adapter.FeedbackSnapshot().valid,
              "disable, unity, injection, safety gate, gesture and explicit pause invalidate feedback");
    }
    ++scenarios; ++transactionRows;
    Driver echoed;
    echoed.native.synchronousEcho = false;
    echoed.Seed();
    echoed.Send({976, 540});
    echoed.Send({980, 540});
    Check(echoed.adapter.BypassEcho({964, 540}, echoed.now) &&
          !echoed.adapter.FeedbackSnapshot().valid,
          "a delayed old echo cannot leave a fractional sample attached to an obsolete physical point");

    ++scenarios; ++transactionRows;
    Driver changed;
    changed.Seed();
    changed.Send({961, 540});
    const auto old = changed.adapter.FeedbackSnapshot();
    changed.view.epoch += 1;
    changed.Send({976, 540});
    const auto current = changed.adapter.FeedbackSnapshot();
    Check(current.valid && current.epoch != old.epoch &&
          !ResolveCursor(old, changed.native.cursor, {4, changed.view.offset}, changed.view.bounds,
                         changed.view.epoch, true, output),
          "new epoch starts verified movement while renderer rejects every old residual");
}

void ResolverRejectionMatrix() {
    const Feedback good{true, {960, 540}, {0.25, -0.25}, {0, 0, 1920, 1080}, 4, 7};
    const Transform goodTransform{4, {720, 405}};
    for (unsigned fault = 0; fault != 16; ++fault) {
        ++scenarios;
        auto value = good;
        auto physical = value.point;
        auto transform = goodTransform;
        auto bounds = value.bounds;
        auto epoch = value.epoch;
        bool enabled = true;
        switch (fault) {
        case 0: enabled = false; break; // Centered, held gesture, capture or other caller gate.
        case 1: value.valid = false; break;
        case 2: physical.x += 1; break;
        case 3: physical.x += 0.5; break;
        case 4: epoch += 1; break;
        case 5: transform.zoom += 0.01; break;
        case 6: bounds.right += 1; break;
        case 7: value.remainder.x = 0.500002; break;
        case 8: value.remainder.y = -0.500002; break;
        case 9: value.remainder.x = std::numeric_limits<double>::quiet_NaN(); break;
        case 10: value.remainder.y = std::numeric_limits<double>::infinity(); break;
        case 11: transform.offset.x = std::numeric_limits<double>::infinity(); break;
        case 12: value.zoom = transform.zoom = 1; break;
        case 13: value.zoom = transform.zoom = 65; break;
        case 14: value.point.x = physical.x = -1; break;
        case 15: bounds.bottom = value.bounds.bottom = bounds.top; break;
        }
        Vec2 output{123, 456};
        Check(!ResolveCursor(value, physical, transform, bounds, epoch, enabled, output) &&
              Same(output, {123, 456}),
              "rejected feedback leaves exact integer fallback to the caller without partial output");
    }
    for (double zoom : {1.01, 2.0, 4.0, 16.0})
        for (unsigned edge = 0; edge != 4; ++edge)
            for (double bandOffset : {-1.0, 0.0, 0.0001, 1.0}) {
                ++scenarios;
                auto value = good;
                value.zoom = zoom;
                const double margin = 5 + 0.500001 * zoom;
                Vec2 screen{960, 540};
                if (edge == 0) screen.x = margin + bandOffset;
                if (edge == 1) screen.x = 1920 - margin - bandOffset;
                if (edge == 2) screen.y = margin + bandOffset;
                if (edge == 3) screen.y = 1080 - margin - bandOffset;
                const Transform transform{zoom, {value.point.x - screen.x / zoom,
                                                value.point.y - screen.y / zoom}};
                Vec2 output{123, 456};
                const bool resolved = ResolveCursor(value, value.point, transform, value.bounds,
                                                     value.epoch, true, output);
                if (bandOffset <= -1 || bandOffset >= 0.0001)
                    Check(resolved == (bandOffset > 0), "four edges preserve camera band and enable precise interior only");
                if (resolved)
                    Check(Near(output.x, value.point.x + value.remainder.x) &&
                          Near(output.y, value.point.y + value.remainder.y), "eligible band boundary uses only bounded residual");
                else Check(Same(output, {123, 456}), "edge fallback cannot move camera or cursor");
            }
    ++scenarios;
    const Feedback tiny{true, {1, 1}, {0.25, 0.25}, {0, 0, 2, 2}, 16, 7};
    Vec2 output{123, 456};
    Check(!ResolveCursor(tiny, tiny.point, {16, {0.9375, 0.9375}}, tiny.bounds, 7, true, output) &&
          Same(output, {123, 456}), "tiny synthetic monitor has no inverted interior region");
}

void NoMotionHoldCannotRestoreOldFraction() {
    for (double zoom : {4.0, 16.0}) {
        ++scenarios; ++transactionRows;
        Driver driver;
        driver.view.zoom = zoom;
        driver.view.offset = {960 * (1 - 1 / zoom), 540 * (1 - 1 / zoom)};
        driver.Seed();
        driver.Send({961, 540});
        const auto before = driver.adapter.FeedbackSnapshot();
        Check(before.valid && before.remainder.x != 0, "establish pre-hold fractional source position");
        // Both production Host gesture-begin paths Pause before queueing Begin.
        // No mouse event occurs between begin and end in this regression.
        driver.adapter.Pause();
        driver.channel.Publish(driver.adapter.FeedbackSnapshot());
        Feedback during;
        Check(driver.channel.Read(during) && !during.valid, "begin immediately invalidates fractional presentation");
        driver.channel.Publish(driver.adapter.FeedbackSnapshot());
        Feedback ended;
        Vec2 output{123, 456};
        Check(driver.channel.Read(ended) && !ended.valid &&
              !ResolveCursor(ended, driver.native.cursor, {zoom, driver.view.offset}, driver.view.bounds,
                             driver.view.epoch, true, output) && Same(output, {123, 456}),
              "ending a no-motion hold at unchanged zoom/epoch cannot resurrect the old fraction");
        const auto physical = driver.native.cursor;
        driver.Send({physical.x + 1, physical.y});
        const auto resumed = driver.adapter.FeedbackSnapshot();
        Check(resumed.valid && Near(resumed.point.x + resumed.remainder.x, physical.x + 1 / zoom),
              "first verified post-hold movement starts fresh with ordinary proportional gain");
    }
}

void RenderInvalidationPreservesPointerIntegration() {
    for (double zoom : {4.0, 16.0}) {
        ++scenarios; ++transactionRows;
        Driver driver;
        driver.view.zoom = zoom;
        driver.view.offset = {960 * (1 - 1 / zoom), 540 * (1 - 1 / zoom)};
        driver.Seed();
        driver.Send({961, 540});
        const auto remainder = driver.adapter.Remainder();
        const auto point = driver.native.cursor;
        Check(driver.adapter.FeedbackSnapshot().valid && remainder.x != 0,
              "establish fractional movement before zoom telemetry changes");
        driver.adapter.InvalidateFeedback(); // Observed zoom-away telemetry.
        driver.view.zoom = zoom + 1;
        Check(!driver.adapter.FeedbackSnapshot().valid && Same(driver.adapter.Remainder(), remainder) &&
              Same(driver.native.cursor, point),
              "render-only revocation does not discard baseline, remainder or move native cursor");
        driver.view.zoom = zoom;
        driver.adapter.InvalidateFeedback(); // Observed zoom-return telemetry.
        Check(!driver.adapter.FeedbackSnapshot().valid && Same(driver.adapter.Remainder(), remainder),
              "zoom returning without mouse input cannot republish an old residual");
        driver.Send({point.x + 1, point.y});
        const auto next = driver.adapter.FeedbackSnapshot();
        Check(next.valid && Near(next.point.x + next.remainder.x, point.x + remainder.x + 1 / zoom),
              "next verified move resumes fractional rendering with unchanged continuous gain integration");
    }
    for (bool duringWrite : {false, true}) {
        ++scenarios; ++transactionRows;
        Driver driver;
        driver.Seed();
        driver.Send({961, 540});
        const auto before = driver.adapter.Remainder();
        const auto point = driver.native.cursor;
        driver.native.invalidateReady = !duringWrite;
        driver.native.invalidateWrite = duringWrite;
        Check(driver.Send({point.x + 16, point.y}) && !driver.adapter.Faulted(),
              "render-only invalidation during native validation/write leaves established gain behavior intact");
        Check(!driver.adapter.FeedbackSnapshot().valid &&
              Near(driver.native.cursor.x + driver.adapter.Remainder().x, point.x + before.x + 4),
              "late commit cannot resurrect feedback revoked inside the native transaction");
        driver.native.invalidateReady = driver.native.invalidateWrite = false;
        driver.Send({driver.native.cursor.x + 1, driver.native.cursor.y});
        Check(driver.adapter.FeedbackSnapshot().valid,
              "later verified movement can publish after reentrant render-feedback cancellation");
    }
}

Feedback Pattern(std::uint64_t index) {
    const double x = 100 + static_cast<double>(index % 1000);
    return {true, {x, x * 2 + 3}, {static_cast<double>(index % 5) / 10 - 0.2, 0.125},
            {0, 0, 10000, 10000}, 1.01 + static_cast<double>(index % 16), index};
}
bool SameFeedback(const Feedback& a, const Feedback& b) {
    return a.valid == b.valid && Same(a.point, b.point) && Same(a.remainder, b.remainder) &&
           Same(a.bounds, b.bounds) && a.zoom == b.zoom && a.epoch == b.epoch;
}

void PublicationCoherence() {
    ++scenarios;
    FeedbackChannel channel;
    Feedback value;
    Check(channel.Read(value) && !value.valid, "new channel has no speculative cursor data");
    const auto initial = Pattern(1);
    Check(channel.Publish(initial) && channel.Read(value) && SameFeedback(value, initial),
          "complete feedback publication round trips all fields exactly");
    auto bad = initial;
    bad.remainder.x = 1;
    Check(!channel.Publish(bad) && channel.Read(value) && !value.valid,
          "malformed publication invalidates old feedback instead of retaining it");
    Check(channel.Publish({}) && channel.Read(value) && !value.valid,
          "repeated invalid publication remains coherently invalid");
    Check(channel.Publish(initial), "establish concurrent channel fixture");
    std::atomic<bool> start{false}, done{false}, failure{false};
    std::thread writer([&] {
        while (!start.load()) std::this_thread::yield();
        for (std::uint64_t index = 2; index <= 50000; ++index) {
            if (!channel.Publish(index % 37 ? Pattern(index) : Feedback{})) failure.store(true);
        }
        done.store(true);
    });
    std::uint64_t reads = 0;
    start.store(true);
    do {
        const Feedback sentinel{false, {-123, -456}, {0.1, 0.2}, {}, 1, 999999};
        value = sentinel;
        if (channel.Read(value)) {
            ++reads;
            Check(!value.valid || (value.epoch >= 1 && value.epoch <= 50000 &&
                                   SameFeedback(value, Pattern(value.epoch))),
                  "concurrent reader accepts only one complete point/residual/zoom/bounds/epoch publication");
        } else {
            Check(SameFeedback(value, sentinel), "contended bounded read leaves fallback output untouched");
        }
    } while (!done.load());
    writer.join();
    Check(!failure.load() && reads > 0 && channel.Read(value) &&
          SameFeedback(value, Pattern(50000)), "publication finishes at the final coherent verified sample");
}
} // namespace

int Run_pointer_feedback() {
    ProportionalInteriorDisplay();
    CandidateAndNativeTransactionBoundaries();
    ResolverRejectionMatrix();
    NoMotionHoldCannotRestoreOldFraction();
    RenderInvalidationPreservesPointerIntegration();
    PublicationCoherence();
    std::cout << "PASS: " << assertions << " pointer feedback assertions; " << scenarios
              << " scenarios; " << displayedMoves << " proportional displayed moves; "
              << transactionRows << " native/cancellation rows; 50000 concurrent publications; "
                 "production adapter/resolver/channel, native Windows/GPU not executed\n";
    return 0;
}
