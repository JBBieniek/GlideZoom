#pragma once

#include <cmath>
#include <cstdint>
#include <type_traits>

// A small, platform-independent owner of the two global magnification values.
// Windows does not expose an ownership token or atomic compare-and-set for
// these APIs. Comparisons detect observable interference; they cannot remove
// the race between the final comparison and a native write by another process.
namespace glide::session
{

    struct Rect
    {
        std::int32_t left = 0, top = 0, right = 0, bottom = 0;
    };
    struct Visual
    {
        float zoom = 1;
        std::int32_t x = 0, y = 0;
    };
    struct Input
    {
        bool enabled = false;
        Rect source{}, destination{};
    };
    struct Snapshot
    {
        Visual visual{};
        Input input{};
    };
    struct RecoveryRecord
    {
        Snapshot baseline{}, before{}, after{};
    };
    static_assert(std::is_trivially_copyable<RecoveryRecord>::value,
                  "Recovery records must be copied without allocation or pointers");

    inline constexpr bool operator==(Rect a, Rect b)
    {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
    inline constexpr bool operator==(Visual a, Visual b)
    {
        // Offsets have no effect at exactly 1x according to the native API contract.
        return a.zoom == b.zoom && (a.zoom == 1 || (a.x == b.x && a.y == b.y));
    }
    inline constexpr bool operator==(Input a, Input b)
    {
        // Windows may retain old rectangles while mapping is disabled.
        return a.enabled == b.enabled && (!a.enabled ||
                                          (a.source == b.source && a.destination == b.destination));
    }
    inline constexpr bool operator==(Snapshot a, Snapshot b)
    {
        return a.visual == b.visual && a.input == b.input;
    }
    inline constexpr bool operator!=(Snapshot a, Snapshot b)
    {
        return !(a == b);
    }
    inline constexpr bool Valid(Rect r)
    {
        return r.right > r.left && r.bottom > r.top;
    }
    inline bool Valid(Visual v)
    {
        return std::isfinite(v.zoom) && v.zoom >= 1 && v.zoom <= 4096 &&
               v.x >= -262144 && v.x <= 262144 && v.y >= -262144 && v.y <= 262144;
    }
    inline constexpr bool Valid(Input i)
    {
        return !i.enabled || (Valid(i.source) && Valid(i.destination));
    }
    inline bool Valid(Snapshot s)
    {
        return Valid(s.visual) && Valid(s.input);
    }
    inline constexpr bool IsIdentity(Snapshot s)
    {
        return s.visual.zoom == 1 && !s.input.enabled;
    }
    inline bool Valid(RecoveryRecord r)
    {
        return Valid(r.baseline) && IsIdentity(r.baseline) && Valid(r.before) && Valid(r.after) &&
               (r.before.visual == r.after.visual || r.before.input == r.after.input);
    }
    inline bool Recognized(RecoveryRecord r, Snapshot observed)
    {
        return Valid(r) && Valid(observed) && (observed == r.before || observed == r.after);
    }

    enum class Result
    {
        Applied,
        Unchanged,
        Conflict,
        ReadFailure,
        WriteFailure,
        Invalid
    };

    // Backend: bool Read(Snapshot&), SetVisual(Visual), SetInput(Input).
    // Journal: void Publish(const RecoveryRecord&), Clear(). Publish must complete
    // without throwing before the native write; it records both sides of that write.
    class Session
    {
    public:
        bool OwnsState() const { return owned_; }
        bool NeedsRecovery() const { return owned_; }
        const Snapshot& Expected() const { return expected_; }
        const RecoveryRecord& Record() const { return record_; }

        template<class Backend, class Journal>
        Result Apply(const Snapshot& desired, Backend& backend, Journal& journal)
        {
            if (!Valid(desired))
                return Result::Invalid;
            if (IsIdentity(desired))
                return Reset(backend, journal);
            if (!owned_)
            {
                Snapshot current{};
                if (!backend.Read(current))
                    return Result::ReadFailure;
                if (!Valid(current) || !IsIdentity(current))
                    return Result::Conflict;
                baseline_ = expected_ = current;
                record_ = { current, current, current };
                owned_ = true;
                uncertain_ = false;
            }
            return Drive(desired, backend, journal);
        }

        template<class Backend, class Journal>
        Result Reset(Backend& backend, Journal& journal)
        {
            if (!owned_)
                return Result::Unchanged;
            // Disable mapping first: if that fails, leave the visible zoom in place
            // rather than make an invisible nonidentity input transform linger.
            const Result result = Drive(baseline_, backend, journal, true);
            if (result == Result::Applied || result == Result::Unchanged)
                Release(journal);
            return result;
        }

        // Observe without mutating. Use on active timer ticks even when a new
        // animation frame does not require an API write, and before taking input.
        template<class Backend, class Journal>
        Result Check(Backend& backend, Journal& journal)
        {
            if (!owned_)
                return Result::Unchanged;
            return Observe(backend, journal);
        }

        template<class Backend>
        static Result Recover(const RecoveryRecord& record, Backend& backend)
        {
            struct NoJournal
            {
                void Publish(const RecoveryRecord&) {}
                void Clear() {}
            } journal;
            return Recover(record, backend, journal);
        }

        // A recovery caller can retain the updated record if a setter or read
        // fails during restoration, and retry only that newly recognized pair.
        template<class Backend, class Journal>
        static Result Recover(const RecoveryRecord& record, Backend& backend, Journal& journal)
        {
            if (!Valid(record))
                return Result::Invalid;
            Session session;
            session.owned_ = true;
            session.uncertain_ = true;
            session.baseline_ = record.baseline;
            session.expected_ = record.before;
            session.record_ = record;
            return session.Reset(backend, journal);
        }

    private:
        template<class Journal>
        void Release(Journal& journal)
        {
            journal.Clear();
            owned_ = uncertain_ = false;
            baseline_ = expected_ = {};
            record_ = {};
        }

        template<class Backend, class Journal>
        Result Observe(Backend& backend, Journal& journal)
        {
            Snapshot current{};
            // A failed read cannot itself broaden the states that we own. Only an
            // unverified native write permits either side of the journal record.
            if (!backend.Read(current))
                return Result::ReadFailure;
            const bool ours = Valid(current) &&
                              (uncertain_ ? Recognized(record_, current) : current == expected_);
            if (!ours)
            {
                // An unrelated state must never be reset on exit or by the guardian.
                Release(journal);
                return Result::Conflict;
            }
            expected_ = current;
            uncertain_ = false;
            return Result::Unchanged;
        }

        template<class Backend, class Journal>
        Result Change(const Snapshot& desired, bool visual, Backend& backend, Journal& journal)
        {
            const Result observed = Observe(backend, journal);
            if (observed != Result::Unchanged)
                return observed;
            Snapshot next = expected_;
            if (visual)
                next.visual = desired.visual;
            else
                next.input = desired.input;
            if (next == expected_)
                return Result::Unchanged;
            record_ = { baseline_, expected_, next };
            journal.Publish(record_);
            // A crash before or after this call is recoverable from the published
            // pair. Even a FALSE native result may have changed global state.
            uncertain_ = true;
            const bool reportedSuccess = visual ? backend.SetVisual(next.visual) : backend.SetInput(next.input);
            (void)reportedSuccess; // The observed postcondition determines success.
            const Result postcondition = Observe(backend, journal);
            if (postcondition != Result::Unchanged)
                return postcondition;
            const bool reached = expected_ == next;
            // Once the outcome is known, a future crash should recognize only that
            // verified outcome, not a superseded `before` value from this write.
            record_ = { baseline_, expected_, expected_ };
            journal.Publish(record_);
            return reached ? Result::Applied : Result::WriteFailure;
        }

        template<class Backend, class Journal>
        Result Drive(const Snapshot& desired, Backend& backend, Journal& journal, bool restoring = false)
        {
            const Result first = Change(desired, !restoring, backend, journal);
            if (first != Result::Applied && first != Result::Unchanged)
                return first;
            const Result second = Change(desired, restoring, backend, journal);
            if (second != Result::Applied && second != Result::Unchanged)
                return second;
            return first == Result::Applied || second == Result::Applied ? Result::Applied : Result::Unchanged;
        }

        bool owned_ = false, uncertain_ = false;
        Snapshot baseline_{}, expected_{};
        RecoveryRecord record_{};
    };

    template<class Backend>
    Result Recover(const RecoveryRecord& record, Backend& backend)
    {
        return Session::Recover(record, backend);
    }
    template<class Backend, class Journal>
    Result Recover(const RecoveryRecord& record, Backend& backend, Journal& journal)
    {
        return Session::Recover(record, backend, journal);
    }
} // namespace glide::session
