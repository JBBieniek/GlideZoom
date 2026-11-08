#pragma once

#include "pointer_motion.h"
#include "pointer_feedback.h"

#include <array>
#include <cstddef>
#include <limits>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "desktop_context.h"
#include <windows.h>
#endif

namespace glide::pointer
{

    enum class Failure
    {
        None,
        Geometry,
        Warp,
        Readback,
        Clock,
        Generation
    };
    enum class ConstraintResult
    {
        Passed,
        Consumed,
        Failed
    };

    // One failed native transaction, not a pointer trace. The hook publishes
    // these bounded POD records only when a native failure first latches.
    struct FailureDetails
    {
        bool valid = false;
        Failure reason = Failure::None;
        std::uint64_t at = 0, epoch = 0;
        Vec2 before{}, candidate{}, requested{}, observed{};
        Rect bounds{};
        Vec2 offset{};
        double zoom = 1, speedMultiplier = 1;
        bool adjustForZoom = true, forwarded = false, constrained = false;
        bool warpSucceeded = false, readSucceeded = false;
        std::uint32_t nativeError = 0;
        // These extra queries occur after failed verification. They are useful
        // context, not an atomic cursor/mapping observation or proof of cause.
        bool logicalRead = false, clipRead = false;
        Vec2 logical{};
        Rect clip{};
        std::uint32_t logicalError = 0, clipError = 0;
    };

    struct PendingStatistics
    {
        std::uint64_t started = 0, acknowledged = 0, interrupted = 0, expired = 0;
    };

    inline constexpr const wchar_t* FailureText(Failure reason) noexcept
    {
        switch (reason)
        {
        case Failure::Geometry:
            return L"Pointer adjustment stopped: invalid pointer geometry or gain.";
        case Failure::Warp:
            return L"Pointer adjustment stopped: Windows rejected the physical cursor write.";
        case Failure::Readback:
            return L"Pointer adjustment stopped: the cursor write could not be verified or another owner moved it.";
        case Failure::Clock:
            return L"Pointer adjustment stopped: pointer event time moved backwards.";
        case Failure::Generation:
            return L"Pointer adjustment stopped: the cancellation generation was exhausted.";
        default:
            return L"Pointer adjustment has no latched native failure.";
        }
    }

    // A hook-thread-only, allocation-free adapter. Native callbacks and cursor
    // ownership cannot be proven by portable tests. Temporary desktop, clipping
    // or cursor-owner changes pause and reseed; uncertain native writes suspend
    // adjustment until bounded read-only recovery or a new control epoch. In
    // particular an ordinary fast move must not
    // permanently turn compensation off.
    // It never owns a button/key, never injects one, and never restores a stale
    // cursor position on failure, pause, reset or exit.
    class Adapter
    {
        struct Echo;

