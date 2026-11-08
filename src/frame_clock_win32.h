#pragma once

#include <windows.h>
#include <cstdint>
#include "frame_timing.h"

namespace glide::timing
{

    // UI-thread clock. One auto-reset wake-up handle, no worker callbacks, no
    // global timer-resolution change, and no queue of obsolete frames.
    class NativeClock
    {
    public:
        enum class Poll
        {
            Idle,
            Frame,
            Failed
        };
        NativeClock() = default;
        ~NativeClock() { Stop(); }
        NativeClock(const NativeClock&) = delete;
        NativeClock& operator=(const NativeClock&) = delete;

        bool Start(double refreshHz = 60.0) noexcept
        {
            Stop();
            LARGE_INTEGER frequency{};
            std::uint64_t now = 0;
            if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 || !Now(now) ||
                !cadence_.Configure(static_cast<std::uint64_t>(frequency.QuadPart), NormalizeRate(refreshHz)) ||
                !cadence_.Reset(now))
                return false;
            // Windows 10 1803 introduced this flag. Older supported Windows 10
            // versions use the same safe schedule with the ordinary waitable timer.
            constexpr DWORD highResolution = 0x00000002;
            timer_ = CreateWaitableTimerExW(nullptr, nullptr, highResolution, TIMER_MODIFY_STATE | SYNCHRONIZE);
            if (!timer_ && GetLastError() == ERROR_INVALID_PARAMETER)
                timer_ = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
            if (!timer_)
                return false;
            previous_ = now;
            return true;
        }

        void Stop() noexcept
        {
            if (timer_)
            {
                CancelWaitableTimer(timer_);
                CloseHandle(timer_);
            }
            timer_ = nullptr;
            cadence_ = {};
            previous_ = 0;
        }

        HANDLE Handle() const noexcept { return timer_; }

        // A screenshot transaction intentionally freezes frame servicing while it
        // captures/restores the view. Start a fresh elapsed-time origin before
        // accepting post-capture input; that input must not inherit capture time.
        bool Rebase() noexcept
        {
            std::uint64_t now = 0;
            if (!timer_ || !Now(now) || !cadence_.Reset(now))
                return false;
            previous_ = now;
            return true;
        }

        bool SetRate(double refreshHz) noexcept
        {
            if (!timer_)
                return false;
            const double rate = NormalizeRate(refreshHz);
            if (rate == cadence_.Rate())
                return true;
            std::uint64_t now = 0;
            return Now(now) && cadence_.Configure(cadence_.Frequency(), rate) && cadence_.Reset(now);
        }

        Poll Take(double& elapsedSeconds) noexcept
        {
            elapsedSeconds = 0;
            std::uint64_t now = 0;
            if (!timer_ || !cadence_.Valid() || !Now(now))
                return Poll::Failed;
            if (!cadence_.Due(now))
                return Poll::Idle;
            elapsedSeconds = now >= previous_ ? static_cast<double>(now - previous_) / static_cast<double>(cadence_.Frequency()) : 0;
            previous_ = now;
            return cadence_.Consume(now) ? Poll::Frame : Poll::Failed;
        }

        bool Arm() noexcept
        {
            std::uint64_t now = 0;
            if (!timer_ || !Now(now))
                return false;
            const auto delay = cadence_.Delay100ns(now);
            if (delay <= 0)
                return false;
            LARGE_INTEGER due{};
            due.QuadPart = -delay;
            return SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) != FALSE;
        }

    private:
        static double NormalizeRate(double refreshHz) noexcept
        {
            // Follow the selected display's refresh request within a bounded work
            // budget. GPU presentation owns completion; this clock only schedules
            // current work, and never queues missed display intervals.
            return std::isfinite(refreshHz) && refreshHz > 0 ? std::clamp(refreshHz, 1.0, 240.0) : 60.0;
        }

        static bool Now(std::uint64_t& now) noexcept
        {
            LARGE_INTEGER counter{};
            if (!QueryPerformanceCounter(&counter) || counter.QuadPart < 0)
                return false;
            now = static_cast<std::uint64_t>(counter.QuadPart);
            return true;
        }
        HANDLE timer_ = nullptr;
        Cadence cadence_;
        std::uint64_t previous_ = 0;
    };

} // namespace glide::timing
