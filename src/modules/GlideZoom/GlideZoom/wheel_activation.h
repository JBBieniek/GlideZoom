// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#pragma once

#include "pointer_motion.h"
#include "wheel_acceleration.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>

namespace glide::wheel
{
    inline constexpr std::uint64_t ActivationMilliseconds = 2000;
    inline constexpr double MaximumActivationTravel = 120.0 * 64;

    // This authorizes camera intent only. It never authorizes forwarding a
    // click or changing the one global input map before its routing barrier.
    struct Activation
    {
        bool enabled = false;
        std::uint64_t epoch = 0, generation = 0, monitorId = 0;
        std::uint64_t startedAt = 0, deadline = 0;
        Rect bounds{};
        bool observeOnly = false;
    };

    inline bool Valid(const Activation& value) noexcept
    {
        return !value.enabled ||
               (value.epoch != 0 && value.generation != 0 && value.monitorId != 0 &&
                value.startedAt != 0 && value.deadline >= value.startedAt &&
                value.deadline - value.startedAt <= ActivationMilliseconds && pointer::ValidBounds(value.bounds));
    }

    inline constexpr std::uint64_t ActivationDeadline(std::uint64_t timestamp) noexcept
    {
        constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
        return timestamp > maximum - ActivationMilliseconds ? maximum : timestamp + ActivationMilliseconds;
    }

    inline bool Admit(const Activation& value, std::uint64_t epoch, std::uint64_t generation, std::uint64_t targetId, Vec2 point, std::uint64_t timestamp, std::uint64_t now, bool chord, bool idle) noexcept
    {
        return value.enabled && !value.observeOnly && Valid(value) && chord && idle && value.epoch == epoch &&
               value.generation == generation && value.monitorId == targetId &&
               pointer::ValidPoint(point) && pointer::Contains(value.bounds, point) &&
               timestamp >= value.startedAt && timestamp <= now && now <= value.deadline;
    }

    // One UI writer, one hook reader, one bounded attempt. Every payload field
    // is atomic, including ARM64; there is no spin, allocation or callback.
    class ActivationChannel
    {
        static_assert(std::atomic<std::uint64_t>::is_always_lock_free && std::atomic<double>::is_always_lock_free);

    public:
        bool Publish(const Activation& value) noexcept
        {
            if (!Valid(value))
                return false;
            const auto sequence = sequence_.load();
            if ((sequence & 1u) || sequence >= (std::numeric_limits<std::uint64_t>::max)() - 1)
                return false;
            sequence_.store(sequence + 1);
            enabled_.store(value.enabled);
            epoch_.store(value.epoch);
            generation_.store(value.generation);
            monitor_.store(value.monitorId);
            start_.store(value.startedAt);
            deadline_.store(value.deadline);
            left_.store(value.bounds.left);
            top_.store(value.bounds.top);
            right_.store(value.bounds.right);
            bottom_.store(value.bounds.bottom);
            observeOnly_.store(value.observeOnly);
            sequence_.store(sequence + 2);
            return true;
        }
        bool Read(Activation& output) const noexcept
        {
            const auto before = sequence_.load();
            if (before & 1u)
                return false;
            const Activation next{ enabled_.load(), epoch_.load(), generation_.load(), monitor_.load(), start_.load(), deadline_.load(), { left_.load(), top_.load(), right_.load(), bottom_.load() }, observeOnly_.load() };
            if (sequence_.load() != before)
                return false;
            output = next;
            return true;
        }

    private:
        std::atomic<std::uint64_t> sequence_{ 0 }, epoch_{ 0 }, generation_{ 0 }, monitor_{ 0 }, start_{ 0 }, deadline_{ 0 };
        std::atomic<bool> enabled_{ false };
        std::atomic<bool> observeOnly_{ false };
        std::atomic<double> left_{ 0 }, top_{ 0 }, right_{ 0 }, bottom_{ 0 };
    };