    public:
        bool Faulted() const noexcept { return failure_ != Failure::None; }
        Failure FailureReason() const noexcept { return failure_; }
        const FailureDetails& LastFailureDetails() const noexcept { return failureDetails_; }
        bool Pending() const noexcept { return pending_.valid; }
        bool PendingConstrained() const noexcept { return pending_.valid && pending_.constrained; }
        PendingStatistics PendingCounts() const noexcept { return pendingStatistics_; }
        static constexpr std::uint64_t PendingLeaseMs = 32;
        bool InWarp() const noexcept { return inWarp_; }
        Vec2 Remainder() const noexcept { return remainder_; }
        Feedback FeedbackSnapshot() const noexcept
        {
            if (!feedbackValid_ || !seeded_ || Faulted() || inWarp_ || Pending() ||
                !view_.requested || !view_.active || view_.zoom <= 1)
                return {};
            return { true, expected_, remainder_, view_.bounds, view_.zoom, view_.epoch, view_.speedMultiplier, view_.adjustForZoom };
        }
        // Applied zoom telemetry may change without a mouse event. Revoke only
        // presentation feedback until a newly verified sample; keep the existing
        // pointer baseline and fractional gain integration completely unchanged.
        void InvalidateFeedback() noexcept
        {
            feedbackValid_ = false;
            if (feedbackGeneration_ != std::numeric_limits<std::uint64_t>::max())
                ++feedbackGeneration_;
        }
        // Call from the Host's early InWarp() bypass for WM_MOUSEMOVE. This avoids
        // retaining an already observed synchronous setter echo in the ledger.
        void ObserveWarp(Vec2 candidate) noexcept
        {
            if (inWarp_ && Same(candidate, warpPoint_))
                synchronousEcho_ = true;
        }
        // The Host must run this before gesture dispatch as well as before its
        // injected-input bypass. True means pass this possible echo immediately.
        // A possible delayed echo passes without another write. Discarding the
        // baseline is enough: an untagged setter echo and a real reversal cannot
        // always be distinguished, and neither should permanently disable gain.
        bool BypassEcho(Vec2 candidate, std::uint64_t now) noexcept
        {
            // Even a synchronously observed setter callback can precede the
            // physical position update. Keep the pending target recognizable
            // after its ordinary echo ledger entry has already been consumed.
            if (Pending() && Same(candidate, pending_.step.point))
            {
                if (now < pending_.at)
                    return Fail(Failure::Clock);
                if (now - pending_.at <= PendingLeaseMs)
                    return true;
            }
            for (auto& echo : echoes_)
            {
                if (!echo.valid)
                    continue;
                if (now < echo.at)
                {
                    Fail(Failure::Clock);
                    return false;
                }
                if (now - echo.at > EchoLeaseMs)
                {
                    echo.valid = false;
                    continue;
                }
                if (Same(candidate, echo.point))
                {
                    echo.valid = false;
                    // The setter may deliver its target before a physical getter
                    // can observe the change. Passing this untagged echo is not
                    // an acknowledgement and must not discard the pending step.
                    if (Pending() && Same(candidate, pending_.step.point))
                        return true;
                    if (seeded_ && !Same(candidate, expected_))
                        Pause();
                    return true;
                }
            }
            return false;
        }

        // A native-write failure latch survives Pause. Transient context changes
        // use Pause alone and can resume from the next verified physical sample.
        void Pause() noexcept
        {
            pending_.valid = false;
            InvalidateFeedback();
            if (pauseVersion_ != std::numeric_limits<std::uint64_t>::max())
                ++pauseVersion_;
            else if (failure_ == Failure::None)
                failure_ = Failure::Generation;
            seeded_ = false;
            remainder_ = {};
            // An already requested native write can still have an asynchronous
            // callback. Keep its short-lived bypass record across reset/gestures;
            // otherwise it could be mistaken for new gesture motion after pause.
        }

        // Only a dedicated forwarded-drag adapter uses this after the final
        // physical UP. Keep owned echo records, but let a later drag retry a
        // newly verified desktop/clip after a transient native race.
        void EndConstraint() noexcept
        {
            if (!inWarp_)
            {
                failure_ = Failure::None;
                Pause();
            }
        }

