#pragma once

#include <cstdint>
#include <utility>
#include "monitor_geometry.h"
#include "runtime_state.h"
#include "zoom_session.h"

namespace glide::frame
{

    // Native window and magnification calls can synchronously dispatch messages.
    // The application defers reset/cleanup until the outer native transaction has
    // returned. This lease prevents the cancelled outer frame from continuing to
    // mutate global state or publishing a stale successful result in the meantime.
    // It is confined to the owner UI thread; it is not a lock or cross-thread guard.
    class Lease
    {
    public:
        explicit Lease(const runtime::State& state) noexcept
            :
            state_(state), epoch_(state.Epoch()), invalid_(!state.Ready()) {}

        bool Valid() const noexcept
        {
            if (!state_.Ready() || state_.Epoch() != epoch_)
                invalid_ = true;
            return !invalid_;
        }

        template<class Function>
        bool Call(Function&& operation) const noexcept(noexcept(std::forward<Function>(operation)()))
        {
            if (!Valid())
                return false;
            const bool success = static_cast<bool>(std::forward<Function>(operation)());
            // Check the lease even if the native call reports failure: cancellation
            // is permanent for this frame, including a temporary pending restore.
            const bool current = Valid();
            return success && current;
        }

    private:
        const runtime::State& state_;
        const std::uint64_t epoch_;
        mutable bool invalid_;
    };

    template<class Native>
    class Backend
    {
    public:
        Backend(Native& native, const Lease& lease) noexcept :
            native_(native), lease_(lease) {}

        bool Read(session::Snapshot& snapshot)
        {
            session::Snapshot observed{};
            if (!lease_.Call([&]() { return native_.Read(observed); }))
                return false;
            snapshot = observed;
            return true;
        }
        bool SetVisual(session::Visual visual)
        {
            return lease_.Call([&]() { return native_.SetVisual(visual); });
        }
        bool SetInput(session::Input input)
        {
            return lease_.Call([&]() { return native_.SetInput(input); });
        }

    private:
        Native& native_;
        const Lease& lease_;
    };

    template<class Native>
    class Renderer
    {
    public:
        Renderer(Native& native, const Lease& lease) noexcept :
            native_(native), lease_(lease) {}

        bool Prepare(const monitor::Frame& frame)
        {
            return lease_.Call([&]() { return native_.Prepare(frame); });
        }
        bool Pending()
        {
            return lease_.Call([&]() { return native_.Pending(); });
        }
        bool CanRetain(const session::Snapshot& snapshot)
        {
            return lease_.Call([&]() { return native_.CanRetain(snapshot); });
        }
        bool Matches(const monitor::Frame& frame)
        {
            return lease_.Call([&]() { return native_.Matches(frame); });
        }
        bool Show()
        {
            return lease_.Call([&]() { return native_.Show(); });
        }
        // Cancellation must never prevent hiding a partial private overlay.
        void Hide() noexcept(noexcept(std::declval<Native&>().Hide())) { native_.Hide(); }

    private:
        Native& native_;
        const Lease& lease_;
    };

} // namespace glide::frame
