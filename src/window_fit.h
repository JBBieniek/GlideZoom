#pragma once

#include <cstdint>

namespace glide::window_fit
{
    using ValidityCallback = bool (*)(void*) noexcept;
    struct Rect
    {
        std::int32_t left = 0, top = 0, right = 0, bottom = 0;
    };
    inline constexpr bool operator==(Rect a, Rect b) noexcept
    {
        return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
    }
    inline constexpr bool operator!=(Rect a, Rect b) noexcept
    {
        return !(a == b);
    }
    inline constexpr bool Valid(Rect r) noexcept
    {
        constexpr std::int32_t limit = 262400;
        return r.left >= -limit && r.top >= -limit && r.right <= limit && r.bottom <= limit &&
               r.right > r.left && r.bottom > r.top;
    }
    inline constexpr bool Contains(Rect outer, Rect inner) noexcept
    {
        return Valid(outer) && Valid(inner) && inner.left >= outer.left && inner.top >= outer.top &&
               inner.right <= outer.right && inner.bottom <= outer.bottom;
    }
    inline constexpr bool CoversMonitor(Rect visible, Rect monitor) noexcept
    {
        return Valid(visible) && Valid(monitor) && visible.left <= monitor.left + 2 &&
               visible.top <= monitor.top + 2 && visible.right >= monitor.right - 2 &&
               visible.bottom >= monitor.bottom - 2;
    }
    enum class Reason
    {
        Idle,
        Ready,
        Pending,
        Fitted,
        Busy,
        InvalidViewport,
        UnavailableDesktop,
        NoWindow,
        OwnWindow,
        NotResizable,
        Fullscreen,
        Maximized,
        Hung,
        UnavailableGeometry,
        ContextChanged,
        NativeFailure,
        AppConstrained,
        TimedOut
    };
    struct Identity
    {
        std::uintptr_t window = 0;
        std::uint32_t process = 0, thread = 0;
    };
    inline constexpr bool operator==(Identity a, Identity b) noexcept
    {
        return a.window == b.window && a.process == b.process && a.thread == b.thread;
    }
    inline constexpr bool operator!=(Identity a, Identity b) noexcept
    {
        return !(a == b);
    }
    struct Observation
    {
        Identity identity{};
        Rect outer{}, visible{}, monitor{};
        bool currentDesktop = true, sameSession = true, sameUser = true, foreground = true;
        bool ownWindow = false, visibleWindow = true, cloaked = false, iconic = false;
        bool maximized = false, hung = false, rootWindow = true, ownedWindow = false;
        bool thickFrame = true, caption = true, enabled = true, moveAllowed = true, sizeAllowed = true;
    };
    inline constexpr Reason Eligible(const Observation& o) noexcept
    {
        if (!o.currentDesktop || !o.sameSession || !o.sameUser)
            return Reason::UnavailableDesktop;
        if (!o.identity.window || !o.identity.process || !o.identity.thread || !o.foreground)
            return Reason::NoWindow;
        if (o.ownWindow)
            return Reason::OwnWindow;
        if (!o.visibleWindow || o.cloaked || o.iconic || !o.rootWindow || o.ownedWindow)
            return Reason::NotResizable;
        if (!Valid(o.outer) || !Valid(o.visible) || !Valid(o.monitor))
            return Reason::UnavailableGeometry;
        if (CoversMonitor(o.visible, o.monitor))
            return Reason::Fullscreen;
        if (o.maximized)
            return Reason::Maximized;
        if (!o.thickFrame || !o.caption || !o.enabled || !o.moveAllowed || !o.sizeAllowed)
            return Reason::NotResizable;
        return o.hung ? Reason::Hung : Reason::Ready;
    }
    struct Request
    {
        Rect source{}, monitor{};
    };
    inline constexpr bool Valid(const Request& r) noexcept
    {
        return Contains(r.monitor, r.source) && r.source != r.monitor;
    }
    // GetWindowRect includes invisible resize borders; the committed source is
    // the desired visible DWM frame. All inputs must be physical desktop pixels.
    inline constexpr bool MakeOuter(Rect outer, Rect visible, Rect desiredVisible, Rect& output) noexcept
    {
        if (!Contains(outer, visible) || !Valid(desiredVisible))
            return false;
        const auto left = visible.left - outer.left, top = visible.top - outer.top;
        const auto right = outer.right - visible.right, bottom = outer.bottom - visible.bottom;
        if (left > 256 || top > 256 || right > 256 || bottom > 256)
            return false;
        const Rect desired{ desiredVisible.left - left, desiredVisible.top - top, desiredVisible.right + right, desiredVisible.bottom + bottom };
        if (!Valid(desired))
            return false;
        output = desired;
        return true;
    }
    struct Result
    {
        Reason reason = Reason::Idle;
        Identity identity{};
        Rect actualVisible{};
        bool actualKnown = false;
        std::uint32_t error = 0;
    };
    // One request, then observation only. Never retry, restore, activate, or undo
    // application-owned geometry after refusal, timeout, focus loss or failure.
    class Operation
    {
    public:
        bool Pending() const noexcept { return pending_; }
        Result Last() const noexcept { return last_; }
        Result Abandon(Reason reason = Reason::ContextChanged) noexcept
        {
            ++revision_;
            last_.actualKnown = false;
            return Finish(reason);
        }

