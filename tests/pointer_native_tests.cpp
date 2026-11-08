#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <limits>
#include <stdexcept>

#ifndef _WIN32
#define _WIN32
#endif
// A private namespace prevents SDK-shim inline definitions from colliding with
// the real-SDK production headers used by other suites in the same test DLL.
// The NativeOps/Adapter implementation itself is included without alteration.
#define glide glide_pointer_native_tests
#include "../src/pointer_dispatch.h"
#undef glide

struct PointerMockDesktop
{
    const wchar_t* name;
};
struct PointerMockMonitor
{
    RECT bounds;
};

namespace
{
    using namespace glide_pointer_native_tests;
    using namespace glide_pointer_native_tests::pointer;

    using CoordinateMap = POINT (*)(POINT, const RECT&, const RECT&) noexcept;
    struct World
    {
        RECT desktop{ -1920, -200, 2560, 1440 };
        RECT clip = desktop;
        std::array<PointerMockMonitor, 2> monitors{ PointerMockMonitor{ { -1920, 0, 0, 1080 } }, PointerMockMonitor{ { 0, -200, 2560, 1440 } } };
        PointerMockDesktop thread{ L"Default" }, input{ L"Default" };
        DPI_AWARENESS_CONTEXT awareness = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
        POINT physical{ -1500, 500 }, logical{ -777, 333 }, requested{}, pending{};
        // Explicit, independent seams for a future measured native contract.
        // Identity is the default; these tests assume no Mag/hook ordering.
        RECT inputSource{ -1600, 100, -1120, 370 }, inputDestination{ -1920, 0, 0, 1080 };
        CoordinateMap setterMap = nullptr, physicalGetterMap = nullptr;
        DWORD error = ERROR_SUCCESS, warpError = ERROR_ACCESS_DENIED;
        unsigned physicalReads = 0, logicalReads = 0, clipReads = 0, writes = 0;
        unsigned opens = 0, closes = 0, monitorQueries = 0, pointQueries = 0;
        unsigned failPhysicalAt = 0, failClipAt = 0;
        bool openOk = true, threadNameOk = true, inputNameOk = true;
        bool clipOk = true, logicalOk = true, warpOk = true;
        bool monitorFound = true, monitorInfoOk = true;
        bool deferWrite = false, pendingWrite = false;
    } world;

    unsigned assertions = 0;
    void Check(bool condition, const char* message)
    {
        ++assertions;
        if (!condition)
            throw std::runtime_error(message);
    }
    bool SamePoint(POINT a, POINT b) noexcept
    {
        return a.x == b.x && a.y == b.y;
    }
    Rect Rectangle(RECT rect) noexcept
    {
        return { static_cast<double>(rect.left), static_cast<double>(rect.top), static_cast<double>(rect.right), static_cast<double>(rect.bottom) };
    }
    View Selected() noexcept
    {
        return { true, true, { -1920, 0, 0, 1080 }, 4, 7, { -1600, 100 } };
    }
    void Reset()
    {
        world = World{};
    }
    bool ApplyPending() noexcept
    {
        if (!world.pendingWrite)
            return false;
        world.physical = world.pending;
        world.pendingWrite = false;
        return true;
    }
    POINT ShiftSetter(POINT value, const RECT& source, const RECT& destination) noexcept
    {
        return { value.x + source.left - destination.left, value.y };
    }
    POINT ShiftGetter(POINT value, const RECT& source, const RECT& destination) noexcept
    {
        return { value.x, value.y + source.top - destination.top };
    }

    void DesktopDpiAndMonitorEligibility()
    {
        Reset();
        NativeOps native;
        auto view = Selected();
        Check(native.Ready(view), "PMv2 accepts exact selected physical monitor with full virtual-desktop clip");
        world.awareness = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE;
        Check(native.Ready(view), "per-monitor awareness is supported");
        for (const auto context : { DPI_AWARENESS_CONTEXT_UNAWARE, DPI_AWARENESS_CONTEXT_SYSTEM_AWARE })
        {
            world.awareness = context;
            Check(!native.Ready(view), "DPI-virtualized contexts refuse adjustment");
        }
        world.awareness = DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2;
        world.input.name = L"Winlogon";
        Check(!native.Ready(view), "another input desktop refuses adjustment");
        world.input.name = L"Default";
        world.openOk = false;
        Check(!native.Ready(view), "unknown input desktop refuses adjustment");
        world.openOk = true;
        world.threadNameOk = false;
        Check(!native.Ready(view), "unreadable thread desktop refuses adjustment");
        world.threadNameOk = true;
        world.inputNameOk = false;
        Check(!native.Ready(view), "unreadable input desktop refuses adjustment");
        Check(world.opens == world.closes + 1, "only the deliberately failed OpenInputDesktop has no handle to close");
        world.inputNameOk = true;
        world.monitorFound = false;
        Check(!native.Ready(view), "missing selected monitor refuses adjustment");
        world.monitorFound = true;
        world.monitorInfoOk = false;
        Check(!native.Ready(view), "unreadable monitor metadata refuses adjustment");
        world.monitorInfoOk = true;
        view.bounds.right = -1;
        Check(!native.Ready(view), "a monitor subset is not accepted as an entire display");
        view = Selected();
        view.bounds.left = -1921;
        Check(!native.Ready(view), "selected bounds outside the virtual desktop are refused");
        view.bounds = Rectangle(world.desktop);
        const auto queries = world.monitorQueries;
        world.monitorFound = false;
        Check(native.Ready(view) && world.monitorQueries == queries, "exact whole-desktop bounds do not require one physical monitor");
        world.desktop.right = world.desktop.left;
        Check(!native.Ready(view), "empty virtual desktop is refused");
        world.desktop.right = world.desktop.left + 262145;
        Check(!native.Ready(view), "unsupported virtual-desktop extent is refused");
        Check(world.writes == 0 && world.physicalReads == 0, "eligibility checks never move or sample the cursor");
    }

