#include <stdexcept>
#include "../GlideZoom/monitor_geometry.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {
unsigned long long assertions = 0, frames = 0, motionFrames = 0, replayFrames = 0, mixedFrames = 0;
void Check(bool condition, const std::string& message) {
    ++assertions;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
void Near(double actual, double expected, const std::string& message, double tolerance = 1e-8) {
    Check(std::isfinite(actual) && std::abs(actual - expected) <= tolerance, message);
}
bool Equal(const glide::monitor::Frame& a, const glide::monitor::Frame& b) {
    return a.transform.zoom == b.transform.zoom && a.transform.offset.x == b.transform.offset.x &&
        a.transform.offset.y == b.transform.offset.y && a.source == b.source && a.destination == b.destination &&
        a.capture == b.capture && a.child == b.child;
}
void Inspect(glide::Rect monitor, glide::Transform requested) {
    using namespace glide;
    monitor::Frame frame{};
    Check(monitor::MakeFrame(monitor, requested, frame), "valid monitor frame builds");
    ++frames;
    Check(monitor::ValidFrame(frame), "frame is valid");
    Check(frame.destination == session::Rect{static_cast<std::int32_t>(monitor.left),
        static_cast<std::int32_t>(monitor.top), static_cast<std::int32_t>(monitor.right),
        static_cast<std::int32_t>(monitor.bottom)}, "destination is exactly the selected monitor");
    Check(frame.source.left >= monitor.left && frame.source.top >= monitor.top &&
        frame.source.right <= monitor.right && frame.source.bottom <= monitor.bottom,
        "touch mapping cannot include another monitor or a desktop gap");
    Check(frame.capture.left >= monitor.left && frame.capture.top >= monitor.top &&
        frame.capture.right <= monitor.right && frame.capture.bottom <= monitor.bottom,
        "overscan capture cannot include another monitor or a desktop gap");
    const Vec2 visualTopLeft = ScreenToSource({monitor.left, monitor.top}, frame.transform);
    Near((visualTopLeft.x - frame.capture.left) * frame.transform.zoom + frame.child.left, 0,
         "clipped child maps its captured pixels to host left", 1e-6);
    Near((visualTopLeft.y - frame.capture.top) * frame.transform.zoom + frame.child.top, 0,
         "clipped child maps its captured pixels to host top", 1e-6);
    const Vec2 visualBottomRight = ScreenToSource({monitor.right, monitor.bottom}, frame.transform);
    Check(visualBottomRight.x <= frame.capture.right + 1e-8 &&
        visualBottomRight.y <= frame.capture.bottom + 1e-8, "overscan covers every output pixel");
    Check(frame.capture.right - visualBottomRight.x < 1.00000001 &&
        frame.capture.bottom - visualBottomRight.y < 1.00000001, "ceil capture adds less than one source pixel");
    monitor::Frame replay{};
    Check(monitor::MakeFrame(monitor, frame.transform, replay) && Equal(frame, replay),
          "reapplying captured actual transform reconstructs exact native frame");
    ++replayFrames;
    for (double fx : {0.0, 0.01, 0.25, 0.5, 0.75, 0.99, 0.999999}) {
        for (double fy : {0.0, 0.01, 0.25, 0.5, 0.75, 0.99, 0.999999}) {
            const Vec2 screen{monitor.left + (monitor.right - monitor.left) * fx,
                              monitor.top + (monitor.bottom - monitor.top) * fy};
            Vec2 mapped{999999, 999999};
            Check(monitor::MapInput(frame, screen, mapped), "selected-monitor point maps");
            Check(monitor::Contains(monitor, mapped), "input source stays on the same monitor");
            const Vec2 visual = ScreenToSource(screen, frame.transform);
            Check(std::abs(mapped.x - visual.x) < 1.00000001 &&
                std::abs(mapped.y - visual.y) < 1.00000001, "input versus image error is bounded in source pixels");
            const auto displayed = SourceToScreen(mapped, frame.transform);
            Check(std::abs(displayed.x - screen.x) < frame.transform.zoom + 1e-7 &&
                std::abs(displayed.y - screen.y) < frame.transform.zoom + 1e-7,
                "fractional input mapping mismatch is bounded in display pixels");
        }
    }
    for (const Vec2 outside : {Vec2{monitor.left - 1, monitor.top}, Vec2{monitor.right, monitor.top},
            Vec2{monitor.left, monitor.top - 1}, Vec2{monitor.left, monitor.bottom}}) {
        Vec2 unchanged{123.25, -78.5};
        Check(!monitor::MapInput(frame, outside, unchanged), "outside monitor is not remapped");
        Near(unchanged.x, 123.25, "outside x left unchanged");
        Near(unchanged.y, -78.5, "outside y left unchanged");
    }
    if (requested.zoom == 1) {
        Check(frame.source == frame.destination, "unity crop equals original monitor");
        Near(frame.transform.offset.x, 0, "unity global x offset");
        Near(frame.transform.offset.y, 0, "unity global y offset");
    }
}
}

