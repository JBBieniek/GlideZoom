#pragma once

#include "zoom_core.h"

namespace glide::animation
{

    // Windows consumes a floating-point scale. Use that representation for native
    // dirty/identity decisions. Keep raw gesture/wheel accumulators as doubles and
    // quantize only the requested output, so small input packets are not discarded.
    inline bool ValidScale(double zoom) noexcept
    {
        return std::isfinite(zoom) && zoom >= 1.0 && zoom <= 4096.0;
    }

    inline bool Magnified(double zoom) noexcept
    {
        return ValidScale(zoom) && static_cast<float>(zoom) > 1.0f;
    }

    inline bool NativeScaleChanged(double requested, double applied) noexcept
    {
        // An invalid value must reach the caller's validation, not look unchanged.
        return !ValidScale(requested) || !ValidScale(applied) ||
               static_cast<float>(requested) != static_cast<float>(applied);
    }

    inline double Bounded(double value, double maximum) noexcept
    {
        maximum = ValidScale(maximum) ? maximum : 1.0;
        value = ValidScale(value) ? value : 1.0;
        return std::clamp(value, 1.0, maximum);
    }

    // A full Windows wheel notch / key step is +/-120. The return value is the RAW
    // accumulator: feeding a rounded output back here would lose partial reports.
    inline double StepZoom(double rawCurrent, double wheelDelta, double maximum = 4096.0, int multiplier = 1) noexcept
    {
        maximum = ValidScale(maximum) ? maximum : 1.0;
        rawCurrent = Bounded(rawCurrent, maximum);
        if (std::isnan(wheelDelta) || multiplier < 1 || multiplier > 100)
            return rawCurrent;
        return std::clamp(rawCurrent + wheelDelta / 120.0 * (0.01 * multiplier), 1.0, maximum);
    }

    // Output changes immediately in 0.01x increments; there is no interpolation or
    // clock dependency. Tiny numerical slack makes decimal half-step ties stable
    // after packet accumulation, without adding a perceptible threshold.
    inline double Quantize(double rawTarget, double maximum = 4096.0) noexcept
    {
        maximum = ValidScale(maximum) ? maximum : 1.0;
        const double bounded = Bounded(rawTarget, maximum);
        if (bounded == maximum)
            return maximum; // Preserve a non-grid configured endpoint.
        constexpr double numericSlack = 0.000000001;
        const double stepped = std::floor((bounded + numericSlack) * 100.0 + 0.5) / 100.0;
        return std::clamp(stepped, 1.0, maximum);
    }

    // Fingers can report alternating tiny displacements while resting near a step
    // midpoint. Keep the current selected step until raw displacement passes the
    // usual half-step plus 0.001x. This is hysteresis, not time-based smoothing.
    inline double QuantizeHeld(double rawTarget, double currentTarget, double maximum = 4096.0) noexcept
    {
        maximum = ValidScale(maximum) ? maximum : 1.0;
        const double bounded = Bounded(rawTarget, maximum);
        if (bounded == 1.0 || bounded == maximum)
            return bounded;
        const double current = Quantize(currentTarget, maximum);
        constexpr double threshold = 0.006000001;
        if (std::abs(bounded - current) <= threshold)
            return current;
        return Quantize(bounded, maximum);
    }

    inline bool RestoreIdentity(double target, double animated) noexcept
    {
        return ValidScale(target) && !Magnified(target) &&
               ValidScale(animated) && !Magnified(animated);
    }

} // namespace glide::animation