    void OrdinaryAndDragClipContracts()
    {
        Reset();
        NativeOps native;
        const auto view = Selected();
        Rect limit{ 1, 2, 3, 4 };
        world.clip = { -1700, 100, -100, 900 };
        Check(!native.Ready(view), "ordinary motion yields to any non-full virtual-desktop clip");
        Adapter ordinary;
        Check(!ordinary.Move({ -1484, 500 }, false, 100, view, true, false, native) && !ordinary.Faulted() && world.physicalReads == 0 && world.writes == 0, "ordinary clip refusal is a read-only pause, not a native write failure");
        Check(native.DragBounds(view, limit) && Same(limit, Rectangle(world.clip)), "forwarded drag accepts the existing application clip intersection");
        DragNative<NativeOps> drag(native);
        Check(drag.Initialize(view) && drag.Ready(view), "drag records and verifies the original clip");
        Check(drag.CanPlace({ -1700, 100 }) && !drag.CanPlace({ -100, 100 }) && !drag.CanPlace({ -1700, 900 }), "drag limit uses exclusive right and bottom edges");
        world.clip.left = -1699;
        Check(!drag.Ready(view), "changed application clipping invalidates the drag transaction");
        world.clip = { -100, -100, 100, 100 };
        Check(native.DragBounds(view, limit) && Same(limit, Rect{ -100, 0, 0, 100 }), "drag intersects clip with its captured physical monitor");
        world.clip = { 0, 0, 100, 100 };
        Check(!native.DragBounds(view, limit), "disjoint clip refuses drag adjustment");
        world.clip = world.desktop;
        world.clipOk = false;
        Check(!native.Ready(view) && !native.DragBounds(view, limit), "failed native clip query refuses both paths");
        Check(world.writes == 0, "clip checks never change ClipCursor or cursor position");
    }

    void PhysicalApiCoordinatesAndPlacement()
    {
        Reset();
        NativeOps native;
        Vec2 output{ 8, 9 };
        Check(native.Read(output) && Same(output, Vec2{ -1500, 500 }), "read uses integer GetPhysicalCursorPos without logical conversion");
        Check(world.logicalReads == 0, "normal physical read never uses GetCursorPos");
        world.failPhysicalAt = world.physicalReads + 1;
        Check(!native.Read(output) && Same(output, Vec2{ -1500, 500 }) && native.LastError() == ERROR_DEVICE_NOT_CONNECTED, "failed physical read leaves output unchanged and exposes native error");
        world.failPhysicalAt = 0;
        Check(native.CanPlace({ -1920, 0 }) && native.CanPlace({ 2559, -200 }), "physical monitor membership includes negative origins");
        Check(!native.CanPlace({ -1, -1 }) && !native.CanPlace({ 2560, 0 }), "monitor gaps and exclusive desktop edges refuse placement");
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (const Vec2 point : { Vec2{ -100.5, 50 }, Vec2{ nan, 50 }, Vec2{ 262145, 0 }, Vec2{ -1, -1 } })
        {
            const auto writes = world.writes;
            Check(!native.Warp(point) && world.writes == writes, "invalid or off-monitor points never reach the native setter");
        }
        Check(native.Warp({ -1496, 500 }) && SamePoint(world.requested, POINT{ -1496, 500 }) && SamePoint(world.physical, world.requested), "warp calls SetPhysicalCursorPos with exact physical integers");
        // Deliberately supplied independent mappings test call boundaries only.
        // They do not claim Windows performs either mapping or in this order.
        world.setterMap = ShiftSetter;
        world.physicalGetterMap = ShiftGetter;
        Check(native.Warp({ -1496, 500 }) && SamePoint(world.requested, POINT{ -1496, 500 }), "committed S/D fixture cannot rewrite the recorded setter request");
        Check(native.Read(output) && Same(output, Vec2{ -1176, 600 }), "getter result remains independent from setter request and logical sample");

        Reset();
        world.deferWrite = true;
        world.logical = world.physical;
        Check(native.Warp({ -1496, 500 }) && world.pendingWrite && SamePoint(world.requested, POINT{ -1496, 500 }), "deferred physical setter acknowledges and records the exact request");
        Check(native.Read(output) && Same(output, Vec2{ -1500, 500 }), "acknowledged setter can leave immediate physical readback at the previous position");
        FailureDetails beforeDelivery;
        native.ObserveFailure(beforeDelivery);
        Check(beforeDelivery.logicalRead && Same(beforeDelivery.logical, Vec2{ -1500, 500 }), "deferred fixture independently retains the previous logical sample too");
        Check(ApplyPending() && native.Read(output) && Same(output, Vec2{ -1496, 500 }) && !ApplyPending(), "only explicit native delivery changes the deferred physical position once");
    }

