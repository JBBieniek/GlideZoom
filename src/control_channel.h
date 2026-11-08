#pragma once

#include "input_state.h"
#include "input_transport.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace glide::input
{

    inline constexpr std::uint64_t DefaultControlOptions = (400ull << 16) | (30000ull << 32);

    // One UI writer. Animation telemetry is independent of the control transaction:
    // changing zoom must never make options/epoch temporarily unreadable.
    class ControlChannel
    {
    public:
        bool Publish(Snapshot value) noexcept
        {
            zoom_.store(std::isfinite(value.zoom) ? std::clamp(value.zoom, 1.0, 64.0) : 1.0,
                        std::memory_order_relaxed);
            if (value.options == publishedOptions_ && value.epoch == publishedEpoch_)
                return false;
            controls_.Publish({ value.options, value.epoch, 1 });
            publishedOptions_ = value.options;
            publishedEpoch_ = value.epoch;
            return true;
        }
        bool Read(Snapshot& value) const noexcept { return controls_.Read(value); }
        double Zoom() const noexcept { return zoom_.load(std::memory_order_relaxed); }

    private:
        SnapshotChannel controls_{};
        std::atomic<double> zoom_{ 1 };
        // Writer-owned deduplication; the hook thread never accesses these fields.
        std::uint64_t publishedOptions_ = DefaultControlOptions, publishedEpoch_ = 0;
    };

    // Hook-thread policy, shared by the native adapter and composition tests. A
    // bounded read can overlap a real control change. Retain the last committed
    // hold until a coherent read, but never start another gesture or intercept a
    // wheel under that uncertain configuration. Independent safety gates still
    // cancel immediately; in particular a stalled writer cannot extend a hold past
    // the UI heartbeat deadline. No retries, locks, or allocations occur here.
    class ControlReader
    {
    public:
        template<class Source, class Gate, class Emit>
        bool Synchronize(const Source& source, State& state, Gate gate, Emit emit)
        {
            Snapshot candidate;
            const bool coherent = source.Read(candidate);
            if (coherent)
            {
                if (candidate.epoch != committed_.epoch)
                {
                    // Cancellation belongs to the preceding epoch. Publish the new
                    // epoch only after the sink has emitted that final old event.
                    emit(state.Cancel());
                    committed_.epoch = candidate.epoch;
                }
                if ((candidate.options & ~std::uint64_t{ 1 }) !=
                    (committed_.options & ~std::uint64_t{ 1 }))
                {
                    emit(state.Configure({ (candidate.options & 2) != 0,
                                           (candidate.options & 4) != 0,
                                           (candidate.options >> 16) & 0xFFFFu,
                                           (candidate.options >> 32) & 0xFFFFu }));
                }
                committed_.options = candidate.options;
            }
            // Evaluate after emitted cancellations: a full event queue may have
            // latched a fault in the sink, which must not be re-enabled below.
            const bool permitted = gate() && (committed_.options & 1) != 0;
            state.SetStartsAllowed(coherent && permitted);
            emit(state.SetEnabled(permitted));
            return coherent;
        }
        std::uint64_t Options() const noexcept { return committed_.options; }
        std::uint64_t Epoch() const noexcept { return committed_.epoch; }

    private:
        Snapshot committed_{ DefaultControlOptions, 0, 1 };
    };

} // namespace glide::input
