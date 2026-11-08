#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace glide
{

    struct Vec2
    {
        double x = 0.0;
        double y = 0.0;
    };

    struct Rect
    {
        double left = 0.0;
        double top = 0.0;
        double right = 0.0;
        double bottom = 0.0;
    };

    struct Transform
    {
        double zoom = 1.0;
        Vec2 offset{};
    };

    namespace detail
    {
        inline double ValidZoom(double zoom)
        {
            return std::isfinite(zoom) && zoom >= 1.0 ? zoom : 1.0;
        }

        inline bool Finite(Vec2 point)
        {
            return std::isfinite(point.x) && std::isfinite(point.y);
        }

        inline double Maximum(double maximum)
        {
            return std::isfinite(maximum) ? std::max(1.0, maximum) : 8.0;
        }

        inline double ZoomFromLog(double logZoom, double maximum)
        {
            return std::exp(std::clamp(logZoom, 0.0, std::log(Maximum(maximum))));
        }
    } // namespace detail

    inline double ClampZoom(double zoom, double maximum = 8.0)
    {
        if (std::isnan(zoom))
            return 1.0;
        return std::clamp(zoom, 1.0, detail::Maximum(maximum));
    }

    // Full-screen offsets are relative to the primary display's (0, 0), even
    // when another display has a negative desktop coordinate.
    inline Vec2 ScreenToSource(Vec2 screen, Transform transform)
    {
        const double zoom = detail::ValidZoom(transform.zoom);
        return { transform.offset.x + screen.x / zoom,
                 transform.offset.y + screen.y / zoom };
    }

    inline Vec2 SourceToScreen(Vec2 source, Transform transform)
    {
        const double zoom = detail::ValidZoom(transform.zoom);
        return { (source.x - transform.offset.x) * zoom,
                 (source.y - transform.offset.y) * zoom };
    }

    inline Vec2 ClampOffset(Vec2 offset, double zoom, Rect desktop)
    {
        zoom = detail::ValidZoom(zoom);
        if (zoom == 1.0 || !(desktop.right > desktop.left) ||
            !(desktop.bottom > desktop.top) ||
            !detail::Finite({ desktop.left, desktop.top }) ||
            !detail::Finite({ desktop.right, desktop.bottom }))
        {
            return {};
        }
        if (!detail::Finite(offset))
            offset = {};
        return {
            std::clamp(offset.x, desktop.left - desktop.left / zoom, desktop.right - desktop.right / zoom),
            std::clamp(offset.y, desktop.top - desktop.top / zoom, desktop.bottom - desktop.bottom / zoom)
        };
    }

    // Preserve the source point below the anchor unless desktop edge clamping
    // requires moving it. Zoom policy (for example the user's 8x limit) is applied
    // by the caller; this function also works with limits above 8x.
    inline Transform AnchorZoom(Transform previous, double newZoom, Vec2 screenAnchor, Rect desktop)
    {
        newZoom = detail::ValidZoom(newZoom);
        const Vec2 sourceAnchor = ScreenToSource(screenAnchor, previous);
        const Vec2 offset{ sourceAnchor.x - screenAnchor.x / newZoom,
                           sourceAnchor.y - screenAnchor.y / newZoom };
        return { newZoom, ClampOffset(offset, newZoom, desktop) };
    }

    // Continuous-time exponential response in log magnification. The same elapsed
    // time gives the same result whether updates arrive at 30, 60, or 144 Hz.
    inline double SmoothZoom(double current, double target, double dt, double tau = 0.09)
    {
        current = detail::ValidZoom(current);
        target = detail::ValidZoom(target);
        if (std::isnan(dt) || dt <= 0.0)
            return current;
        if (!std::isfinite(dt) || !std::isfinite(tau) || tau <= 0.0)
            return target;
        const double alpha = -std::expm1(-dt / tau);
        const double start = std::log(current);
        const double end = std::log(target);
        return std::exp(start + (end - start) * alpha);
    }

    inline double WheelZoom(double current, double delta, double factor = 1.12, double maxZoom = 8.0)
    {
        current = ClampZoom(current, maxZoom);
        if (std::isnan(delta))
            return current;
        if (!std::isfinite(factor) || factor <= 1.0)
            factor = 1.12;
        return detail::ZoomFromLog(std::log(current) + std::log(factor) * delta / 120.0,
                                   maxZoom);
    }

    // deltaY is currentY - startingY: moving upward increases magnification.
    inline double DragZoom(double start, double deltaY, double pixelsPerDoubling = 220.0, double maxZoom = 8.0)
    {
        start = ClampZoom(start, maxZoom);
        if (std::isnan(deltaY))
            return start;
        if (!std::isfinite(pixelsPerDoubling) || pixelsPerDoubling <= 0.0)
        {
            pixelsPerDoubling = 220.0;
        }
        return detail::ZoomFromLog(std::log(start) - std::log(2.0) * deltaY / pixelsPerDoubling,
                                   maxZoom);
    }

    class DoubleTapHold
    {
    public:
        explicit DoubleTapHold(std::uint64_t doubleTapMs = 400,
                               double movementSlop = 6.0,
                               std::uint64_t maxTapMs = 250) :
            doubleTapMs_(doubleTapMs),
            movementSlop_(std::isfinite(movementSlop) ? std::max(0.0, movementSlop) : 6.0),
            maxTapMs_(maxTapMs) {}

        // Returns true only on activation by the second down. Auto-repeat does
        // not activate, restart timing, or advance the state machine.
        bool Down(std::uint64_t nowMs, Vec2 point)
        {
            if (held_)
                return false;
            if (!detail::Finite(point))
            {
                Reset();
                return false;
            }
            const bool second = armed_ && nowMs >= firstUpMs_ &&
                                nowMs - firstUpMs_ <= doubleTapMs_ && WithinSlop(point, firstPoint_);
            armed_ = false;
            held_ = true;
            active_ = second;
            moved_ = false;
            downMs_ = nowMs;
            downPoint_ = point;
            return active_;
        }

        void Move(Vec2 point)
        {
            if (held_ && !active_ && !WithinSlop(point, downPoint_))
                moved_ = true;
        }

        void Up(std::uint64_t nowMs)
        {
            if (!held_)
                return;
            if (!active_ && !moved_ && nowMs >= downMs_ &&
                nowMs - downMs_ <= maxTapMs_)
            {
                armed_ = true;
                firstUpMs_ = nowMs;
                firstPoint_ = downPoint_;
            }
            else
            {
                armed_ = false;
            }
            held_ = false;
            active_ = false;
            moved_ = false;
        }

        bool Active() const { return active_; }
        bool Held() const { return held_; }

        void Reset()
        {
            armed_ = false;
            held_ = false;
            active_ = false;
            moved_ = false;
            firstUpMs_ = downMs_ = 0;
            firstPoint_ = downPoint_ = {};
        }

    private:
        bool WithinSlop(Vec2 a, Vec2 b) const
        {
            return detail::Finite(a) && detail::Finite(b) &&
                   std::hypot(a.x - b.x, a.y - b.y) <= movementSlop_;
        }

        std::uint64_t doubleTapMs_;
        double movementSlop_;
        std::uint64_t maxTapMs_;
        bool armed_ = false;
        bool held_ = false;
        bool active_ = false;
        bool moved_ = false;
        std::uint64_t firstUpMs_ = 0;
        std::uint64_t downMs_ = 0;
        Vec2 firstPoint_{};
        Vec2 downPoint_{};
    };

} // namespace glide
