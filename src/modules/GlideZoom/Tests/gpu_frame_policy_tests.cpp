#include <stdexcept>
#include "../GlideZoom/gpu_frame_policy.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using glide::gpu::FrameGate;
using glide::gpu::FrameState;
using glide::gpu::SamplingRect;
std::uint64_t assertions = 0, scenarios = 0, samplingFrames = 0, sampledPoints = 0;

void Check(bool value, const char* message) {
    ++assertions;
    if (!value) {
        std::cerr << "FAIL GPU frame policy: " << message << " [scenario=" << scenarios << "]\n";
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}

bool Near(double left, double right, double tolerance = 1e-7) {
    return std::abs(left - right) <= tolerance;
}

void CaptureReadiness() {
    ++scenarios;
    FrameGate gate;
    Check(gate.State(0) == FrameState::Closed && !gate.Ready() &&
          !gate.Ready(0) && !gate.Accept(0, 1920, 1080), "default gate cannot expose an absent image");
    const auto first = gate.Begin(1920, 1080, 100);
    Check(first != 0 && gate.Generation() == first && gate.State(100) == FrameState::Pending &&
          !gate.Ready(), "begin starts a distinct pending capture generation");
    Check(!gate.Accept(first + 1, 1920, 1080) && !gate.Accept(0, 1920, 1080) &&
          !gate.Accept(first, 1919, 1080) && !gate.Accept(first, 1920, 1079) &&
          !gate.Accept(first, -1920, 1080), "wrong generation or dimensions cannot initialize the frame");
    Check(gate.State(2099) == FrameState::Pending, "deadline has not elapsed one millisecond before two seconds");
    Check(gate.Accept(first, 1920, 1080) && gate.Ready(first) && gate.Ready() &&
          gate.State(2100) == FrameState::Ready, "complete frame wins before its first-frame deadline");
    Check(gate.State(std::numeric_limits<std::uint64_t>::max()) == FrameState::Ready &&
          gate.State(0) == FrameState::Ready,
          "a captured stationary desktop has no stale-frame timeout or clock-driven loss");
    Check(gate.Accept(first, 1920, 1080) && gate.Ready(first), "same-generation replacement is idempotently ready");
    Check(!gate.Accept(first, 1920, 1090) && gate.Ready(first),
          "a rejected unrelated-sized frame does not erase the last complete image");

    const auto second = gate.Begin(1080, 1920, 500);
    Check(second != 0 && second != first && !gate.Ready() && !gate.Ready(first) &&
          gate.State(500) == FrameState::Pending, "monitor rotation revokes old image readiness");
    Check(!gate.Accept(first, 1080, 1920) && !gate.Accept(second, 1920, 1080),
          "late old capture and old geometry cannot satisfy a rotated monitor");
    Check(gate.Accept(second, 1080, 1920) && gate.Ready(second), "only matching rotated capture becomes ready");
    gate.Close();
    Check(gate.State(600) == FrameState::Closed && !gate.Ready() &&
          !gate.Accept(second, 1080, 1920), "shutdown rejects late captured frames");
    const auto third = gate.Begin(1080, 1920, 700);
    Check(third != second && !gate.Ready() && !gate.Accept(second, 1080, 1920),
          "same-size restart still requires a new complete generation");

    for (const auto badSize : {std::array<std::int32_t, 2>{0, 1}, {-1, 1}, {1, 0}, {1, -1}}) {
        ++scenarios;
        FrameGate invalid;
        const auto old = invalid.Begin(640, 480, 0);
        Check(invalid.Accept(old, 640, 480), "establish capture before invalid reconfiguration");
        Check(invalid.Begin(badSize[0], badSize[1], 1) == 0 &&
              invalid.State(1) == FrameState::Closed && !invalid.Ready() &&
              !invalid.Accept(old, 640, 480), "invalid dimensions close and invalidate previous capture");
    }
}

void InitialTimeoutAndReset() {
    for (const std::uint64_t started : {std::uint64_t{0}, std::uint64_t{100},
                                       std::numeric_limits<std::uint64_t>::max() - 2000}) {
        ++scenarios;
        FrameGate gate;
        const auto generation = gate.Begin(3840, 2160, started);
        Check(gate.State(started + 1999) == FrameState::Pending,
              "first-frame timeout uses an overflow-safe elapsed interval");
        Check(gate.State(started + 2000) == FrameState::TimedOut && !gate.Ready() &&
              !gate.Accept(generation, 3840, 2160), "exact initial deadline expires capture and rejects a late first frame");
        Check(gate.State(started) == FrameState::TimedOut,
              "timed-out capture cannot become pending again after a clock change");
        const auto replacement = gate.Begin(3840, 2160, 5);
        Check(replacement != generation && gate.State(5) == FrameState::Pending &&
              !gate.Accept(generation, 3840, 2160) && gate.Accept(replacement, 3840, 2160),
              "only explicit recreation can recover from an initial timeout");
    }
    ++scenarios;
    FrameGate backwards;
    const auto generation = backwards.Begin(1920, 1080, 1000);
    Check(backwards.State(999) == FrameState::TimedOut &&
          backwards.State(1000) == FrameState::TimedOut && !backwards.Accept(generation, 1920, 1080),
          "clock reversal while waiting fails closed and remains latched");
}

void SamplingOracle() {
    const std::array<glide::Rect, 6> monitors{{
        {0, 0, 1920, 1080}, {-1920, 0, 0, 1080}, {1920, -2160, 5760, 0},
        {-1080, -1920, 0, 0}, {-1, -1, 0, 0}, {262000, 262000, 262144, 262144}
    }};
    for (const auto bounds : monitors)
        for (double zoom : {1.0, 1.01, 1.05, 1.99999, 2.0, 4.0, 16.0, 4096.0})
            for (double fraction : {0.0, 0.5, 1.0}) {
                ++scenarios;
                ++samplingFrames;
                const glide::Vec2 anchor{bounds.left + fraction * (bounds.right - bounds.left - 1),
                                         bounds.top + fraction * (bounds.bottom - bounds.top - 1)};
                glide::monitor::Frame frame;
                Check(glide::monitor::MakeFrame(bounds, glide::AnchorZoom({}, zoom, anchor, bounds), frame),
                      "construct valid physical-monitor frame");
                SamplingRect sampling;
                Check(glide::gpu::MakeSamplingRect(frame, sampling), "valid monitor frame produces GPU sampling coordinates");
                Check(sampling.left >= 0 && sampling.top >= 0 && sampling.right <= 1 && sampling.bottom <= 1 &&
                      sampling.right > sampling.left && sampling.bottom > sampling.top,
                      "normalized crop remains strictly nonempty and inside the captured monitor");
                Check(Near(sampling.right - sampling.left, 1.0 / frame.transform.zoom) &&
                      Near(sampling.bottom - sampling.top, 1.0 / frame.transform.zoom),
                      "sampling span retains the uniform visual magnification");
                const double width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
                for (const double x : {0.0, 0.5, 1.0}) for (const double y : {0.0, 0.5, 1.0}) {
                    ++sampledPoints;
                    const double gpuX = bounds.left + width * (sampling.left + x * (sampling.right - sampling.left));
                    const double gpuY = bounds.top + height * (sampling.top + y * (sampling.bottom - sampling.top));
                    // Independent oracle uses the integer capture and displayed
                    // child compensation, not the offset formula under test.
                    const double expectedX = frame.capture.left + (x * width - frame.child.left) / frame.transform.zoom;
                    const double expectedY = frame.capture.top + (y * height - frame.child.top) / frame.transform.zoom;
                    Check(Near(gpuX, expectedX) && Near(gpuY, expectedY),
                          "GPU crop agrees with existing physical affine at edges and center");
                }
            }

    ++scenarios;
    glide::monitor::Frame before, after;
    Check(glide::monitor::MakeFrame({0, 0, 1920, 1080}, {4, {100.75, 40}}, before) &&
          glide::monitor::MakeFrame({0, 0, 1920, 1080}, {4, {101, 40}}, after),
          "construct integer capture-boundary regression");
    Check(before.capture.left == 100 && before.child.left == -3 &&
          after.capture.left == 101 && after.child.left == 0,
          "fixture crosses the old move-child then update-source discontinuity");
    SamplingRect oldSampling, newSampling;
    Check(glide::gpu::MakeSamplingRect(before, oldSampling) && glide::gpu::MakeSamplingRect(after, newSampling) &&
          Near((newSampling.left - oldSampling.left) * 1920, 0.25),
          "one crop update represents exactly one displayed pixel without separate child positioning");
}

void InvalidSamplingLeavesOutputAlone() {
    glide::monitor::Frame valid;
    Check(glide::monitor::MakeFrame({-1920, -1080, 0, 0}, {4, {-1300, -700}}, valid),
          "construct validation fixture");
    for (unsigned fault = 0; fault < 8; ++fault) {
        ++scenarios;
        auto frame = valid;
        if (fault == 0) frame.transform.zoom = std::numeric_limits<double>::quiet_NaN();
        if (fault == 1) frame.transform.zoom = 0.99;
        if (fault == 2) frame.transform.offset.x = std::numeric_limits<double>::infinity();
        if (fault == 3) frame.destination.right = frame.destination.left;
        if (fault == 4) frame.source.left = frame.destination.left - 1;
        if (fault == 5) frame.child.left = 1;
        if (fault == 6) frame.capture.bottom = frame.capture.top;
        if (fault == 7) frame.transform.offset.y += 0.123;
        SamplingRect output{11, 22, 33, 44};
        Check(!glide::gpu::MakeSamplingRect(frame, output) && output.left == 11 && output.top == 22 &&
              output.right == 33 && output.bottom == 44,
              "invalid native geometry cannot partially publish new shader constants");
    }
}

} // namespace

int Run_gpu_frame_policy() {
    CaptureReadiness();
    InitialTimeoutAndReset();
    SamplingOracle();
    InvalidSamplingLeavesOutputAlone();
    std::cout << "PASS: " << assertions << " GPU frame policy assertions; " << scenarios
              << " scenarios; " << samplingFrames << " sampling frames; " << sampledPoints
              << " independent affine samples; production readiness/crop helpers, no Windows compositor executed\n";
    return 0;
}
