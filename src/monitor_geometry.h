#pragma once

#include "zoom_core.h"
#include "zoom_session.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace glide::monitor
{

    // The window magnifier uses a uniform float scale and integer source pixels.
    // Quantizing the source origin directly makes an anchor jump by up to zoom
    // DISPLAY pixels. Capture a small overscan and position the child in output
    // pixels instead: the parent clips the excess to this one monitor. Source and
    // destination remain the separate, integer Windows input mapping (including
    // mouse routing on Windows 10 1703 and later); movement gain is independent.
    // Transform retains the existing global-coordinate convention:
    // source = offset + screen / zoom, even for a monitor with a nonzero origin.
    struct Frame
    {
        Transform transform{};
        session::Rect source{}, destination{};
        session::Rect capture{}, child{}; // Child coordinates are relative to host.
    };

    inline constexpr bool SameFrame(const Frame& a, const Frame& b) noexcept
    {
        return a.transform.zoom == b.transform.zoom &&
               a.transform.offset.x == b.transform.offset.x &&
               a.transform.offset.y == b.transform.offset.y &&
               a.source == b.source && a.destination == b.destination &&
               a.capture == b.capture && a.child == b.child;
    }

    inline bool ValidBounds(Rect bounds) noexcept
    {
        constexpr double limit = 262144;
        if (!glide::detail::Finite({ bounds.left, bounds.top }) ||
            !glide::detail::Finite({ bounds.right, bounds.bottom }) ||
            bounds.left < -limit || bounds.top < -limit ||
            bounds.right > limit || bounds.bottom > limit ||
            bounds.right <= bounds.left || bounds.bottom <= bounds.top ||
            bounds.right - bounds.left > limit || bounds.bottom - bounds.top > limit)
            return false;
        return std::floor(bounds.left) == bounds.left && std::floor(bounds.top) == bounds.top &&
               std::floor(bounds.right) == bounds.right && std::floor(bounds.bottom) == bounds.bottom;
    }

    inline bool Contains(Rect bounds, Vec2 point) noexcept
    {
        return ValidBounds(bounds) && glide::detail::Finite(point) &&
               point.x >= bounds.left && point.x < bounds.right &&
               point.y >= bounds.top && point.y < bounds.bottom;
    }

    inline constexpr Rect AsRect(session::Rect bounds) noexcept
    {
        return { static_cast<double>(bounds.left), static_cast<double>(bounds.top), static_cast<double>(bounds.right), static_cast<double>(bounds.bottom) };
    }

    namespace detail
    {
        struct Axis
        {
            std::int32_t captureLow = 0, captureHigh = 0;
            std::int32_t childLow = 0, childHigh = 0;
            std::int32_t inputLow = 0, inputHigh = 0;
            double offset = 0;
        };

        inline Axis MakeAxis(double low, double high, double desiredOffset, double zoom) noexcept
        {
            const double size = high - low;
            const double minimum = low, maximum = high - size / zoom;
            double origin = std::clamp(desiredOffset + low / zoom, minimum, maximum);
            // Re-presenting an applied transform (including after a screenshot) must
            // reconstruct exactly the same native frame. Cancellation at a nonzero
            // monitor origin can leave a value a few ulps below an integer source pixel.
            const double nearest = std::round(origin);
            const double tolerance = 16 * std::numeric_limits<double>::epsilon() *
                                     (1 + std::abs(origin) + std::abs(desiredOffset) + std::abs(low / zoom));
            if (std::abs(origin - nearest) <= tolerance)
                origin = std::clamp(nearest, minimum, maximum);
            auto captureLow = static_cast<std::int32_t>(std::floor(origin));
            auto shift = static_cast<std::int32_t>(std::lround((captureLow - origin) * zoom));
            // The nearest output pixel must not request any capture from a neighbor.
            // At the far edge the feasible shift is rounded inward, losing <1 output
            // pixel of accuracy; everywhere else the bound is <=0.5 output pixel.
            const auto minimumShift = static_cast<std::int32_t>(std::ceil(size - (high - captureLow) * zoom));
            shift = std::clamp(shift, minimumShift, 0);
            // Output rounding can cross the next integer source pixel (for example a
            // -2 output shift at 1.99999x). Canonicalize that edge to the next source
            // pixel with zero shift. Otherwise replaying the applied affine would
            // select a different crop and slightly change the restored transform.
            if (-static_cast<double>(shift) >= zoom)
            {
                ++captureLow;
                shift = 0;
            }
            const auto captureHigh = static_cast<std::int32_t>(std::min(high,
                                                                        captureLow + std::ceil((size - shift) / zoom)));
            const double visibleLow = captureLow - shift / zoom;
            const double visibleHigh = visibleLow + size / zoom;
            auto inputLow = static_cast<std::int32_t>(std::clamp(std::round(visibleLow), low, high));
            auto inputHigh = static_cast<std::int32_t>(std::clamp(std::round(visibleHigh), low, high));
            // The exclusive input edge must retain the final physical source
            // pixel whenever that pixel is visible. At fractional zoom, output
            // quantization can leave visibleHigh less than half a pixel past it;
            // nearest rounding would otherwise make that edge unreachable.
            // Derive this from applied geometry so frame replay stays identical.
            if (visibleHigh > high - 1)
                inputHigh = static_cast<std::int32_t>(high);
            if (inputHigh <= inputLow)
            {
                // A subpixel visible span still needs a nonempty integer input RECT.
                // Pick its containing source pixel; rounding its left endpoint up
                // before extending would produce an error greater than one pixel.
                inputLow = static_cast<std::int32_t>(std::clamp(std::floor(visibleLow), low, high - 1));
                inputHigh = inputLow + 1;
            }
            return { captureLow, captureHigh, shift, static_cast<std::int32_t>(size), inputLow, inputHigh, captureLow - (low + shift) / zoom };
        }
    } // namespace detail

    inline bool ValidFrame(const Frame& frame) noexcept
    {
        if (!(ValidBounds(AsRect(frame.source)) && ValidBounds(AsRect(frame.destination)) &&
              ValidBounds(AsRect(frame.capture)) &&
              frame.source.left >= frame.destination.left && frame.source.top >= frame.destination.top &&
              frame.source.right <= frame.destination.right && frame.source.bottom <= frame.destination.bottom &&
              frame.capture.left >= frame.destination.left && frame.capture.top >= frame.destination.top &&
              frame.capture.right <= frame.destination.right && frame.capture.bottom <= frame.destination.bottom &&
              std::isfinite(frame.transform.zoom) && frame.transform.zoom >= 1 &&
              frame.transform.zoom <= 4096 && glide::detail::Finite(frame.transform.offset)))
            return false;
        const double width = static_cast<double>(frame.destination.right) - frame.destination.left;
        const double height = static_cast<double>(frame.destination.bottom) - frame.destination.top;
        if (frame.child.left > 0 || frame.child.top > 0 ||
            frame.child.left < -4096 || frame.child.top < -4096 ||
            frame.child.right != width || frame.child.bottom != height)
            return false;
        const double zoom = frame.transform.zoom;
        if (static_cast<float>(zoom) != zoom ||
            frame.transform.offset.x != frame.capture.left -
                                            (static_cast<double>(frame.destination.left) + frame.child.left) / zoom ||
            frame.transform.offset.y != frame.capture.top -
                                            (static_cast<double>(frame.destination.top) + frame.child.top) / zoom)
            return false;
        const auto x = detail::MakeAxis(frame.destination.left, frame.destination.right, frame.transform.offset.x, zoom);
        const auto y = detail::MakeAxis(frame.destination.top, frame.destination.bottom, frame.transform.offset.y, zoom);
        if (!(frame.source == session::Rect{ x.inputLow, y.inputLow, x.inputHigh, y.inputHigh }) ||
            !(frame.capture == session::Rect{ x.captureLow, y.captureLow, x.captureHigh, y.captureHigh }) ||
            !(frame.child == session::Rect{ x.childLow, y.childLow, x.childHigh, y.childHigh }))
            return false;
        constexpr double tolerance = 1e-7;
        return (static_cast<double>(frame.capture.right) - frame.capture.left) * zoom + tolerance >= width - frame.child.left &&
               (static_cast<double>(frame.capture.bottom) - frame.capture.top) * zoom + tolerance >= height - frame.child.top;
    }

    // Failure leaves output untouched. Bounds are physical desktop pixels, not
    // DPI-scaled logical units. The caller pins the monitor for an active gesture.
    inline bool MakeFrame(Rect bounds, Transform desired, Frame& output) noexcept
    {
        if (!ValidBounds(bounds) || !std::isfinite(desired.zoom) || desired.zoom < 1 ||
            desired.zoom > 4096 || !glide::detail::Finite(desired.offset))
            return false;
        const double zoom = static_cast<float>(desired.zoom);
        if (!std::isfinite(zoom) || zoom < 1 || zoom > 4096)
            return false;
        const Vec2 offset = ClampOffset(desired.offset, zoom, bounds);
        const auto x = detail::MakeAxis(bounds.left, bounds.right, offset.x, zoom);
        const auto y = detail::MakeAxis(bounds.top, bounds.bottom, offset.y, zoom);
        const Frame next{
            { zoom, { x.offset, y.offset } },
            { x.inputLow, y.inputLow, x.inputHigh, y.inputHigh },
            { static_cast<std::int32_t>(bounds.left), static_cast<std::int32_t>(bounds.top), static_cast<std::int32_t>(bounds.right), static_cast<std::int32_t>(bounds.bottom) },
            { x.captureLow, y.captureLow, x.captureHigh, y.captureHigh },
            { x.childLow, y.childLow, x.childHigh, y.childHigh }
        };
        if (!ValidFrame(next))
            return false;
        output = next;
        return true;
    }

    // Model the rectangular Windows input mapping only inside its destination.
    // Outside the selected monitor, Windows input must remain unchanged. The
    // rounded visible crop differs from uniform visual scaling by <1 source
    // pixel on either axis (at most zoom displayed pixels), not an exact mapping.
    // Mouse movement gain is handled separately from Windows input routing.
    inline bool MapInput(const Frame& frame, Vec2 screen, Vec2& source) noexcept
    {
        if (!ValidFrame(frame) || !Contains(AsRect(frame.destination), screen))
            return false;
        const double width = static_cast<double>(frame.destination.right) - frame.destination.left;
        const double height = static_cast<double>(frame.destination.bottom) - frame.destination.top;
        source = {
            frame.source.left + (screen.x - frame.destination.left) *
                                    (static_cast<double>(frame.source.right) - frame.source.left) / width,
            frame.source.top + (screen.y - frame.destination.top) *
                                   (static_cast<double>(frame.source.bottom) - frame.source.top) / height
        };
        return true;
    }

} // namespace glide::monitor
