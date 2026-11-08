#include <stdexcept>
#include "../GlideZoom/cursor_visibility.h"
#include "../GlideZoom/gpu_queue_policy.h"
#include "../GlideZoom/monitor_geometry.h"
#include "../GlideZoom/cursor_coverage.h"

#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <type_traits>
#include <vector>

namespace {
static_assert(!std::is_copy_constructible<glide::cursor::Lease>::value &&
              !std::is_copy_assignable<glide::cursor::Lease>::value,
              "cursor recovery responsibility cannot be copied into a second owner");
std::uint64_t assertions = 0, scenarios = 0, failures = 0, crashCuts = 0;
void Check(bool condition, const char* message) {
    ++assertions;
    if (!condition) {
        std::cerr << "FAIL cursor visibility: " << message << " [scenario=" << scenarios << "]\n";
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}

enum class Point { DesktopBefore, DesktopAfter, InfoBefore, InfoAfter, ArmBefore, ArmAfter, HideBefore, HideAfter,
                   ShowBefore, ShowAfter, ClearBefore, ClearAfter };
enum class Failure { None, Before, After };
struct Snapshot { bool armed, shown, desktop; Point point; };

// These are the production policy's native/journal boundaries. The mock owns a
// cursor independently of Lease's state, and can fail before OR after changing
// that cursor. Assertions check effects and publication order at those calls.
struct World {
    bool shown = true, suppressed = false, desktop = true, armed = false;
    bool infoFails = false, armFails = false;
    Failure hideFailure = Failure::None, showFailure = Failure::None;
    unsigned hideFailureAt = 1, showFailureAt = 1;
    unsigned desktopSwitchAt = 0, desktopReads = 0;
    unsigned infoReads = 0, publications = 0, clears = 0, hides = 0, shows = 0;
    unsigned reached = 0;
    bool capture = true, hookFired = false;
    Point hookPoint = Point::InfoBefore;
    std::function<void()> hook;
    std::vector<Snapshot> cuts;

    void Observe(Point point) {
        if (capture) cuts.push_back({armed, shown, desktop, point});
        if (hook && !hookFired && point == hookPoint) {
            hookFired = true;
            hook();
        }
    }
    bool OnInputDesktop() {
        Observe(Point::DesktopBefore);
        if (++desktopReads == desktopSwitchAt) { desktop = false; ++reached; }
        Observe(Point::DesktopAfter);
        return desktop;
    }
    bool CursorShowing(bool& output) {
        ++infoReads;
        Observe(Point::InfoBefore);
        if (infoFails) { ++reached; return false; }
        output = shown && !suppressed;
        Observe(Point::InfoAfter);
        return true;
    }
    bool ArmCursorRecovery() {
        Observe(Point::ArmBefore);
        if (armFails) { ++reached; return false; }
        Check(!armed, "one publication per acquire; no nested hide ownership");
        armed = true;
        ++publications;
        Observe(Point::ArmAfter);
        return true;
    }
    void ClearCursorRecovery() {
        Observe(Point::ClearBefore);
        Check(armed && shown, "journal clears only after ordinary cursor is visible");
        armed = false;
        ++clears;
        Observe(Point::ClearAfter);
    }
    bool ShowSystemCursor(bool show) {
        Check(armed, "every native cursor write has an already durable recovery bit");
        // This check represents the native adapter's second desktop gate.
        if (!desktop) return false;
        const unsigned ordinal = show ? ++shows : ++hides;
        const Failure failure = show ? showFailure : hideFailure;
        const unsigned failureAt = show ? showFailureAt : hideFailureAt;
        Observe(show ? Point::ShowBefore : Point::HideBefore);
        if (failure == Failure::Before && ordinal == failureAt) { ++reached; return false; }
        shown = show;
        Observe(show ? Point::ShowAfter : Point::HideAfter);
        if (failure == Failure::After && ordinal == failureAt) { ++reached; return false; }
        return true;
    }
    void Healthy() {
        desktop = true;
        infoFails = armFails = false;
        hideFailure = showFailure = Failure::None;
        desktopSwitchAt = 0;
        hook = {};
    }
};

// Snapshots are taken directly from the running production code's boundaries,
// not from a second state machine. Treat the owner as terminated at each cut,
// then execute the same recovery routine the guardian uses.
void CrashMatrix(const std::vector<Snapshot>& cuts) {
    for (const auto cut : cuts) {
        ++crashCuts;
        World recovery;
        recovery.capture = false;
        recovery.armed = cut.armed;
        recovery.shown = cut.shown;
        recovery.desktop = cut.desktop;
        const bool restored = glide::cursor::Recover(cut.armed, recovery, recovery);
        if (!cut.armed) {
            Check(restored && recovery.shown == cut.shown && !recovery.armed &&
                  recovery.shows == 0 && recovery.desktopReads == 0,
                  "unarmed guardian leaves a pre-existing hidden or visible cursor untouched");
        } else if (!cut.desktop) {
            Check(!restored && recovery.shown == cut.shown && recovery.armed &&
                  recovery.shows == 0 && recovery.clears == 0,
                  "guardian retains recovery without touching another desktop");
            recovery.desktop = true;
            Check(glide::cursor::Recover(true, recovery, recovery),
                  "guardian resumes restoration on its original desktop");
            Check(recovery.shown && !recovery.armed && recovery.shows == 1,
                  "resumed guardian restores one ordinary cursor");
        } else {
            Check(restored && recovery.shown && !recovery.armed &&
                  recovery.shows == 1 && recovery.clears == 1 && recovery.hides == 0,
                  "every armed crash cut recovers visibly, including a death before hide");
        }
    }
}

void Cleanup(glide::cursor::Lease& lease, World& world) {
    const bool needed = lease.NeedsRecovery();
    const bool before = world.shown;
    world.Healthy();
    Check(lease.Release(world, world), "healthy cleanup always completes");
    Check(!lease.NeedsRecovery() && !lease.Hidden() && !world.armed,
          "cleanup clears local ownership and durable recovery together");
    Check(needed ? world.shown : world.shown == before,
          "cleanup restores only a cursor this lease may have hidden");
    const unsigned writes = world.hides + world.shows, reads = world.desktopReads;
    Check(lease.Release(world, world) && world.hides + world.shows == writes &&
          world.desktopReads == reads, "repeated release performs no unowned native operations");
    failures += world.reached;
    CrashMatrix(world.cuts);
}

void BaselineMatrix() {
    for (bool shown : {false, true}) for (bool suppressed : {false, true})
    for (bool desktop : {false, true}) for (bool wanted : {false, true})
    for (bool infoFails : {false, true}) for (bool armFails : {false, true}) {
        ++scenarios;
        World world;
        world.shown = shown; world.suppressed = suppressed; world.desktop = desktop;
        world.infoFails = infoFails; world.armFails = armFails;
        glide::cursor::Lease lease;
        const bool result = lease.Update(wanted, world, world);
        const bool acquire = wanted && desktop && !infoFails && shown && !suppressed && !armFails;
        Check(lease.NeedsRecovery() == acquire && world.armed == acquire &&
              lease.Hidden() == acquire && world.hides == (acquire ? 1u : 0u),
              "acquisition requires request, original desktop, readable visible baseline and durable journal");
        Check(world.shown == (acquire ? false : shown),
              "all excluded baselines remain exactly as the environment supplied them");
        if (acquire) {
            Check(result, "valid acquisition succeeds");
            const unsigned calls = world.infoReads + world.hides + world.shows + world.publications;
            for (unsigned i = 0; i < 120; ++i) Check(lease.Update(true, world, world), "steady active view stays owned");
            Check(world.infoReads + world.hides + world.shows + world.publications == calls,
                  "steady frames do not repeat cursor queries, publication or visibility writes");
        }
        Cleanup(lease, world);
    }
}

void FailureMatrix() {
    for (Failure hide : {Failure::None, Failure::Before, Failure::After})
    for (Failure show : {Failure::None, Failure::Before, Failure::After})
    for (unsigned switchAt = 0; switchAt < 10; ++switchAt) {
        ++scenarios;
        World world;
        world.hideFailure = hide; world.showFailure = show; world.desktopSwitchAt = switchAt;
        glide::cursor::Lease lease;
        const bool acquired = lease.Update(true, world, world);
        Check(lease.NeedsRecovery() == world.armed, "failed acquire retains exactly the durable cleanup responsibility");
        if (acquired) Check(lease.Hidden() && !world.shown, "successful acquire has really hidden the mock native cursor");
        const bool released = lease.Release(world, world);
        Check(lease.NeedsRecovery() == world.armed, "failed release cannot lose its recovery journal");
        if (released) Check(!lease.NeedsRecovery(), "reported release success has no pending effect");
        if (world.hides && !released) Check(world.armed, "uncertain show or desktop loss remains recoverable");
        Cleanup(lease, world);
    }
    // A retry after an ambiguous TRUE call first finishes restoration and does
    // not conceal that failure by immediately hiding the cursor again.
    for (Failure failure : {Failure::Before, Failure::After}) {
        ++scenarios;
        World world;
        glide::cursor::Lease lease;
        Check(lease.Update(true, world, world), "uncertain restore fixture acquires");
        world.showFailure = failure;
        Check(!lease.Release(world, world) && lease.NeedsRecovery(), "ambiguous restore is retained");
        world.showFailure = Failure::None;
        Check(!lease.Update(true, world, world) && !lease.NeedsRecovery() && world.shown && world.hides == 1,
              "pending restore completes before the caller may initiate another acquire");
        Cleanup(lease, world);
    }
}

void ReentryMatrix() {
    for (Point point : {Point::DesktopBefore, Point::DesktopAfter,
                        Point::InfoBefore, Point::InfoAfter, Point::ArmBefore,
                        Point::ArmAfter, Point::HideBefore, Point::HideAfter}) {
        ++scenarios;
        World world;
        glide::cursor::Lease lease;
        world.hookPoint = point;
        world.hook = [&] {
            Check(!lease.Release(world, world), "nested release queues cancellation until the native boundary returns");
            Check(!lease.Update(true, world, world), "nested acquire cannot overwrite pending cancellation");
        };
        Check(!lease.Update(true, world, world) && world.hookFired,
              "in-flight acquire reports cancellation from every reachable publication/write boundary");
        Check(world.shown && !lease.NeedsRecovery() && !world.armed,
              "nested reset wins even if the interrupted native hide takes effect afterwards");
        Cleanup(lease, world);
    }
    for (Point point : {Point::DesktopBefore, Point::DesktopAfter}) {
        ++scenarios;
        World world;
        glide::cursor::Lease lease;
        Check(lease.Update(true, world, world), "owned desktop-query reentry fixture acquires");
        world.hookPoint = point;
        world.hook = [&] {
            Check(!lease.Release(world, world), "nested reset at the first desktop query requests release");
        };
        Check(!lease.Update(true, world, world) && world.hookFired && world.shown &&
              !lease.NeedsRecovery() && !world.armed && world.hides == 1 && world.shows == 1,
              "reset from initial query wins over a stale wantHidden and cannot acquire again");
        Cleanup(lease, world);
    }
    for (Point point : {Point::ShowBefore, Point::ShowAfter, Point::ClearBefore, Point::ClearAfter}) {
        ++scenarios;
        World world;
        glide::cursor::Lease lease;
        Check(lease.Update(true, world, world), "release reentry fixture acquires");
        world.hookPoint = point;
        world.hook = [&] {
            Check(!lease.Release(world, world), "nested release cannot create a second native writer");
            Check(!lease.Update(true, world, world), "nested acquire cannot replace a completing restoration");
        };
        Check(lease.Release(world, world) && world.hookFired && world.shown &&
              world.shows == 1 && world.hides == 1 && !world.armed,
              "restoration completes with one native show despite reentry");
        Cleanup(lease, world);
    }
    // Secure-desktop loss after a completed hide or show must not discard the
    // owner's original-desktop journal, even when the native call succeeded.
    for (Point point : {Point::HideAfter, Point::ShowAfter}) {
        ++scenarios;
        World world;
        glide::cursor::Lease lease;
        if (point == Point::ShowAfter) Check(lease.Update(true, world, world), "desktop-loss fixture acquires");
        world.hookPoint = point;
        world.hook = [&] { world.desktop = false; };
        if (point == Point::HideAfter) lease.Update(true, world, world);
        else Check(!lease.Release(world, world), "post-show desktop loss defers journal clear");
        Check(world.hookFired && lease.NeedsRecovery() && world.armed,
              "desktop loss after native effect retains cleanup responsibility");
        const unsigned writes = world.hides + world.shows;
        Check(!lease.Release(world, world) && world.hides + world.shows == writes,
              "restoration never writes on a known foreign desktop");
        Cleanup(lease, world);
    }
}

void GuardianFailures() {
    for (Failure failure : {Failure::None, Failure::Before, Failure::After})
    for (unsigned switchAt = 0; switchAt < 4; ++switchAt)
    for (bool shown : {false, true}) {
        ++scenarios;
        World world;
        world.armed = true; world.shown = shown; world.showFailure = failure;
        world.desktopSwitchAt = switchAt;
        const bool complete = glide::cursor::Recover(true, world, world);
        Check(complete ? !world.armed && world.shown : world.armed,
              "guardian clears only a successful show completed on its original desktop");
        world.Healthy();
        Check(glide::cursor::Recover(world.armed, world, world) && !world.armed && world.shown,
              "guardian retry repairs pre-effect and post-effect failures");
        Check(world.hides == 0, "guardian never hides a cursor");
        failures += world.reached;
        CrashMatrix(world.cuts);
    }
}

// The first image may stay on screen while DWM acknowledges its presentation.
// Cursor coverage still changes independently when the user leaves that monitor.
// Exercise the real queue, projection math and cursor ownership policy together;
// this models the pending App::Apply boundary, not a native Windows GPU run.
void PendingPresentationCursorCoverage() {
    using glide::gpu::QueueDecision;
    using glide::gpu::QueueSignal;
    const glide::Rect bounds{-1920, -1080, 0, 0};
    const glide::Vec2 inside{-960, -540};
    glide::monitor::Frame displayed;
    Check(glide::monitor::MakeFrame(bounds, glide::AnchorZoom({}, 2, inside, bounds), displayed),
          "pending cursor fixture has a valid negative-coordinate monitor frame");
    const auto covered = [&](glide::Vec2 source) {
        return glide::cursor::HotspotVisible(displayed, source);
    };
    const glide::Vec2 neighbors[]{{-1921, -540}, {1, -540}, {-960, -1081}, {-960, 1}};
    for (const auto outside : neighbors)
    for (const std::uint64_t delay : {std::uint64_t{101}, std::uint64_t{1000}, std::uint64_t{1999}})
    for (const Failure failure : {Failure::None, Failure::Before, Failure::After}) {
        ++scenarios;
        glide::gpu::PresentationQueue queue;
        Check(queue.Observe(QueueSignal::Ready, 0) == QueueDecision::Render && queue.Submitted(1),
              "first cursor-bearing image is submitted before the delayed compositor acknowledgement");
        World world;
        glide::cursor::Lease lease;
        Check(covered(inside) && !covered(outside) && lease.Update(covered(inside), world, world),
              "only a pointer covered by the displayed image hides the ordinary cursor");
        Check(queue.Observe(QueueSignal::Busy, 1 + delay) == QueueDecision::Pending &&
              queue.Cold() && queue.HasSubmittedFrame() && queue.WaitMilliseconds() == 0,
              "an unacknowledged first image retains nonblocking startup rather than failing at 100 ms");
        Check(lease.Update(covered(inside), world, world) && lease.Hidden() && world.hides == 1,
              "a pending frame does not duplicate the hide while the pointer stays covered");

        world.showFailure = failure;
        const bool released = lease.Update(covered(outside), world, world);
        Check(released == (failure == Failure::None) && !lease.Hidden() && world.shows == 1 && world.hides == 1,
              "moving across any monitor edge attempts cursor restoration while presentation is still pending");
        Check(world.armed == (failure != Failure::None) && lease.NeedsRecovery() == world.armed &&
              world.shown == (failure != Failure::Before),
              "pre-effect and post-effect show failures retain their real recovery responsibility");

        world.showFailure = Failure::None;
        Check(queue.Observe(QueueSignal::Busy, 1 + delay) == QueueDecision::Pending &&
              lease.Update(covered(outside), world, world) && world.shown && !world.armed &&
              !lease.NeedsRecovery() && world.hides == 1,
              "another pending tick completes recovery and never re-hides a cursor on another monitor");
        const unsigned writes = world.shows + world.hides;
        Check(queue.Observe(QueueSignal::Ready, 2000) == QueueDecision::Render && queue.Submitted(2000) &&
              !queue.Cold() && lease.Update(covered(outside), world, world) &&
              world.shows + world.hides == writes && world.shown,
              "compositor readiness cannot hide a cursor whose position is still outside the displayed image");
        Cleanup(lease, world);
    }
}

void SubmittedCursorGeometryAndOtherMonitors()
{
    const glide::Rect monitors[]{
        { -1920, -1080, 0, 0 }, { 0, 0, 1920, 1080 }, { 1920, -360, 4480, 1080 }, { -1200, 1080, 0, 3000 }
    };
    for (const auto bounds : monitors)
    {
        ++scenarios;
        const glide::Vec2 center{ (bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2 };
        glide::monitor::Frame displayed, prepared;
        Check(glide::monitor::MakeFrame(bounds, glide::AnchorZoom({}, 2, center, bounds), displayed) &&
                  glide::monitor::MakeFrame(bounds, glide::AnchorZoom({}, 3, center, bounds), prepared),
              "cursor fixtures cover negative origins, vertically arranged and differently sized physical monitors");
        glide::cursor::Presentation cursor;
        World world;
        glide::cursor::Lease lease;
        Check(!cursor.Covers(displayed, center, 11, false), "an unsubmitted cursor cannot justify global hiding");
        cursor.Commit(displayed, 11);
        Check(cursor.Covers(displayed, center, 11, false) &&
                  lease.Update(cursor.Covers(displayed, center, 11, false), world, world) && lease.Hidden(),
              "a visible submitted cursor and matching geometry acquire one journaled hide");
        Check(!cursor.Covers(prepared, center, 11, false) &&
                  !cursor.Covers(displayed, center, 12, false) &&
                  !cursor.Covers(displayed, center, 11, true),
              "prepared-but-unsubmitted geometry, changed shapes and pen suppression revoke replacement eligibility");
        for (const auto other : monitors)
        {
            if (other.left == bounds.left && other.top == bounds.top)
                continue;
            const glide::Vec2 outside{ (other.left + other.right) / 2, (other.top + other.bottom) / 2 };
            Check(!cursor.Covers(displayed, outside, 11, false),
                  "a cursor sample on a different physical monitor cannot keep the native cursor hidden");
        }
        const glide::Vec2 outside{ bounds.right, center.y };
        Check(lease.Update(cursor.Covers(displayed, outside, 11, false), world, world) &&
                  world.shown && !lease.Hidden() && !lease.NeedsRecovery() && !world.armed,
              "crossing the half-open monitor edge restores the ordinary cursor without waiting for another GPU image");
        cursor.Commit(prepared, 12);
        Check(cursor.Covers(prepared, center, 12, false) && !cursor.Covers(displayed, center, 11, false),
              "only successful submission can replace the committed geometry and cursor shape");
        cursor.Clear();
        Check(!cursor.Covers(prepared, center, 12, false), "hide, reset and lost HWND revoke committed cursor coverage");
        Cleanup(lease, world);
    }
}

void CoverageLossDuringNativeHide()
{
    const glide::Rect bounds{ 0, 0, 1920, 1080 };
    const glide::Vec2 inside{ 960, 540 }, outside{ 1920, 540 };
    glide::monitor::Frame frame;
    Check(glide::monitor::MakeFrame(bounds, glide::AnchorZoom({}, 2, inside, bounds), frame),
          "native hide-race fixture has a valid submitted image");
    struct CoverageWorld : World
    {
        glide::cursor::Presentation cursor;
        glide::monitor::Frame frame{};
        glide::Vec2 physical{};
        bool coverageLost = false, epochCurrent = true;
        bool Covered() const noexcept { return cursor.Covers(frame, physical, 11, suppressed); }
        bool CursorShowing(bool& showing)
        {
            return epochCurrent && World::CursorShowing(showing) && epochCurrent;
        }
        bool ShowSystemCursor(bool show)
        {
            if (!show && epochCurrent && !Covered())
            {
                coverageLost = true;
                return false;
            }
            if ((!show && !epochCurrent) || !World::ShowSystemCursor(show))
                return false;
            if (!show && !Covered())
            {
                coverageLost = true;
                return false;
            }
            return show || epochCurrent;
        }
    };
    const auto initialize = [&](CoverageWorld& world) {
        world.frame = frame;
        world.physical = inside;
        world.cursor.Commit(frame, 11);
    };
    const auto recovered = [](bool result, const CoverageWorld& world, const glide::cursor::Lease& lease) {
        return result || (world.coverageLost && !lease.NeedsRecovery() && !lease.Hidden());
    };
    {
        ++scenarios;
        CoverageWorld world;
        initialize(world);
        glide::cursor::Lease lease;
        const bool acquired = lease.Update(world.Covered(), world, world);
        Check(acquired && recovered(acquired, world, lease) && lease.Hidden() && !world.shown &&
                  world.armed && world.shows == 0 && world.Covered(),
              "first covered hide remains successful even though the ordinary cursor flags are now hidden");
        const bool retained = lease.Update(world.Covered(), world, world);
        Check(retained && recovered(retained, world, lease) && world.hides == 1 && world.shows == 0,
              "unchanged submitted coverage retains the first hide without a show/hide bootstrap loop");
        Cleanup(lease, world);
    }
    for (const Point boundary : { Point::InfoAfter, Point::ArmAfter, Point::HideAfter })
        for (const Failure restoreFailure : { Failure::None, Failure::Before, Failure::After })
        {
            ++scenarios;
            CoverageWorld world;
            initialize(world);
            world.showFailure = restoreFailure;
            world.hookPoint = boundary;
            world.hook = [&] { world.physical = outside; };
            glide::cursor::Lease lease;
            const bool result = lease.Update(true, world, world);
            Check(!result && world.hookFired && world.coverageLost,
                  "a cursor crossing after the caller's predicate cancels a late native hide");
            const bool healthy = recovered(result, world, lease);
            Check(healthy == (restoreFailure == Failure::None),
                  "the same-frame main-loop result accepts natural coverage loss only after complete recovery");
            Check(!lease.Hidden() && world.armed == lease.NeedsRecovery() &&
                      lease.NeedsRecovery() == (restoreFailure != Failure::None) && world.shows == 1 &&
                      world.hides == (boundary == Point::HideAfter ? 1u : 0u),
                  "failed cursor restoration remains unhealthy and journaled even if a failed call already showed it");
            if (healthy)
                Check(world.shown && !world.armed,
                      "a recovered monitor crossing already has a visible ordinary cursor in this frame");
            Cleanup(lease, world);
        }
    for (const Failure hideFailure : { Failure::Before, Failure::After })
    {
        ++scenarios;
        CoverageWorld world;
        initialize(world);
        world.hideFailure = hideFailure;
        glide::cursor::Lease lease;
        const bool result = lease.Update(true, world, world);
        Check(!result && !world.coverageLost && !recovered(result, world, lease) &&
                  world.shown && !lease.NeedsRecovery() && !world.armed,
              "a genuine native hide failure is not mislabeled a healthy monitor crossing after compensation");
        Cleanup(lease, world);
    }
    for (const Point boundary : { Point::InfoAfter, Point::ArmAfter, Point::HideAfter })
        for (const Failure releaseFailure : { Failure::None, Failure::Before, Failure::After })
        {
            ++scenarios;
            CoverageWorld world;
            initialize(world);
            world.showFailure = Failure::After;
            world.hookPoint = boundary;
            world.hook = [&] { world.epochCurrent = false; };
            glide::cursor::Lease lease;
            Check(!lease.Update(true, world, world) && !world.epochCurrent && !world.coverageLost,
                  "epoch cancellation never masquerades as ordinary coverage loss or resurrects a hide");
            world.showFailure = releaseFailure;
            world.showFailureAt = world.shows + 1;
            const bool released = lease.Release(world, world);
            Check(released ? (world.shown && !lease.Hidden() && !lease.NeedsRecovery() && !world.armed) :
                             (lease.NeedsRecovery() && world.armed),
                  "the canceled-epoch path is healthy only when its explicit Release completes restoration");
            Cleanup(lease, world);
        }
}
std::uint64_t Next(std::uint64_t& seed)
{
    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
    return seed;
}
void MixedSequences() {
    std::uint64_t seed = 0xc1b57a3f0836d9e1ULL;
    for (unsigned sequence = 0; sequence < 1000; ++sequence) {
        ++scenarios;
        World world;
        glide::cursor::Lease lease;
        for (unsigned step = 0; step < 50; ++step) {
            const auto value = Next(seed);
            world.Healthy();
            world.desktop = (value & 7) != 0;
            world.infoFails = (value & 127) == 1;
            world.armFails = (value & 127) == 2;
            world.hideFailure = (value & 31) == 3 ? Failure::After : Failure::None;
            world.showFailure = (value & 31) == 4 ? Failure::Before : Failure::None;
            world.hideFailureAt = world.hides + 1;
            world.showFailureAt = world.shows + 1;
            // An outside application or touch/pen suppression can change the
            // baseline between independent leases. No owner is then entitled
            // to undo that change just because zoom is paused or stopped.
            if (!lease.NeedsRecovery()) {
                world.shown = (value & 15) != 5;
                world.suppressed = (value & 31) == 6;
            }
            const bool wanted = (value & 3) != 0;
            const bool ownedBefore = lease.NeedsRecovery();
            const bool shownBefore = world.shown;
            const unsigned writesBefore = world.shows + world.hides;
            lease.Update(wanted, world, world);
            Check(lease.NeedsRecovery() == world.armed, "mixed inside/outside events preserve journal equivalence");
            if (!wanted && !ownedBefore)
                Check(world.shown == shownBefore && world.shows + world.hides == writesBefore,
                      "unowned mixed pause/reset cannot change a foreign cursor baseline");
            if (lease.Hidden()) Check(lease.NeedsRecovery() && !world.shown,
                                      "known hidden state corresponds to a real native effect and armed recovery");
        }
        Cleanup(lease, world);
    }
}
} // namespace

int Run_cursor_visibility() {
    BaselineMatrix();
    FailureMatrix();
    ReentryMatrix();
    GuardianFailures();
    PendingPresentationCursorCoverage();
    SubmittedCursorGeometryAndOtherMonitors();
    CoverageLossDuringNativeHide();
    MixedSequences();
    std::cout << "PASS: " << assertions << " cursor visibility assertions; " << scenarios
              << " scenarios; 50000 mixed transitions; " << failures << " reached failures; "
              << crashCuts << " production-boundary crash recovery cuts.\n";
    return 0;
}
