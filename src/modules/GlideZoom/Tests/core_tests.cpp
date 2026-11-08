#include <stdexcept>
#include "../GlideZoom/zoom_core.h"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
int assertions = 0;

void Check(bool result, const std::string& message) {
    ++assertions;
    if (!result) {
        std::cerr << "FAIL: " << message << '\n';
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}

void Near(double actual, double expected, const std::string& message,
          double tolerance = 1e-9) {
    Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance,
          message + " (actual=" + std::to_string(actual) +
          ", expected=" + std::to_string(expected) + ")");
}

void Near(glide::Vec2 actual, glide::Vec2 expected, const std::string& message,
          double tolerance = 1e-9) {
    Near(actual.x, expected.x, message + " x", tolerance);
    Near(actual.y, expected.y, message + " y", tolerance);
}
} // namespace

int Run_core() {
    using namespace glide;
    const Rect primary{0, 0, 1920, 1080};
    const Rect multi{-1920, -1080, 2560, 1440};

    Near(ClampZoom(0.1), 1, "minimum zoom");
    Near(ClampZoom(9), 8, "default maximum zoom");
    Near(ClampZoom(12, 16), 12, "custom maximum zoom");
    Near(ClampZoom(std::numeric_limits<double>::quiet_NaN()), 1, "NaN zoom is safe");
    Near(ClampZoom(std::numeric_limits<double>::infinity()), 8, "infinite zoom is capped");

    Near(ScreenToSource({500, 300}, {2, {100, 200}}), {350, 350}, "known source location");
    Near(SourceToScreen({350, 350}, {2, {100, 200}}), {500, 300}, "known screen location");
    for (const double zoom : {1.0, 1.25, 2.0, 8.0, 16.0}) {
        for (const Vec2 p : {Vec2{-1800, -900}, Vec2{0, 0}, Vec2{2500, 1400}}) {
            const Transform t{zoom, {-175.25, 91.5}};
            Near(ScreenToSource(SourceToScreen(p, t), t), p, "transform round trip");
        }
    }

    Near(ClampOffset({5000, -100}, 2, primary), {960, 0}, "primary desktop limits");
    Near(ClampOffset({-5000, 5000}, 2, multi), {-960, 720}, "negative desktop limits");
    Near(ClampOffset({5000, -5000}, 2, multi), {1280, -540}, "opposite desktop limits");
    Near(ClampOffset({100, -400}, 1, multi), {0, 0}, "unity always has zero offset");
    Near(ClampOffset({100, 200}, 2, {}), {0, 0}, "invalid desktop fails safe");

    // Every visible output pixel must map inside the desktop bounding rectangle,
    // including arrangements with screens above or left of the primary display.
    for (const double zoom : {1.0, 1.33, 2.0, 7.5}) {
        for (const Vec2 raw : {Vec2{-9000, -9000}, Vec2{0, 0}, Vec2{9000, 9000}}) {
            const Transform t{zoom, ClampOffset(raw, zoom, multi)};
            const Vec2 tl = ScreenToSource({multi.left, multi.top}, t);
            const Vec2 br = ScreenToSource({multi.right, multi.bottom}, t);
            Check(tl.x >= multi.left - 1e-8 && tl.y >= multi.top - 1e-8 &&
                  br.x <= multi.right + 1e-8 && br.y <= multi.bottom + 1e-8,
                  "visible source stays in desktop bounds");
        }
    }

    const Vec2 anchor{-1200, 800};
    const Transform anchored = AnchorZoom({}, 2, anchor, multi);
    Near(anchored.offset, {-600, 400}, "negative-origin anchor offset");
    Near(SourceToScreen(anchor, anchored), anchor, "point under cursor stays fixed");
    const Transform reanchored = AnchorZoom(anchored, 3.5, anchor, multi);
    Near(ScreenToSource(anchor, reanchored), ScreenToSource(anchor, anchored),
         "anchor remains unchanged across repeated zoom");
    Near(AnchorZoom(reanchored, 1, anchor, multi).offset, {}, "zoom out restores origin");
    Near(AnchorZoom({}, 16, {960, 540}, primary).zoom, 16, "geometry allows configured limits above 8x");

    Near(WheelZoom(2, 120), 2.24, "wheel up");
    Near(WheelZoom(WheelZoom(2, 120), -120), 2, "wheel inverse");
    double partial = 2;
    for (int i = 0; i < 8; ++i) partial = WheelZoom(partial, 15);
    Near(partial, WheelZoom(2, 120), "high-resolution wheel preserves fractional deltas");
    Near(WheelZoom(7, 12000), 8, "large wheel delta clamps safely");
    Near(WheelZoom(2, -12000), 1, "large negative wheel delta clamps safely");
    Near(DragZoom(2, -220), 4, "upward drag doubles");
    Near(DragZoom(4, 220), 2, "downward drag halves");
    Near(DragZoom(2, -1e200), 8, "extreme drag cannot overflow result");
    Near(DragZoom(2, 1e200), 1, "extreme downward drag stops at unity");

    const double oneStep = SmoothZoom(1, 4, 0.2);
    double manySteps = 1;
    for (int i = 0; i < 20; ++i) manySteps = SmoothZoom(manySteps, 4, 0.01);
    Near(manySteps, oneStep, "smoothing is independent of frame subdivision");
    Near(SmoothZoom(2, 4, 0), 2, "zero elapsed time does not advance");
    Near(SmoothZoom(2, 4, 0.1, 0), 4, "zero smoothing duration is immediate");
    double up = 1, down = 8;
    for (int i = 0; i < 100; ++i) {
        const double nextUp = SmoothZoom(up, 8, 1.0 / 144.0);
        const double nextDown = SmoothZoom(down, 1, 1.0 / 30.0);
        Check(nextUp >= up && nextUp <= 8, "zoom-in smoothing is monotonic without overshoot");
        Check(nextDown <= down && nextDown >= 1, "zoom-out smoothing is monotonic without overshoot");
        up = nextUp;
        down = nextDown;
    }

    DoubleTapHold gesture;
    Check(!gesture.Down(100, {50, 50}), "first down does not activate");
    Check(!gesture.Down(150, {50, 50}), "repeat down does not activate");
    gesture.Up(200);
    Check(gesture.Down(350, {52, 51}), "second down activates");
    Check(gesture.Active() && gesture.Held(), "active second press is held");
    Check(!gesture.Down(370, {52, 51}), "repeat of second down does not reactivate");
    gesture.Move({52, -500});
    Check(gesture.Active(), "movement during hold is allowed");
    gesture.Up(1000);
    Check(!gesture.Active() && !gesture.Held(), "release ends gesture");
    Check(!gesture.Down(1100, {52, -500}), "release of active gesture does not arm a triple tap");
    gesture.Reset();

    gesture.Down(100, {});
    gesture.Up(351);
    Check(!gesture.Down(400, {}), "long first hold does not count as a tap");
    gesture.Reset();

    gesture.Down(100, {});
    gesture.Up(150);
    Check(!gesture.Down(551, {}), "second down outside time window is rejected");
    gesture.Reset();

    gesture.Down(100, {});
    gesture.Move({7, 0});
    gesture.Move({0, 0});
    gesture.Up(150);
    Check(!gesture.Down(250, {}), "first drag cannot become a tap by returning to its origin");
    gesture.Reset();

    gesture.Down(100, {});
    gesture.Up(150);
    Check(!gesture.Down(250, {7, 0}), "spatially distant second tap rejected");
    gesture.Reset();

    gesture.Down(100, {});
    gesture.Up(150);
    Check(!gesture.Down(50, {}), "backward timestamps do not underflow");
    gesture.Reset();

    gesture.Down(100, {});
    gesture.Up(150);
    Check(gesture.Down(550, {6, 0}), "time and movement boundaries are inclusive");
    gesture.Reset();
    Check(!gesture.Active() && !gesture.Held(), "reset clears active held state");

    std::cout << "PASS: " << assertions << " core assertions\n";
    return 0;
}
