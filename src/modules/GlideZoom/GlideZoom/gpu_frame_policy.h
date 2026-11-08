#pragma once

#include "monitor_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace glide::gpu
{

    enum class FrameState
    {
        Closed,
        Pending,
        Ready,
        TimedOut
    };

    // UI-thread state for the capture resources owned by the renderer. This is not
    // a cross-thread mailbox: the native adapter pulls frames on its owner thread.
    // A texture from an old capture generation must never make a new monitor ready.
    // Only the first complete frame has a deadline; unchanged desktop content may
    // legitimately produce no new frames for an arbitrarily long time.
    class FrameGate
    {
    public:
        static constexpr std::uint64_t InitialTimeoutMs = 2000;

        std::uint64_t Begin(std::int32_t width, std::int32_t height, std::uint64_t nowMs) noexcept
        {
            Close();
            if (generation_ == std::numeric_limits<std::uint64_t>::max())
                return 0;
            ++generation_;
            if (width <= 0 || height <= 0)
                return 0;
            width_ = width;
            height_ = height;
            started_ = nowMs;
            active_ = true;
            return generation_;
        }

        // The native adapter calls State(now) before accepting a polled frame, so a
        // frame arriving after the initial deadline cannot revive a failed capture.
        bool Accept(std::uint64_t generation, std::int32_t width, std::int32_t height) noexcept
        {
            if (!active_ || expired_ || generation != generation_ ||
                width != width_ || height != height_)
                return false;
            ready_ = true;
            return true;
        }

        bool Ready() const noexcept { return active_ && ready_ && !expired_; }
        bool Ready(std::uint64_t generation) const noexcept
        {
            return generation == generation_ && Ready();
        }
        std::uint64_t Generation() const noexcept { return generation_; }

        FrameState State(std::uint64_t nowMs) noexcept
        {
            if (!active_)
                return FrameState::Closed;
            if (Ready())
                return FrameState::Ready;
            if (nowMs < started_ || nowMs - started_ >= InitialTimeoutMs)
                expired_ = true;
            return expired_ ? FrameState::TimedOut : FrameState::Pending;
        }

        void Close() noexcept
        {
            active_ = ready_ = expired_ = false;
            width_ = height_ = 0;
            started_ = 0;
        }

    private:
        std::uint64_t generation_ = 0, started_ = 0;
        std::int32_t width_ = 0, height_ = 0;
        bool active_ = false, ready_ = false, expired_ = false;
    };

    struct SamplingRect
    {
        double left = 0, top = 0, right = 0, bottom = 0;
    };

    // Sample a complete captured monitor texture with one uniform affine. These
    // normalized coordinates are applied together in a GPU draw; there is no child
    // window movement between the source and scale changes. The input rectangle
    // remains the separately rounded mapping already validated by MakeFrame.
    // The caller converts these double values to the shader's float constants.
    // Failure leaves output unchanged.
    inline bool MakeSamplingRect(const monitor::Frame& frame, SamplingRect& output) noexcept
    {
        if (!monitor::ValidFrame(frame))
            return false;
        const double width = static_cast<double>(frame.destination.right) - frame.destination.left;
        const double height = static_cast<double>(frame.destination.bottom) - frame.destination.top;
        const double zoom = frame.transform.zoom;
        const double left = (frame.transform.offset.x + frame.destination.left / zoom -
                             frame.destination.left) /
                            width;
        const double top = (frame.transform.offset.y + frame.destination.top / zoom -
                            frame.destination.top) /
                           height;
        const double right = left + 1.0 / zoom;
        const double bottom = top + 1.0 / zoom;
        if (!std::isfinite(left) || !std::isfinite(top) ||
            !std::isfinite(right) || !std::isfinite(bottom))
            return false;
        // Permit only cancellation error at monitor edges, then clamp it away.
        // Native coordinates are bounded by ValidFrame; this is not a broad crop
        // repair that could silently accept an out-of-monitor source.
        const double tolerance = 64 * std::numeric_limits<double>::epsilon() *
                                 (1 + std::abs(frame.transform.offset.x) + std::abs(frame.transform.offset.y) +
                                  std::abs(static_cast<double>(frame.destination.left)) +
                                  std::abs(static_cast<double>(frame.destination.top))) /
                                 std::min(width, height);
        if (left < -tolerance || top < -tolerance ||
            right > 1 + tolerance || bottom > 1 + tolerance)
            return false;
        const SamplingRect next{ std::clamp(left, 0.0, 1.0), std::clamp(top, 0.0, 1.0), std::clamp(right, 0.0, 1.0), std::clamp(bottom, 0.0, 1.0) };
        if (next.right <= next.left || next.bottom <= next.top)
            return false;
        output = next;
        return true;
    }

} // namespace glide::gpu
