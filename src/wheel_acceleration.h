// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace glide::wheel
{
    // One boost belongs to 120 units of physical wheel travel, never to an input
    // packet or UI frame. Fractional/high-resolution packets spend that same
    // credit immediately, retaining their original responsiveness and total gain.
    class Accelerator
    {
    public:
        void Reset() noexcept { *this = {}; }

        double Apply(double delta, std::uint64_t timestamp, std::uint64_t epoch, bool enabled, int limit = 3) noexcept
        {
            if (!std::isfinite(delta))
            {
                Reset();
                return 0;
            }
            if (!enabled || !timestamp || limit < 1 || limit > 4)
            {
                Reset();
                return delta;
            }
            if (delta == 0)
                return delta;
            const int direction = delta > 0 ? 1 : -1;
            if (!havePacket_ || epoch != epoch_ || limit != limit_ || direction != direction_ ||
                timestamp < lastPacket_ || timestamp - lastPacket_ > 250)
                Reset();
            epoch_ = epoch;
            limit_ = limit;
            direction_ = direction;
            havePacket_ = true;
            lastPacket_ = timestamp;
            // The native adapter supplies a signed 16-bit wheel delta. Bound
            // malformed callers too, so credit processing has constant work.
            double remaining = (std::min)(std::abs(delta), 32768.0);
            double output = 0;
            for (unsigned group = 0; remaining > 0 && group < 275; ++group)
            {
                if (credit_ == 0)
                {
                    boost_ = haveGroup_ && timestamp - groupStarted_ < 80 ?
                                 (std::min)(boost_ + 1, limit) :
                                 1;
                    groupStarted_ = timestamp;
                    haveGroup_ = true;
                }
                const double consumed = (std::min)(remaining, 120.0 - credit_);
                output += consumed * boost_;
                remaining -= consumed;
                credit_ += consumed;
                if (credit_ >= 120.0 - 1e-9)
                    credit_ = 0;
            }
            return std::copysign(output, delta);
        }

    private:
        std::uint64_t epoch_ = 0, lastPacket_ = 0, groupStarted_ = 0;
        double credit_ = 0;
        int boost_ = 1, limit_ = 0, direction_ = 0;
        bool havePacket_ = false, haveGroup_ = false;
    };
}
