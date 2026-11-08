// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#pragma once

#include "pointer_motion.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace glide::input::routing
{
    inline constexpr std::size_t MaximumRoutes = 16;
    struct Route
    {
        std::uint64_t id = 0;
        Rect bounds{};
        Rect source{};
        bool zoomed = false;
    };
    struct Snapshot
    {
        bool enabled = false;
        bool busy = false;
        std::uint64_t generation = 0;
        std::uint64_t epoch = 0;
        std::uint64_t activeId = 0;
        std::uint64_t ackSerial = 0;
        std::size_t count = 0;
        std::array<Route, MaximumRoutes> routes{};
    };
    struct Feedback
    {
        std::uint64_t observedGeneration = 0;
        std::uint64_t requestSerial = 0;
        std::uint64_t targetId = 0;
        std::uint64_t epoch = 0;
        Vec2 point{};
        unsigned blockedButtons = 0;
        unsigned forwardedButtons = 0;
        bool canCommit = false;
        bool faulted = false;
        std::uint64_t reportedAt = 0;
    };

    inline bool CommitReady(const Feedback& value, std::uint64_t generation, std::uint64_t epoch, std::uint64_t now) noexcept
    {
        return !value.faulted && value.canCommit && value.observedGeneration == generation &&
               value.epoch == epoch && value.reportedAt != 0 && now >= value.reportedAt &&
               now - value.reportedAt <= 250;
    }

    inline bool Valid(const Snapshot& value) noexcept
    {
        if (!value.enabled)
            return true;
        if (!value.generation || !value.count || value.count > MaximumRoutes)
            return false;
        bool activeFound = value.activeId == 0;
        for (std::size_t index = 0; index < value.count; ++index)
        {
            const auto& route = value.routes[index];
            if (!route.id || !pointer::ValidBounds(route.bounds) || !pointer::ValidBounds(route.source) ||
                route.source.left < route.bounds.left || route.source.top < route.bounds.top ||
                route.source.right > route.bounds.right || route.source.bottom > route.bounds.bottom)
                return false;
            activeFound = activeFound || route.id == value.activeId;
            for (std::size_t previous = 0; previous < index; ++previous)
            {
                const auto& other = value.routes[previous];
                if (other.id == route.id ||
                    (other.bounds.left < route.bounds.right && other.bounds.right > route.bounds.left &&
                     other.bounds.top < route.bounds.bottom && other.bounds.bottom > route.bounds.top))
                    return false;
            }
        }
        return activeFound;
    }

    // Hook-thread policy. No input is synthesized. A new blocked DOWN owns its
    // complete pair, while an already forwarded drag keeps its original monitor.
    class State
    {
    public:
        void Observe(const Snapshot& value, bool coherent, std::uint64_t epoch) noexcept
        {
            coherent_ = coherent && Valid(value) && (!value.enabled || value.epoch == epoch);
            if (value.enabled && snapshot_.enabled && value.generation < snapshot_.generation)
                coherent_ = false;
            if (!coherent_)
                return;
            snapshot_ = value;
            if (HasForwardedDrag() && value.activeId == drag_.id)
                for (std::size_t index = 0; index < value.count; ++index)
                    if (value.routes[index].id == drag_.id && pointer::Same(value.routes[index].bounds, drag_.bounds))
                        drag_.source = value.routes[index].source;
            if (!value.enabled)
            {
                haveTarget_ = false;
                target_ = 0;
            }
            else if (havePoint_)
                ObservePoint(point_);
        }

        void ObservePoint(Vec2 point) noexcept
        {
            if (!pointer::ValidPoint(point))
            {
                faulted_ = true;
                return;
            }
            point_ = point;
            havePoint_ = true;
            if (!snapshot_.enabled)
                return;
            const auto* route = Find(point);
            // A forwarded native drag owns this monitor until its physical UP.
            // A pre-confinement edge packet must not request a handoff that then
            // waits for this very drag and freezes same-monitor edge panning.
            const auto target = HasForwardedDrag() ? drag_.id : (route ? route->id : 0);
            if ((!haveTarget_ && target != snapshot_.activeId) || (haveTarget_ && target != target_))
            {
                if (serial_ == (std::numeric_limits<std::uint64_t>::max)())
                    faulted_ = true;
                else
                    ++serial_;
            }
            target_ = target;
            haveTarget_ = true;
        }

        bool ZoomReady() const noexcept
        {
            if (!snapshot_.enabled)
                return true;
            return coherent_ && !faulted_ && !snapshot_.busy && havePoint_ && haveTarget_ &&
                   target_ != 0 && target_ == snapshot_.activeId &&
                   (serial_ == 0 || snapshot_.ackSerial == serial_);
        }

        bool BlockNewButton() const noexcept
        {
            if (!snapshot_.enabled)
                return false;
            if (faulted_ || snapshot_.busy)
                return true;
            if (HasForwardedDrag() && !pointer::Contains(drag_.bounds, point_))
                return true;
            const auto* route = havePoint_ ? Find(point_) : nullptr;
            // Stable ordinary displays stay native. A failed coherent read may
            // never authorize input through a known zoomed display.
            return route && route->zoomed &&
                   (!ZoomReady() || !pointer::Contains(route->source, point_));
        }
        bool BlockWheel(bool zoomChord) const noexcept
        {
            return BlockNewButton() || (snapshot_.enabled && zoomChord && !ZoomReady());
        }
        bool Busy() const noexcept { return coherent_ && snapshot_.enabled && snapshot_.busy; }
        bool CanRequestWheel(Vec2 point, bool allowBusy = false) const noexcept
        {
            const auto* route = Find(point);
            return coherent_ && !faulted_ && snapshot_.enabled && (!snapshot_.busy || allowBusy) && !down_ &&
                   route && !route->zoomed;
        }

        // Called after input::State has observed this same physical event. Its
        // gesture ledger remains independent; combine both consumption results.
        bool Button(unsigned button, bool down, bool gestureConsumed) noexcept
        {
            if (button >= 5)
                return gestureConsumed;
            const unsigned bit = 1u << button;
            if (!down)
            {
                const bool forwarded = (forwarded_ & bit) != 0;
                const bool consumed = (blocked_ & bit) != 0;
                down_ &= ~bit;
                blocked_ &= ~bit;
                forwarded_ &= ~bit;
                if (!forwarded_)
                    drag_ = {};
                return !forwarded && (gestureConsumed || consumed);
            }
            if (down_ & bit)
                return gestureConsumed || (blocked_ & bit) != 0;
            down_ |= bit;
            if (BlockNewButton())
            {
                blocked_ |= bit;
                return true;
            }
            if (!gestureConsumed)
            {
                if (!forwarded_)
                {
                    const auto* route = havePoint_ ? Find(point_) : nullptr;
                    if (route)
                        drag_ = *route;
                }
                forwarded_ |= bit;
            }
            return gestureConsumed;
        }

        void SeedButtons(unsigned physical) noexcept
        {
            down_ = forwarded_ = physical & 31u;
            blocked_ = 0;
            drag_ = {};
            if (forwarded_)
            {
                const auto* route = havePoint_ ? Find(point_) : nullptr;
                if (route)
                    drag_ = *route;
            }
        }
        void SessionReset() noexcept
        {
            down_ = forwarded_ = blocked_ = 0;
            drag_ = {};
        }
        bool HasBlockedButtons() const noexcept { return blocked_ != 0; }
        bool HasForwardedDrag() const noexcept { return snapshot_.enabled && forwarded_ != 0 && drag_.id != 0; }
        bool DragConstraint(Vec2 proposed, Route& output) const noexcept
        {
            if (!HasForwardedDrag())
                return false;
            if (pointer::Contains(drag_.bounds, proposed))
                return false;
            output = drag_;
            return true;
        }
        void Fault() noexcept { faulted_ = true; }
        bool Faulted() const noexcept { return faulted_; }
        Feedback Report(bool gesturesIdle, std::uint64_t now = 0) const noexcept
        {
            return { coherent_ ? snapshot_.generation : 0, serial_, target_, snapshot_.epoch, point_, blocked_, forwarded_, coherent_ && havePoint_ && haveTarget_ && target_ != 0 && snapshot_.enabled && snapshot_.busy && !faulted_ && !forwarded_ && gesturesIdle, faulted_, now };
        }

    private:
        const Route* Find(Vec2 point) const noexcept
        {
            for (std::size_t index = 0; index < snapshot_.count && index < MaximumRoutes; ++index)
                if (pointer::Contains(snapshot_.routes[index].bounds, point))
                    return &snapshot_.routes[index];
            return nullptr;
        }
        Snapshot snapshot_{};
        Route drag_{};
        Vec2 point_{};
        std::uint64_t serial_ = 0, target_ = 0;
        unsigned down_ = 0, forwarded_ = 0, blocked_ = 0;
        bool coherent_ = true, faulted_ = false, havePoint_ = false, haveTarget_ = false;
    };

    // One writer and one bounded reader per channel. Atomic payload fields and
    // seq-cst revisions avoid data races and ABA, including on ARM64.
    class Channel
    {
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<double>::is_always_lock_free);

    public:
        bool Publish(const Snapshot& value) noexcept
        {
            if (!Valid(value))
                return false;
            const auto sequence = sequence_.load();
            if ((sequence & 1u) || sequence >= (std::numeric_limits<std::uint64_t>::max)() - 1)
                return false;
            sequence_.store(sequence + 1);
            enabled_.store(value.enabled);
            busy_.store(value.busy);
            generation_.store(value.generation);
            epoch_.store(value.epoch);
            active_.store(value.activeId);
            ack_.store(value.ackSerial);
            count_.store(value.count);
            for (std::size_t index = 0; index < MaximumRoutes; ++index)
                routes_[index].Store(value.routes[index]);
            sequence_.store(sequence + 2);
            return true;
        }
        bool Read(Snapshot& output) const noexcept
        {
            const auto before = sequence_.load();
            if (before & 1u)
                return false;
            Snapshot candidate;
            candidate.enabled = enabled_.load();
            candidate.busy = busy_.load();
            candidate.generation = generation_.load();
            candidate.epoch = epoch_.load();
            candidate.activeId = active_.load();
            candidate.ackSerial = ack_.load();
            candidate.count = count_.load();
            for (std::size_t index = 0; index < MaximumRoutes; ++index)
                candidate.routes[index] = routes_[index].Load();
            if (sequence_.load() != before)
                return false;
            output = candidate;
            return true;
        }

    private:
        struct AtomicRoute
        {
            std::atomic<std::uint64_t> id{ 0 };
            std::atomic<bool> zoomed{ false };
            std::atomic<double> left{ 0 }, top{ 0 }, right{ 0 }, bottom{ 0 };
            std::atomic<double> sourceLeft{ 0 }, sourceTop{ 0 }, sourceRight{ 0 }, sourceBottom{ 0 };
            void Store(const Route& value) noexcept
            {
                id.store(value.id);
                zoomed.store(value.zoomed);
                left.store(value.bounds.left);
                top.store(value.bounds.top);
                right.store(value.bounds.right);
                bottom.store(value.bounds.bottom);
                sourceLeft.store(value.source.left);
                sourceTop.store(value.source.top);
                sourceRight.store(value.source.right);
                sourceBottom.store(value.source.bottom);
            }
            Route Load() const noexcept
            {
                return { id.load(), { left.load(), top.load(), right.load(), bottom.load() }, { sourceLeft.load(), sourceTop.load(), sourceRight.load(), sourceBottom.load() }, zoomed.load() };
            }
        };
        std::atomic<std::uint64_t> sequence_{ 0 }, generation_{ 0 }, epoch_{ 0 }, active_{ 0 }, ack_{ 0 };
        std::atomic<std::size_t> count_{ 0 };
        std::atomic<bool> enabled_{ false }, busy_{ false };
        std::array<AtomicRoute, MaximumRoutes> routes_{};
    };

    class FeedbackChannel
    {
    public:
        bool Publish(const Feedback& value) noexcept
        {
            const auto sequence = sequence_.load();
            if ((sequence & 1u) || sequence >= (std::numeric_limits<std::uint64_t>::max)() - 1)
                return false;
            sequence_.store(sequence + 1);
            generation_.store(value.observedGeneration);
            serial_.store(value.requestSerial);
            target_.store(value.targetId);
            epoch_.store(value.epoch);
            x_.store(value.point.x);
            y_.store(value.point.y);
            blocked_.store(value.blockedButtons);
            forwarded_.store(value.forwardedButtons);
            canCommit_.store(value.canCommit);
            faulted_.store(value.faulted);
            reportedAt_.store(value.reportedAt);
            sequence_.store(sequence + 2);
            return true;
        }
        bool Read(Feedback& output) const noexcept
        {
            const auto before = sequence_.load();
            if (before & 1u)
                return false;
            const Feedback candidate{ generation_.load(), serial_.load(), target_.load(), epoch_.load(), { x_.load(), y_.load() }, blocked_.load(), forwarded_.load(), canCommit_.load(), faulted_.load(), reportedAt_.load() };
            if (sequence_.load() != before)
                return false;
            output = candidate;
            return true;
        }

    private:
        std::atomic<std::uint64_t> sequence_{ 0 }, generation_{ 0 }, serial_{ 0 }, target_{ 0 }, epoch_{ 0 };
        std::atomic<double> x_{ 0 }, y_{ 0 };
        std::atomic<unsigned> blocked_{ 0 }, forwarded_{ 0 };
        std::atomic<bool> canCommit_{ false }, faulted_{ false };
        std::atomic<std::uint64_t> reportedAt_{ 0 };
    };
}