    // The UI coalesces accepted wheel travel while the first renderer warms.
    // The first target is fixed once; additional input is applied only after
    // that exact monitor has a verified presented frame and matching input map.
    class ActivationIntent
    {
    public:
        void Reset() noexcept { *this = {}; }
        bool Active() const noexcept { return activation_.enabled; }
        bool Committed() const noexcept { return committedEpoch_ != 0; }
        bool Accepting() const noexcept { return Active() && accepting_; }
        std::uint64_t MonitorId() const noexcept { return activation_.monitorId; }
        std::uint64_t StartedAt() const noexcept { return activation_.startedAt; }
        bool MatchesEpoch(std::uint64_t epoch) const noexcept
        {
            return Active() && (epoch == previousEpoch_ || epoch == activation_.epoch || (Committed() && epoch == committedEpoch_));
        }
        Vec2 Anchor() const noexcept { return anchor_; }
        const Rect& Bounds() const noexcept { return activation_.bounds; }
        bool Current(std::uint64_t now) const noexcept
        {
            return Active() && now >= activation_.startedAt && now <= activation_.deadline;
        }
        Activation Admission() const noexcept
        {
            auto value = activation_;
            value.enabled = Accepting() && !Committed();
            return value;
        }
        Activation Observation() const noexcept
        {
            // After Commit the old routing epoch/generation cannot admit any
            // more busy input, but the hook still observes physical release or
            // departure until this same bounded burst finishes.
            auto value = activation_;
            value.enabled = Active();
            value.observeOnly = true;
            return value;
        }
        bool Begin(const Activation& admission, std::uint64_t previousEpoch, double delta, Vec2 point, std::uint64_t now, bool accelerated, int limit) noexcept
        {
            Reset();
            if (!admission.enabled || admission.observeOnly || !Valid(admission) || !pointer::ValidPoint(point) ||
                !pointer::Contains(admission.bounds, point) || !std::isfinite(delta) || delta == 0 ||
                std::abs(delta) > MaximumActivationTravel || now < admission.startedAt || now > admission.deadline)
                return false;
            activation_ = admission;
            previousEpoch_ = previousEpoch;
            lastTimestamp_ = admission.startedAt;
            anchor_ = point;
            travel_ = std::abs(delta);
            first_ = accelerator_.Apply(delta, admission.startedAt, admission.epoch, accelerated, limit);
            accepting_ = true;
            return true;
        }
        bool Accepts(std::uint64_t epoch, std::uint64_t timestamp, Vec2 point, std::uint64_t now) const noexcept
        {
            return Accepting() && Current(now) &&
                   MatchesEpoch(epoch) &&
                   timestamp >= lastTimestamp_ && timestamp >= activation_.startedAt && timestamp <= now &&
                   pointer::ValidPoint(point) && pointer::Contains(activation_.bounds, point);
        }
        bool Push(double delta, std::uint64_t timestamp, std::uint64_t epoch, Vec2 point, std::uint64_t now, bool accelerated, int limit) noexcept
        {
            if (!Accepts(epoch, timestamp, point, now) || !std::isfinite(delta) ||
                std::abs(delta) > MaximumActivationTravel - travel_)
                return false;
            lastTimestamp_ = timestamp;
            travel_ += std::abs(delta);
            pending_ += accelerator_.Apply(delta, timestamp, activation_.epoch, accelerated, limit);
            return true;
        }
        bool TakeInitial(double& output) noexcept
        {
            if (!Active() || initialTaken_)
                return false;
            initialTaken_ = true;
            output = first_;
            return true;
        }
        double TakePending() noexcept
        {
            const double value = pending_;
            pending_ = 0;
            return value;
        }
        bool Commit(std::uint64_t epoch, std::uint64_t now) noexcept
        {
            if (!Current(now) || !epoch || !initialTaken_)
                return false;
            committedEpoch_ = epoch;
            return true;
        }
        // A modifier release ends admission, but an already accepted camera
        // target remains valid within this same monitor's short startup lease.
        void CloseAdmission() noexcept { accepting_ = false; }

    private:
        Activation activation_{};
        Accelerator accelerator_{};
        Vec2 anchor_{};
        std::uint64_t previousEpoch_ = 0, committedEpoch_ = 0, lastTimestamp_ = 0;
        double first_ = 0, pending_ = 0, travel_ = 0;
        bool accepting_ = false, initialTaken_ = false;
    };
}
