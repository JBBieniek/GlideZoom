// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "../src/pointer_motion.h"
#include "../src/pointer_dispatch.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace
{
    void HistoricalReadbacks(const std::uint8_t* data, std::size_t size)
    {
        using namespace glide;
        struct Native
        {
            Vec2 point{ 10000, 10000 }, requested{}, historical{};
            bool delayed = false, readable = true;
            unsigned writes = 0;
            bool Ready(const pointer::View&) const noexcept { return true; }
            bool Read(Vec2& output) const noexcept
            {
                output = point;
                return readable;
            }
            bool CanPlace(Vec2) const noexcept { return true; }
            bool Warp(Vec2 value) noexcept
            {
                ++writes;
                requested = value;
                point = delayed ? historical : value;
                return true;
            }
        };
        for (std::size_t at = 0; at + 3 < size && at < 128; at += 4)
        {
            Native native;
            pointer::Adapter adapter;
            pointer::View view{ true, true, { 0, 0, 40000, 40000 }, 2.0 + data[at] % 63, 7 };
            if (!adapter.Move({ 10256, 10000 }, false, 100, view, true, false, native))
                std::abort();
            native.historical = native.point;
            if (!adapter.Move({ native.point.x + 256, 10000 }, false, 101, view, true, false, native))
                std::abort();
            const unsigned history = data[at + 1] % 4;
            const std::uint64_t start = history == 1 ? 165 : 102;
            if (history == 2)
                adapter.Pause();
            if (history == 3)
                ++view.epoch;
            native.delayed = true;
            const bool consumed = adapter.Move({ native.point.x + 256, 10000 }, false, start, view, true, false, native);
            if (native.writes != 3 || adapter.FeedbackSnapshot().valid)
                std::abort();
            if (history != 0)
            {
                // Identical coordinates from expired or canceled history are
                // still interference, not authority for a pending proposal.
                if (consumed || adapter.Pending() || adapter.FailureReason() != pointer::Failure::Readback)
                    std::abort();
                continue;
            }
            if (!consumed || !adapter.Pending() || adapter.Faulted() || adapter.PendingCounts().acknowledged != 0)
                std::abort();
            const unsigned action = data[at + 2] % 8;
            const std::uint64_t now = start + data[at + 3] % 40;
            auto current = view;
            if (action == 0)
                native.point = native.requested;
            if (action == 2)
                native.point.x += 2000;
            if (action == 3)
                native.readable = false;
            if (action == 5)
                ++current.epoch;
            if (action == 6)
                adapter.Pause();
            if (action == 7)
            {
                native.point.x += 2000;
                if (adapter.Move(native.point, false, now, current, true, false, native) ||
                    adapter.Pending() || adapter.Faulted() || adapter.PendingCounts().interrupted != 1)
                    std::abort();
            }
            const bool accepted = adapter.ServicePending(now, current, action != 4, false, native);
            const bool expected = action == 0 && now - start <= pointer::Adapter::PendingLeaseMs;
            if (accepted != expected || native.writes != 3 ||
                adapter.FeedbackSnapshot().valid != expected ||
                adapter.PendingCounts().acknowledged != (expected ? 1u : 0u))
                std::abort();
            if (adapter.Pending())
            {
                // Repeated history cannot extend the original request's lease,
                // even if the exact new target appears after its deadline.
                if (action != 1 || adapter.ServicePending(start + 32, current, true, false, native) || !adapter.Pending())
                    std::abort();
                native.point = native.requested;
                if (adapter.ServicePending(start + 33, current, true, false, native) ||
                    adapter.Pending() || adapter.FailureReason() != pointer::Failure::Readback || native.writes != 3)
                    std::abort();
            }
        }
    }

    void DeferredWrites(const std::uint8_t* data, std::size_t size)
    {
        using namespace glide;
        struct Native
        {
            Vec2 point{ 10000, 10000 }, requested{};
            bool ready = true, read = true;
            bool synchronousEcho = false;
            pointer::Adapter* adapter = nullptr;
            unsigned writes = 0;
            bool Ready(const pointer::View&) const noexcept { return ready; }
            bool Read(Vec2& output) const noexcept
            {
                output = point;
                return read;
            }
            bool CanPlace(Vec2) const noexcept { return true; }
            bool Warp(Vec2 value) noexcept
            {
                ++writes;
                requested = value; // Measured success before native position update.
                if (synchronousEcho)
                    adapter->ObserveWarp(value);
                return true;
            }
        };
        for (std::size_t at = 0; at + 3 < size && at < 256; at += 4)
        {
            Native native;
            pointer::Adapter adapter;
            native.adapter = &adapter;
            native.synchronousEcho = (data[at + 3] & 1) != 0;
            const pointer::View view{ true, true, { 0, 0, 40000, 40000 }, 2.0 + data[at] % 63, 7, {}, 1, 0.25, true };
            if (!adapter.Move({ 10256, 10000 }, false, 100, view, true, false, native) ||
                !adapter.Pending() || adapter.Faulted() || adapter.FeedbackSnapshot().valid || native.writes != 1)
                std::abort();
            const unsigned action = data[at + 1] % 9;
            auto current = view;
            const std::uint64_t now = 100 + data[at + 2] % 40;
            bool callbackDelivered = false;
            if (action == 0)
                native.point = native.requested;
            if (action == 1)
                native.point.x += 2000; // Arbitrary interference never verifies.
            if (action == 2)
                adapter.Pause();
            if (action == 3)
                ++current.epoch;
            if (action == 4)
                native.ready = false;
            if (action == 5)
                native.read = false;
            if (action == 6)
            {
                callbackDelivered = adapter.BypassEcho(native.requested, now);
                // Callback alone is no ACK. The short echo lease promises one
                // bypass, not an unlimited number of duplicate callbacks.
                if (!callbackDelivered || !adapter.Pending() || adapter.FeedbackSnapshot().valid ||
                    adapter.PendingCounts().acknowledged != 0 || native.writes != 1)
                    std::abort();
            }
            if (action == 8)
            {
                // Only a distinct real packet supplies evidence for this new
                // committed position. The timer mismatch above stays strict.
                native.point.x += 1200;
                const Vec2 candidate = native.point;
                if (adapter.Move(candidate, false, now, current, true, false, native) ||
                    adapter.Pending() || adapter.Faulted() || native.writes != 1 ||
                    adapter.PendingCounts().interrupted != 1 || adapter.FeedbackSnapshot().valid)
                    std::abort();
            }
            const bool accepted = adapter.ServicePending(now, current, action != 7, false, native);
            const bool expected = action == 0 && now - 100 <= pointer::Adapter::PendingLeaseMs;
            if (accepted != expected || native.writes != 1 ||
                (adapter.FeedbackSnapshot().valid && !expected) ||
                adapter.PendingCounts().acknowledged != (expected ? 1u : 0u))
                std::abort();
            if (adapter.Pending())
            {
                // No second write or motion replay while native completion is
                // still unobserved. Windows receives the distinct new packet.
                const Vec2 candidate{ native.point.x + 13, native.point.y + 7 };
                if (adapter.Move(candidate, false, now + 1, current, true, false, native) ||
                    adapter.Pending() || native.writes != 1 || adapter.FeedbackSnapshot().valid)
                    std::abort();
            }
            adapter.Pause();
            native.point = native.requested;
            const bool lateBypassed = adapter.BypassEcho(native.requested, now + 2);
            if ((!callbackDelivered && !lateBypassed) ||
                adapter.ServicePending(now + 2, current, true, false, native) ||
                adapter.FeedbackSnapshot().valid || native.writes != 1)
                std::abort();
        }
    }

    void Recovery(const std::uint8_t* data, std::size_t size)
    {
        using namespace glide;
        struct Native
        {
            Vec2 point{ 10000, 10000 };
            bool ready = true;
            unsigned writes = 0;
            bool Ready(const pointer::View&) const noexcept { return ready; }
            bool Read(Vec2& output) const noexcept
            {
                output = point;
                return true;
            }
            bool DragBounds(const pointer::View& view, Rect& output) const noexcept
            {
                output = view.bounds;
                return ready;
            }
            bool CanPlace(Vec2) const noexcept { return true; }
            bool Warp(Vec2) noexcept
            {
                ++writes;
                return false;
            }
        } native;
        pointer::Adapter ordinary, drag;
        input::routing::State routing;
        routing.Observe({}, true, 7);
        input::PointerSnapshot policy{ true, true, { 0, 0, 40000, 40000 }, 4, 7, {}, 1, 0.25, true };
        if (input::DispatchPointer({ 10016, 10000 }, 1, policy, 7, true, true, true, false, routing, ordinary, drag, native) ||
            ordinary.FailureReason() != pointer::Failure::Warp)
            std::abort();
        bool recovered = false;
        for (std::size_t at = 0; at + 1 < size && at < 64; at += 2)
        {
            const auto bits = data[at];
            const std::uint64_t now = 501 + at * 50;
            native.ready = (bits & 1) == 0;
            native.point.x = 10000 + data[at + 1];
            const auto writes = native.writes;
            const bool accepted = input::RecoverPointer(now, policy, 7, (bits & 2) == 0, (bits & 4) == 0, (bits & 8) == 0, (bits & 16) == 0, routing, ordinary, native);
            if (native.writes != writes ||
                (accepted && (recovered || (bits & 31) != 0 || ordinary.Faulted() || ordinary.FeedbackSnapshot().valid ||
                              !pointer::Same(ordinary.Remainder(), {}))))
                std::abort();
            if (accepted)
            {
                recovered = true;
                const Vec2 candidate{ native.point.x + 16, native.point.y };
                if (input::DispatchPointer(candidate, now + 1, policy, 7, true, true, true, false, routing, ordinary, drag, native) ||
                    ordinary.FailureReason() != pointer::Failure::Warp)
                    std::abort();
            }
        }
    }

    // Independent one-dimensional travel-budget oracle. Native displacement is
    // spent in pixels before entry, source pixels/gain within, then pixels after
    // exit. It does not use the production ray/slab intersection implementation.
    double Horizontal(double start, double delta, double low, double high, double gain)
    {
        if (delta == 0)
            return start;
        const double direction = delta > 0 ? 1 : -1;
        double budget = std::abs(delta);
        double cursor = start;
        const double entry = direction > 0 ? low : high;
        const double exit = direction > 0 ? high : low;
        const bool inside = start >= low && start < high;
        if (!inside)
        {
            const double distance = (entry - cursor) * direction;
            if (distance < 0 || distance >= budget)
                return start + delta;
            cursor = entry;
            budget -= distance;
        }
        const double cost = (exit - cursor) * direction / gain;
        if (budget <= cost)
            return cursor + direction * budget * gain;
        return exit + direction * (budget - cost);
    }
}

