// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#pragma once

#include "input_routing.h"
#include "pointer_adapter.h"
#include "pointer_channel.h"

namespace glide::input
{
    inline constexpr pointer::View PointerView(const PointerSnapshot& snapshot, bool forwarded) noexcept
    {
        return { snapshot.requested, snapshot.active, snapshot.bounds, snapshot.zoom, snapshot.epoch, snapshot.offset, forwarded ? 1 : snapshot.edgePanMultiplier, snapshot.speedMultiplier, snapshot.adjustForZoom };
    }

    template<class Native>
    bool RecoverPointer(std::uint64_t now, const PointerSnapshot& snapshot, std::uint64_t controlsEpoch, bool coherent, bool enabled, bool inputGate, bool inputIdle, const routing::State& routing, pointer::Adapter& ordinary, Native& native) noexcept
    {
        return ordinary.Recover(now, PointerView(snapshot, false), coherent && enabled && inputGate && inputIdle && routing.ZoomReady() && !routing.AnyButtonDown() && snapshot.epoch == controlsEpoch, native);
    }
    // The real hook and composed tests share this decision. Free pointer motion
    // needs no held mouse button; a forwarded application's drag only selects a
    // clip-respecting native adapter. Neither path authorizes a new click.
    template<class Native>
    bool DispatchPointer(Vec2 point, std::uint64_t now, const PointerSnapshot& snapshot, std::uint64_t controlsEpoch, bool coherent, bool enabled, bool inputGate, bool gestureActive, const routing::State& routing, pointer::Adapter& ordinary, pointer::Adapter& drag, Native& native) noexcept
    {
        if (!coherent || !routing.ZoomReady() || snapshot.epoch != controlsEpoch || !enabled || !inputGate)
        {
            ordinary.Pause();
            drag.Pause();
            return false;
        }
        const bool forwarded = routing.HasForwardedDrag();
        const auto view = PointerView(snapshot, forwarded);
        if (!forwarded)
            return ordinary.Move(point, false, now, view, true, gestureActive, native);
        pointer::DragNative<Native> clipped{ native };
        if (clipped.Initialize(view))
            return drag.Move(point, false, now, view, true, gestureActive, clipped);
        drag.Pause();
        return false;
    }
}
