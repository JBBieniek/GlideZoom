#pragma once
#include <cstdint>
#include <limits>

namespace glide::runtime
{
    enum class Phase
    {
        Starting,
        Ready,
        Paused,
        Suspended,
        Faulted,
        Stopping
    };

    // UI-thread policy shared by the native app and the composition tests. A
    // pending restoration is independent of the UI mode: Paused must not hide it.
    // This class never touches Windows, input hooks, files, or magnification.
    class State
    {
    public:
        Phase Current() const noexcept { return phase_; }
        std::uint64_t Epoch() const noexcept { return epoch_; }
        bool Terminal() const noexcept { return phase_ == Phase::Faulted || phase_ == Phase::Stopping; }
        bool NeedsRestore() const noexcept { return pendingRestore_; }
        bool Ready() const noexcept { return phase_ == Phase::Ready && !pendingRestore_; }
        bool Accept(std::uint64_t epoch) const noexcept { return Ready() && epoch == epoch_; }
        bool ResumeRequested() const noexcept { return resumeAfterSuspend_; }

        // Every mode change invalidates queued commands. Physical release ownership
        // lives separately in input::State and is never cleared by these methods.
        void Cancel() noexcept
        {
            if (epoch_ == std::numeric_limits<std::uint64_t>::max())
            {
                if (phase_ != Phase::Stopping)
                    phase_ = Phase::Faulted;
                resumeAfterSuspend_ = false;
            }
            else
                ++epoch_;
        }
        bool PrepareEnable() noexcept
        {
            if (Terminal())
                return false;
            phase_ = Phase::Paused;
            resumeAfterSuspend_ = false;
            Cancel();
            return true;
        }
        bool Activate() noexcept
        {
            if (phase_ != Phase::Paused || pendingRestore_)
                return false;
            phase_ = Phase::Ready;
            Cancel();
            return Ready();
        }
        void Pause() noexcept
        {
            if (Terminal())
                return;
            phase_ = Phase::Paused;
            resumeAfterSuspend_ = false;
            Cancel();
        }
        void Suspend(bool explicitResumeRequest = false) noexcept
        {
            if (Terminal())
                return;
            if (phase_ != Phase::Suspended)
                resumeAfterSuspend_ = phase_ == Phase::Ready;
            resumeAfterSuspend_ = resumeAfterSuspend_ || explicitResumeRequest;
            phase_ = Phase::Suspended;
            Cancel();
        }
        // Caller first verifies current desktop and finishes pending restoration.
        // Returning the resume intent does not itself enable input or acquire zoom.
        bool ReturnFromSuspend() noexcept
        {
            if (phase_ != Phase::Suspended || pendingRestore_)
                return false;
            const bool resume = resumeAfterSuspend_;
            phase_ = Phase::Paused;
            resumeAfterSuspend_ = false;
            Cancel();
            return resume;
        }
        void Fault() noexcept
        {
            if (phase_ != Phase::Stopping)
                phase_ = Phase::Faulted;
            resumeAfterSuspend_ = false;
            Cancel();
        }
        void Stop() noexcept
        {
            phase_ = Phase::Stopping;
            resumeAfterSuspend_ = false;
            Cancel();
        }
        void RequestRestore() noexcept { pendingRestore_ = true; }
        void RestoreComplete() noexcept { pendingRestore_ = false; }
        void RestoreConflict() noexcept
        {
            pendingRestore_ = false; // The Session has relinquished its snapshot.
            if (!Terminal())
                phase_ = Phase::Paused;
            resumeAfterSuspend_ = false;
            Cancel();
        }

    private:
        Phase phase_ = Phase::Starting;
        std::uint64_t epoch_ = 1;
        bool pendingRestore_ = false;
        bool resumeAfterSuspend_ = false;
    };
} // namespace glide::runtime
