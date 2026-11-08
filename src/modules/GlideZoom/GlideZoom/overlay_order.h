// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace glide::overlay
{
    using Window = std::uintptr_t;

    enum class OrderResult
    {
        Ready,
        Unavailable,
        Protected,
        Contended,
        Blocked
    };

    enum class CaptureProtection
    {
        NotDetected,
        Detected,
        Unavailable
    };

    inline constexpr CaptureProtection ObserveCaptureProtection(bool layered, bool querySucceeded, std::uint32_t affinity) noexcept
    {
        if (querySucceeded)
            return affinity ? CaptureProtection::Detected : CaptureProtection::NotDetected;
        return layered ? CaptureProtection::Unavailable : CaptureProtection::NotDetected;
    }

    // Operates on the application's overlay only. The native boundary observes
    // other windows but never activates, hides, moves or sends messages to them.
    // One repair covers ordinary menus and dropdowns, followed by a fresh check.
    // A window reclaiming the same place, or many new contenders, revokes the
    // view instead of starting a Z-order fight. A stable clear interval permits
    // later menus, including a new menu that reuses an earlier HWND.
    class OrderGuard
    {
    public:
        static constexpr std::size_t MaximumWindows = 64;
        static constexpr std::size_t MaximumRaises = 8;
        static constexpr std::uint64_t RaiseWindowMs = 1000;
        static constexpr std::uint64_t ClearRearmMs = 100;

        void Reset() noexcept { *this = {}; }

        // Preflight and retained-frame checks pass show=false. They may repair
        // order, but must not display a hidden image before its matching map.
        template<class Backend>
        OrderResult Ensure(Backend& backend, std::uint64_t now, bool show = true) noexcept
        {
            if (failure_ != OrderResult::Ready)
                return failure_;
            if ((clockReady_ && now < lastTime_) || !backend.Current())
                return Fail(OrderResult::Unavailable);
            if (!clockReady_)
            {
                budgetStart_ = now;
                clockReady_ = true;
            }
            lastTime_ = now;
            const bool visible = backend.Visible();
            Window blocker = 0;
            const auto observed = FindBlocker(backend, blocker);
            if (observed != OrderResult::Ready)
                return Fail(observed);
            if (!blocker)
            {
                if (visible)
                    ObserveClear(now);
                if (visible || !show)
                    return OrderResult::Ready;
            }
            else if (!AllowRaise(blocker, now))
                return Fail(OrderResult::Contended);
            if (!backend.Raise(show && !visible) || !backend.Current() ||
                ((visible || show) && !backend.Visible()))
                return Fail(OrderResult::Unavailable);
            Window remaining = 0;
            const auto verified = FindBlocker(backend, remaining);
            if (verified != OrderResult::Ready)
                return Fail(verified);
            if (remaining)
                return Fail(OrderResult::Blocked);
            return OrderResult::Ready;
        }

    private:
        std::array<Window, MaximumRaises> recent_{};
        std::size_t recentCount_ = 0, raises_ = 0;
        std::uint64_t lastTime_ = 0, budgetStart_ = 0, clearSince_ = 0;
        bool clockReady_ = false, clearObserved_ = false;
        OrderResult failure_ = OrderResult::Ready;

        OrderResult Fail(OrderResult result) noexcept
        {
            failure_ = result;
            return result;
        }

        void ObserveClear(std::uint64_t now) noexcept
        {
            if (!clearObserved_)
            {
                clearObserved_ = true;
                clearSince_ = now;
            }
            if (now - clearSince_ >= ClearRearmMs)
                recentCount_ = 0;
        }

        bool AllowRaise(Window blocker, std::uint64_t now) noexcept
        {
            clearObserved_ = false;
            if (now - budgetStart_ >= RaiseWindowMs)
            {
                budgetStart_ = now;
                raises_ = 0;
            }
            if (raises_ == MaximumRaises || recentCount_ == recent_.size())
                return false;
            for (std::size_t index = 0; index < recentCount_; ++index)
                if (recent_[index] == blocker)
                    return false;
            recent_[recentCount_++] = blocker;
            ++raises_;
            return true;
        }

        template<class Backend>
        static OrderResult FindBlocker(Backend& backend, Window& blocker) noexcept
        {
            std::array<Window, MaximumWindows> visited{};
            std::size_t count = 0;
            const Window overlay = backend.Overlay();
            Window current = overlay;
            blocker = 0;
            for (;;)
            {
                Window previous = 0;
                if (!backend.Previous(current, previous))
                    return OrderResult::Unavailable;
                if (!previous)
                    return backend.Current() ? OrderResult::Ready : OrderResult::Unavailable;
                if (previous == overlay || count == visited.size())
                    return OrderResult::Unavailable;
                for (std::size_t index = 0; index < count; ++index)
                    if (visited[index] == previous)
                        return OrderResult::Unavailable;
                visited[count++] = previous;
                bool overlaps = false, protectedWindow = false;
                if (!backend.Inspect(previous, overlaps, protectedWindow))
                    return OrderResult::Unavailable;
                if (overlaps)
                {
                    if (protectedWindow)
                        return OrderResult::Protected;
                    if (!blocker)
                        blocker = previous;
                }
                current = previous;
            }
        }
    };
}
