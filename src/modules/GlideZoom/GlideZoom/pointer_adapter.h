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

    // A hook-thread-only, allocation-free adapter. Native callbacks and cursor
    // ownership cannot be proven by portable tests. Temporary desktop, clipping
    // or cursor-owner changes pause and reseed; uncertain native writes stop this
    // feature for the process. In particular an ordinary fast move must not
    // permanently turn compensation off.
    // It never owns a button/key, never injects one, and never restores a stale
    // cursor position on failure, pause, reset or exit.
    class Adapter
    {
        struct Echo;

    public:
        bool Faulted() const noexcept { return failure_ != Failure::None; }
        Failure FailureReason() const noexcept { return failure_; }
        bool InWarp() const noexcept { return inWarp_; }
        Vec2 Remainder() const noexcept { return remainder_; }
        Feedback FeedbackSnapshot() const noexcept
        {
            if (!feedbackValid_ || !seeded_ || Faulted() || inWarp_ ||
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

            return WriteStep(step, now, version, feedbackGeneration, slot, native);
        }

        // A forwarded drag cannot enter a passive zoom using another monitor's
        // global input map. Keep its already processed native displacement up to
        // the captured boundary; no device counts or global ClipCursor are used.
        template<class Native>
        ConstraintResult Constrain(Vec2 candidate, std::uint64_t now, const View& view, Rect limit, Native& native) noexcept
        {
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
            return WriteStep(step, now, version, generation, slot, native) ?
                       ConstraintResult::Consumed :
                       ConstraintResult::Failed;
        }

    private:
        template<class Native>
        bool WriteStep(const Step& step, std::uint64_t now, std::uint64_t version, std::uint64_t feedbackGeneration, Echo* slot, Native& native) noexcept
        {
            // No loops, waiting, SendInput, raw-device conversion or global mouse
            // settings. Readback is mandatory. If another owner wins a write/readback
            // race, fail open and stop; never repeatedly restore against that owner.
            synchronousEcho_ = false;
            warpPoint_ = step.point;
            *slot = { step.point, now, true };
            inWarp_ = true;
            const bool warped = native.Warp(step.point);
            Vec2 observed{};
            const bool read = warped && native.Read(observed);
            inWarp_ = false;
            if (!warped)
            {
                slot->valid = false;
                return Fail(Failure::Warp);
            }
            if (synchronousEcho_)
                slot->valid = false;
            if (!read || !Same(observed, step.point))
                return Fail(Failure::Readback);
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
        // 1024 records accommodate an 8000 Hz mouse within the short echo lease.
        // Excess pressure only passes one event and discards its motion history;
        // it never allocates, spins, or disables gain for the whole process.
        static constexpr std::uint64_t EchoLeaseMs = 64, BaselineLeaseMs = 250;
        std::array<Echo, 1024> echoes_{};
        View view_{};
        Vec2 expected_{}, remainder_{}, warpPoint_{};
        std::uint64_t lastAt_ = 0, pauseVersion_ = 0, feedbackGeneration_ = 0;
        Failure failure_ = Failure::None;
        bool seeded_ = false, inWarp_ = false, synchronousEcho_ = false;
        bool feedbackValid_ = false;

        bool Fail(Failure reason) noexcept
        {
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

    private:
        Native& native_;
        Rect limit_{};
    };

} // namespace glide::pointer