        // A successful physical setter can still read back the exact old point
        // while its low-level hook is on the stack. Only a later exact physical
        // observation under the same policy acknowledges that one request. No
        // native write, waiting, or movement replay occurs in this verifier.
        template<class Native>
        bool ServicePending(std::uint64_t now, const View& view, bool permitted, bool gestureActive, Native& native, const Vec2* interveningCandidate = nullptr) noexcept
        {
            if (!Pending() || inWarp_)
                return false;
            if (!permitted || gestureActive || !view.requested || !view.active || view.zoom <= 1 ||
                !Valid(view) || !SamePolicy(view, pending_.view) || pending_.version != pauseVersion_)
            {
                Pause();
                return false;
            }
            if (now < pending_.at)
                return Fail(Failure::Clock);
            const auto version = pauseVersion_;
            if (!native.Ready(view) ||
                (pending_.hasLimit && !Same(pending_.limit, NativeLimit(native, 0))))
            {
                Pause();
                return false;
            }
            if (!Pending() || version != pauseVersion_ || Faulted())
                return false;
            Vec2 observed{};
            const bool read = native.Read(observed);
            const auto error = read ? std::uint32_t{} : static_cast<std::uint32_t>(NativeError(native, 0));
            if (!Pending() || version != pauseVersion_ || Faulted())
                return false;
            // Expiration wins even if the target finally appears: a late point
            // is not authority to revive an old fraction or completed gesture.
            const bool expired = now - pending_.at > PendingLeaseMs;
            if (read && !expired && Same(observed, pending_.step.point))
            {
                if (!native.CanPlace(observed) || !native.Ready(view) ||
                    !Pending() || version != pauseVersion_ || Faulted())
                {
                    Pause();
                    return false;
                }
                const Step step = pending_.step;
                const auto generation = pending_.feedbackGeneration;
                pending_.valid = false;
                Increment(pendingStatistics_.acknowledged);
                Commit(step, now, generation);
                return true;
            }
            // A later real hook packet can already be the committed physical
            // position. It supersedes the old request; it does not verify that
            // request or justify replaying its fraction. Timer service has no
            // such event evidence and retains the strict mismatch policy.
            if (read && interveningCandidate && ValidPoint(*interveningCandidate) &&
                Same(observed, *interveningCandidate) && !Same(observed, pending_.step.point))
            {
                Increment(pendingStatistics_.interrupted);
                Pause();
                return false;
            }
            if (!expired && read && Same(observed, pending_.before))
                return false;
            if (expired)
                Increment(pendingStatistics_.expired);
            // Keep the normal failure record and strict mismatch policy. The
            // initiating coordinates remain separate from this later observation.
            inWarp_ = true;
            RecordNativeFailure(Failure::Readback, pending_.step, pending_.before, pending_.candidate, pending_.constrained, now, true, read, observed, error, native);
            inWarp_ = false;
            failedAt_ = now;
            return Fail(Failure::Readback);
        }

        // Read-only probation for an uncertain native write. This never warps,
        // restores an old location, or carries pre-failure fractional motion.
        // The Host permits it only while controls are current and input is idle.
        // One rearm per epoch bounds contention with another cursor owner.
        template<class Native>
        bool Recover(std::uint64_t now, const View& view, bool permitted, Native& native) noexcept
        {
            if ((failure_ != Failure::Warp && failure_ != Failure::Readback) || inWarp_ ||
                !permitted || !view.requested || !view.active || view.zoom <= 1 || !Valid(view) ||
                pauseVersion_ == (std::numeric_limits<std::uint64_t>::max)() ||
                now < failedAt_ || now - failedAt_ < RecoveryCooldownMs ||
                (recoveredInEpoch_ && recoveredEpoch_ == view.epoch))
            {
                recoveryProbe_ = false;
                return false;
            }
            for (auto& echo : echoes_)
            {
                if (!echo.valid)
                    continue;
                if (now < echo.at || now - echo.at <= EchoLeaseMs)
                {
                    recoveryProbe_ = false;
                    return false;
                }
                echo.valid = false;
            }
            const auto version = pauseVersion_;
            Vec2 observed{}, confirmed{};
            if (!native.Ready(view) || !native.Read(observed) || !ValidPoint(observed) || !Contains(view.bounds, observed) || !native.CanPlace(observed) ||
                !native.Ready(view) || !native.Read(confirmed) || !Same(observed, confirmed) ||
                pauseVersion_ != version)
            {
                recoveryProbe_ = false;
                return false;
            }
            const bool samePolicy = recoveryView_.epoch == view.epoch && Same(recoveryView_.bounds, view.bounds) &&
                                    recoveryView_.zoom == view.zoom && Same(recoveryView_.offset, view.offset) &&
                                    recoveryView_.edgePanMultiplier == view.edgePanMultiplier &&
                                    recoveryView_.speedMultiplier == view.speedMultiplier && recoveryView_.adjustForZoom == view.adjustForZoom;
            if (!recoveryProbe_ || !samePolicy || recoveryVersion_ != version ||
                !Same(recoveryPoint_, observed) || now < recoveryAt_)
            {
                recoveryProbe_ = true;
                recoveryPoint_ = observed;
                recoveryView_ = view;
                recoveryVersion_ = version;
                recoveryAt_ = now;
                return false;
            }
            if (now - recoveryAt_ < RecoveryStableMs)
                return false;
            failure_ = Failure::None;
            Pause();
            recoveryProbe_ = false;
            recoveredInEpoch_ = true;
            recoveredEpoch_ = view.epoch;
            return true;
        }

