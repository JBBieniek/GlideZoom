#pragma once

#include "zoom_core.h"
#include "viewport_motion.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace glide::pointer
{

    // Scale Windows-processed mouse displacement by user speed and optionally
    // divide by applied zoom. Windows' own pointer-speed/acceleration processing
    // therefore remains in the input;
    // no device counts or global mouse preferences are changed. The caller owns
    // the setting/default. MagSetInputTransform routes clicks and is not used as
    // a promise that Windows also compensates mouse velocity.
    struct View
    {
        bool requested = false;
        bool active = false;
        Rect bounds{}; // Physical display rectangle, not the captured source crop.
        double zoom = 1;
        std::uint64_t epoch = 0;
        Vec2 offset{}; // Applied source transform; no unpresented camera target.
        double edgePanMultiplier = 1;
        double speedMultiplier = 1;
        bool adjustForZoom = true;
    };

    inline constexpr bool Same(Vec2 a, Vec2 b) noexcept
    {
        return a.x == b.x && a.y == b.y;
    }
    inline constexpr bool Same(Rect a, Rect b) noexcept
    {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
    inline bool Finite(Vec2 value) noexcept
    {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }
    inline bool ValidPoint(Vec2 value) noexcept
    {
        constexpr double limit = 262144;
        return Finite(value) && value.x >= -limit && value.x <= limit &&
               value.y >= -limit && value.y <= limit &&
               value.x == std::round(value.x) && value.y == std::round(value.y);
    }
    inline bool ValidBounds(Rect value) noexcept
    {
        return ValidPoint({ value.left, value.top }) && ValidPoint({ value.right, value.bottom }) &&
               value.right > value.left && value.bottom > value.top &&
               value.right - value.left <= 262144 && value.bottom - value.top <= 262144;
    }
    inline constexpr bool Contains(Rect value, Vec2 point) noexcept
    {
        return point.x >= value.left && point.x < value.right &&
               point.y >= value.top && point.y < value.bottom;
    }
    inline bool Valid(const View& value) noexcept
    {
        return ValidBounds(value.bounds) && std::isfinite(value.zoom) &&
               value.zoom >= 1 && value.zoom <= 64 &&
               viewport::ValidTransform({ value.zoom, value.offset }) &&
               std::isfinite(value.edgePanMultiplier) &&
               value.edgePanMultiplier >= 1 && value.edgePanMultiplier <= 10 &&
               std::isfinite(value.speedMultiplier) && value.speedMultiplier >= 0.25 && value.speedMultiplier <= 3;
    }

    // The native displacement is a vector in Windows-processed screen pixels.
    // Follow its ray, spending insideCost times as much displacement budget inside the
    // selected rectangle and ordinary budget outside. This handles entry, exit,
    // negative monitor origins, diagonals and crossing both edges in one event.
    // No raw mouse device counts are interpreted as pixels.
    inline Vec2 Advance(Vec2 start, Vec2 delta, Rect bounds, double insideCost) noexcept
    {
        // Fast inside gain can carry the pointer beyond the native endpoint.
        // Intersect the entire forward ray: clipping leave to t=1 would spend
        // too little budget inside and invent native-speed travel before exit.
        constexpr double unbounded = std::numeric_limits<double>::infinity();
        double enter = 0;
        double leave = unbounded;
        const auto intersect = [&enter, &leave](double at, double direction, double low, double high) noexcept {
            if (direction == 0)
                return at >= low && at < high;
            double first = (low - at) / direction, last = (high - at) / direction;
            if (first > last)
                std::swap(first, last);
            enter = std::max(enter, first);
            leave = std::min(leave, last);
            return enter < leave;
        };
        if (!intersect(start.x, delta.x, bounds.left, bounds.right) ||
            !intersect(start.y, delta.y, bounds.top, bounds.bottom) ||
            enter >= 1 || leave <= 0)
            return { start.x + delta.x, start.y + delta.y };
        enter = std::max(0.0, enter);
        const double insideBudget = (leave - enter) * insideCost;
        const double remaining = 1 - enter;
        const double distance = insideBudget >= remaining ? enter + remaining / insideCost : leave + remaining - insideBudget;
        return { start.x + delta.x * distance, start.y + delta.y * distance };
    }

    struct Step
    {
        Vec2 point{}; // Integer cursor location to request or pass through.
        Vec2 remainder{}; // Subpixel remainder, never written to Windows settings.
    };

    // Stateless proposal. The adapter commits the remainder only after accepting
    // the move or verifying its cursor write; errors cannot commit half a move.
    inline bool Propose(Vec2 current, Vec2 candidate, Vec2 remainder, const View& view, Step& output) noexcept
    {
        if (!Valid(view) || !ValidPoint(current) || !ValidPoint(candidate) ||
            !Finite(remainder) || std::abs(remainder.x) > 0.500001 ||
            std::abs(remainder.y) > 0.500001)
            return false;
        if (view.zoom == 1 || (!view.adjustForZoom && view.speedMultiplier == 1))
        {
            output = { candidate, {} };
            return true;
        }
        const Vec2 delta{ candidate.x - current.x, candidate.y - current.y };
        if (delta.x == 0 && delta.y == 0)
        {
            output = { candidate, remainder };
            return true;
        }
        const Vec2 precise{ current.x + remainder.x, current.y + remainder.y };
        const double insideCost = (view.adjustForZoom ? view.zoom : 1) / view.speedMultiplier;
        Vec2 next = Advance(precise, delta, view.bounds, insideCost);
        // Keep the established path bitwise unchanged at the default multiplier.
        // A boost moves pointer and following camera together, avoiding the
        // inward cursor jump caused by multiplying only the camera correction.
        if (view.edgePanMultiplier != 1 &&
            !viewport::AccelerateEdges(precise, next, { view.zoom, view.offset }, view.bounds, view.edgePanMultiplier, next, remainder, 1))
            return false;
        if (!Finite(next))
            return false;
        const Vec2 rounded{ std::round(next.x), std::round(next.y) };
        if (!ValidPoint(rounded))
        {
            // Valid very large native motion can gain extra distance before
            // exiting this view. Do not fault the feature at the supported
            // desktop-coordinate limit; yield that packet to Windows intact.
            output = { candidate, {} };
            return true;
        }
        Vec2 residual{ next.x - rounded.x, next.y - rounded.y };
        // A native move wholly outside the selected monitor must remain exact.
        // Discard fractional history on leaving the selected area.
        if (!Contains(view.bounds, rounded))
            residual = {};
        if (!Contains(view.bounds, current) && !Contains(view.bounds, precise) &&
            Same(next, Vec2{ precise.x + delta.x, precise.y + delta.y }))
        {
            output = { candidate, {} };
            return true;
        }
        output = { rounded, residual };
        return true;
    }

} // namespace glide::pointer
