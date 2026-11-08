// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#pragma once

#include "monitor_geometry.h"

namespace glide::monitor
{
    enum class VisualRefresh
    {
        Presented,
        Pending,
        Failed
    };

    // Refreshes private windows only. Deliberately accepts no Session, input
    // backend or recovery journal: another monitor owns the single input map.
    // Pending capture is retried without discarding the desired zoom state.
    template<class Renderer>
    VisualRefresh RefreshPassive(Renderer& renderer, const Frame& frame) noexcept
    {
        renderer.SetCursorEnabled(false);
        if (!ValidFrame(frame))
        {
            renderer.Hide();
            return VisualRefresh::Failed;
        }
        const auto deferred = [&]() noexcept {
            if (!renderer.Pending())
            {
                renderer.Hide();
                return VisualRefresh::Failed;
            }
            if (!renderer.Active())
                return VisualRefresh::Pending;
            Frame displayed;
            if (renderer.TryGetPresentedFrame(displayed) && renderer.CanRetainVisual(displayed))
                return VisualRefresh::Pending;
            // CanRetainVisual may have discovered a native/order failure and
            // already revoked Pending. Never disguise that as capture startup.
            const bool retry = renderer.Pending();
            // An old active image can contain a baked cursor. Until a cursorless
            // frame is available, hide only this view and retry its same zoom.
            renderer.Hide();
            return retry ? VisualRefresh::Pending : VisualRefresh::Failed;
        };
        if (!renderer.Matches(frame) && !renderer.Prepare(frame))
            return deferred();
        if (!renderer.Show())
            return deferred();
        return VisualRefresh::Presented;
    }
}
