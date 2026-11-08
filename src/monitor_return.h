#pragma once

#include "monitor_geometry.h"
#include "viewport_motion.h"

namespace glide::monitor_return
{
    // A returning pointer is a physical monitor-entry position, not a movement
    // delta from the parked camera's last sample. Preserve a reachable crop;
    // otherwise anchor that source point at its present screen position. No
    // cursor write, synthetic movement, zoom change or input admission occurs.
    inline bool RevealEntry(Transform parked, Rect bounds, Vec2 entry, Transform& output) noexcept
    {
        if (!monitor::Contains(bounds, entry) || !viewport::ValidTransform(parked))
            return false;
        monitor::Frame frame{};
        if (!monitor::MakeFrame(bounds, parked, frame))
            return false;
        if (monitor::Contains(monitor::AsRect(frame.source), entry))
        {
            output = parked;
            return true;
        }
        Transform candidate{ parked.zoom,
                             ClampOffset({ entry.x - entry.x / parked.zoom, entry.y - entry.y / parked.zoom }, parked.zoom, bounds) };
        if (!monitor::MakeFrame(bounds, candidate, frame))
            return false;
        if (!monitor::Contains(monitor::AsRect(frame.source), entry))
        {
            // The input RECT rounds to integer source pixels with an exclusive
            // right/bottom edge. At high zoom, anchoring at the last output pixel
            // can round that edge onto the entry itself. Allow one source pixel
            // of output inset so the verified input crop actually includes it.
            const double insetX = (std::min)(parked.zoom, (bounds.right - bounds.left) / 2);
            const double insetY = (std::min)(parked.zoom, (bounds.bottom - bounds.top) / 2);
            const Vec2 destination{ (std::clamp)(entry.x, bounds.left + insetX, bounds.right - insetX),
                                    (std::clamp)(entry.y, bounds.top + insetY, bounds.bottom - insetY) };
            candidate.offset = ClampOffset({ entry.x - destination.x / parked.zoom, entry.y - destination.y / parked.zoom }, parked.zoom, bounds);
            if (!monitor::MakeFrame(bounds, candidate, frame) || !monitor::Contains(monitor::AsRect(frame.source), entry))
                return false;
        }
        output = candidate;
        return true;
    }
}
