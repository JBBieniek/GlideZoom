#pragma once

#include <cstdint>

namespace glide::input
{
    enum class HostHealth
    {
        Ready,
        Stale,
        Failed
    };

    // Freshness gates input, not process lifetime. A scheduler or resume interval
    // can age a live worker's heartbeat before it gets another timeslice. Callers
    // must suspend/cancel on Stale and may resume only after a fresh observation;
    // Failed requires a fault latch or an unavailable/stopped worker.
    inline constexpr bool FreshHeartbeat(std::uint64_t now, std::uint64_t beat) noexcept
    {
        return now >= beat && now - beat <= 250;
    }

    inline constexpr HostHealth EvaluateHostHealth(bool ready, bool faulted, bool running,
                                                  std::uint64_t now, std::uint64_t beat) noexcept
    {
        if (!ready || faulted || !running)
            return HostHealth::Failed;
        return FreshHeartbeat(now, beat) ? HostHealth::Ready : HostHealth::Stale;
    }
}
