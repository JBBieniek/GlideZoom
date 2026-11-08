#pragma once

#include <cstdint>

namespace glide::input
{

    // Hook-thread state. Ordinary application activation is not a loss of the
    // global zoom gesture's authority. Desktop/session, heartbeat, configuration
    // and physical-key checks remain with Host and the input state machines.
    // Only pointer history is scoped to foreground activation: another app may
    // reposition the cursor, so the next physical sample must use a fresh baseline.
    class ForegroundContext
    {
    public:
        template<class Pointer>
        void Observe(std::uintptr_t foreground, Pointer& pointer) noexcept
        {
            if (foreground == foreground_)
                return;
            foreground_ = foreground;
            pointer.Pause();
        }

    private:
        std::uintptr_t foreground_ = 0;
    };

} // namespace glide::input
