// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#include "../GlideZoom/overlay_order.h"
#include "../GlideZoom/monitor_presenter.h"

#include <array>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    using glide::overlay::OrderGuard;
    using glide::overlay::OrderResult;
    using glide::overlay::Window;
    unsigned assertions = 0;

    void Check(bool value, const char* message)
    {
        ++assertions;
        if (!value)
            throw std::runtime_error(message);
    }

    struct WindowBackend
    {
        struct Node
        {
            Window previous = 0;
            bool overlaps = true, protectedWindow = false, alive = true;
        };
        std::array<Node, OrderGuard::MaximumWindows + 4> windows{};
        bool valid = true, visible = true, pinnedAbove = false, rejectRaise = false;
        unsigned raises = 0, shows = 0, queries = 0, inspections = 0;
        std::function<void()> afterRaise, duringInspection;

        bool Current() const noexcept { return valid; }
        bool Visible() const noexcept { return visible; }
        static constexpr Window Overlay() noexcept { return 1; }
        bool Previous(Window from, Window& result) noexcept
        {
            ++queries;
            if (from >= windows.size() || !windows[from].alive)
                return false;
            result = windows[from].previous;
            return true;
        }
        bool Inspect(Window candidate, bool& overlaps, bool& protectedWindow)
        {
            ++inspections;
            if (candidate >= windows.size() || !windows[candidate].alive)
                return false;
            overlaps = windows[candidate].overlaps;
            protectedWindow = windows[candidate].protectedWindow;
            if (duringInspection)
                duringInspection();
            return true;
        }
        bool Raise(bool show)
        {
            ++raises;
            shows += show ? 1u : 0u;
            if (rejectRaise)
                return false;
            if (!pinnedAbove)
                windows[Overlay()].previous = 0;
            if (show)
                visible = true;
            if (afterRaise)
                afterRaise();
            return true;
        }
        void Popup(Window window = 2) noexcept { windows[Overlay()].previous = window; }
    };

    void PopupAndStableView()
    {
        OrderGuard guard;
        WindowBackend windows;
        Check(guard.Ensure(windows, 0) == OrderResult::Ready && windows.raises == 0,
              "an unobstructed active zoom never changes Z-order");
        windows.Popup();
        const auto originalPopup = windows.windows[2];
        Check(guard.Ensure(windows, 16) == OrderResult::Ready && windows.raises == 1 && windows.shows == 0,
              "a new native dropdown above zoom causes one own-overlay raise");
        Check(windows.windows[1].previous == 0 && windows.windows[2].alive &&
                  windows.windows[2].previous == originalPopup.previous &&
                  windows.windows[2].overlaps == originalPopup.overlaps,
              "the original popup remains available to source capture and is covered by the enlarged view");
        for (std::uint64_t now = 32; now < 500; now += 16)
            Check(guard.Ensure(windows, now) == OrderResult::Ready && windows.raises == 1,
                  "ordinary animation and pan do not repeatedly move an unobstructed overlay");
        windows.visible = false;
        guard.Reset();
        Check(guard.Ensure(windows, 500) == OrderResult::Ready && windows.shows == 1,
              "initial activation shows only the private overlay");
    }

    void IgnoreUnrelatedWindows()
    {
        OrderGuard guard;
        WindowBackend windows;
        windows.Popup();
        windows.windows[2].overlaps = false;
        windows.windows[2].protectedWindow = true;
        windows.windows[2].previous = 3;
        windows.windows[3].overlaps = false;
        Check(guard.Ensure(windows, 0) == OrderResult::Ready && windows.raises == 0 && windows.inspections == 2,
              "hidden, cloaked, minimized and other-monitor windows reported as non-overlapping do not trigger repairs");
        windows.windows[3].overlaps = true;
        Check(guard.Ensure(windows, 16) == OrderResult::Ready && windows.raises == 1,
              "an unrelated predecessor cannot conceal a later overlapping native popup");
    }

    void StopContention()
    {
        OrderGuard guard;
        WindowBackend windows;
        windows.Popup();
        Check(guard.Ensure(windows, 0) == OrderResult::Ready, "first popup repair succeeds");
        windows.Popup();
        Check(guard.Ensure(windows, 16) == OrderResult::Contended && windows.raises == 1,
              "a window immediately reclaiming topmost stops zoom without another raise");
        windows.windows[1].previous = 0;
        Check(guard.Ensure(windows, 2000) == OrderResult::Contended,
              "a failed activation cannot silently resume without the owner's reset");

        guard.Reset();
        windows.raises = 0;
        for (Window popup = 2; popup < 2 + OrderGuard::MaximumRaises; ++popup)
        {
            windows.Popup(popup);
            Check(guard.Ensure(windows, static_cast<std::uint64_t>(popup) * 16) == OrderResult::Ready,
                  "a bounded cascade of distinct menus is permitted");
        }
        windows.Popup(2 + OrderGuard::MaximumRaises);
        Check(guard.Ensure(windows, 200) == OrderResult::Contended && windows.raises == OrderGuard::MaximumRaises,
              "rotating HWNDs cannot evade the aggregate topmost repair budget");
    }

    void ReusedMenuHandle()
    {
        OrderGuard guard;
        WindowBackend windows;
        windows.Popup();
        Check(guard.Ensure(windows, 0) == OrderResult::Ready, "open first menu");
        Check(guard.Ensure(windows, 16) == OrderResult::Ready &&
                  guard.Ensure(windows, 16 + OrderGuard::ClearRearmMs) == OrderResult::Ready,
              "observe a stable clear view after the menu closes");
        windows.Popup();
        Check(guard.Ensure(windows, 200) == OrderResult::Ready && windows.raises == 2,
              "a new ordinary menu may safely reuse a previously observed HWND");
        windows.Popup(3);
        Check(guard.Ensure(windows, 216) == OrderResult::Ready, "new submenu is covered");
        windows.Popup(2);
        Check(guard.Ensure(windows, 232) == OrderResult::Contended && windows.raises == 3,
              "two windows alternating above the overlay cannot oscillate indefinitely");
    }

    void RateBudgetSurvivesClearIntervals()
    {
        OrderGuard guard;
        WindowBackend windows;
        for (std::uint64_t menu = 0; menu != OrderGuard::MaximumRaises; ++menu)
        {
            windows.Popup();
            Check(guard.Ensure(windows, menu * 120) == OrderResult::Ready,
                  "a later menu can reuse its HWND after the preceding clear interval");
            Check(guard.Ensure(windows, menu * 120 + 1) == OrderResult::Ready &&
                      guard.Ensure(windows, menu * 120 + 101) == OrderResult::Ready,
                  "each ordinary menu is followed by a stable clear observation");
        }
        windows.Popup();
        Check(guard.Ensure(windows, 960) == OrderResult::Contended && windows.raises == OrderGuard::MaximumRaises,
              "brief clear intervals cannot reset the aggregate one-second contention budget");

        guard.Reset();
        windows.raises = 0;
        windows.windows[1].previous = 0;
        Check(guard.Ensure(windows, 0) == OrderResult::Ready, "fresh activation has no obstruction");
        for (std::uint64_t menu = 0; menu != 20; ++menu)
        {
            const auto now = menu * 1000;
            windows.Popup();
            Check(guard.Ensure(windows, now) == OrderResult::Ready,
                  "normal later menu use is not permanently locked out by an old repair budget");
            Check(guard.Ensure(windows, now + 1) == OrderResult::Ready &&
                      guard.Ensure(windows, now + 101) == OrderResult::Ready,
                  "stable clear observations rearm HWND reuse across budget periods");
        }
    }

    void NativeFailuresAndProtectedWindows()
    {
        for (unsigned scenario = 0; scenario != 7; ++scenario)
        {
            OrderGuard guard;
            WindowBackend windows;
            windows.Popup();
            OrderResult expected = OrderResult::Unavailable;
            if (scenario == 0)
            {
                windows.windows[2].previous = 3;
                windows.windows[3].protectedWindow = true;
                expected = OrderResult::Protected;
            }
            if (scenario == 1)
            {
                windows.pinnedAbove = true;
                expected = OrderResult::Blocked;
            }
            if (scenario == 2)
                windows.rejectRaise = true;
            if (scenario == 3)
                windows.windows[2].alive = false;
            if (scenario == 4)
                windows.duringInspection = [&] { windows.valid = false; };
            if (scenario == 5)
                windows.afterRaise = [&] { windows.valid = false; };
            if (scenario == 6)
                windows.afterRaise = [&] { windows.visible = false; };
            Check(guard.Ensure(windows, 0) == expected, "native uncertainty or obstruction revokes presentation");
            if (scenario == 0 || scenario == 3 || scenario == 4)
                Check(windows.raises == 0, "protected, vanished or cancelled observations never authorize a raise");
            if (scenario == 1)
                Check(windows.raises == 1, "an un-coverable higher-band window receives one bounded attempt");
        }
    }

    void HiddenPreflight()
    {
        for (const bool show : { false, true })
        {
            OrderGuard guard;
            WindowBackend windows;
            windows.visible = false;
            windows.Popup();
            windows.windows[2].protectedWindow = true;
            Check(guard.Ensure(windows, 0, show) == OrderResult::Protected &&
                      windows.raises == 0 && !windows.visible,
                  "a protected predecessor is inspected before even an initially hidden overlay can be raised");
        }
        OrderGuard guard;
        WindowBackend windows;
        windows.visible = false;
        windows.Popup();
        Check(guard.Ensure(windows, 0, false) == OrderResult::Ready &&
                  !windows.visible && windows.raises == 1 && windows.shows == 0,
              "initial preflight may order the private host without displaying an image before its input map");
        Check(guard.Ensure(windows, 1, true) == OrderResult::Ready && windows.visible && windows.shows == 1,
              "the initial view is displayed only when its owner explicitly requests show");
    }

    void CaptureProtectionObservation()
    {
        using glide::overlay::CaptureProtection;
        using glide::overlay::ObserveCaptureProtection;
        for (const bool layered : { false, true })
        {
            Check(ObserveCaptureProtection(layered, true, 0) == CaptureProtection::NotDetected,
                  "a successful zero-affinity observation does not invent capture protection");
            Check(ObserveCaptureProtection(layered, true, 1) == CaptureProtection::Detected &&
                      ObserveCaptureProtection(layered, true, 0x11) == CaptureProtection::Detected,
                  "both supported protection modes prevent covering the original window");
        }
        Check(ObserveCaptureProtection(true, false, 0) == CaptureProtection::Unavailable,
              "a layered window whose supported affinity query fails cannot be assumed capturable");
        Check(ObserveCaptureProtection(false, false, 0) == CaptureProtection::NotDetected,
              "the documented non-layered query limitation is not misreported as verified protection");
    }

    void BoundedObservationAndClock()
    {
        for (unsigned scenario = 0; scenario != 3; ++scenario)
        {
            OrderGuard guard;
            WindowBackend windows;
            windows.Popup();
            if (scenario == 0)
                windows.windows[2].previous = 1;
            if (scenario == 1)
                windows.windows[2].previous = 2;
            if (scenario == 2)
                for (Window index = 2; index < windows.windows.size() - 1; ++index)
                {
                    windows.windows[index].previous = index + 1;
                    windows.windows[index].overlaps = false;
                }
            Check(guard.Ensure(windows, 0) == OrderResult::Unavailable && windows.raises == 0 &&
                      windows.queries <= OrderGuard::MaximumWindows + 1,
                  "cyclic, invalid or overlong predecessor chains are rejected with bounded work");
        }
        OrderGuard guard;
        WindowBackend windows;
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        Check(guard.Ensure(windows, maximum - 1000) == OrderResult::Ready,
              "a large monotonic clock remains valid");
        windows.Popup();
        Check(guard.Ensure(windows, maximum - 999) == OrderResult::Ready,
              "deadline arithmetic near uint64 maximum does not overflow");
        Check(guard.Ensure(windows, 0) == OrderResult::Unavailable,
              "a backwards clock cannot renew a contention allowance");
    }

    struct MappingBackend
    {
        glide::session::Snapshot state{};
        unsigned inputWrites = 0;
        std::function<void()> afterInput;
        bool Read(glide::session::Snapshot& value) const noexcept
        {
            value = state;
            return true;
        }
        bool SetVisual(glide::session::Visual value) noexcept
        {
            state.visual = value;
            return true;
        }
        bool SetInput(glide::session::Input value)
        {
            ++inputWrites;
            state.input = value;
            if (afterInput)
                afterInput();
            return true;
        }
    };

    struct Journal
    {
        bool armed = false;
        void Publish(const glide::session::RecoveryRecord&) noexcept { armed = true; }
        void Clear() noexcept { armed = false; }
    };

    struct PresentedOverlay
    {
        WindowBackend windows;
        OrderGuard guard;
        glide::monitor::Frame displayed{};
        bool prepared = false, pending = false;
        std::uint64_t now = 0;
        std::function<void()> duringPending;

        PresentedOverlay() noexcept { windows.visible = false; }
        bool Matches(const glide::monitor::Frame& frame) const noexcept
        {
            return prepared && glide::monitor::SameFrame(displayed, frame);
        }
        bool Prepare(const glide::monitor::Frame& frame)
        {
            if (guard.Ensure(windows, now, false) != OrderResult::Ready)
            {
                Hide();
                return false;
            }
            if (pending)
            {
                if (duringPending)
                    duringPending();
                return false;
            }
            displayed = frame;
            prepared = true;
            return true;
        }
        bool Show() noexcept
        {
            return guard.Ensure(windows, now, false) == OrderResult::Ready &&
                   guard.Ensure(windows, now, true) == OrderResult::Ready;
        }
        void Hide() noexcept
        {
            prepared = pending = windows.visible = false;
            guard.Reset();
        }
        bool Pending() const noexcept { return pending; }
        bool CanRetain(const glide::session::Snapshot& snapshot) noexcept
        {
            const glide::session::Snapshot expected{ { 1, 0, 0 }, { true, displayed.source, displayed.destination } };
            return pending && prepared && windows.visible && snapshot == expected &&
                   guard.Ensure(windows, now, false) == OrderResult::Ready;
        }
    };

    void MappingRecoveryAfterBlockedPresentation()
    {
        MappingBackend mapping;
        Journal journal;
        PresentedOverlay renderer;
        // This popup arrives after the successful preparation preflight, during
        // the owner's input write. The display-side check must still reject it.
        mapping.afterInput = [&] {
            if (mapping.state.input.enabled)
            {
                renderer.windows.Popup();
                renderer.windows.pinnedAbove = true;
            }
        };
        glide::monitor::Frame frame;
        Check(glide::monitor::MakeFrame({ 0, 0, 1920, 1080 }, { 2, { 100, 100 } }, frame), "valid zoom frame");
        glide::session::Session session;
        const auto outcome = glide::monitor::Present(frame, renderer, session, mapping, journal);
        Check(outcome.localFailure && outcome.result == glide::session::Result::WriteFailure &&
                  !renderer.windows.visible && session.NeedsRecovery() && journal.armed,
              "blocked production presentation hides the overlay and retains its input recovery journal");
        const auto restored = session.Reset(mapping, journal);
        Check((restored == glide::session::Result::Applied || restored == glide::session::Result::Unchanged) &&
                  glide::session::IsIdentity(mapping.state) && !session.NeedsRecovery() && !journal.armed,
              "the owner's existing failure path can restore the recognized input mapping");
    }

    void KnownBlockerPrecedesMapping()
    {
        MappingBackend mapping;
        Journal journal;
        PresentedOverlay renderer;
        renderer.windows.Popup();
        renderer.windows.pinnedAbove = true;
        glide::monitor::Frame frame;
        Check(glide::monitor::MakeFrame({ 0, 0, 1920, 1080 }, { 2, { 100, 100 } }, frame), "valid initial frame");
        glide::session::Session session;
        const auto outcome = glide::monitor::Present(frame, renderer, session, mapping, journal);
        Check(outcome.localFailure && outcome.result == glide::session::Result::WriteFailure &&
                  !renderer.windows.visible && mapping.inputWrites == 0 && !session.NeedsRecovery() && !journal.armed,
              "a known un-coverable window is rejected before any new input mapping is acquired");
    }

    void PendingRetentionRechecksNewPopup()
    {
        for (unsigned scenario = 0; scenario != 3; ++scenario)
        {
            MappingBackend mapping;
            Journal journal;
            PresentedOverlay renderer;
            glide::monitor::Frame oldFrame, nextFrame;
            Check(glide::monitor::MakeFrame({ 0, 0, 1920, 1080 }, { 2, { 100, 100 } }, oldFrame) &&
                      glide::monitor::MakeFrame({ 0, 0, 1920, 1080 }, { 3, { 100, 100 } }, nextFrame),
                  "valid old and requested images");
            glide::session::Session session;
            const auto initial = glide::monitor::Present(oldFrame, renderer, session, mapping, journal);
            Check(!initial.localFailure && renderer.windows.visible && session.OwnsState(),
                  "establish a visible old image and its recoverable input map");
            const auto oldMapping = mapping.state;
            const auto writes = mapping.inputWrites;
            renderer.pending = true;
            renderer.now = 16;
            renderer.duringPending = [&] {
                // Model a popup arriving after Prepare's preflight, while the
                // compositor is pending. CanRetain must observe it afresh.
                renderer.windows.Popup();
                renderer.windows.pinnedAbove = scenario == 1;
                renderer.windows.windows[2].protectedWindow = scenario == 2;
            };
            const auto outcome = glide::monitor::Present(nextFrame, renderer, session, mapping, journal);
            Check(mapping.state == oldMapping && mapping.inputWrites == writes,
                  "pending readiness never applies the newly requested image's input map");
            if (scenario == 0)
            {
                Check(outcome.pending && !outcome.localFailure && renderer.windows.visible &&
                          renderer.windows.windows[1].previous == 0,
                      "retaining the old image also covers a newly arrived ordinary popup");
            }
            else
            {
                Check(outcome.localFailure && !outcome.pending && !renderer.windows.visible &&
                          session.NeedsRecovery() && journal.armed,
                      "protected or un-coverable popups revoke an otherwise retainable old image");
                const auto restored = session.Reset(mapping, journal);
                Check((restored == glide::session::Result::Applied || restored == glide::session::Result::Unchanged) &&
                          glide::session::IsIdentity(mapping.state) && !journal.armed,
                      "retention failure leaves the existing conditional input recovery usable");
            }
        }
    }
}

int Run_overlay_order()
{
    PopupAndStableView();
    IgnoreUnrelatedWindows();
    StopContention();
    ReusedMenuHandle();
    RateBudgetSurvivesClearIntervals();
    NativeFailuresAndProtectedWindows();
    HiddenPreflight();
    CaptureProtectionObservation();
    BoundedObservationAndClock();
    MappingRecoveryAfterBlockedPresentation();
    KnownBlockerPrecedesMapping();
    PendingRetentionRechecksNewPopup();
    std::cout << "PASS: " << assertions << " overlay order assertions; production bounded ordering and input recovery, no Windows compositor executed\n";
    return 0;
}