        // Native must supply:
        //   bool Ready(const View&) noexcept   (read-only desktop/clip check)
        //   bool Read(Vec2&) noexcept          (current physical cursor position)
        //   bool CanPlace(Vec2) noexcept       (point is on a real display)
        //   bool Warp(Vec2) noexcept           (integer physical cursor setter)
        // Return true only to consume this ordinary WM_MOUSEMOVE. The Host must
        // pass foreign injected movement here before its injected-input bypass,
        // and pass InWarp() callbacks straight through before gesture processing.
        template<class Native>
        bool Move(Vec2 candidate, bool injected, std::uint64_t now, const View& view, bool permitted, bool gestureActive, Native& native) noexcept
        {
            if (inWarp_)
            {
                ObserveWarp(candidate);
                return false;
            }
            if (Faulted())
                return false;
            // Recognize a possible owned echo before the generic injected-input
            // bypass. A foreign injected move to this exact same point is also
            // harmlessly passed; other injected movement discards all history.
            if (BypassEcho(candidate, now) || Faulted())
                return false;
            if (injected)
            {
                Pause();
                return false;
            }
            if (Pending())
            {
                if (!ServicePending(now, view, permitted, gestureActive, native, &candidate))
                {
                    // This is a distinct real packet before the old request
                    // could be verified. Let Windows process it, discard the
                    // proposal, and never pull it back or replay its delta.
                    if (Pending())
                    {
                        Increment(pendingStatistics_.interrupted);
                        Pause();
                    }
                    return false;
                }
            }
            if (!view.requested || !view.active || view.zoom == 1 || gestureActive)
            {
                Pause();
                return false;
            }
            if (!Valid(view) || !ValidPoint(candidate))
                return Fail(Failure::Geometry);
            if (!permitted)
            {
                Pause();
                return false;
            }
            if (seeded_ && now < lastAt_)
                return Fail(Failure::Clock);
            if (seeded_ && (view.epoch != view_.epoch || !Same(view.bounds, view_.bounds) ||
                            view.speedMultiplier != view_.speedMultiplier || view.adjustForZoom != view_.adjustForZoom))
                Pause();
            if (seeded_ && now - lastAt_ > BaselineLeaseMs)
                Pause();
            view_ = view;

            // The physical setter has no event cookie. A possible echo is only
            // bypassed, never consumed or adjusted. A delayed echo to an older
            // point (which could also be fast user backtracking) is passed without
            // another cursor write. It never consumes the reversal or latches gain
            // off for the remainder of the application session.
            const auto version = pauseVersion_;
            const auto feedbackGeneration = feedbackGeneration_;
            if (!native.Ready(view))
            {
                Pause();
                return false;
            }
            if (pauseVersion_ != version || Faulted())
                return false;
            Vec2 current{};
            if (!native.Read(current) || !ValidPoint(current))
            {
                Pause();
                return false;
            }
            if (pauseVersion_ != version || Faulted())
                return false;
            if (!seeded_)
            {
                // A pre-event physical position supplies a fresh baseline, so the
                // first move after idle/zoom/pause receives the same gain as the
                // next ones. If Windows already committed this candidate there
                // is no measured displacement yet; accept it once, never invent a
                // delta from an old epoch/monitor.
                expected_ = current;
                seeded_ = true;
                lastAt_ = now;
                if (Same(current, candidate))
                {
                    ValidateFeedback(feedbackGeneration);
                    return false;
                }
            }
            if (!Same(current, expected_) && !Same(current, candidate))
            {
                Pause();
                return false; // Another owner moved it. Do not pull it back.
            }
            const Vec2 delta{ candidate.x - expected_.x, candidate.y - expected_.y };
            // Both endpoints were validated. A 512-pixel cutoff used to disable
            // this feature on fast mice and large/high-DPI desktops. The bounded
            // geometric proposal handles the full valid displacement instead.
            if (delta.x == 0 && delta.y == 0)
            {
                lastAt_ = now;
                ValidateFeedback(feedbackGeneration);
                return false;
            }
            Step step;
            if (!Propose(expected_, candidate, remainder_, view, step))
                return Fail(Failure::Geometry);
            if (Same(step.point, candidate))
            {
                Commit(step, now, feedbackGeneration);
                return false;
            }
            // If the native event is still proposed and subpixel accumulation has
            // not earned a whole pixel, suppress it without writing the cursor.
            if (Same(step.point, current))
            {
                Commit(step, now, feedbackGeneration);
                return true;
            }
            // A nonrectangular desktop can contain gaps. Never force the cursor
            // there or mistake a normal diagonal monitor crossing for a failed
            // setter. Its original Windows movement remains usable.
            if (!native.CanPlace(step.point))
            {
                Pause();
                return false;
            }
            if (pauseVersion_ != version || Faulted())
                return false;
            Echo* slot = nullptr;
            for (auto& echo : echoes_)
                if (!echo.valid)
                {
                    slot = &echo;
                    break;
                }
            if (!slot)
            {
                Pause();
                return false;
            }

            return WriteStep(step, current, candidate, false, now, version, feedbackGeneration, slot, native);
        }