        template<class Native>
        Result Begin(Native& native, Request request, std::uint64_t now, ValidityCallback valid = nullptr, void* context = nullptr) noexcept
        {
            if (pending_ || active_)
            {
                Result busy = last_;
                busy.reason = Reason::Busy;
                return busy;
            }
            CallScope call{ active_ };
            const std::uint64_t revision = revision_;
            BeginValidity authorization{ *this, revision, valid, context };
            last_ = {};
            if (!CheckBeginValidity(&authorization))
                return revision_ != revision ? last_ : Finish(Reason::ContextChanged);
            if (!Valid(request))
                return Finish(Reason::InvalidViewport);
            Observation observed{};
            const Reason read = native.Read({}, observed, last_.error);
            if (!CheckBeginValidity(&authorization))
                return revision_ != revision ? last_ : Finish(Reason::ContextChanged);
            if (read != Reason::Ready)
                return Finish(read);
            SetActual(observed);
            const Reason eligible = Eligible(observed);
            if (eligible != Reason::Ready)
                return Finish(eligible);
            Rect desired{};
            if (!MakeOuter(observed.outer, observed.visible, request.source, desired))
                return Finish(Reason::UnavailableGeometry);
            if (observed.visible == request.source)
                return Finish(Reason::Fitted);
            // Re-read immediately before the only mutation. Do not resize a new
            // foreground window or stale geometry selected by a previous read.
            Observation fresh{};
            const Reason reread = native.Read(observed.identity, fresh, last_.error);
            if (!CheckBeginValidity(&authorization))
                return revision_ != revision ? last_ : Finish(Reason::ContextChanged);
            if (reread != Reason::Ready)
            {
                last_.actualKnown = false;
                return Finish(reread);
            }
            if (fresh.identity != observed.identity || Eligible(fresh) != Reason::Ready ||
                fresh.outer != observed.outer || fresh.visible != observed.visible)
            {
                last_.actualKnown = false;
                return Finish(Reason::ContextChanged);
            }
            original_ = observed.visible;
            desired_ = request.source;
            started_ = now;
            if (!CheckBeginValidity(&authorization))
                return revision_ != revision ? last_ : Finish(Reason::ContextChanged);
            const Reason queued = native.Queue(observed, desired, last_.error, CheckBeginValidity, &authorization);
            if (!CheckBeginValidity(&authorization))
                return revision_ != revision ? last_ : Finish(Reason::ContextChanged);
            if (queued != Reason::Ready)
            {
                last_.actualKnown = false;
                return Finish(queued);
            }
            pending_ = true;
            last_.reason = Reason::Pending;
            return last_;
        }

        template<class Native>
        Result Poll(Native& native, std::uint64_t now) noexcept
        {
            if (!pending_)
                return last_;
            if (active_)
            {
                Result busy = last_;
                busy.reason = Reason::Busy;
                return busy;
            }
            CallScope call{ active_ };
            const std::uint64_t revision = revision_;
            Observation observed{};
            const Reason read = native.Read(last_.identity, observed, last_.error);
            if (revision_ != revision)
                return last_;
            if (read != Reason::Ready)
            {
                last_.actualKnown = false;
                return Finish(read);
            }
            if (observed.identity != last_.identity)
            {
                last_.actualKnown = false;
                return Finish(Reason::ContextChanged);
            }
            SetActual(observed);
            const Reason eligible = Eligible(observed);
            if (eligible != Reason::Ready)
                return Finish(eligible);
            if (observed.visible == desired_)
                return Finish(Reason::Fitted);
            if (now < started_ || now - started_ >= 500)
                return Finish(observed.visible == original_ ? Reason::TimedOut : Reason::AppConstrained);
            return last_;
        }

    private:
        struct CallScope
        {
            bool& active;
            explicit CallScope(bool& value) noexcept :
                active(value) { active = true; }
            ~CallScope() { active = false; }
        };
        struct BeginValidity
        {
            Operation& owner;
            std::uint64_t revision;
            ValidityCallback valid;
            void* context;
        };
        static bool CheckBeginValidity(void* context) noexcept
        {
            const auto& authorization = *static_cast<const BeginValidity*>(context);
            if (authorization.owner.revision_ != authorization.revision)
                return false;
            const bool external = !authorization.valid || authorization.valid(authorization.context);
            // The caller's predicate can itself dispatch cancellation. Recheck
            // after it returns; the callback and context never escape Begin.
            return external && authorization.owner.revision_ == authorization.revision;
        }
        Result Finish(Reason reason) noexcept
        {
            pending_ = false;
            last_.reason = reason;
            return last_;
        }
        void SetActual(const Observation& observed) noexcept
        {
            last_.identity = observed.identity;
            last_.actualVisible = observed.visible;
            last_.actualKnown = Valid(observed.visible);
        }
        Result last_{};
        Rect original_{}, desired_{};
        std::uint64_t started_ = 0;
        std::uint64_t revision_ = 0;
        bool pending_ = false, active_ = false;
    };
} // namespace glide::window_fit

#ifdef _WIN32
#include <windows.h>

namespace glide::window_fit
{
    class NativeOperation
    {
    public:
        Result Begin(HWND owner, DWORD ownProcess, const RECT& committedSource, const RECT& monitor, ULONGLONG now, ValidityCallback valid = nullptr, void* context = nullptr) noexcept;
        Result Poll(ULONGLONG now) noexcept;
        Result Abandon() noexcept { return operation_.Abandon(); }
        bool Pending() const noexcept { return operation_.Pending(); }
        Result Last() const noexcept { return operation_.Last(); }

    private:
        Operation operation_{};
        HWND owner_ = nullptr;
        DWORD ownProcess_ = 0;
    };
    const wchar_t* ReasonText(Reason reason) noexcept;
} // namespace glide::window_fit
#endif