    void FailureObservationAndAdapterTransactions()
    {
        Reset();
        NativeOps native;
        FailureDetails direct;
        direct.nativeError = 123;
        native.ObserveFailure(direct);
        Check(direct.logicalRead && Same(direct.logical, Vec2{ -777, 333 }) && direct.clipRead && Same(direct.clip, Rectangle(world.desktop)), "failure probe preserves separate logical and clip observations");
        Check(direct.nativeError == 123 && world.physicalReads == 0 && world.writes == 0, "failure probes do not replace the transaction error or retry native writes");

        Reset();
        Adapter verified;
        Check(verified.Move({ -1484, 500 }, false, 100, Selected(), true, false, native) && !verified.Faulted(), "production NativeOps commits only the verified physical write");
        Check(world.writes == 1 && world.physicalReads == 2 && Same(verified.FeedbackSnapshot().point, Vec2{ -1496, 500 }), "successful transaction samples a baseline and exactly one native readback");

        Reset();
        world.warpOk = false;
        world.logicalOk = false;
        world.failClipAt = 2; // Ready succeeds; the later failure-context query fails.
        Adapter adapter;
        Check(!adapter.Move({ -1484, 500 }, false, 100, Selected(), true, false, native), "rejected native write is passed without claiming a successful adjustment");
        const auto failed = adapter.LastFailureDetails();
        Check(adapter.FailureReason() == Failure::Warp && failed.valid && !failed.warpSucceeded && !failed.readSucceeded, "setter rejection latches the precise Warp failure");
        Check(Same(failed.before, Vec2{ -1500, 500 }) && Same(failed.candidate, Vec2{ -1484, 500 }) && Same(failed.requested, Vec2{ -1496, 500 }), "failure retains physical before, hook candidate and exact setter request separately");
        Check(failed.nativeError == ERROR_ACCESS_DENIED && failed.logicalError == ERROR_GEN_FAILURE && failed.clipError == ERROR_INVALID_PARAMETER, "native write error is captured before failure probes overwrite GetLastError");
        Check(!failed.logicalRead && !failed.clipRead && world.writes == 1 && world.physicalReads == 1, "failed setter is neither read back nor retried");
        Check(!adapter.Move({ -1480, 500 }, false, 101, Selected(), true, false, native) && world.writes == 1, "latched failure never pulls the cursor back on the next packet");

        Reset();
        world.failPhysicalAt = 2;
        Adapter unreadable;
        Check(!unreadable.Move({ -1484, 500 }, false, 100, Selected(), true, false, native), "unreadable first setter readback refuses commit");
        const auto missing = unreadable.LastFailureDetails();
        Check(missing.reason == Failure::Readback && missing.warpSucceeded && !missing.readSucceeded && missing.nativeError == ERROR_DEVICE_NOT_CONNECTED, "readback failure carries the getter error and successful setter status");
        Check(world.physicalReads == 2 && world.writes == 1, "readback failure performs no compensating write or second verification read");

        Reset();
        world.setterMap = ShiftSetter;
        world.error = 1234; // Successful APIs need not clear a stale thread error.
        Adapter mismatch;
        Check(!mismatch.Move({ -1484, 500 }, false, 100, Selected(), true, false, native), "a deliberately differing native readback refuses commit");
        const auto different = mismatch.LastFailureDetails();
        Check(different.reason == Failure::Readback && different.warpSucceeded && different.readSucceeded && different.nativeError == ERROR_SUCCESS, "successful-but-different readback is not mislabeled as an API error");
        Check(Same(different.requested, Vec2{ -1496, 500 }) && Same(different.observed, Vec2{ -1176, 500 }) && Same(different.logical, Vec2{ -777, 333 }), "mismatch preserves requested, physical readback and logical sample independently");

        Reset();
        world.clip = { -1700, 100, -100, 900 };
        world.warpOk = false;
        DragNative<NativeOps> drag(native);
        Check(drag.Initialize(Selected()), "forwarded drag acquires native application clip bounds");
        Adapter dragAdapter;
        Check(!dragAdapter.Move({ -1484, 500 }, false, 100, Selected(), true, false, drag), "drag wrapper forwards native setter failure");
        const auto dragFailure = dragAdapter.LastFailureDetails();
        Check(dragFailure.nativeError == ERROR_ACCESS_DENIED && dragFailure.logicalRead && dragFailure.clipRead && Same(dragFailure.clip, Rectangle(world.clip)), "DragNative optional error/observation probes forward the actual NativeOps diagnostics");
    }

