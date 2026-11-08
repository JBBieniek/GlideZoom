#include <stdexcept>
#include "../GlideZoom/viewport_motion.h"
#include "../GlideZoom/zoom_animation.h"
#include "../GlideZoom/monitor_geometry.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
using glide::Rect;
using glide::Transform;
using glide::Vec2;
using glide::viewport::Anchor;
using glide::viewport::Follow;
using glide::viewport::FollowMode;

unsigned long long assertions = 0, anchorFrames = 0, paths = 0, mixedSteps = 0;
unsigned long long edgeCases = 0, acceleratedMoves = 0;
void Check(bool value, const std::string& message) {
    ++assertions;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
void Near(double a, double b, const std::string& message, double tolerance = 1e-8) {
    Check(std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance, message);
}
void Same(Transform a, Transform b, const std::string& message) {
    Check(a.zoom == b.zoom && a.offset.x == b.offset.x && a.offset.y == b.offset.y, message);
}
void Bounded(Transform value, Rect bounds) {
    Check(glide::viewport::ValidTransform(value), "result remains finite and valid");
    const auto bounded = glide::ClampOffset(value.offset, value.zoom, bounds);
    Near(value.offset.x, bounded.x, "camera x remains within source desktop");
    Near(value.offset.y, bounded.y, "camera y remains within source desktop");
}
Vec2 Center(Rect b) { return {(b.left + b.right) / 2, (b.top + b.bottom) / 2}; }
Transform Step(Follow& follow, Transform view, Vec2 source, Rect bounds,
               bool enabled = true, FollowMode mode = FollowMode::Edges) {
    Transform next{999, {999, 999}};
    Check(follow.Advance(view, source, bounds, enabled, mode, Center(bounds), next),
          "production follow accepts valid source sample");
    Bounded(next, bounds);
    return next;
}

const std::array<Rect, 7> displays{{
    {0, 0, 1920, 1080}, {-1920, 0, 0, 1080}, {1920, -720, 4480, 720},
    {-1080, -1920, 0, 0}, {-3072, -100, -1152, 980},
    {-1920, -2160, 3840, 1440}, {20000, -16000, 23840, -13840}
}};

void StableZoomAnchors() {
    for (const Rect bounds : displays)
    for (const double fx : {0.1, 0.25, 0.5, 0.75, 0.9})
    for (const double fy : {0.1, 0.5, 0.9})
    for (const unsigned rate : {30u, 60u, 144u, 240u}) {
        ++paths;
        const Vec2 cursor{bounds.left + (bounds.right - bounds.left) * fx,
                          bounds.top + (bounds.bottom - bounds.top) * fy};
        Anchor anchor;
        Transform precise;
        Check(anchor.Set(precise, cursor, bounds), "first anchor starts from precise identity");
        const Vec2 source = anchor.Source(), screen = anchor.Screen();
        for (unsigned i = 0; i < 360; ++i) {
            // This reproduces repeated wheel/key commands arriving before
            // the previous zoom command is presented. The controller state is
            // retained; native output rounding must never replace it.
            const double target = i < 180 ? 1.001 + 0.025 * i : 1.001 + 0.025 * (359 - i);
            Check(anchor.Set(precise, cursor, bounds), "repeated command retains controller anchor");
            Near(anchor.Source().x, source.x, "wheel burst source anchor x does not walk");
            Near(anchor.Source().y, source.y, "wheel burst source anchor y does not walk");
            Near(anchor.Screen().x, screen.x, "wheel burst screen anchor x does not walk");
            Near(anchor.Screen().y, screen.y, "wheel burst screen anchor y does not walk");
            const double zoom = glide::animation::Quantize(target);
            Check(rate > 0, "direct step has no timer-dependent interpolation");
            Check(anchor.At(zoom, bounds, precise), "precise zoom computes anchored offset");
            const auto visible = glide::SourceToScreen(source, precise);
            Near(visible.x, screen.x, "same source stays at the same screen x throughout stepped zoom");
            Near(visible.y, screen.y, "same source stays at the same screen y throughout stepped zoom");
            Bounded(precise, bounds);
            if (i % 13 == 0) {
                glide::monitor::Frame frame;
                Check(glide::monitor::MakeFrame(bounds, precise, frame),
                      "precise anchored animation presents through production monitor geometry");
                const auto rendered = glide::SourceToScreen(source, frame.transform);
                Check(std::abs(rendered.x - screen.x) < 1.001 &&
                      std::abs(rendered.y - screen.y) < 1.001,
                      "integer capture/child rendering does not amplify anchor quantization by zoom");
                // The renderer's actual frame is evidence/output, never the
                // next controller accumulator; Set still receives precise.
            }
            ++anchorFrames;
        }
    }

    const Rect bounds{0, 0, 1920, 1080};
    Anchor anchor;
    Transform precise;
    Check(anchor.Set(precise, {960, 540}, bounds) && anchor.At(4, bounds, precise),
          "panning/reanchor fixture establishes magnification");
    const Vec2 oldScreen = anchor.Screen();
    precise.offset.x += 40; // A deliberate camera pan, not a native frame roundtrip.
    Check(anchor.Set(precise, {960, 540}, bounds), "changed precise camera rebases old identical cursor");
    Near(anchor.Screen().x, oldScreen.x - 160, "camera change cannot reuse stale anchor");
    const Vec2 nextCursor{1000, 600};
    const auto projected = glide::SourceToScreen(nextCursor, precise);
    Check(anchor.Set(precise, nextCursor, bounds), "actual source movement starts a new anchor");
    Near(anchor.Source().x, nextCursor.x, "new anchor uses current source x");
    Near(anchor.Source().y, nextCursor.y, "new anchor uses current source y");
    Near(anchor.Screen().x, projected.x, "new anchor uses unquantized projected x");
    Near(anchor.Screen().y, projected.y, "new anchor uses unquantized projected y");
    anchor.Reset();
    Check(!anchor.Valid(), "reset invalidates the anchor");
    Transform untouched{2, {12, 34}};
    Check(!anchor.At(2, bounds, untouched), "reset cannot reuse old topology/anchor");
    Same(untouched, {2, {12, 34}}, "failed anchor operation leaves caller state untouched");
}

void ZoomOutAfterPanningOrCapture() {
    for (const Rect bounds : displays)
    for (const double zoom : {1.05, 2.0, 4.0, 16.0})
    for (const bool captureClearedAnchor : {false, true})
    for (const bool cursorOutside : {false, true}) {
        ++paths;
        const Vec2 center = Center(bounds);
        Transform view = glide::AnchorZoom({}, zoom, center, bounds);
        Anchor anchor;
        Check(anchor.Set(view, center, bounds), "zoom-out fixture retains an earlier anchor");
        view.offset = glide::ClampOffset({view.offset.x + 45.25, view.offset.y - 17.125}, zoom, bounds);
        if (captureClearedAnchor) anchor.Reset();
        // This is App's deliberate zoom-out fallback for a cursor on a
        // different monitor: keep the existing monitor and choose its
        // current visible center, rather than revive an old anchor.
        const Vec2 source = cursorOutside ? glide::ScreenToSource(center, view)
            : Vec2{bounds.left + (bounds.right - bounds.left) * 0.75,
                   bounds.top + (bounds.bottom - bounds.top) * 0.40};
        Check(anchor.Set(view, source, bounds), "pan/capture state establishes a fresh zoom-out anchor");
        const auto original = view;
        Check(anchor.At(zoom, bounds, view), "zero-time zoom-out frame is computable");
        Near(view.offset.x, original.offset.x, "zoom-out command cannot jump back to old pan x");
        Near(view.offset.y, original.offset.y, "zoom-out command cannot jump back to old pan y");
        const auto screen = anchor.Screen();
        const double tinyZoom = zoom - 1e-9;
        Check(anchor.At(tinyZoom, bounds, view), "first tiny zoom-out increment is computable");
        const auto expected = glide::AnchorZoom(original, tinyZoom, screen, bounds);
        Near(view.offset.x, expected.offset.x, "zoom-out after pan/capture is continuous in x");
        Near(view.offset.y, expected.offset.y, "zoom-out after pan/capture is continuous in y");
        Check(anchor.At(1, bounds, view), "held gesture can reach identity with anchor retained");
        Same(view, {}, "anchor identity always restores ordinary desktop transform");
        Check(anchor.At(zoom, bounds, view), "same held gesture reverses from1x using retained anchor");
        Near(view.offset.x, original.offset.x, "same-hold reversal returns x without stale native rounding");
        Near(view.offset.y, original.offset.y, "same-hold reversal returns y without stale native rounding");
    }
}

void BorderSpeedAndStopping() {
    const Rect bounds{0, 0, 1920, 1080};
    const Transform original{2, {480, 270}};
    Follow follow;
    auto view = Step(follow, original, {960, 540}, bounds);
    Same(view, original, "first cursor sample seeds without camera motion");
    view = Step(follow, view, {1437.5, 540}, bounds);
    Same(view, original, "motion up to inner edge does not move camera");
    view = Step(follow, view, {1447.5, 540}, bounds);
    Near(view.offset.x, 490, "10 source pixels beyond edge pans exactly10 pixels");
    view = Step(follow, view, {1547.5, 540}, bounds);
    Near(view.offset.x, 590, "100 source pixels beyond edge pans exactly100 pixels");
    Near(glide::SourceToScreen({1547.5, 540}, view).x, 1915,
         "outward camera tracks cursor at inner screen edge");
    const auto stopped = view;
    for (unsigned i = 0; i < 10000; ++i)
        Same(Step(follow, view, {1547.5, 540}, bounds), stopped,
             "stationary edge cursor never starts a timer-driven pan or tail");
    view = Step(follow, view, {1537.5, 540}, bounds);
    Same(view, stopped, "immediate inward reversal stops camera instead of fighting movement");
    const auto movedIn = view;
    view = Step(follow, view, {1000, 540}, bounds);
    Same(view, movedIn, "moving across visible middle does not drag camera");
    view = Step(follow, view, {600, 540}, bounds);
    Near(view.offset.x, 590, "cursor twenty pixels from the left edge leaves the camera unchanged");
    Near(glide::SourceToScreen({600, 540}, view).x, 20,
         "old wide-padding region no longer pans the camera");
    view = Step(follow, view, {591, 540}, bounds);
    Near(view.offset.x, 588.5, "crossing the five-pixel left margin pans only its overshoot");
    Near(glide::SourceToScreen({591, 540}, view).x, 5,
         "left edge uses exactly five displayed pixels");
    view = Step(follow, view, {0, 0}, bounds);
    Near(view.offset.x, 0, "outer left desktop edge clamps camera");
    Near(view.offset.y, 0, "outer top desktop edge clamps camera");
    view = Step(follow, view, {1919, 1079}, bounds);
    Near(view.offset.x, 960, "fast diagonal crossing reaches rightmost source immediately");
    Near(view.offset.y, 540, "fast diagonal crossing reaches bottommost source immediately");
    ++paths;
}

void PacketAndScaleMatrix() {
    for (const Rect bounds : displays)
    for (const double zoom : {1.0, 1.001, 1.05, 1.5, 2.0, 4.0, 8.0, 16.0})
    for (const bool reverse : {false, true}) {
        const auto center = Center(bounds);
        const Transform initial = glide::AnchorZoom({}, zoom, center, bounds);
        Transform reference{};
        for (const unsigned packets : {1u, 2u, 7u, 30u, 60u, 144u, 240u, 1000u}) {
            Follow follow;
            auto view = Step(follow, initial, center, bounds);
            const Vec2 last{reverse ? bounds.left + 1 : bounds.right - 1,
                            reverse ? bounds.top + 1 : bounds.bottom - 1};
            for (unsigned i = 1; i <= packets; ++i) {
                const double t = static_cast<double>(i) / packets;
                const Vec2 cursor{center.x + (last.x - center.x) * t,
                                  center.y + (last.y - center.y) * t};
                view = Step(follow, view, cursor, bounds);
            }
            if (packets == 1) reference = view;
            Near(view.offset.x, reference.offset.x,
                 "monotone horizontal camera path is independent of timer/mouse packet rate");
            Near(view.offset.y, reference.offset.y,
                 "monotone vertical camera path is independent of timer/mouse packet rate");
            const auto stable = view;
            for (unsigned i = 0; i < 20; ++i) {
                view = Step(follow, view, last, bounds);
                Same(view, stable, "matrix idle samples remain bitwise stable");
            }
            ++paths;
        }
    }
}

void FivePixelEdgeMatrix() {
    for (const auto bounds : displays)
    for (double zoom : {2.0, 4.0, 8.0, 16.0})
    for (bool horizontal : {false, true})
    for (double direction : {-1.0, 1.0}) {
        const auto center = Center(bounds);
        const auto initial = glide::AnchorZoom({}, zoom, center, bounds);
        Follow follow;
        auto view = Step(follow, initial, center, bounds);
        const double low = horizontal ? bounds.left : bounds.top;
        const double high = horizontal ? bounds.right : bounds.bottom;
        const double threshold = direction > 0 ? high - 5 : low + 5;
        Vec2 screen = center;
        if (horizontal) screen.x = threshold - direction * 0.25;
        else screen.y = threshold - direction * 0.25;
        const auto inside = glide::ScreenToSource(screen, initial);
        view = Step(follow, view, inside, bounds);
        Near(view.offset.x, initial.offset.x, "camera remains stationary outside the five-pixel horizontal margin");
        Near(view.offset.y, initial.offset.y, "camera remains stationary outside the five-pixel vertical margin");
        if (horizontal) screen.x = threshold;
        else screen.y = threshold;
        view = Step(follow, view, glide::ScreenToSource(screen, initial), bounds);
        Near(view.offset.x, initial.offset.x, "exact five-pixel boundary has no horizontal overshoot");
        Near(view.offset.y, initial.offset.y, "exact five-pixel boundary has no vertical overshoot");
        if (horizontal) screen.x = threshold + direction * 0.5;
        else screen.y = threshold + direction * 0.5;
        const auto outside = glide::ScreenToSource(screen, initial);
        view = Step(follow, view, outside, bounds);
        const auto projected = glide::SourceToScreen(outside, view);
        Near(horizontal ? projected.x : projected.y, threshold,
             "every border tracks exactly five displayed pixels at every zoom and monitor origin");
        const double movement = horizontal ? view.offset.x - initial.offset.x : view.offset.y - initial.offset.y;
        Near(movement, direction * 0.5 / zoom,
             "five-pixel border correction converts only the current overshoot into source displacement");
        ++paths;
    }

    for (double dimension : {0.25, 1.0, 4.0, 8.0, 10.0})
    for (double zoom : {2.0, 8.0})
    for (double direction : {-1.0, 1.0}) {
        const Rect bounds{-dimension, -dimension, 0, 0};
        const auto center = Center(bounds);
        const auto initial = glide::AnchorZoom({}, zoom, center, bounds);
        Follow follow;
        auto view = Step(follow, initial, center, bounds);
        const Vec2 moved{center.x + direction * dimension / 100,
                         center.y + direction * dimension / 100};
        view = Step(follow, view, moved, bounds);
        const auto projected = glide::SourceToScreen(moved, view);
        Near(projected.x, center.x, "sub-ten-pixel viewport clamps both horizontal edge margins to half its width");
        Near(projected.y, center.y, "sub-ten-pixel viewport clamps both vertical edge margins to half its height");
        Bounded(view, bounds);
        ++paths;
    }
}

void RebasingAndCenteredMode() {
    for (const auto bounds : displays) {
        const auto center = Center(bounds);
        Follow follow;
        Anchor anchor;
        Transform view;
        const Vec2 source{bounds.left + (bounds.right - bounds.left) * 0.97,
                          bounds.top + (bounds.bottom - bounds.top) * 0.95};
        Check(anchor.Set(view, source, bounds), "edge zoom anchor");
        for (unsigned i = 0; i < 500; ++i) {
            Check(anchor.At(1 + i / 100.0, bounds, view), "animation fixture zooms near an edge");
            Same(Step(follow, view, source, bounds, false), view,
                 "source is rebased while animation/gesture has follow disabled");
        }
        const auto ended = view;
        Same(Step(follow, view, source, bounds), ended,
             "ending zoom cannot turn its projected pointer shift into camera movement");
        view = Step(follow, view, {source.x - 10, source.y - 10}, bounds);
        Same(view, ended, "first inward movement after edge zoom does not chase stale border overflow");
        const Vec2 outside{bounds.right + 100, bounds.top};
        Same(Step(follow, view, outside, bounds), view, "other monitor leaves this private view stationary");
        Same(Step(follow, view, source, bounds), view, "reentry seeds without applying cross-monitor displacement");
        follow.Reset();
        Same(Step(follow, view, center, bounds), view, "lifecycle reset discards old movement baseline");

        view = glide::AnchorZoom({}, 4, center, bounds);
        follow.Reset();
        Step(follow, view, center, bounds, true, FollowMode::Centered);
        const Vec2 moved{center.x + 10, center.y - 5};
        view = Step(follow, view, moved, bounds, true, FollowMode::Centered);
        auto screen = glide::SourceToScreen(moved, view);
        Near(screen.x, center.x, "center mode follows actual displacement immediately in x");
        Near(screen.y, center.y, "center mode follows actual displacement immediately in y");
        Same(Step(follow, view, moved, bounds, true, FollowMode::Centered), view,
             "center mode has no delayed settling after movement stops");
        ++paths;
    }
}

void InvalidAndMixedStates() {
    const Rect bounds{0, 0, 1920, 1080};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const Transform sentinel{2, {12, 34}};
    for (Rect invalid : {Rect{}, Rect{1, 1, 0, 0}, Rect{nan, 0, 100, 100},
                        Rect{0, 0, inf, 100}, Rect{-262145, 0, 1920, 1080}}) {
        Anchor anchor;
        Check(!anchor.Set({}, {10, 10}, invalid), "invalid anchor bounds rejected");
        Follow follow;
        auto out = sentinel;
        Check(!follow.Advance({}, {10, 10}, invalid, true, FollowMode::Edges, {}, out),
              "invalid follow bounds rejected");
        Same(out, sentinel, "invalid follow bounds cannot partially write transform");
    }
    for (Transform invalid : {Transform{nan, {}}, Transform{inf, {}}, Transform{0.5, {}},
                              Transform{4097, {}}, Transform{2, {nan, 0}}, Transform{2, {0, inf}},
                              Transform{2, {1e300, 0}}, Transform{2, {0, -1e300}}}) {
        Anchor anchor;
        Check(!anchor.Set(invalid, {960, 540}, bounds), "invalid anchor transform rejected");
        Follow follow;
        auto out = sentinel;
        Check(!follow.Advance(invalid, {960, 540}, bounds, true, FollowMode::Edges, {}, out),
              "invalid follow transform rejected");
        Same(out, sentinel, "invalid follow transform leaves caller output");
    }
    {
        Follow follow;
        auto out = sentinel;
        Check(!follow.Advance({}, {nan, 540}, bounds, true, FollowMode::Edges, {}, out),
              "invalid source cursor rejected");
        Check(!follow.Advance({}, {960, 540}, bounds, true, FollowMode::Centered, {inf, 0}, out),
              "invalid center rejected");
        Check(!follow.Advance({}, {960, 540}, bounds, true, static_cast<FollowMode>(99), {}, out),
              "invalid enum rejected");
        Same(out, sentinel, "invalid mixed inputs leave output");
    }

    // Deterministic mixed trace: resets, inactive zoom frames, stationary
    // frames, pointer reversals, monitor changes and both follow policies.
    unsigned long long random = 0x513c97a1u;
    auto nextRandom = [&]() { random ^= random << 13; random ^= random >> 7; random ^= random << 17; return random; };
    Follow follow;
    Rect active = bounds;
    Transform view = glide::AnchorZoom({}, 4, Center(active), active);
    Vec2 source = Center(active);
    for (unsigned i = 0; i < 50000; ++i) {
        const auto event = nextRandom() % 10;
        const auto previous = view;
        bool stationary = false, disabled = false, reset = false;
        switch (event) {
        case 0: follow.Reset(); reset = true; break;
        case 1: stationary = true; break;
        case 2: disabled = true; break;
        case 3:
            active = displays[nextRandom() % displays.size()];
            source = Center(active);
            view = glide::AnchorZoom({}, 1 + (nextRandom() % 15000) / 1000.0, source, active);
            follow.Reset(); reset = true; break;
        default:
            source.x = std::clamp(source.x + static_cast<double>(static_cast<int>(nextRandom() % 201) - 100),
                                  active.left, active.right - 1);
            source.y = std::clamp(source.y + static_cast<double>(static_cast<int>(nextRandom() % 201) - 100),
                                  active.top, active.bottom - 1);
            break;
        }
        const auto before = view;
        view = Step(follow, view, source, active, !disabled,
                    (nextRandom() & 1) ? FollowMode::Edges : FollowMode::Centered);
        if (disabled || reset) Same(view, before, "disabled/reset mixed step cannot replay motion");
        if (stationary) Same(view, previous, "stationary mixed step cannot change camera even when mode changes");
        ++mixedSteps;
    }
}

Vec2 Boost(Vec2 start, Vec2 proposed, Transform view, Rect bounds, double multiplier) {
    Vec2 output{999999, -999999};
    Check(glide::viewport::AccelerateEdges(start, proposed, view, bounds, multiplier, output),
          "valid edge acceleration proposal succeeds");
    Check(glide::detail::Finite(output), "accelerated source stays finite");
    ++acceleratedMoves;
    return output;
}

void AcceleratedBorderMath() {
    for (const auto bounds : displays)
    for (const double zoom : {1.05, 2.0, 4.0, 8.0, 16.0})
    for (const double gain : {1.0, 2.0, 4.0, 10.0})
    for (const bool horizontal : {false, true})
    for (const double direction : {-1.0, 1.0}) {
        const auto center = Center(bounds);
        const auto view = glide::AnchorZoom({}, zoom, center, bounds);
        Vec2 thresholdScreen = center;
        const double low = horizontal ? bounds.left : bounds.top;
        const double high = horizontal ? bounds.right : bounds.bottom;
        if (horizontal) thresholdScreen.x = direction > 0 ? high - 5 : low + 5;
        else thresholdScreen.y = direction > 0 ? high - 5 : low + 5;
        const auto threshold = glide::ScreenToSource(thresholdScreen, view);
        auto start = threshold, proposed = threshold;
        if (horizontal) {
            start.x -= direction * 2 / zoom;
            proposed.x += direction * 0.25 / zoom;
        } else {
            start.y -= direction * 2 / zoom;
            proposed.y += direction * 0.25 / zoom;
        }
        const auto accelerated = Boost(start, proposed, view, bounds, gain);
        Near(horizontal ? accelerated.x : accelerated.y,
             (horizontal ? threshold.x : threshold.y) + direction * gain * 0.25 / zoom,
             "only the quarter displayed pixel beyond the five-pixel border receives the multiplier");
        Near(horizontal ? accelerated.y : accelerated.x,
             horizontal ? proposed.y : proposed.x, "edge acceleration never invents orthogonal motion");
        const auto inward = Boost(proposed, start, view, bounds, gain);
        Near(inward.x, start.x, "inward x reversal gets ordinary compensated pointer motion");
        Near(inward.y, start.y, "inward y reversal gets ordinary compensated pointer motion");
        const auto stopped = Boost(accelerated, accelerated, view, bounds, gain);
        Check(glide::viewport::Same(stopped, accelerated),
              "stationary pointer has no acceleration tail even beyond the border");
        const auto inside = Boost(center, start, view, bounds, gain);
        Check(glide::viewport::Same(inside, start), "visible interior remains at the established pointer gain");
        const auto identity = Boost(start, proposed, {1, {}}, bounds, gain);
        Check(glide::viewport::Same(identity, proposed), "identity zoom never accelerates pointer movement");
        ++edgeCases;
    }
}

void AcceleratedPacketAndCameraMatrix() {
    // Model pointer input both between rendered frames and with a camera
    // update after every packet. Faster source movement and ordinary Follow
    // must agree: there is no accelerated camera pulling the pointer inward.
    for (const auto bounds : displays)
    for (const double zoom : {1.05, 2.0, 4.0, 16.0})
    for (const double gain : {1.0, 2.0, 4.0, 10.0})
    for (const double direction : {-1.0, 1.0})
    for (const unsigned axes : {1u, 2u, 3u}) {
        const auto center = Center(bounds);
        const auto initial = glide::AnchorZoom({}, zoom, center, bounds);
        const Vec2 screen{(axes & 1) ? (direction > 0 ? bounds.right - 5 : bounds.left + 5) : center.x,
                          (axes & 2) ? (direction > 0 ? bounds.bottom - 5 : bounds.top + 5) : center.y};
        const auto initialSource = glide::ScreenToSource(screen, initial);
        const double xRoom = direction > 0 ? bounds.right - 1 - initialSource.x
                                          : initialSource.x - bounds.left;
        const double yRoom = direction > 0 ? bounds.bottom - 1 - initialSource.y
                                          : initialSource.y - bounds.top;
        const double budget = std::min(16.0, std::min(xRoom, yRoom) / (gain * 4));
        Check(budget > 0, "grouping fixture stays away from physical desktop clamp");
        for (const unsigned packets : {1u, 2u, 7u, 60u, 240u})
        for (const unsigned renderEvery : {1u, 4u, 1000u}) {
            Follow follow;
            auto view = initial;
            auto source = initialSource;
            Check(follow.Rebase(source, bounds), "edge grouping seeds the source movement baseline");
            for (unsigned i = 1; i <= packets; ++i) {
                const Vec2 base{source.x + ((axes & 1) ? direction * budget / packets : 0),
                                source.y + ((axes & 2) ? direction * budget / packets : 0)};
                const auto previous = source;
                source = Boost(source, base, view, bounds, gain);
                Check(direction * (source.x - previous.x) >= -1e-8 &&
                      direction * (source.y - previous.y) >= -1e-8,
                      "accelerated pointer cannot reverse during monotone edge travel");
                if (i % renderEvery == 0 || i == packets) {
                    const auto priorView = view;
                    view = Step(follow, view, source, bounds);
                    Check(direction * (view.offset.x - priorView.offset.x) >= -1e-8 &&
                          direction * (view.offset.y - priorView.offset.y) >= -1e-8,
                          "following camera cannot alternate direction or sawtooth at the edge");
                    const auto projected = glide::SourceToScreen(source, view);
                    Near(projected.x, screen.x, "accelerated source and camera preserve the displayed x border", 1e-6);
                    Near(projected.y, screen.y, "accelerated source and camera preserve the displayed y border", 1e-6);
                    Same(Step(follow, view, source, bounds), view,
                         "stopping after an accelerated packet never advances the camera");
                }
            }
            Near(source.x, initialSource.x + ((axes & 1) ? direction * gain * budget : 0),
                 "horizontal gain is independent of input grouping and camera cadence", 1e-6);
            Near(source.y, initialSource.y + ((axes & 2) ? direction * gain * budget : 0),
                 "vertical gain is independent of input grouping and camera cadence", 1e-6);
            Near(view.offset.x, initial.offset.x + ((axes & 1) ? direction * gain * budget : 0),
                 "camera horizontal travel follows the entire accelerated source displacement", 1e-6);
            Near(view.offset.y, initial.offset.y + ((axes & 2) ? direction * gain * budget : 0),
                 "camera vertical travel follows the entire accelerated source displacement", 1e-6);
            auto inward = source;
            if (axes & 1) inward.x -= direction * 0.25;
            if (axes & 2) inward.y -= direction * 0.25;
            Check(glide::viewport::Same(Boost(source, inward, view, bounds, gain), inward),
                  "first inward movement after every grouped accelerated path is not boosted");
            Same(Step(follow, view, inward, bounds), view,
                 "reversing inward immediately releases the edge camera without a latch");
            ++edgeCases;
        }
    }
}

void AcceleratedBoundariesAndFailures() {
    for (const auto bounds : displays)
    for (const double zoom : {1.05, 2.0, 16.0})
    for (const double gain : {1.0, 2.0, 10.0}) {
        const auto center = Center(bounds);
        const auto view = glide::AnchorZoom({}, zoom, center, bounds);
        const Vec2 right{bounds.right - 2, bounds.bottom - 2};
        const Vec2 base{bounds.right - 1.5, bounds.bottom - 1.5};
        const auto boosted = Boost(right, base, view, bounds, gain);
        Check(boosted.x >= base.x && boosted.y >= base.y &&
              boosted.x <= bounds.right - 1 && boosted.y <= bounds.bottom - 1,
              "bonus stops at last in-bounds integer pixel without slowing ordinary motion");
        const Vec2 almostOutside{bounds.right - 0.1, bounds.bottom - 0.1};
        Check(glide::viewport::Same(Boost(right, almostOutside, view, bounds, gain), almostOutside),
              "already fractional ordinary exit is never shortened or pulled inward");
        const Vec2 left{bounds.left + 2, bounds.top + 2};
        const Vec2 leftBase{bounds.left + 1.5, bounds.top + 1.5};
        const auto leftBoosted = Boost(left, leftBase, view, bounds, gain);
        Check(leftBoosted.x <= leftBase.x && leftBoosted.y <= leftBase.y &&
              leftBoosted.x >= bounds.left && leftBoosted.y >= bounds.top,
              "negative bonus remains within physical monitor bounds");
        for (const Vec2 outside : {Vec2{bounds.right, center.y}, Vec2{bounds.left - 1, center.y},
                                  Vec2{center.x, bounds.bottom}, Vec2{center.x, bounds.top - 1}}) {
            Check(glide::viewport::Same(Boost(center, outside, view, bounds, gain), outside),
                  "base movement leaving selected monitor retains exact original target");
            Check(glide::viewport::Same(Boost(outside, center, view, bounds, gain), center),
                  "entry from another monitor receives no fabricated edge bonus");
        }
        ++edgeCases;
    }
    for (const double dimension : {0.25, 1.0, 4.0, 8.0, 10.0}) {
        const Rect bounds{-dimension, -dimension, 0, 0};
        const auto center = Center(bounds);
        const auto view = glide::AnchorZoom({}, 4, center, bounds);
        const Vec2 next{center.x + dimension / 100, center.y - dimension / 100};
        const auto boosted = Boost(center, next, view, bounds, 10);
        Check(glide::viewport::Contains(bounds, boosted), "tiny synthetic display cannot reverse a clamp interval");
        Check(boosted.x >= next.x && boosted.y <= next.y, "tiny display bonus cannot oppose base displacement");
        ++edgeCases;
    }
    const Rect bounds{0, 0, 1920, 1080};
    const Transform view{4, {720, 405}};
    const Vec2 sentinel{12, 34};
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const double invalid : {nan, inf, -inf, 0.0, -1.0, 0.999, 10.001, 1e300}) {
        auto output = sentinel;
        Check(!glide::viewport::AccelerateEdges({960, 540}, {961, 540}, view, bounds, invalid, output),
              "malformed or out-of-range acceleration is rejected");
        Check(glide::viewport::Same(output, sentinel), "invalid acceleration cannot partially write output");
    }
    for (const Transform invalid : {Transform{nan, {}}, Transform{0.5, {}}, Transform{4, {inf, 0}},
                                   Transform{4, {0, 262145}}}) {
        auto output = sentinel;
        Check(!glide::viewport::AccelerateEdges({960, 540}, {961, 540}, invalid, bounds, 4, output),
              "invalid applied transform cannot participate in edge acceleration");
        Check(glide::viewport::Same(output, sentinel), "invalid edge transform leaves output untouched");
    }
    auto output = sentinel;
    Check(!glide::viewport::AccelerateEdges({nan, 540}, {961, 540}, view, bounds, 4, output),
          "invalid edge movement start is rejected");
    Check(!glide::viewport::AccelerateEdges({960, 540}, {961, inf}, view, bounds, 4, output),
          "invalid edge movement endpoint is rejected");
    Check(!glide::viewport::AccelerateEdges({960, 540}, {961, 540}, view, {}, 4, output),
          "invalid edge movement bounds are rejected");
    Check(glide::viewport::Same(output, sentinel), "all malformed edge proposals are atomic failures");

    for (const Vec2 invalidCarry : {Vec2{nan, 0}, Vec2{0, inf}, Vec2{0.50001, 0}, Vec2{0, -0.50001}}) {
        output = sentinel;
        Check(!glide::viewport::AccelerateEdges({960, 540}, {961, 540}, view, bounds, 4,
                                               output, invalidCarry, 1),
              "invalid previously processed fractional distance is rejected");
        Check(glide::viewport::Same(output, sentinel), "invalid fractional carry leaves output untouched");
    }
    for (const double invalidRounding : {nan, inf, -0.001, 1.001}) {
        output = sentinel;
        Check(!glide::viewport::AccelerateEdges({960, 540}, {961, 540}, view, bounds, 4,
                                               output, {}, invalidRounding),
              "rounding uncertainty cannot exceed its documented one-output-pixel bound");
        Check(glide::viewport::Same(output, sentinel), "invalid rounding policy leaves output untouched");
    }
}

