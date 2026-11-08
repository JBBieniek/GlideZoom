#pragma once

#include "zoom_session.h"

namespace glide::view
{

    enum class Scope
    {
        Monitor,
        Desktop,
        Independent
    };

    // A mode selection is committed only after the caller has restored the old
    // shared transforms and hidden its old local overlay. There is deliberately
    // no queued request: returning to a desktop never executes an old mode switch.
    // Input epochs and physical release pairing remain owned by runtime/input.
    class State
    {
    public:
        Scope Current() const noexcept { return current_; }

        bool CommitSwitch(Scope requested, session::Result restoration, bool desktopCurrent, bool recoveryPending = false, bool terminal = false) noexcept
        {
            if ((requested != Scope::Monitor && requested != Scope::Desktop && requested != Scope::Independent) ||
                !desktopCurrent || recoveryPending || terminal ||
                (restoration != session::Result::Applied && restoration != session::Result::Unchanged))
                return false;
            current_ = requested;
            return true;
        }

    private:
        Scope current_ = Scope::Monitor;
    };

} // namespace glide::view