        // A forwarded drag cannot enter a passive zoom using another monitor's
        // global input map. Keep its already processed native displacement up to
        // the captured boundary; no device counts or global ClipCursor are used.
        template<class Native>
        ConstraintResult Constrain(Vec2 candidate, std::uint64_t now, const View& view, Rect limit, Native& native) noexcept
        {
            if (Pending())
            {
                Increment(pendingStatistics_.interrupted);
                Pause();
                return ConstraintResult::Failed; // Never overlap a second native correction.
            }
            if (!ValidPoint(candidate) || !ValidBounds(limit) || !Valid(view) ||
                limit.left < view.bounds.left || limit.top < view.bounds.top ||
                limit.right > view.bounds.right || limit.bottom > view.bounds.bottom || Faulted() || inWarp_)
                return ConstraintResult::Failed;
            if (Contains(limit, candidate))
                return ConstraintResult::Passed;
            Pause();
            view_ = view;
            const auto version = pauseVersion_, generation = feedbackGeneration_;
            if (!native.Ready(view) || pauseVersion_ != version)
                return ConstraintResult::Failed;
            Vec2 current{};
            if (!native.Read(current) || !ValidPoint(current) || pauseVersion_ != version ||
                (!Contains(limit, current) && !Same(current, candidate)))
                return ConstraintResult::Failed;
            const Step step{ { std::clamp(candidate.x, limit.left, limit.right - 1),
                               std::clamp(candidate.y, limit.top, limit.bottom - 1) },
                             {} };
            if (Same(step.point, current))
                return ConstraintResult::Consumed;
            if (!native.CanPlace(step.point) || pauseVersion_ != version)
                return ConstraintResult::Failed;
            Echo* slot = nullptr;
            for (auto& echo : echoes_)
                if (!echo.valid || (now >= echo.at && now - echo.at > EchoLeaseMs))
                {
                    slot = &echo;
                    break;
                }
            if (!slot)
                return ConstraintResult::Failed;
            return WriteStep(step, current, candidate, true, now, version, generation, slot, native) ?
                       ConstraintResult::Consumed :
                       ConstraintResult::Failed;
        }

    private:
        static constexpr void Increment(std::uint64_t& value) noexcept
        {
            if (value != (std::numeric_limits<std::uint64_t>::max)())
                ++value;
        }
        static constexpr bool SamePolicy(const View& a, const View& b) noexcept
        {
            return a.requested == b.requested && a.active == b.active && a.epoch == b.epoch &&
                   Same(a.bounds, b.bounds) && a.zoom == b.zoom && Same(a.offset, b.offset) &&
                   a.edgePanMultiplier == b.edgePanMultiplier && a.speedMultiplier == b.speedMultiplier &&
                   a.adjustForZoom == b.adjustForZoom;
        }
        template<class Native>
        static auto NativeLimit(Native& native, int) noexcept -> decltype(native.Limit())
        {
            return native.Limit();
        }
        template<class Native>
        static constexpr Rect NativeLimit(Native&, long) noexcept
        {
            return {};
        }
        template<class Native>
        static auto NativeError(Native& native, int) noexcept -> decltype(native.LastError())
        {
            return native.LastError();
        }
        template<class Native>
        static constexpr std::uint32_t NativeError(Native&, long) noexcept
        {
            return 0;
        }
        template<class Native>
        static auto NativeFailureObservation(Native& native, FailureDetails& output, int) noexcept -> decltype(native.ObserveFailure(output), void())
        {
            native.ObserveFailure(output);
        }
        template<class Native>
        static constexpr void NativeFailureObservation(Native&, FailureDetails&, long) noexcept
        {
        }

