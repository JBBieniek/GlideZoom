#pragma once

#include <cmath>

#include "settings.h"

namespace glide::settings::update
{
    enum class Action
    {
        Invalid,
        Unchanged,
        Defer,
        Live,
        Restore
    };

    struct Context
    {
        bool safeBoundary = true;
        bool scopePending = false;
        double largestZoomInUse = 1;
    };

    // Include submitted, precise and requested zoom on every live/parked view.
    // An inactive remembered toggle value is clamped when used, not a camera.
    inline void ObserveZoom(Context& context, double zoom) noexcept
    {
        if (!std::isfinite(context.largestZoomInUse))
            return;
        if (!std::isfinite(zoom) || zoom < 1)
            context.largestZoomInUse = std::nan("");
        else if (zoom > context.largestZoomInUse)
            context.largestZoomInUse = zoom;
    }

    inline Action Classify(const Values& current, const Values& candidate, const Context& context) noexcept
    {
        if (!Valid(candidate))
            return Action::Invalid;
        if (current == candidate && !context.scopePending)
            return Action::Unchanged;
        if (!context.safeBoundary)
            return Action::Defer;
        if (context.scopePending || current.zoomScope != candidate.zoomScope ||
            !std::isfinite(context.largestZoomInUse) || context.largestZoomInUse < 1 ||
            context.largestZoomInUse > candidate.maxZoom)
            return Action::Restore;
        return Action::Live;
    }

    inline bool InputChanged(const Values& current, const Values& candidate) noexcept
    {
        return current.modifier != candidate.modifier || current.sensitivity != candidate.sensitivity ||
               current.keyboardGesture != candidate.keyboardGesture || current.mouseGesture != candidate.mouseGesture ||
               current.reverse != candidate.reverse || current.centerFollow != candidate.centerFollow ||
               current.touchpadGesture != candidate.touchpadGesture || current.pointerScaling != candidate.pointerScaling ||
               current.zoomMultiplier != candidate.zoomMultiplier || current.edgePanMultiplier != candidate.edgePanMultiplier ||
               current.wheelAcceleration != candidate.wheelAcceleration || current.wheelAccelerationLimit != candidate.wheelAccelerationLimit ||
               current.pointerSpeed != candidate.pointerSpeed;
    }
}