    void DeferredNativeWriteAcknowledgement()
    {
        Reset();
        world.deferWrite = true;
        world.physical = world.logical = { 2011, 809 };
        NativeOps native;
        Adapter measured;
        const View measuredView{ true, true, { 0, -200, 2560, 1440 }, 2.2, 7 };
        Check(measured.Move({ 2011, 813 }, false, 100, measuredView, true, false, native) && measured.Pending() && !measured.Faulted(), "measured 2.2x ordinary packet becomes pending instead of latching old-position readback as failure");
        Check(SamePoint(world.requested, POINT{ 2011, 811 }) && SamePoint(world.physical, POINT{ 2011, 809 }) && SamePoint(world.logical, POINT{ 2011, 809 }), "measured candidate, accepted target and both old getters remain distinct in the native boundary fixture");
        Check(ApplyPending(), "measured physical target is delivered after the initiating native call returns");
        (void)measured.ServicePending(101, measuredView, true, false, native);
        const auto measuredFeedback = measured.FeedbackSnapshot();
        const auto precise = Vec2{ measuredFeedback.point.x + measuredFeedback.remainder.x, measuredFeedback.point.y + measuredFeedback.remainder.y };
        const auto from = SourceToScreen({ 2011, 809 }, { measuredView.zoom, measuredView.offset });
        const auto to = SourceToScreen(precise, { measuredView.zoom, measuredView.offset });
        Check(measuredFeedback.valid && !measured.Pending() && !measured.Faulted() && std::abs(to.y - from.y - 4) < 1e-8 && world.writes == 1, "later native confirmation preserves the measured packet's fractional gain exactly once");

        Reset();
        world.deferWrite = true;
        world.logical = world.physical;
        Adapter adapter;
        const auto view = Selected();
        Check(adapter.Move({ -1484, 500 }, false, 100, view, true, false, native), "accepted deferred native write consumes its original movement once");
        Check(adapter.Pending() && !adapter.Faulted() && !adapter.FeedbackSnapshot().valid && world.writes == 1 && world.physicalReads == 2, "old-position immediate readback creates a pending proposal without publishing unverified cursor feedback");
        (void)adapter.ServicePending(101, view, true, false, native);
        Check(adapter.Pending() && !adapter.Faulted() && world.writes == 1, "still-old physical sample waits without retrying the acknowledged setter");
        Check(ApplyPending(), "native delivery applies the deferred request");
        Check(adapter.BypassEcho({ -1496, 500 }, 102) && adapter.Pending() && !adapter.FeedbackSnapshot().valid, "owned callback passes without treating its point alone as physical acknowledgement or canceling the pending proposal");
        (void)adapter.ServicePending(103, view, true, false, native);
        const auto feedback = adapter.FeedbackSnapshot();
        Check(!adapter.Pending() && !adapter.Faulted() && feedback.valid && Same(feedback.point, Vec2{ -1496, 500 }), "fresh exact native readback commits the same policy's deferred proposal");
        Check(world.writes == 1, "deferred acknowledgement never repeats the original cursor write");
        const auto displayedBefore = SourceToScreen({ -1500, 500 }, { view.zoom, view.offset });
        const auto displayedAfter = SourceToScreen(feedback.point, { view.zoom, view.offset });
        Check(displayedAfter.x - displayedBefore.x == 16, "deferred acknowledgement applies the selected speed/zoom factor exactly once");

        // Every cancellation revokes only the proposal and subpixel history.
        // Windows can still deliver the already accepted setter afterwards.
        for (unsigned cancellation = 0; cancellation < 12; ++cancellation)
        {
            Reset();
            world.deferWrite = true;
            Adapter cancelled;
            auto current = view;
            Check(cancelled.Move({ -1484, 500 }, false, 100, current, true, false, native) && cancelled.Pending(), "cancellation fixture starts with an actual pending NativeOps transaction");
            bool permitted = true, gesture = false;
            switch (cancellation)
            {
            case 0:
                cancelled.Pause();
                break;
            case 1:
                ++current.epoch;
                break;
            case 2:
                permitted = false;
                break;
            case 3:
                gesture = true;
                break;
            case 4:
                world.clip = { -1700, 100, -100, 900 };
                break;
            case 5:
                world.input.name = L"Winlogon";
                break;
            case 6:
                current.speedMultiplier = 2;
                break;
            case 7:
                current.zoom = 2;
                break;
            case 8:
                ++current.offset.x;
                break;
            case 9:
                current.adjustForZoom = false;
                break;
            case 10:
                current.requested = false;
                break;
            case 11:
                current.active = false;
                break;
            }
            (void)cancelled.ServicePending(101, current, permitted, gesture, native);
            Check(!cancelled.Pending() && !cancelled.Faulted() && !cancelled.FeedbackSnapshot().valid, "changed cancellation, desktop, clip or complete pointer policy revokes unverified ownership without a native-failure latch");
            Check(ApplyPending(), "a canceled application's acknowledged setter may still arrive");
            (void)cancelled.BypassEcho({ -1496, 500 }, 102);
            (void)cancelled.ServicePending(103, current, true, false, native);
            Check(!cancelled.Pending() && !cancelled.Faulted() && !cancelled.FeedbackSnapshot().valid && world.writes == 1, "late delivery cannot resurrect canceled feedback or issue a stale cursor correction");
        }

        Reset();
        world.deferWrite = true;
        Adapter expired;
        Check(expired.Move({ -1484, 500 }, false, 100, view, true, false, native) && expired.Pending(), "timeout fixture starts pending");
        (void)expired.ServicePending(100 + Adapter::PendingLeaseMs, view, true, false, native);
        Check(expired.Pending() && !expired.Faulted(), "the exact pending deadline still permits an old-position sample");
        (void)expired.ServicePending(101 + Adapter::PendingLeaseMs, view, true, false, native);
        Check(!expired.Pending() && expired.FailureReason() == Failure::Readback && !expired.FeedbackSnapshot().valid && world.writes == 1, "a never-delivered accepted write expires with a readback failure and no retry");
        Check(ApplyPending(), "expired setter can still be delivered by the native system");
        (void)expired.ServicePending(102 + Adapter::PendingLeaseMs, view, true, false, native);
        Check(expired.Faulted() && !expired.FeedbackSnapshot().valid && world.writes == 1, "delivery after expiry cannot silently clear a failure or replay motion");

        Reset();
        world.deferWrite = true;
        Adapter interfered;
        Check(interfered.Move({ -1484, 500 }, false, 100, view, true, false, native) && interfered.Pending(), "interference fixture starts pending");
        world.physical = { -1480, 500 };
        (void)interfered.ServicePending(101, view, true, false, native);
        Check(!interfered.Pending() && interfered.FailureReason() == Failure::Readback && world.writes == 1, "a third physical position is interference, not deferred acknowledgement or permission to restore the cursor");
        const auto failure = interfered.LastFailureDetails();
        Check(failure.readSucceeded && Same(failure.requested, Vec2{ -1496, 500 }) && Same(failure.observed, Vec2{ -1480, 500 }), "timer-only verification preserves a third position as interference without any current real packet authorizing cancellation");

        Reset();
        world.deferWrite = true;
        Adapter unreadable;
        Check(unreadable.Move({ -1484, 500 }, false, 100, view, true, false, native) && unreadable.Pending(), "deferred read-failure fixture starts pending");
        world.failPhysicalAt = world.physicalReads + 1;
        (void)unreadable.ServicePending(101, view, true, false, native);
        Check(!unreadable.Pending() && unreadable.FailureReason() == Failure::Readback && unreadable.LastFailureDetails().nativeError == ERROR_DEVICE_NOT_CONNECTED && world.writes == 1, "failed later native getter preserves its actual error and never becomes an acknowledgement");

        Reset();
        world.deferWrite = true;
        Adapter interrupted;
        Check(interrupted.Move({ -1484, 500 }, false, 100, view, true, false, native) && interrupted.Pending(), "intervening packet fixture starts pending");
        Check(!interrupted.Move({ -1480, 500 }, false, 101, view, true, false, native) && !interrupted.Pending() && !interrupted.Faulted() && world.writes == 1, "a new real packet while the native getter is still old cancels the proposal and passes without inventing movement");
        Check(ApplyPending(), "interrupted accepted setter can arrive after the real packet was forwarded");
        (void)interrupted.BypassEcho({ -1496, 500 }, 102);
        Check(!interrupted.FeedbackSnapshot().valid && world.writes == 1, "late owned echo cannot re-scale the intervening real packet");

        Reset();
        world.deferWrite = true;
        Adapter committedReal;
        Check(committedReal.Move({ -1484, 500 }, false, 100, view, true, false, native) && committedReal.Pending(), "already-committed real packet fixture starts with a pending setter");
        world.physical = { -1480, 500 };
        Check(!committedReal.Move({ -1480, 500 }, false, 101, view, true, false, native), "real packet already visible through the physical getter is forwarded instead of consumed");
        Check(!committedReal.Pending() && !committedReal.Faulted() && !committedReal.FeedbackSnapshot().valid && Same(committedReal.Remainder(), Vec2{}) && committedReal.PendingCounts().interrupted == 1 && world.writes == 1, "observing the current real candidate cancels the old proposal with no failure, fraction, speculative feedback or additional write");
        Check(ApplyPending(), "an older accepted setter can still arrive after already-committed real movement");
        (void)committedReal.BypassEcho({ -1496, 500 }, 102);
        Check(!committedReal.Faulted() && !committedReal.FeedbackSnapshot().valid && world.writes == 1, "late setter echo cannot resurrect the canceled proposal after committed real input");

        Reset();
        world.deferWrite = true;
        Adapter continuous;
        Check(continuous.Move({ -1484, 500 }, false, 100, view, true, false, native) && ApplyPending(), "next-packet verification fixture has a delivered but unserviced request");
        Check(continuous.Move({ -1480, 500 }, false, 101, view, true, false, native) && continuous.Pending() && world.writes == 2 && SamePoint(world.requested, POINT{ -1492, 500 }), "the next real packet first confirms native delivery then receives one fresh gain calculation");
        Check(ApplyPending(), "second deferred correction is delivered once");
        (void)continuous.ServicePending(102, view, true, false, native);
        Check(!continuous.Pending() && !continuous.Faulted() && Same(continuous.FeedbackSnapshot().point, Vec2{ -1492, 500 }), "consecutive native acknowledgements keep the verified source displacement continuous");

        Reset();
        world.deferWrite = true;
        world.clip = { -1700, 100, -100, 900 };
        DragNative<NativeOps> drag(native);
        Check(drag.Initialize(view), "deferred app drag retains its native clip");
        Adapter dragging;
        for (unsigned packet = 0; packet < 5; ++packet)
        {
            const double before = world.physical.x;
            const auto now = std::uint64_t{ 100 } + packet * 4;
            Check(dragging.Move({ before + 16, 500 }, false, now, view, true, false, drag) && dragging.Pending(), "each forwarded drag move owns one deferred correction");
            Check(!dragging.FeedbackSnapshot().valid && ApplyPending(), "app drag has no speculative cursor feedback before native delivery");
            (void)dragging.BypassEcho({ before + 4, 500 }, now + 1);
            (void)dragging.ServicePending(now + 2, view, true, false, drag);
            Check(!dragging.Pending() && !dragging.Faulted() && Same(dragging.FeedbackSnapshot().point, Vec2{ before + 4, 500 }), "verified deferred app drag keeps the baseline continuous across packets");
        }
        Check(world.writes == 5 && world.physical.x == -1480 && world.clip.left == -1700 && world.clip.right == -100, "drag continuity issues one write per packet without replacing the application's clip");

        Reset();
        world.deferWrite = true;
        world.clip = { -1700, 100, -100, 900 };
        DragNative<NativeOps> originalClip(native);
        Adapter changedClip;
        Check(originalClip.Initialize(view) && changedClip.Move({ -1484, 500 }, false, 100, view, true, false, originalClip) && changedClip.Pending(), "rebuilt drag-clip fixture has a pending write under the original clip");
        world.clip.left = -1699;
        DragNative<NativeOps> replacementClip(native);
        Check(replacementClip.Initialize(view), "current application clip is independently eligible for a new transaction");
        Check(ApplyPending(), "original drag write can be delivered under a changed application clip");
        (void)changedClip.ServicePending(101, view, true, false, replacementClip);
        Check(!changedClip.Pending() && !changedClip.Faulted() && !changedClip.FeedbackSnapshot().valid && world.writes == 1, "a rebuilt drag wrapper cannot acknowledge a proposal made under a different application clip");
    }

