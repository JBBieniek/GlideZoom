// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#pragma once

#include "monitor_geometry.h"

#include <cstdint>

namespace glide::cursor
{
    // Both the physical point and its projected hotspot must belong to this
    // captured display. A global cursor hide must never be justified by pixels
    // on another monitor, or by a source point outside the routed input crop.
    inline bool HotspotVisible(const monitor::Frame& frame, Vec2 physical) noexcept
    {
        return monitor::ValidFrame(frame) &&
               monitor::Contains(monitor::AsRect(frame.destination), physical) &&
               monitor::Contains(monitor::AsRect(frame.source), physical) &&
               monitor::Contains(monitor::AsRect(frame.destination), SourceToScreen(physical, frame.transform));
    }

    // Describes the cursor shape and geometry of the submitted image, rather
    // than a pending renderer target. Native code additionally verifies HWND,
    // desktop, current monitor and cursor flags before taking a visibility lease.
    // Submission does not claim that Windows has atomically displayed the image.
    class Presentation
    {
    public:
        constexpr bool HasCursor() const noexcept { return shape_ != 0; }

        void Clear() noexcept
        {
            shape_ = 0;
            frame_ = {};
        }

        void Commit(const monitor::Frame& frame, std::uintptr_t shape) noexcept
        {
            if (!shape || !monitor::ValidFrame(frame))
            {
                Clear();
                return;
            }
            frame_ = frame;
            shape_ = shape;
        }

        bool Covers(const monitor::Frame& current, Vec2 physical, std::uintptr_t currentShape, bool suppressed) const noexcept
        {
            return shape_ && currentShape == shape_ && !suppressed &&
                   monitor::SameFrame(current, frame_) && HotspotVisible(frame_, physical);
        }

    private:
        monitor::Frame frame_{};
        std::uintptr_t shape_ = 0;
    };
}
