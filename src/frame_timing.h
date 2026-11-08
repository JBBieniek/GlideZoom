#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace glide::timing
{

    // Single-threaded frame deadlines for a QPC-backed native message pump.
    // A timer is only a wake-up: the deadline decides whether one current frame is
    // due. A late wake skips obsolete slots without queuing or replaying frames.
    class Cadence
    {
    public:
        bool Configure(std::uint64_t frequency, double refreshHz) noexcept
        {
            configured_ = seeded_ = false;
            frequency_ = interval_ = deadline_ = last_ = 0;
            rate_ = 60.0;
            // QueryPerformanceFrequency uses a positive signed LARGE_INTEGER.
            if (!frequency || frequency > static_cast<std::uint64_t>(
                                              std::numeric_limits<std::int64_t>::max()))
                return false;
            frequency_ = frequency;
            rate_ = std::isfinite(refreshHz) && refreshHz > 0.0 ? std::clamp(refreshHz, 1.0, 240.0) : 60.0;
            interval_ = static_cast<std::uint64_t>(
                std::ceil(static_cast<long double>(frequency_) / rate_));
            interval_ = std::clamp<std::uint64_t>(interval_, 1, frequency_);
            configured_ = true;
            return true;
        }

        bool Reset(std::uint64_t now) noexcept
        {
            if (!configured_)
                return false;
            deadline_ = last_ = now;
            seeded_ = true; // The first current frame may be rendered immediately.
            return true;
        }

        bool Valid() const noexcept { return configured_ && seeded_; }
        std::uint64_t Frequency() const noexcept { return frequency_; }
        std::uint64_t IntervalTicks() const noexcept { return interval_; }
        double Rate() const noexcept { return rate_; }

        bool Due(std::uint64_t now) const noexcept
        {
            return Valid() && (now < last_ || now >= deadline_);
        }

        bool Consume(std::uint64_t now) noexcept
        {
            if (!Valid())
                return false;
            if (!Due(now))
                return true;
            // Preserve the existing phase where possible. Clock rollback instead
            // seeds a fresh phase; it must not strand the app behind an old time.
            const std::uint64_t delay = now < last_ ? interval_ : interval_ - ((now - deadline_) % interval_);
            if (now > std::numeric_limits<std::uint64_t>::max() - delay)
            {
                configured_ = seeded_ = false;
                return false;
            }
            deadline_ = now + delay;
            last_ = now;
            return true;
        }

        // Magnitude of a relative SetWaitableTimer due time. Zero means invalid;
        // valid waits are at least 100 ns and never more than one second. Negate
        // this value at the Win32 boundary. No signed multiplication can overflow.
        std::int64_t Delay100ns(std::uint64_t now) const noexcept
        {
            if (!Valid())
                return 0;
            if (Due(now))
                return 1;
            const long double delay = std::ceil(
                static_cast<long double>(deadline_ - now) * 10000000.0L / frequency_);
            return static_cast<std::int64_t>(std::clamp(delay, 1.0L, 10000000.0L));
        }

    private:
        bool configured_ = false, seeded_ = false;
        std::uint64_t frequency_ = 0, interval_ = 0, deadline_ = 0, last_ = 0;
        double rate_ = 60.0;
    };

} // namespace glide::timing
