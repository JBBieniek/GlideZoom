#pragma once

#include "pointer_motion.h"

#include <atomic>
#include <cstdint>
#include <limits>

namespace glide::pointer
{

    // Feedback describes committed compensation only. Windows still owns the
    // integer cursor; the residual is used solely to position its GPU replacement.
    struct Feedback
    {
        bool valid = false;
        Vec2 point{}, remainder{};
        Rect bounds{};
        double zoom = 1;
        std::uint64_t epoch = 0;
        double speedMultiplier = 1;
        bool adjustForZoom = true;
    };

    inline bool ValidFeedback(const Feedback& value) noexcept
    {
        return value.valid && ValidPoint(value.point) && ValidBounds(value.bounds) &&
               std::isfinite(value.zoom) && value.zoom > 1 && value.zoom <= 64 &&
               std::isfinite(value.speedMultiplier) && value.speedMultiplier >= 0.25 && value.speedMultiplier <= 3 &&
               Finite(value.remainder) && std::abs(value.remainder.x) <= 0.500001 &&
               std::abs(value.remainder.y) <= 0.500001;
    }

    // The UI supplies its current applied geometry and explicit eligibility gate.
    // No fractional cursor is shown in the camera's edge band: retain the existing
    // integer camera/cursor relationship there, including pinned edges and corners.
    // Expand the five-pixel band by half a source pixel of display uncertainty.
    // Any mismatch leaves output untouched and the caller uses the physical point.
    inline bool ResolveCursor(const Feedback& feedback, Vec2 physical, Transform applied, Rect bounds, std::uint64_t epoch, bool enabled, Vec2& output, double expectedSpeedMultiplier = 1, bool expectedAdjustForZoom = true) noexcept
    {
        if (!enabled || !ValidFeedback(feedback) || !ValidPoint(physical) ||
            !Same(feedback.point, physical) || feedback.epoch != epoch ||
            feedback.zoom != applied.zoom || !Same(feedback.bounds, bounds) ||
            feedback.speedMultiplier != expectedSpeedMultiplier || feedback.adjustForZoom != expectedAdjustForZoom ||
            !viewport::ValidTransform(applied) || !Contains(bounds, physical))
            return false;
        const Vec2 screen = SourceToScreen(physical, applied);
        const double margin = 5.0 + 0.500001 * applied.zoom;
        if (!Finite(screen) || screen.x <= bounds.left + margin ||
            screen.x >= bounds.right - margin || screen.y <= bounds.top + margin ||
            screen.y >= bounds.bottom - margin)
            return false;
        const Vec2 precise{ physical.x + feedback.remainder.x, physical.y + feedback.remainder.y };
        if (!Finite(precise) || !Contains(bounds, precise))
            return false;
        output = precise;
        return true;
    }

    // One hook-thread writer, one UI-thread reader. All payload fields are atomic;
    // a reader tries once and falls back to the physical cursor on contention.
    // No locks, loops, allocation, native cursor writes or UI calls occur here.
    class FeedbackChannel
    {
    public:
        bool Publish(const Feedback& value) noexcept
        {
            const bool valid = ValidFeedback(value);
            const Feedback next = valid ? value : Feedback{};
            const auto previous = sequence_.load();
            if (previous & 1u)
                return false;
            if (previous >= std::numeric_limits<std::uint64_t>::max() - 1)
            {
                sequence_.store(std::numeric_limits<std::uint64_t>::max());
                return false;
            }
            if (published_ && SameFeedback(next, last_))
                return valid || !value.valid;
            sequence_.store(previous + 1);
            valid_.store(next.valid);
            x_.store(next.point.x);
            y_.store(next.point.y);
            remainderX_.store(next.remainder.x);
            remainderY_.store(next.remainder.y);
            left_.store(next.bounds.left);
            top_.store(next.bounds.top);
            right_.store(next.bounds.right);
            bottom_.store(next.bounds.bottom);
            zoom_.store(next.zoom);
            epoch_.store(next.epoch);
            speedMultiplier_.store(next.speedMultiplier);
            adjustForZoom_.store(next.adjustForZoom);
            sequence_.store(previous + 2);
            last_ = next;
            published_ = true;
            return valid || !value.valid;
        }

        bool Read(Feedback& output) const noexcept
        {
            const auto before = sequence_.load();
            if (before & 1u)
                return false;
            Feedback next;
            next.valid = valid_.load();
            next.point = { x_.load(), y_.load() };
            next.remainder = { remainderX_.load(), remainderY_.load() };
            next.bounds = { left_.load(), top_.load(), right_.load(), bottom_.load() };
            next.zoom = zoom_.load();
            next.epoch = epoch_.load();
            next.speedMultiplier = speedMultiplier_.load();
            next.adjustForZoom = adjustForZoom_.load();
            if (sequence_.load() != before)
                return false;
            output = next;
            return true;
        }

    private:
        static bool SameFeedback(const Feedback& a, const Feedback& b) noexcept
        {
            return a.valid == b.valid && Same(a.point, b.point) && Same(a.remainder, b.remainder) &&
                   Same(a.bounds, b.bounds) && a.zoom == b.zoom && a.epoch == b.epoch &&
                   a.speedMultiplier == b.speedMultiplier && a.adjustForZoom == b.adjustForZoom;
        }
        static_assert(std::atomic<double>::is_always_lock_free &&
                          std::atomic<std::uint64_t>::is_always_lock_free,
                      "Pointer feedback requires native lock-free atomics");
        std::atomic<std::uint64_t> sequence_{ 0 }, epoch_{ 0 };
        std::atomic<bool> valid_{ false }, adjustForZoom_{ true };
        std::atomic<double> x_{ 0 }, y_{ 0 }, remainderX_{ 0 }, remainderY_{ 0 };
        std::atomic<double> left_{ 0 }, top_{ 0 }, right_{ 0 }, bottom_{ 0 }, zoom_{ 1 };
        std::atomic<double> speedMultiplier_{ 1 };
        Feedback last_{}; // Sole writer only.
        bool published_ = false;
    };

} // namespace glide::pointer
