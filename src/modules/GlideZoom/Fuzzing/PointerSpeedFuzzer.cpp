// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "../GlideZoom/pointer_motion.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace
{
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
