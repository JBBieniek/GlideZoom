#pragma once
#include "zoom_core.h"
#include <atomic>
#include <cstdint>
#include <limits>

namespace glide::input
{
    struct PointerSnapshot
    {
        bool requested = false;
        bool active = false;
        Rect bounds{};
        double zoom = 1;
        std::uint64_t epoch = 0;
        Vec2 offset{}; // Applied visual transform, never an unpresented target.
        double edgePanMultiplier = 1;
        double speedMultiplier = 1;
        bool adjustForZoom = true;
    };

    // One UI-thread writer, one hook-thread reader. Every field is atomic: a failed
    // read cannot race on plain memory. A reader tries once and passes input through
    // during publication; the low-level hook never spins, allocates, or takes a lock.
    // Seq-cst operations also put the two version reads and all fields in one order.
    class PointerChannel
    {
    public:
        bool Publish(const PointerSnapshot& value) noexcept
        {
            const auto previous = sequence_.load();
            if (previous & 1u)
                return false;
            if (previous >= std::numeric_limits<std::uint64_t>::max() - 1)
            {
                sequence_.store(std::numeric_limits<std::uint64_t>::max());
                return false; // Saturate closed instead of permitting an ABA wrap.
            }
            // Rendering can re-publish identical controls many times. The caller
            // omits unused offset changes when edge acceleration is disabled.
            // Do not open a false contention window
            // for the bounded hook read when the published policy is unchanged.
            // Closed/saturated sequences above remain failures even for duplicates.
            if (published_ && Same(lastPublished_, value))
                return true;
            sequence_.store(previous + 1);
            requested_.store(value.requested);
            active_.store(value.active);
            left_.store(value.bounds.left);
            top_.store(value.bounds.top);
            right_.store(value.bounds.right);
            bottom_.store(value.bounds.bottom);
            zoom_.store(value.zoom);
            epoch_.store(value.epoch);
            offsetX_.store(value.offset.x);
            offsetY_.store(value.offset.y);
            edgePanMultiplier_.store(value.edgePanMultiplier);
            speedMultiplier_.store(value.speedMultiplier);
            adjustForZoom_.store(value.adjustForZoom);
            sequence_.store(previous + 2);
            // The cache belongs to the sole writer. Readers use only the atomic
            // fields and revision, never this plain-memory bookkeeping.
            lastPublished_ = value;
            published_ = true;
            return true;
        }

        bool Read(PointerSnapshot& output) const noexcept
        {
            const auto before = sequence_.load();
            if (before & 1u)
                return false;
            PointerSnapshot candidate;
            candidate.requested = requested_.load();
            candidate.active = active_.load();
            candidate.bounds = { left_.load(), top_.load(), right_.load(), bottom_.load() };
            candidate.zoom = zoom_.load();
            candidate.epoch = epoch_.load();
            candidate.offset = { offsetX_.load(), offsetY_.load() };
            candidate.edgePanMultiplier = edgePanMultiplier_.load();
            candidate.speedMultiplier = speedMultiplier_.load();
            candidate.adjustForZoom = adjustForZoom_.load();
            if (sequence_.load() != before)
                return false;
            output = candidate;
            return true;
        }

    private:
        static bool Same(const PointerSnapshot& a, const PointerSnapshot& b) noexcept
        {
            return a.requested == b.requested && a.active == b.active &&
                   a.bounds.left == b.bounds.left && a.bounds.top == b.bounds.top &&
                   a.bounds.right == b.bounds.right && a.bounds.bottom == b.bounds.bottom &&
                   a.zoom == b.zoom && a.epoch == b.epoch &&
                   a.offset.x == b.offset.x && a.offset.y == b.offset.y &&
                   a.edgePanMultiplier == b.edgePanMultiplier && a.speedMultiplier == b.speedMultiplier &&
                   a.adjustForZoom == b.adjustForZoom;
        }
        static_assert(std::atomic<double>::is_always_lock_free &&
                          std::atomic<std::uint64_t>::is_always_lock_free,
                      "Pointer publication requires native lock-free 64-bit atomics");
        std::atomic<std::uint64_t> sequence_{ 0 }, epoch_{ 0 };
        std::atomic<bool> requested_{ false }, active_{ false }, adjustForZoom_{ true };
        std::atomic<double> left_{ 0 }, top_{ 0 }, right_{ 0 }, bottom_{ 0 }, zoom_{ 1 };
        std::atomic<double> offsetX_{ 0 }, offsetY_{ 0 }, edgePanMultiplier_{ 1 };
        std::atomic<double> speedMultiplier_{ 1 };
        PointerSnapshot lastPublished_{}; // UI writer only; no reader access.
        bool published_ = false; // Commit only after a complete publication.
    };
} // namespace glide::input
