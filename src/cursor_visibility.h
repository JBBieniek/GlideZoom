#pragma once

// MagShowSystemCursor controls the ordinary cursor independently of the cursor
// drawn by a magnifier control. Unlike ShowCursor it has no counter, getter or
// ownership token. Acquire only from an observed visible baseline, journal
// before hiding, and retain recovery responsibility across uncertain failures.
// Another process can still change the same setting between native calls.
namespace glide::cursor
{

    // Shared by normal release and guardian recovery. A guardian may call this
    // only after its owner has stopped, so a late hide cannot race the restoration.
    // A desktop transition after a successful call still leaves the journal armed
    // for a retry on the original desktop; there is no counter to unbalance.
    template<class Backend, class Journal>
    bool Recover(bool armed, Backend& backend, Journal& journal) noexcept
    {
        if (!armed)
            return true;
        if (!backend.OnInputDesktop() || !backend.ShowSystemCursor(true) ||
            !backend.OnInputDesktop())
            return false;
        journal.ClearCursorRecovery();
        return true;
    }

    // Backend: bool OnInputDesktop(), CursorShowing(bool&), ShowSystemCursor(bool).
    // CursorShowing must treat hidden and touch/pen-suppressed cursors as false.
    // ShowSystemCursor must also check the input desktop at its native boundary.
    // Journal: bool ArmCursorRecovery(), void ClearCursorRecovery(). A failed Arm
    // publishes nothing; a successful Arm is durable before it returns. All calls
    // run on one owner thread. Nested release requests cancel an in-flight acquire.
    class Lease
    {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        bool NeedsRecovery() const noexcept { return recovery_; }
        bool Hidden() const noexcept { return hidden_; }

        // A rendered replacement is a revocable prerequisite, including while
        // retaining an existing hide. The owner also calls this before deferred
        // handoff/capture work with acquisition disabled, so an early return can
        // never extend global hiding onto an ordinary, unzoomed monitor.
        // Backend additionally supplies a fresh ReplacementVisible() check.
        template<class Backend, class Journal>
        bool UpdateCovered(bool eligible, bool allowAcquire, Backend& backend, Journal& journal) noexcept
        {
            if (!eligible || (!allowAcquire && !hidden_) || !backend.ReplacementVisible())
                return Release(backend, journal);
            const bool result = Update(true, backend, journal);
            // Update's retained-hide fast path does not issue a native hide.
            // Recheck coverage anyway: the physical pointer can have crossed a
            // monitor edge since the first check, even without a new GPU frame.
            if (hidden_ && !backend.ReplacementVisible())
                return Release(backend, journal);
            return result;
        }

        template<class Backend, class Journal>
        bool Update(bool wantHidden, Backend& backend, Journal& journal) noexcept
        {
            if (!wantHidden)
                return Release(backend, journal);
            if (busy_)
                return false;
            Busy operation(*this);
            if (!backend.OnInputDesktop())
                return false;
            if (releaseRequested_)
            {
                if (recovery_)
                    Restore(backend, journal);
                return false;
            }
            if (hidden_)
                return true;
            // An ambiguous restore must finish before any new hide can be issued.
            if (recovery_)
            {
                Restore(backend, journal);
                return false;
            }

            bool showing = false;
            if (!backend.CursorShowing(showing))
                return false;
            // Do not take responsibility for a cursor another app, or touch/pen
            // suppression, had already hidden. Release must then be a no-op.
            if (!showing)
                return !releaseRequested_;
            if (releaseRequested_ || !backend.OnInputDesktop())
                return false;
            if (!journal.ArmCursorRecovery())
                return false;
            recovery_ = true;
            // A nested reset during publication occurs before any native effect.
            if (releaseRequested_)
            {
                recovery_ = false;
                journal.ClearCursorRecovery();
                return false;
            }
            const bool changed = backend.ShowSystemCursor(false);
            if (changed && !releaseRequested_)
            {
                hidden_ = true;
                return true;
            }
            // A failed native call may already have hidden the cursor. A nested
            // reset must also win over a hide completing after that reset began.
            Restore(backend, journal);
            return false;
        }

        template<class Backend, class Journal>
        bool Release(Backend& backend, Journal& journal) noexcept
        {
            if (busy_)
            {
                releaseRequested_ = true;
                return false;
            }
            if (!recovery_)
                return true;
            Busy operation(*this);
            return Restore(backend, journal);
        }

    private:
        bool recovery_ = false, hidden_ = false;
        bool busy_ = false, releaseRequested_ = false;

        struct Busy
        {
            Lease& owner;
            explicit Busy(Lease& value) noexcept :
                owner(value)
            {
                owner.busy_ = true;
                owner.releaseRequested_ = false;
            }
            ~Busy() noexcept
            {
                owner.busy_ = false;
                owner.releaseRequested_ = false;
            }
            Busy(const Busy&) = delete;
            Busy& operator=(const Busy&) = delete;
        };

        template<class Backend, class Journal>
        bool Restore(Backend& backend, Journal& journal) noexcept
        {
            if (!backend.OnInputDesktop())
                return false;
            // From this point a failure is ambiguous: a TRUE call can have taken
            // effect even when it reports failure. Never issue a fresh hide while
            // this restoration is still pending.
            hidden_ = false;
            if (!Recover(true, backend, journal))
                return false;
            recovery_ = false;
            return true;
        }
    };

} // namespace glide::cursor
