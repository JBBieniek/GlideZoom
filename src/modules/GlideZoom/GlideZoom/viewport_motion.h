#pragma once

#include "zoom_core.h"

#include <algorithm>
#include <cmath>

namespace glide::viewport
{

    inline constexpr bool Same(Rect a, Rect b) noexcept
    {
        return a.left == b.left && a.top == b.top &&
               a.right == b.right && a.bottom == b.bottom;
    }
    inline constexpr bool Same(Vec2 a, Vec2 b) noexcept
    {
        return a.x == b.x && a.y == b.y;
    }
    inline bool ValidBounds(Rect value) noexcept
    {
        constexpr double limit = 262144;
        return detail::Finite({ value.left, value.top }) &&
               detail::Finite({ value.right, value.bottom }) &&
               value.left >= -limit && value.top >= -limit &&
               value.right <= limit && value.bottom <= limit &&
               value.right > value.left && value.bottom > value.top &&
               value.right - value.left <= limit && value.bottom - value.top <= limit;
    }
    inline bool ValidTransform(Transform value) noexcept
    {
        return std::isfinite(value.zoom) && value.zoom >= 1 && value.zoom <= 4096 &&
               detail::Finite(value.offset) && std::abs(value.offset.x) <= 262144 &&
               std::abs(value.offset.y) <= 262144;
    }
    inline bool Contains(Rect bounds, Vec2 point) noexcept
    {
        return detail::Finite(point) && point.x >= bounds.left && point.x < bounds.right &&
               point.y >= bounds.top && point.y < bounds.bottom;
    }

    // Retain one unrounded anchor while a wheel/key burst changes its target.
    // Never replace the controller's double-precision state with a quantized
    // native frame merely because another command arrived. A changed cursor,
    // bounds, or deliberately panned view starts a new anchor from precise state.
    class Anchor
    {
    public:
        void Reset() noexcept { valid_ = false; }
        bool Valid() const noexcept { return valid_; }
        Vec2 Screen() const noexcept { return screen_; }
        Vec2 Source() const noexcept { return source_; }

        bool Set(Transform precise, Vec2 cursor, Rect bounds) noexcept
        {
            if (!ValidBounds(bounds) || !ValidTransform(precise) || !Contains(bounds, cursor))
                return false;
            Transform previous{};
            if (valid_ && Same(bounds_, bounds) && Same(cursor_, cursor) &&
                At(precise.zoom, bounds, previous) &&
                std::abs(previous.offset.x - precise.offset.x) <= 0.0000001 &&
                std::abs(previous.offset.y - precise.offset.y) <= 0.0000001)
                return true;

            Vec2 screen = SourceToScreen(cursor, precise);
            if (!detail::Finite(screen))
                return false;
            // Physical desktops have at least a one-pixel extent. Accept smaller
            // synthetic rectangles safely too, without reversing clamp bounds.
            screen.x = std::clamp(screen.x, bounds.left, std::max(bounds.left, bounds.right - 1));
            screen.y = std::clamp(screen.y, bounds.top, std::max(bounds.top, bounds.bottom - 1));
            const Vec2 source = ScreenToSource(screen, precise);
            if (!detail::Finite(source))
                return false;
            bounds_ = bounds;
            cursor_ = cursor;
            screen_ = screen;
            source_ = source;
            valid_ = true;
            return true;
        }

        // Failure leaves output untouched. The bounds check also prevents an old
        // anchor from silently following a different monitor/topology.
        bool At(double zoom, Rect bounds, Transform& output) const noexcept
        {
            if (!valid_ || !Same(bounds_, bounds) || !ValidBounds(bounds) ||
                !std::isfinite(zoom) || zoom < 1 || zoom > 4096)
                return false;
            const Vec2 offset{ source_.x - screen_.x / zoom, source_.y - screen_.y / zoom };
            if (!detail::Finite(offset))
                return false;
            output = { zoom, ClampOffset(offset, zoom, bounds) };
            return true;
        }

    private:
        Rect bounds_{};
        Vec2 cursor_{}, screen_{}, source_{};
        bool valid_ = false;
    };

    enum class FollowMode
    {
        Edges,
        Centered
    };