        template<class Native>
        void RecordNativeFailure(Failure reason, const Step& step, Vec2 before, Vec2 candidate, bool constrained, std::uint64_t now, bool warped, bool read, Vec2 observed, std::uint32_t error, Native& native) noexcept
        {
            failureDetails_ = {};
            failureDetails_.valid = true;
            failureDetails_.reason = reason;
            failureDetails_.at = now;
            failureDetails_.epoch = view_.epoch;
            failureDetails_.before = before;
            failureDetails_.candidate = candidate;
            failureDetails_.requested = step.point;
            failureDetails_.observed = observed;
            failureDetails_.bounds = view_.bounds;
            failureDetails_.offset = view_.offset;
            failureDetails_.zoom = view_.zoom;
            failureDetails_.speedMultiplier = view_.speedMultiplier;
            failureDetails_.adjustForZoom = view_.adjustForZoom;
            failureDetails_.constrained = constrained;
            failureDetails_.warpSucceeded = warped;
            failureDetails_.readSucceeded = read;
            failureDetails_.nativeError = error;
            NativeFailureObservation(native, failureDetails_, 0);
        }

        template<class Native>
        bool WriteStep(const Step& step, Vec2 before, Vec2 candidate, bool constrained, std::uint64_t now, std::uint64_t version, std::uint64_t feedbackGeneration, Echo* slot, Native& native) noexcept
        {
            // No loops, waiting, SendInput, raw-device conversion or global mouse
            // settings. Readback is mandatory. If another owner wins a write/readback
            // race, fail open and stop; never repeatedly restore against that owner.
            synchronousEcho_ = false;
            warpPoint_ = step.point;
            *slot = { step.point, now, true };
            inWarp_ = true;
            const bool warped = native.Warp(step.point);
            const auto warpError = warped ? std::uint32_t{} : static_cast<std::uint32_t>(NativeError(native, 0));
            Vec2 observed{};
            const bool read = warped && native.Read(observed);
            const auto readError = read ? std::uint32_t{} : static_cast<std::uint32_t>(NativeError(native, 0));
            if (!warped)
            {
                RecordNativeFailure(Failure::Warp, step, before, candidate, constrained, now, false, false, observed, warpError, native);
                inWarp_ = false;
                slot->valid = false;
                failedAt_ = now;
                return Fail(Failure::Warp);
            }
            if (synchronousEcho_)
                slot->valid = false;
            if (!constrained && read && Same(observed, before) && !Same(observed, step.point) &&
                version == pauseVersion_ && !Faulted())
            {
                // A measured pre-commit getter is neither a successful commit
                // nor arbitrary interference. Own just this original packet;
                // later service must confirm the exact requested physical point.
                pending_ = { true, constrained, step, view_, before, candidate, NativeLimit(native, 0), now, version, feedbackGeneration };
                pending_.hasLimit = ValidBounds(pending_.limit);
                // An observed callback did not yet commit the physical point.
                // Retain the bounded bypass even if cancellation later removes
                // the pending proposal before another target callback arrives.
                slot->valid = true;
                Increment(pendingStatistics_.started);
                feedbackValid_ = false;
                inWarp_ = false;
                return true;
            }
            if (!read || !Same(observed, step.point))
            {
                RecordNativeFailure(Failure::Readback, step, before, candidate, constrained, now, true, read, observed, readError, native);
                inWarp_ = false;
                failedAt_ = now;
                return Fail(Failure::Readback);
            }
            inWarp_ = false;
            // A nested desktop/configuration/gesture cancellation may have run
            // during a native call. Do not resurrect its discarded fractional
            // state or consume the original move after that cancellation.
            if (pauseVersion_ != version || Faulted())
                return false;
            Commit(step, now, feedbackGeneration);
            return true;
        }