int Run_monitor_geometry() {
    using namespace glide;
    const std::array<Rect, 9> monitors{{
        {0, 0, 1920, 1080}, {-1920, 0, 0, 1080}, {1920, 0, 4480, 1440},
        {-1080, -1920, 0, 0}, {300, -2160, 4140, 0}, {-3072, -100, -1152, 980},
        {8000, 7000, 8001, 7001}, {-262144, -262144, -1, -1}, {1, 1, 262144, 262144}}};
    for (const auto bounds : monitors) {
        for (double zoom : {1.0, 1.0001, 1.05, 1.25, 1.99999, 2.0, 7.5, 8.0, 16.0, 4096.0}) {
            for (const Vec2 pan : {Vec2{}, Vec2{-1000000, -1000000}, Vec2{1000000, 1000000},
                    Vec2{123.4999, -98.5001}, Vec2{-1e200, 1e200}})
                Inspect(bounds, {zoom, pan});
            for (const Vec2 anchor : {Vec2{bounds.left, bounds.top},
                    Vec2{bounds.right - 1, bounds.bottom - 1},
                    Vec2{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2}}) {
                const Transform ideal = AnchorZoom({}, zoom, anchor, bounds);
                monitor::Frame frame{};
                Check(monitor::MakeFrame(bounds, ideal, frame), "anchored monitor frame builds");
                const Vec2 idealSource = ScreenToSource(anchor, {frame.transform.zoom,
                    ClampOffset(ideal.offset, frame.transform.zoom, bounds)});
                const Vec2 actual = ScreenToSource(anchor, frame.transform);
                Check(std::abs(actual.x - idealSource.x) * frame.transform.zoom < 1.0000001 &&
                    std::abs(actual.y - idealSource.y) * frame.transform.zoom < 1.0000001,
                    "quantization moves anchor by less than one DISPLAY pixel at every zoom");
            }
        }
    }

    // Regression scene: three maximized windows use disjoint labels. No pixel
    // in a selected monitor may sample a neighbor's label in monitor mode.
    const std::array<Rect, 3> triple{{{-1920, 0, 0, 1080}, {0, 0, 1920, 1080}, {1920, 0, 3840, 1080}}};
    auto label = [&](Vec2 source) {
        for (std::size_t i = 0; i < triple.size(); ++i)
            if (monitor::Contains(triple[i], source)) return static_cast<int>(i);
        return -1;
    };
    for (std::size_t selected = 0; selected < triple.size(); ++selected) {
        const auto bounds = triple[selected];
        for (double zoom : {1.05, 1.5, 2.0, 8.0}) {
            for (double pan : {-1e5, 0.0, 1e5}) {
                monitor::Frame frame{};
                Check(monitor::MakeFrame(bounds, {zoom, {pan, 0}}, frame), "triple display frame");
                for (int y = 0; y < 1080; y += 19) for (int x = 0; x < 1920; x += 17) {
                    const Vec2 screen{bounds.left + x, bounds.top + y};
                    Check(label(ScreenToSource(screen, frame.transform)) == static_cast<int>(selected),
                        "monitor output cannot mix maximized windows from two monitors");
                }
                for (std::size_t other = 0; other < triple.size(); ++other) if (other != selected) {
                    Vec2 output{-999999, -999999};
                    Check(!monitor::MapInput(frame, {(triple[other].left + triple[other].right) / 2, 540}, output),
                        "nonselected maximized windows keep their original input mapping");
                }
            }
        }
    }
    const Transform desktopZoom{2, ClampOffset({}, 2, {-1920, 0, 3840, 1080})};
    Check(label(ScreenToSource({1920, 540}, desktopZoom)) == 1 &&
          label(ScreenToSource({3839, 540}, desktopZoom)) == 1,
          "desktop mode deliberately permits neighboring monitor content");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    monitor::Frame sentinel;
    Check(monitor::MakeFrame({0, 0, 200, 200}, {2, {7, 11}}, sentinel), "valid rejection sentinel");
    for (Rect bad : {Rect{}, Rect{1, 0, 0, 1}, Rect{0, 0, 1, 0}, Rect{0.5, 0, 100, 100},
            Rect{nan, 0, 100, 100}, Rect{0, 0, inf, 100}, Rect{-262145, 0, 0, 100},
            Rect{0, 0, 262145, 100}, Rect{-262144, 0, 262144, 100}}) {
        auto output = sentinel;
        Check(!monitor::MakeFrame(bad, {}, output), "invalid topology rejected");
        Check(Equal(output, sentinel), "invalid topology does not overwrite previous frame");
    }
    for (Transform bad : {Transform{0.99, {}}, Transform{4096.1, {}}, Transform{nan, {}},
            Transform{inf, {}}, Transform{2, {nan, 0}}, Transform{2, {0, inf}}}) {
        auto output = sentinel;
        Check(!monitor::MakeFrame(monitors[0], bad, output), "invalid transform rejected");
        Check(Equal(output, sentinel), "invalid transform does not overwrite previous frame");
    }
    for (Vec2 bad : {Vec2{nan, 0}, Vec2{0, inf}}) {
        Vec2 output{7, 8};
        Check(!monitor::MapInput(sentinel, bad, output), "invalid pointer rejected");
        Near(output.x, 7, "invalid pointer leaves x"); Near(output.y, 8, "invalid pointer leaves y");
    }
    auto corrupt = sentinel;
    corrupt.source.left = -1;
    Vec2 output{7, 8};
    Check(!monitor::MapInput(corrupt, {10, 10}, output), "crop outside selected monitor rejected");

    // A constant anchor while zoom rises smoothly used to move backward each
    // time the rounded SOURCE origin crossed an integer. Check the real frame
    // layout against an independent output-pixel oracle, including replay used
    // by screenshot restoration. No mocked renderer simply agrees with itself.
    for (const auto bounds : monitors) {
        const Vec2 anchor{bounds.left + (bounds.right - bounds.left) * 0.371,
                          bounds.top + (bounds.bottom - bounds.top) * 0.613};
        for (unsigned step = 0; step <= 3072; ++step) {
            const double zoom = static_cast<float>(1 + step / 204.8);
            const Transform wanted = AnchorZoom({}, zoom, anchor, bounds);
            monitor::Frame frame, replay;
            Check(monitor::MakeFrame(bounds, wanted, frame), "continuous anchored scale builds");
            const Vec2 displayed{
                bounds.left + frame.child.left + (anchor.x - frame.capture.left) * zoom,
                bounds.top + frame.child.top + (anchor.y - frame.capture.top) * zoom};
            // A synthetic 1x1 monitor can hit the far-edge feasibility clamp
            // even for an interior anchor; ordinary displays retain nearest
            // output-pixel error. Both stay independent of the zoom factor.
            const double bound = bounds.right - bounds.left == 1 ? 1.000001 : 0.500001;
            Check(std::abs(displayed.x - anchor.x) <= bound &&
                  std::abs(displayed.y - anchor.y) <= bound,
                  "stationary anchor never develops zoom-amplified jitter");
            Check(monitor::MakeFrame(bounds, frame.transform, replay) && Equal(frame, replay),
                  "animated frame replay is bit-exact for restore ownership");
            ++motionFrames; ++replayFrames;
        }
    }
    // Smooth sub-source-pixel panning at a settled zoom must remain monotonic.
    // The step is independently bounded by requested motion plus one output
    // quantization pixel, instead of a full source pixel multiplied by zoom.
    for (const Rect bounds : {Rect{0, 0, 3840, 2160}, Rect{-3840, -2160, 0, 0},
                              Rect{300, -100, 4140, 2060}}) {
        for (double zoom : {1.05, 2.0, 8.0, 16.0, 4096.0}) {
            zoom = static_cast<float>(zoom);
            Vec2 previous{};
            for (unsigned step = 0; step <= 4096; ++step) {
                const Vec2 desiredOrigin{bounds.left + 1 + step / 64.0,
                                         bounds.top + 1 + step / 64.0};
                const Transform wanted{zoom, {desiredOrigin.x - bounds.left / zoom,
                                              desiredOrigin.y - bounds.top / zoom}};
                monitor::Frame frame, replay;
                Check(monitor::MakeFrame(bounds, wanted, frame), "subpixel pan frame builds");
                const Vec2 actualOrigin{frame.capture.left - frame.child.left / zoom,
                                        frame.capture.top - frame.child.top / zoom};
                if (step) {
                    Check(actualOrigin.x >= previous.x && actualOrigin.y >= previous.y,
                          "continuous pan cannot reverse due to capture or output rounding");
                    Check((actualOrigin.x - previous.x) * zoom <= zoom / 64 + 1.000001 &&
                          (actualOrigin.y - previous.y) * zoom <= zoom / 64 + 1.000001,
                          "no zoom-sized jump at integer source boundaries");
                }
                const Vec2 idealOrigin = ScreenToSource({bounds.left, bounds.top},
                    {zoom, ClampOffset(wanted.offset, zoom, bounds)});
                Check(std::abs(actualOrigin.x - idealOrigin.x) * zoom <= 1.000001 &&
                      std::abs(actualOrigin.y - idealOrigin.y) * zoom <= 1.000001,
                      "pan rendering error is bounded in OUTPUT pixels");
                Check(monitor::MakeFrame(bounds, frame.transform, replay) && Equal(frame, replay),
                      "subpixel pan replay is bit-exact");
                previous = actualOrigin;
                ++motionFrames; ++replayFrames;
            }
        }
    }

    for (unsigned variant = 0; variant < 8; ++variant) {
        auto bad = sentinel;
        if (variant == 0) bad.capture.left = -1;
        if (variant == 1) bad.capture.right = bad.capture.left;
        if (variant == 2) bad.child.left = -4097;
        if (variant == 3) bad.child.top = 1;
        if (variant == 4) bad.child.right++;
        if (variant == 5) bad.transform.offset.x += 0.001;
        if (variant == 6) bad.source.left++;
        if (variant == 7) bad.source.bottom--;
        Check(!monitor::ValidFrame(bad), "inconsistent capture/child/actual transform rejected");
    }

    // Deterministic mixed topology/float-scale replay exercises cancellation
    // near source integer boundaries beyond the hand-picked desktop layouts.
    // Reconstructing a saved frame must retain exact restoration ownership.
    std::uint64_t seed = 0x708ea14c2e9d;
    const auto random = [&seed]() {
        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed;
    };
    for (unsigned i = 0; i < 250000; ++i) {
        const int width = 1 + random() % 262144, height = 1 + random() % 262144;
        const int left = -262144 + static_cast<int>(random() % (524289 - width));
        const int top = -262144 + static_cast<int>(random() % (524289 - height));
        const Rect bounds{static_cast<double>(left), static_cast<double>(top),
                          static_cast<double>(left + width), static_cast<double>(top + height)};
        const double zoom = static_cast<float>(std::exp((random() % 1000001) / 1e6 * std::log(4096.0)));
        const Vec2 anchor{left + width * ((random() % 1000001) / 1e6),
                          top + height * ((random() % 1000001) / 1e6)};
        monitor::Frame frame, replay;
        Check(monitor::MakeFrame(bounds, AnchorZoom({}, zoom, anchor, bounds), frame),
              "mixed bounded geometry builds");
        Check(monitor::MakeFrame(bounds, frame.transform, replay) && Equal(frame, replay),
              "mixed captured transform replays exactly");
        const Vec2 displayed{
            bounds.left + frame.child.left + (anchor.x - frame.capture.left) * zoom,
            bounds.top + frame.child.top + (anchor.y - frame.capture.top) * zoom};
        Check(std::abs(displayed.x - anchor.x) <= 1.000002 &&
              std::abs(displayed.y - anchor.y) <= 1.000002,
              "mixed offset quantization remains bounded in output pixels");
        ++mixedFrames; ++replayFrames;
    }

    std::cout << "PASS: " << assertions << " monitor geometry assertions; " << frames
              << " parameter frames; " << motionFrames << " continuous zoom/pan frames; " << replayFrames
              << " exact replay frames; " << mixedFrames
              << " seeded mixed cases; triple-monitor isolation, output-pixel jitter and input bounds\n";
    return 0;
}