void EdgeContactRoundingBound() {
    for (const auto bounds : displays)
    for (const double zoom : {1.01, 1.05, 3.17, 16.0})
    for (const bool horizontal : {false, true})
    for (const double direction : {-1.0, 1.0}) {
        const auto center = Center(bounds);
        const auto view = glide::AnchorZoom({}, zoom, center, bounds);
        auto border = center;
        if (horizontal) border.x = direction > 0 ? bounds.right - 5 : bounds.left + 5;
        else border.y = direction > 0 ? bounds.bottom - 5 : bounds.top + 5;
        const auto edge = glide::ScreenToSource(border, view);
        for (const double gapPixels : {0.999, 1.001}) {
            auto start = edge, proposed = edge;
            if (horizontal) {
                start.x -= direction * gapPixels / zoom;
                proposed.x = start.x + direction * 0.25 / zoom;
            } else {
                start.y -= direction * gapPixels / zoom;
                proposed.y = start.y + direction * 0.25 / zoom;
            }
            Vec2 output;
            Check(glide::viewport::AccelerateEdges(start, proposed, view, bounds, 4, output, {}, 1),
                  "quantized contact uncertainty remains a valid source movement proposal");
            const double expectedGain = gapPixels < 1 ? 4 : 1;
            Near(horizontal ? output.x - start.x : output.y - start.y,
                 direction * expectedGain * 0.25 / zoom,
                 "quantized edge contact tolerance covers at most one displayed pixel at every zoom and origin");
            ++edgeCases;
        }
        for (const double carry : {-0.5, -0.25, 0.0, 0.25, 0.5}) {
            Vec2 retained{}, start = edge, proposed = edge;
            if (horizontal) { retained.x = carry; start.x += carry; proposed.x = start.x + direction * 0.25 / zoom; }
            else { retained.y = carry; start.y += carry; proposed.y = start.y + direction * 0.25 / zoom; }
            Vec2 output;
            Check(glide::viewport::AccelerateEdges(start, proposed, view, bounds, 4, output, retained, 1),
                  "already processed residual remains separate from new edge movement");
            Near(horizontal ? output.x - start.x : output.y - start.y,
                 direction / zoom, "negative or positive carried residual never changes the new movement multiplier");
            ++edgeCases;
        }
    }
}
}

int Run_viewport_motion() {
    StableZoomAnchors();
    ZoomOutAfterPanningOrCapture();
    BorderSpeedAndStopping();
    PacketAndScaleMatrix();
    FivePixelEdgeMatrix();
    RebasingAndCenteredMode();
    InvalidAndMixedStates();
    AcceleratedBorderMath();
    AcceleratedPacketAndCameraMatrix();
    AcceleratedBoundariesAndFailures();
    EdgeContactRoundingBound();
    std::cout << "PASS: " << assertions << " viewport motion assertions; " << anchorFrames
              << " continuous anchor frames; " << paths << " pointer/zoom paths; " << mixedSteps
              << " mixed lifecycle/topology/movement steps; " << edgeCases
              << " edge acceleration cases; " << acceleratedMoves << " accelerated movement proposals\n";
    return 0;
}