        struct Echo
        {
            Vec2 point{};
            std::uint64_t at = 0;
            bool valid = false;
        };
        struct PendingWrite
        {
            bool valid = false, constrained = false;
            Step step{};
            View view{};
            Vec2 before{}, candidate{};
            Rect limit{};
            std::uint64_t at = 0, version = 0, feedbackGeneration = 0;
            bool hasLimit = false;
        };
        // 1024 records accommodate an 8000 Hz mouse within the short echo lease.
        // Excess pressure only passes one event and discards its motion history;
        // it never allocates, spins, or disables gain for the whole process.
        static constexpr std::uint64_t EchoLeaseMs = 64, BaselineLeaseMs = 250;
        static constexpr std::uint64_t RecoveryCooldownMs = 500, RecoveryStableMs = 50;
        std::array<Echo, 1024> echoes_{};
        View view_{};
        Vec2 expected_{}, remainder_{}, warpPoint_{};
        std::uint64_t lastAt_ = 0, pauseVersion_ = 0, feedbackGeneration_ = 0;
        Failure failure_ = Failure::None;
        FailureDetails failureDetails_{};
        PendingWrite pending_{};
        PendingStatistics pendingStatistics_{};
        bool seeded_ = false, inWarp_ = false, synchronousEcho_ = false;
        bool feedbackValid_ = false;
        View recoveryView_{};
        Vec2 recoveryPoint_{};
        std::uint64_t failedAt_ = 0, recoveryAt_ = 0, recoveryVersion_ = 0, recoveredEpoch_ = 0;
        bool recoveryProbe_ = false, recoveredInEpoch_ = false;

        bool Fail(Failure reason) noexcept
        {
            recoveryProbe_ = false;
            failure_ = reason;
            Pause();
            return false;
        }
        void ValidateFeedback(std::uint64_t generation) noexcept
        {
            feedbackValid_ = generation == feedbackGeneration_ &&
                             generation != std::numeric_limits<std::uint64_t>::max();
        }
        void Commit(const Step& step, std::uint64_t now, std::uint64_t feedbackGeneration) noexcept
        {
            expected_ = step.point;
            remainder_ = step.remainder;
            lastAt_ = now;
            ValidateFeedback(feedbackGeneration);
        }
    };

#ifdef _WIN32
    // These are the ONLY cursor-writing calls introduced by pointer scaling.
    // None of the checks changes desktop, capture, clipping, DPI or mouse settings.
    class NativeOps
    {
    public:
        std::uint32_t LastError() const noexcept { return GetLastError(); }
        void ObserveFailure(FailureDetails& output) const noexcept
        {
            POINT logical{};
            output.logicalRead = GetCursorPos(&logical) != FALSE;
            output.logicalError = output.logicalRead ? ERROR_SUCCESS : GetLastError();
            if (output.logicalRead)
                output.logical = { static_cast<double>(logical.x), static_cast<double>(logical.y) };
            RECT clip{};
            output.clipRead = GetClipCursor(&clip) != FALSE;
            output.clipError = output.clipRead ? ERROR_SUCCESS : GetLastError();
            if (output.clipRead)
                output.clip = { static_cast<double>(clip.left), static_cast<double>(clip.top), static_cast<double>(clip.right), static_cast<double>(clip.bottom) };
        }
        bool Ready(const View& view) const noexcept
        {
            Rect limit;
            return Context(view, limit, false);
        }
        bool DragBounds(const View& view, Rect& limit) const noexcept
        {
            return Context(view, limit, true);
        }