    // Increase only real outward source-pointer travel beyond the same five-pixel
    // edge used by Follow. The pointer adapter applies this proposal through its
    // existing verified cursor transaction; Follow then tracks the accepted source
    // displacement normally. Multiplying camera movement alone would pull the
    // projected cursor away from the border and create packet-dependent stop/start
    // panning. No clock, retained velocity or synthetic movement is introduced here.
    //
    // Both endpoints must belong to the selected display to earn an edge bonus.
    // Original cross-monitor movement stays untouched. Bonus movement stops at the
    // last in-bounds integer pixel and never shortens a fractional base proposal
    // that already rounds across the edge, so normal monitor exit is not trapped.
    inline bool AccelerateEdges(Vec2 start, Vec2 proposed, Transform applied, Rect bounds, double multiplier, Vec2& output, Vec2 carried = {}, double roundingPixels = 0) noexcept
    {
        if (!ValidBounds(bounds) || !ValidTransform(applied) ||
            !detail::Finite(start) || !detail::Finite(proposed) ||
            !std::isfinite(multiplier) || multiplier < 1 || multiplier > 10 ||
            !detail::Finite(carried) || std::abs(carried.x) > 0.500001 || std::abs(carried.y) > 0.500001 ||
            !std::isfinite(roundingPixels) || roundingPixels < 0 || roundingPixels > 1)
            return false;
        if (multiplier == 1 || applied.zoom == 1 || Same(start, proposed) ||
            !Contains(bounds, start) || !Contains(bounds, proposed))
        {
            output = proposed;
            return true;
        }
        const auto axis = [&](double at, double next, double carry, double offset, double low, double high) noexcept {
            const double motion = next - at;
            const double margin = std::min(5.0, (high - low) * 0.5);
            // Carried fractional distance was already processed on an earlier
            // event. Classify only NEW movement from the actual integer cursor,
            // without scaling that residual again. Otherwise a negative residual
            // after rounding up would look like inward movement after Follow pans
            // to that integer cursor, reducing gain for small input packets.
            const double referenceStart = at - carry;
            const double referenceNext = next - carry;
            // Native presentation quantizes an edge by at most one output pixel
            // (normally half a pixel). Treat an already contacting cursor within
            // that known uncertainty as still contacting it. Otherwise every new
            // rendered frame could repeatedly remove a fraction of the boost.
            // This changes no camera boundary: Follow still uses exactly five
            // displayed pixels. Unquantized callers leave roundingPixels at zero.
            const double uncertainty = roundingPixels / applied.zoom;
            if (motion > 0)
            {
                const double threshold = offset + (high - margin) / applied.zoom;
                const double outward = referenceStart >= threshold - uncertainty ? motion : std::clamp(referenceNext - threshold, 0.0, motion);
                const double bonus = outward * (multiplier - 1);
                return std::min(next + bonus, std::max(next, std::max(low, high - 1)));
            }
            if (motion < 0)
            {
                const double threshold = offset + (low + margin) / applied.zoom;
                const double outward = referenceStart <= threshold + uncertainty ? motion : std::clamp(referenceNext - threshold, motion, 0.0);
                const double bonus = outward * (multiplier - 1);
                return std::max(next + bonus, std::min(next, low));
            }
            return next;
        };
        const Vec2 candidate{
            axis(start.x, proposed.x, carried.x, applied.offset.x, bounds.left, bounds.right),
            axis(start.y, proposed.y, carried.y, applied.offset.y, bounds.top, bounds.bottom)
        };
        if (!detail::Finite(candidate))
            return false;
        output = candidate;
        return true;
    }

    // A displacement-driven camera. The input cursor is the real desktop/source
    // coordinate, BEFORE SourceToScreen. Neither a timer nor a rendered cursor's
    // changing projection is movement. Feed every sample, with enabled=false
    // during held gestures, so ending a gesture cannot replay old motion.
    class Follow
    {
    public:
        void Reset() noexcept { seeded_ = false; }

        bool Rebase(Vec2 source, Rect bounds) noexcept
        {
            if (!ValidBounds(bounds) || !detail::Finite(source))
            {
                Reset();
                return false;
            }
            if (!Contains(bounds, source))
            {
                Reset();
                return true;
            }
            source_ = source;
            bounds_ = bounds;
            seeded_ = true;
            return true;
        }

        // No dt: the same pointer path produces the same camera travel whether
        // it arrives quickly, slowly, or in differently sized monotone packets.
        // A paused pointer causes exactly zero camera movement, even at an edge.
        bool Advance(Transform precise, Vec2 source, Rect bounds, bool enabled, FollowMode mode, Vec2 center, Transform& output) noexcept
        {
            if (!ValidBounds(bounds) || !ValidTransform(precise) ||
                !detail::Finite(source) ||
                (mode != FollowMode::Edges && mode != FollowMode::Centered) ||
                (mode == FollowMode::Centered && !Contains(bounds, center)))
            {
                Reset();
                return false;
            }
            if (!Contains(bounds, source))
            {
                Reset();
                output = precise;
                return true;
            }
            const bool previous = seeded_ && Same(bounds_, bounds);
            const Vec2 delta = previous ? Vec2{ source.x - source_.x, source.y - source_.y } : Vec2{};
            Rebase(source, bounds);
            if (!enabled || !previous || precise.zoom == 1 ||
                (delta.x == 0 && delta.y == 0))
            {
                output = precise;
                return true;
            }

            Vec2 offset = precise.offset;
            if (mode == FollowMode::Centered)
            {
                // Follow the actual source displacement without an artificial
                // speed limit or a smoothing tail that continues after stopping.
                offset = { source.x - center.x / precise.zoom,
                           source.y - center.y / precise.zoom };
            }
            else
            {
                const Vec2 screen = SourceToScreen(source, precise);
                if (!detail::Finite(screen))
                {
                    Reset();
                    return false;
                }
                offset.x += EdgeDelta(delta.x, screen.x, bounds.left, bounds.right, precise.zoom);
                offset.y += EdgeDelta(delta.y, screen.y, bounds.top, bounds.bottom, precise.zoom);
            }
            output = { precise.zoom, ClampOffset(offset, precise.zoom, bounds) };
            return true;
        }

    private:
        Rect bounds_{};
        Vec2 source_{};
        bool seeded_ = false;

        static double EdgeDelta(double motion, double screen, double low, double high, double zoom) noexcept
        {
            // Five displayed pixels, independent of magnification. A tiny synthetic
            // viewport may have less room: never invert its central safe region.
            const double margin = std::min(5.0, (high - low) * 0.5);
            if (motion > 0)
                return std::clamp((screen - (high - margin)) / zoom, 0.0, motion);
            if (motion < 0)
                return std::clamp((screen - (low + margin)) / zoom, motion, 0.0);
            return 0;
        }
    };

} // namespace glide::viewport
