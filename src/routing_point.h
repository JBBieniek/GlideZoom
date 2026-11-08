// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#pragma once

#include "input_routing.h"

namespace glide::input::routing
{
    struct PhysicalPoint
    {
        bool read = false;
        Vec2 point{};
        std::uint64_t monitorId = 0;
        Rect bounds{};
    };
    inline constexpr bool CanNormalizeMove(bool mouseMove, bool injected) noexcept
    {
        return mouseMove && !injected;
    }

    // A raw proposed MOVE may lie beyond the desktop while Windows keeps its
    // physical cursor on the last pixel. Never replace a proposal that already
    // belongs to any known route, including a real adjacent-monitor crossing.
    inline bool NeedsPhysicalMovePoint(const Snapshot& snapshot, bool coherent, std::uint64_t epoch, Vec2 raw) noexcept
    {
        if (!coherent || !snapshot.enabled || snapshot.epoch != epoch ||
            !Valid(snapshot) || !pointer::ValidPoint(raw))
            return false;
        for (std::size_t index = 0; index < snapshot.count; ++index)
            if (pointer::Contains(snapshot.routes[index].bounds, raw))
                return false;
        return true;
    }

    // Caller supplies a fresh physical getter and live monitor metadata, only
    // for a noninjected MOVE on the current input desktop. No nearest-monitor
    // selection or synthetic clamp can authorize a point. Failure leaves output
    // unchanged so the ordinary unknown-route protection still applies.
    inline bool TryPhysicalMovePoint(const Snapshot& snapshot, bool coherent, std::uint64_t epoch, Vec2 raw, const PhysicalPoint& physical, Vec2& output) noexcept
    {
        if (!NeedsPhysicalMovePoint(snapshot, coherent, epoch, raw) || !physical.read ||
            !physical.monitorId || !pointer::ValidPoint(physical.point) || !pointer::ValidBounds(physical.bounds) ||
            !pointer::Contains(physical.bounds, physical.point))
            return false;
        for (std::size_t index = 0; index < snapshot.count; ++index)
        {
            const auto& route = snapshot.routes[index];
            if (route.id == physical.monitorId && pointer::Same(route.bounds, physical.bounds) &&
                pointer::Contains(route.bounds, physical.point))
            {
                output = physical.point;
                return true;
            }
        }
        return false;
    }
}