    void DeferredDispatchPreservesDragRelease()
    {
        Reset();
        world.deferWrite = true;
        NativeOps native;
        Adapter ordinary, dragging;
        const auto view = Selected();
        input::PointerSnapshot policy{ view.requested, view.active, view.bounds, view.zoom, view.epoch, view.offset, view.edgePanMultiplier, view.speedMultiplier, view.adjustForZoom };
        input::routing::Snapshot snapshot;
        snapshot.enabled = true;
        snapshot.generation = 1;
        snapshot.epoch = view.epoch;
        snapshot.activeId = 1;
        snapshot.count = 1;
        snapshot.routes[0] = { 1, view.bounds, { -1700, 300, -1220, 570 }, true };
        input::routing::State routing;
        routing.Observe(snapshot, true, view.epoch);
        routing.ObservePoint({ -1500, 500 });
        Check(routing.ZoomReady(), "composed pending fixture starts with a verified source/destination route");
        Check(input::DispatchPointer({ -1484, 500 }, 100, policy, view.epoch, true, true, true, false, routing, ordinary, dragging, native) && ordinary.Pending() && !dragging.Pending(), "ordinary production dispatch consumes only its own deferred pointer proposal");
        Check(ApplyPending(), "ordinary proposal is delivered before the application drag begins");
        input::ServicePointerPending(101, policy, view.epoch, true, true, true, false, routing, ordinary, dragging, native);
        Check(!ordinary.Pending() && !ordinary.Faulted(), "ordinary correction is confirmed before the button transition");

        routing.ObservePoint({ -1496, 500 });
        Check(!routing.Button(0, true, false) && routing.HasForwardedDrag(), "the application owns its complete physical drag pair");
        ordinary.Pause();
        dragging.EndConstraint();
        world.clip = { -1700, 100, -100, 900 };
        Check(input::DispatchPointer({ -1480, 500 }, 102, policy, view.epoch, true, true, true, false, routing, ordinary, dragging, native) && dragging.Pending() && !ordinary.Pending(), "forwarded dispatch switches to the real clip-respecting native adapter");
        Check(!routing.Button(0, false, false) && !routing.HasForwardedDrag() && !routing.AnyButtonDown(), "physical application UP is forwarded while its cursor write is still pending");
        input::ServicePointerPending(103, policy, view.epoch, true, true, true, false, routing, ordinary, dragging, native);
        Check(!dragging.Pending(), "shared pending service revokes the drag proposal as soon as release changes pointer ownership");
        dragging.EndConstraint();
        Check(!dragging.Pending() && !dragging.Faulted(), "ending the forwarded pair cancels unverified drag feedback without waiting for native delivery");
        Check(ApplyPending(), "the already accepted drag setter can arrive after physical UP");
        (void)dragging.BypassEcho({ -1492, 500 }, 103);
        (void)dragging.ServicePending(104, view, true, false, native);
        Check(!dragging.FeedbackSnapshot().valid && !dragging.Faulted() && world.writes == 2, "late drag delivery cannot resurrect released ownership, feedback or an additional gain application");

        for (const bool forwarded : { false, true })
        {
            Reset();
            world.deferWrite = true;
            Adapter committedOrdinary, committedDrag;
            input::routing::State committedRouting;
            committedRouting.Observe(snapshot, true, view.epoch);
            committedRouting.ObservePoint({ -1500, 500 });
            if (forwarded)
            {
                Check(!committedRouting.Button(0, true, false) && committedRouting.HasForwardedDrag(), "committed real drag packet starts with the application's forwarded DOWN");
                world.clip = { -1700, 100, -100, 900 };
            }
            auto& active = forwarded ? committedDrag : committedOrdinary;
            Check(input::DispatchPointer({ -1484, 500 }, 100, policy, view.epoch, true, true, true, false, committedRouting, committedOrdinary, committedDrag, native) && active.Pending(), "shared dispatch starts the native proposal for ordinary or forwarded motion");
            world.physical = { -1480, 500 };
            committedRouting.ObservePoint({ -1480, 500 });
            Check(!input::DispatchPointer({ -1480, 500 }, 101, policy, view.epoch, true, true, true, false, committedRouting, committedOrdinary, committedDrag, native), "production dispatch forwards an already-committed real candidate instead of labeling it foreign interference");
            Check(!active.Pending() && !active.Faulted() && !active.FeedbackSnapshot().valid && Same(active.Remainder(), Vec2{}) && active.PendingCounts().interrupted == 1 && world.writes == 1, "ordinary and forwarded paths both cancel without a second write, fraction or native-failure latch");
            if (forwarded)
                Check(!committedRouting.Button(0, false, false) && !committedRouting.AnyButtonDown(), "interruption by committed real movement still forwards the application's owed UP");
        }

        for (unsigned gate = 0; gate < 7; ++gate)
        {
            Reset();
            world.deferWrite = true;
            Adapter gatedOrdinary, gatedDrag;
            input::routing::State gatedRouting;
            auto current = policy;
            auto currentRoute = snapshot;
            gatedRouting.Observe(currentRoute, true, view.epoch);
            gatedRouting.ObservePoint({ -1500, 500 });
            Check(input::DispatchPointer({ -1484, 500 }, 100, current, view.epoch, true, true, true, false, gatedRouting, gatedOrdinary, gatedDrag, native) && gatedOrdinary.Pending(), "shared-service gate fixture starts through production dispatch");
            bool coherent = true, enabled = true, inputGate = true, gesture = false;
            switch (gate)
            {
            case 0:
                coherent = false;
                break;
            case 1:
                enabled = false;
                break;
            case 2:
                inputGate = false;
                break;
            case 3:
                gesture = true;
                break;
            case 4:
                ++current.epoch;
                break;
            case 5:
                currentRoute.busy = true;
                ++currentRoute.generation;
                gatedRouting.Observe(currentRoute, true, view.epoch);
                break;
            case 6:
                Check(!gatedRouting.Button(0, true, false) && gatedRouting.HasForwardedDrag(), "a forwarded DOWN can change ownership before the ordinary acknowledgement");
                break;
            }
            Check(ApplyPending(), "native delivery alone cannot override a revoked shared-service gate");
            input::ServicePointerPending(101, current, view.epoch, coherent, enabled, inputGate, gesture, gatedRouting, gatedOrdinary, gatedDrag, native);
            Check(!gatedOrdinary.Pending() && !gatedOrdinary.Faulted() && !gatedOrdinary.FeedbackSnapshot().valid && !gatedDrag.Faulted() && world.writes == 1, "shared service cancels stale coherence, enable, heartbeat, gesture, epoch, busy-route or changed pointer ownership without writing");
            if (gate == 6)
                Check(!gatedRouting.Button(0, false, false) && !gatedRouting.AnyButtonDown(), "canceling a pending ordinary transaction never consumes the application's later drag UP");
        }
    }
}