    private:
        bool Context(const View& view, Rect& limit, bool allowAppClip) const noexcept
        {
            if (QueryInputDesktop() != InputDesktop::Current || !Valid(view))
                return false;
            const auto awareness = GetThreadDpiAwarenessContext();
            if (!AreDpiAwarenessContextsEqual(awareness, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
                !AreDpiAwarenessContextsEqual(awareness, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE))
                return false;
            RECT clip{};
            if (!GetClipCursor(&clip))
                return false;
            const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (width <= 0 || height <= 0 || width > 262144 || height > 262144)
                return false;
            const double right = static_cast<double>(left) + width;
            const double bottom = static_cast<double>(top) + height;
            if ((!allowAppClip && (clip.left != left || clip.top != top || clip.right != right || clip.bottom != bottom)) ||
                view.bounds.left < left || view.bounds.top < top ||
                view.bounds.right > right || view.bounds.bottom > bottom)
                return false;
            limit = { std::max(view.bounds.left, static_cast<double>(clip.left)), std::max(view.bounds.top, static_cast<double>(clip.top)), std::min(view.bounds.right, static_cast<double>(clip.right)), std::min(view.bounds.bottom, static_cast<double>(clip.bottom)) };
            if (!ValidBounds(limit))
                return false;
            if (Same(view.bounds, Rect{ static_cast<double>(left), static_cast<double>(top), right, bottom }))
                return true; // The current whole-desktop rectangle.
            const RECT selected{ static_cast<LONG>(view.bounds.left), static_cast<LONG>(view.bounds.top), static_cast<LONG>(view.bounds.right), static_cast<LONG>(view.bounds.bottom) };
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            const HMONITOR handle = MonitorFromRect(&selected, MONITOR_DEFAULTTONULL);
            return handle && GetMonitorInfoW(handle, &monitor) &&
                   monitor.rcMonitor.left == selected.left && monitor.rcMonitor.top == selected.top &&
                   monitor.rcMonitor.right == selected.right && monitor.rcMonitor.bottom == selected.bottom;
        }

    public:
        bool Read(Vec2& value) const noexcept
        {
            POINT point{};
            if (!GetPhysicalCursorPos(&point))
                return false;
            value = { static_cast<double>(point.x), static_cast<double>(point.y) };
            return true;
        }
        bool CanPlace(Vec2 value) const noexcept
        {
            if (!ValidPoint(value))
                return false;
            const POINT point{ static_cast<LONG>(value.x), static_cast<LONG>(value.y) };
            return MonitorFromPoint(point, MONITOR_DEFAULTTONULL) != nullptr;
        }
        bool Warp(Vec2 value) const noexcept
        {
            if (!ValidPoint(value))
                return false;
            const POINT point{ static_cast<LONG>(value.x), static_cast<LONG>(value.y) };
            if (!MonitorFromPoint(point, MONITOR_DEFAULTTONULL))
                return false;
            return SetPhysicalCursorPos(point.x, point.y) != FALSE;
        }
    };
#endif

    // A forwarded application drag may legitimately own ClipCursor. Respect its
    // current intersection with the original monitor; never remove/replace it.
    // Recheck before a write, and let readback detect a clip changing in flight.
    template<class Native>
    class DragNative
    {
    public:
        explicit DragNative(Native& native) noexcept :
            native_(native) {}
        bool Initialize(const View& view) noexcept { return native_.DragBounds(view, limit_); }
        Rect Limit() const noexcept { return limit_; }
        bool Ready(const View& view) noexcept
        {
            Rect current;
            return native_.DragBounds(view, current) && Same(current, limit_);
        }
        bool Read(Vec2& point) noexcept { return native_.Read(point); }
        bool CanPlace(Vec2 point) noexcept { return Contains(limit_, point) && native_.CanPlace(point); }
        bool Warp(Vec2 point) noexcept { return native_.Warp(point); }
        template<class Probe = Native>
        auto LastError() const noexcept -> decltype(static_cast<Probe*>(nullptr)->LastError())
        {
            return native_.LastError();
        }
        template<class Probe = Native>
        auto ObserveFailure(FailureDetails& output) const noexcept -> decltype(static_cast<Probe*>(nullptr)->ObserveFailure(output), void())
        {
            native_.ObserveFailure(output);
        }

    private:
        Native& native_;
        Rect limit_{};
    };

} // namespace glide::pointer
