#pragma once

#include <cstdint>

namespace glide::gpu
{

    enum class QueueSignal
    {
        Ready,
        Busy,
        Failed
    };
    enum class QueueDecision
    {
        Render,
        Pending,
        Failed
    };

    enum class Preparation
    {
        Acquire,
        Reuse,
        Restage,
        Rebuild
    };

    // A rendered but unsubmitted frame owns its native readiness grant. The same
    // compatible back buffer may be reused or redrawn without another wait or
    // submission. Abandoning the buffer requires a new swap chain, including when
    // staged metadata and grant ownership disagree.
    inline constexpr Preparation PrepareDecision(bool staged, bool grant, bool matches, bool compatible) noexcept
    {
        if (!staged && !grant)
            return Preparation::Acquire;
        if (!staged || !grant || !compatible)
            return Preparation::Rebuild;
        return matches ? Preparation::Reuse : Preparation::Restage;
    }

    // A new capture activation may need longer than one frame to wake the display
    // queue. The initial grant and acknowledgement of the first successful Present
    // have separate fixed deadlines. Present only submits work: especially while
    // the window is being shown for the first time, it does not prove the compositor
    // has started consuming it. Poll both startup phases without blocking the UI.
    // After the first post-submission grant, retain the short active failure bound.
    // A Ready observation is a consumed grant. Exactly one successful Present must
    // finish it before another wait, including the first frame on the swap chain.
    class PresentationQueue
    {
    public:
        static constexpr std::uint64_t InitialTimeoutMs = 2000;
        static constexpr unsigned ActiveWaitMs = 100;

        void Reset() noexcept
        {
            started_ = last_ = activePollStarted_ = 0;
            observed_ = submitted_ = acknowledged_ = granted_ = failed_ = false;
            activePolling_ = false;
        }

        unsigned WaitMilliseconds(bool nonblocking = false) const noexcept { return Cold() || nonblocking ? 0 : ActiveWaitMs; }
        bool Cold() const noexcept { return !acknowledged_; }
        bool HasSubmittedFrame() const noexcept { return submitted_; }
        bool HasUnsubmittedGrant() const noexcept { return granted_; }

        QueueDecision Observe(QueueSignal signal, std::uint64_t nowMs, bool nonblocking = false) noexcept
        {
            const bool previousGrant = granted_;
            // Even a signal arriving at an expired deadline was consumed from the
            // native waitable object. The adapter must rebuild that swap chain if
            // it abandons the corresponding frame instead of presenting it.
            if (signal == QueueSignal::Ready)
                granted_ = true;
            if (failed_ || previousGrant)
                return Fail();
            if (!observed_)
            {
                started_ = last_ = nowMs;
                observed_ = true;
            }
            if (nowMs < last_ || (Cold() && nowMs - started_ >= InitialTimeoutMs) ||
                (activePolling_ && nowMs - activePollStarted_ >= ActiveWaitMs) ||
                signal == QueueSignal::Failed)
                return Fail();
            last_ = nowMs;
            if (signal == QueueSignal::Ready)
            {
                activePolling_ = false;
                if (submitted_)
                    acknowledged_ = true;
                return QueueDecision::Render;
            }
            if (signal == QueueSignal::Busy && Cold())
                return QueueDecision::Pending;
            if (signal == QueueSignal::Busy && nonblocking)
            {
                if (!activePolling_)
                {
                    activePolling_ = true;
                    activePollStarted_ = nowMs;
                }
                return QueueDecision::Pending;
            }
            return Fail();
        }

        bool Submitted(std::uint64_t nowMs) noexcept
        {
            if (!granted_ || failed_ || nowMs < last_)
            {
                Fail();
                return false;
            }
            // Start this deadline exactly once, at the first successful Present.
            // Busy polls and later frames cannot extend a stalled activation.
            if (!submitted_)
                started_ = nowMs;
            last_ = nowMs;
            granted_ = false;
            submitted_ = true;
            return true;
        }

    private:
        std::uint64_t started_ = 0, last_ = 0, activePollStarted_ = 0;
        bool observed_ = false, submitted_ = false, acknowledged_ = false;
        bool granted_ = false, failed_ = false;
        bool activePolling_ = false;

        QueueDecision Fail() noexcept
        {
            failed_ = true;
            return QueueDecision::Failed;
        }
    };

} // namespace glide::gpu