DWORD GetLastError() noexcept
{
    return world.error;
}
DWORD GetCurrentThreadId() noexcept
{
    return 42;
}
HDESK GetThreadDesktop(DWORD) noexcept
{
    return &world.thread;
}
HDESK OpenInputDesktop(DWORD, BOOL, DWORD) noexcept
{
    ++world.opens;
    if (!world.openOk)
    {
        world.error = ERROR_ACCESS_DENIED;
        return nullptr;
    }
    return &world.input;
}
BOOL GetUserObjectInformationW(HDESK desktop, int, void* output, DWORD capacity, DWORD* needed) noexcept
{
    const bool ok = desktop == &world.thread ? world.threadNameOk : world.inputNameOk;
    if (!ok)
    {
        world.error = ERROR_ACCESS_DENIED;
        return FALSE;
    }
    const auto length = (std::wcslen(desktop->name) + 1) * sizeof(wchar_t);
    *needed = static_cast<DWORD>(length);
    if (capacity < length)
    {
        world.error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    std::wmemcpy(static_cast<wchar_t*>(output), desktop->name, length / sizeof(wchar_t));
    return TRUE;
}
BOOL CloseDesktop(HDESK) noexcept
{
    ++world.closes;
    return TRUE;
}
DPI_AWARENESS_CONTEXT GetThreadDpiAwarenessContext() noexcept
{
    return world.awareness;
}
BOOL AreDpiAwarenessContextsEqual(DPI_AWARENESS_CONTEXT a, DPI_AWARENESS_CONTEXT b) noexcept
{
    return a == b;
}
BOOL GetClipCursor(RECT* output) noexcept
{
    ++world.clipReads;
    if (!world.clipOk || world.failClipAt == world.clipReads)
    {
        world.error = ERROR_INVALID_PARAMETER;
        return FALSE;
    }
    *output = world.clip;
    return TRUE;
}
int GetSystemMetrics(int metric) noexcept
{
    switch (metric)
    {
    case SM_XVIRTUALSCREEN:
        return world.desktop.left;
    case SM_YVIRTUALSCREEN:
        return world.desktop.top;
    case SM_CXVIRTUALSCREEN:
        return world.desktop.right - world.desktop.left;
    case SM_CYVIRTUALSCREEN:
        return world.desktop.bottom - world.desktop.top;
    default:
        return 0;
    }
}
HMONITOR MonitorFromRect(const RECT* selected, DWORD) noexcept
{
    ++world.monitorQueries;
    if (!world.monitorFound)
        return nullptr;
    for (auto& monitor : world.monitors)
        if (selected->left < monitor.bounds.right && selected->right > monitor.bounds.left &&
            selected->top < monitor.bounds.bottom && selected->bottom > monitor.bounds.top)
            return &monitor;
    return nullptr;
}
BOOL GetMonitorInfoW(HMONITOR monitor, MONITORINFO* output) noexcept
{
    if (!world.monitorInfoOk || output->cbSize != sizeof(MONITORINFO))
    {
        world.error = ERROR_GEN_FAILURE;
        return FALSE;
    }
    output->rcMonitor = output->rcWork = monitor->bounds;
    return TRUE;
}
BOOL GetPhysicalCursorPos(POINT* output) noexcept
{
    ++world.physicalReads;
    if (world.failPhysicalAt == world.physicalReads)
    {
        world.error = ERROR_DEVICE_NOT_CONNECTED;
        return FALSE;
    }
    *output = world.physicalGetterMap ? world.physicalGetterMap(world.physical, world.inputSource, world.inputDestination) : world.physical;
    return TRUE;
}
BOOL GetCursorPos(POINT* output) noexcept
{
    ++world.logicalReads;
    if (!world.logicalOk)
    {
        world.error = ERROR_GEN_FAILURE;
        return FALSE;
    }
    *output = world.logical;
    return TRUE;
}
HMONITOR MonitorFromPoint(POINT point, DWORD) noexcept
{
    ++world.pointQueries;
    for (auto& monitor : world.monitors)
        if (point.x >= monitor.bounds.left && point.x < monitor.bounds.right && point.y >= monitor.bounds.top && point.y < monitor.bounds.bottom)
            return &monitor;
    return nullptr;
}
BOOL SetPhysicalCursorPos(int x, int y) noexcept
{
    ++world.writes;
    world.requested = { x, y };
    if (!world.warpOk)
    {
        world.error = world.warpError;
        return FALSE;
    }
    const auto applied = world.setterMap ? world.setterMap(world.requested, world.inputSource, world.inputDestination) : world.requested;
    if (world.deferWrite)
    {
        world.pending = applied;
        world.pendingWrite = true;
    }
    else
        world.physical = applied;
    return TRUE;
}

int main()
{
    DesktopDpiAndMonitorEligibility();
    OrdinaryAndDragClipContracts();
    PhysicalApiCoordinatesAndPlacement();
    FailureObservationAndAdapterTransactions();
    DeferredNativeWriteAcknowledgement();
    DeferredDispatchPreservesDragRelease();
    std::cout << "PASS: " << assertions << " production NativeOps boundary assertions\n";
    return 0;
}
