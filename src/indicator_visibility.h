// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#pragma once

#include "zoom_core.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace glide::indicator
{
    class State
    {
    public:
        void Reset() noexcept { *this = State{}; }

        void Reveal(std::uint64_t now, int duration) noexcept
        {
            if (observed_ && now < last_)
            {
                Reset();
                return;
            }
            manualUntil_ = std::max(manualUntil_, Deadline(now, duration));
            observed_ = true;
            last_ = now;
        }

        double Advance(std::uint64_t now, Transform requested, int mode, int duration, int fade, bool magnified) noexcept
        {
            if ((observed_ && now < last_) || !std::isfinite(requested.zoom) || requested.zoom < 1 ||
                requested.zoom > 4096 || !glide::detail::Finite(requested.offset))
            {
                Reset();
                return 0;
            }
            mode = mode >= 0 && mode <= 3 ? mode : 0;
            fade = std::clamp(fade, 0, 400);
            // Account for a hold ending between calls before processing new
            // motion/reveal. A delayed UI tick cannot extend a visible hold or
            // reverse a fade from an alpha that should already have decayed.
            if (target_ && previousMode_ != 3 && holdUntil_ && now >= holdUntil_)
                Transition(holdUntil_, false, fade_);
            if (magnified)
            {
                if (!hasTransform_ || requested.zoom != transform_.zoom || requested.offset.x != transform_.offset.x ||
                    requested.offset.y != transform_.offset.y)
                    motionAt_ = now;
                transform_ = requested;
                hasTransform_ = true;
            }
            else
                hasTransform_ = false;
            const auto motionUntil = hasTransform_ && mode == 1 ? Deadline(motionAt_, duration) : 0;
            const auto until = std::max(manualUntil_, motionUntil);
            const bool desired = magnified && (mode == 3 || now < until);
            if (desired != target_ || fade != fade_)
                Transition(now, desired, fade);
            holdUntil_ = until;
            previousMode_ = mode;
            observed_ = true;
            last_ = now;
            return Sample(now);
        }

    private:
        static constexpr std::uint64_t Deadline(std::uint64_t now, int duration) noexcept
        {
            const auto bounded = static_cast<std::uint64_t>(std::clamp(duration, 0, 60000));
            constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
            return now > maximum - bounded ? maximum : now + bounded;
        }

        double Sample(std::uint64_t now) const noexcept
        {
            const double target = target_ ? 1.0 : 0.0;
            if (!fade_)
                return target;
            const auto elapsed = now >= transitionAt_ ? now - transitionAt_ : 0;
            const double t = std::min(1.0, static_cast<double>(elapsed) / fade_);
            const double smooth = t * t * (3 - 2 * t);
            return from_ + (target - from_) * smooth;
        }

        void Transition(std::uint64_t now, bool target, int fade) noexcept
        {
            from_ = Sample(now);
            transitionAt_ = now;
            target_ = target;
            fade_ = fade;
        }

        Transform transform_{};
        std::uint64_t last_ = 0, manualUntil_ = 0, motionAt_ = 0, holdUntil_ = 0, transitionAt_ = 0;
        double from_ = 0;
        int previousMode_ = 0, fade_ = 0;
        bool observed_ = false, hasTransform_ = false, target_ = false;
    };
}