void FuzzPointerSpeed(const std::uint8_t* data, std::size_t size)
{
    using namespace glide;
    size = (std::min)(size, std::size_t{ 16384 });
    Recovery(data, size);
    DeferredWrites(data, size);
    HistoricalReadbacks(data, size);
    for (std::size_t at = 0; at + 7 < size && at < 4096; at += 8)
    {
        pointer::View view{ true, true, { -1000, -1000, 1000, 1000 }, 1, 7 };
        view.zoom = 1 + data[at] % 64;
        view.speedMultiplier = (25 + (data[at + 1] * 256u + data[at + 2]) % 276u) / 100.0;
        view.adjustForZoom = (data[at + 3] & 1) != 0;
        const double start = (static_cast<int>(data[at + 4]) - 128) * 10.0;
        const double delta = (static_cast<int>(data[at + 5]) - 128) * 4.0;
        const Vec2 current{ start, 0 }, candidate{ start + delta, 0 };
        pointer::Step step;
        if (!pointer::Propose(current, candidate, {}, view, step) || !pointer::ValidPoint(step.point) ||
            !pointer::Finite(step.remainder) || std::abs(step.remainder.x) > 0.500001 || step.remainder.y != 0)
            std::abort();
        const double gain = view.zoom == 1 ? 1 : view.speedMultiplier / (view.adjustForZoom ? view.zoom : 1);
        const double expected = Horizontal(start, delta, -1000, 1000, gain);
        // Equivalent floating-point arithmetic can land on opposite sides of
        // an exact half-pixel tie. Both nearest integers are valid there; the
        // committed residual must still reconstruct the same source position.
        const double expectedRemainder = pointer::Contains(view.bounds, step.point) ? expected - step.point.x : 0;
        if (std::abs(step.point.x - expected) > 0.500001 || std::abs(step.remainder.x - expectedRemainder) > 1e-8 || step.point.y != 0)
            std::abort();

        const auto valid = view;
        if (data[at + 6] & 1)
        {
            view.speedMultiplier = (data[at + 7] & 1) ? 0.249 : 3.001;
            const pointer::Step before{ { 123, 456 }, { 0.25, -0.25 } };
            step = before;
            if (pointer::Propose(current, candidate, {}, view, step) ||
                !pointer::Same(step.point, before.point) || !pointer::Same(step.remainder, before.remainder))
                std::abort();
        }
        // Motion wholly above a monitor must preserve Windows exactly even with
        // prior fractional history and the maximum optional speed.
        const Vec2 outside{ start, -1100 }, outsideCandidate{ start + delta, -1101 };
        if (!pointer::Propose(outside, outsideCandidate, { 0.25, -0.25 }, valid, step) ||
            !pointer::Same(step.point, outsideCandidate) || !pointer::Same(step.remainder, {}))
            std::abort();
    }
}
