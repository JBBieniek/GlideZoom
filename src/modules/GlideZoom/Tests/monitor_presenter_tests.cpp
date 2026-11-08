#include <stdexcept>
#include "../GlideZoom/monitor_presenter.h"
#include "../GlideZoom/frame_transaction.h"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using glide::monitor::Frame;
using glide::session::Result;
unsigned assertions = 0, scenarios = 0, injected = 0, reachedBoundaries = 0;

void Check(bool condition, const std::string& message) {
    ++assertions;
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
bool Success(Result result) { return result == Result::Applied || result == Result::Unchanged; }

enum class Fault {
    None, PrepareBefore, PrepareAfter, ShowBefore, ShowAfter,
    Read1, Read2, Read3, Read4, InputBefore, InputFalseAfter,
    ForeignBefore, ForeignAfter
};

struct Backend {
    glide::session::Snapshot state{};
    Fault fault = Fault::None;
    unsigned reads = 0, inputWrites = 0, visualWrites = 0;
    bool faultReached = false;
    std::vector<std::string> trace;
    std::function<void()> onRead{};
    void RestartCounters() { reads = inputWrites = visualWrites = 0; faultReached = false; trace.clear(); }
    bool Read(glide::session::Snapshot& result) {
        trace.push_back("read");
        ++reads;
        if (onRead) onRead();
        const unsigned ordinal = fault >= Fault::Read1 && fault <= Fault::Read4 ?
            static_cast<unsigned>(fault) - static_cast<unsigned>(Fault::Read1) + 1 : 0;
        if (ordinal && reads == ordinal) { faultReached = true; return false; }
        result = state;
        return true;
    }
    bool SetVisual(glide::session::Visual visual) {
        ++visualWrites;
        trace.push_back("visual");
        state.visual = visual;
        return true;
    }
    bool SetInput(glide::session::Input input) {
        ++inputWrites;
        trace.push_back("input");
        if (fault == Fault::InputBefore) { faultReached = true; return false; }
        state.input = input;
        if (fault == Fault::InputFalseAfter) { faultReached = true; return false; }
        if (fault == Fault::ForeignAfter) {
            faultReached = true;
            state.visual = {3.0f, 71, -19};
        }
        return true;
    }
};

struct Journal {
    glide::session::RecoveryRecord record{};
    bool armed = false;
    unsigned publications = 0;
    void Publish(const glide::session::RecoveryRecord& next) {
        Check(glide::session::Valid(next), "every published recovery record is valid");
        record = next; armed = true; ++publications;
    }
    void Clear() { armed = false; }
};

struct Renderer {
    Backend& backend;
    Frame prepared{};
    Fault fault = Fault::None;
    unsigned preparations = 0, shows = 0, hides = 0;
    unsigned pendingChecks = 0, retentionChecks = 0;
    bool visible = false, faultReached = false, pending = false, retain = false;
    std::function<void()> onPrepare{}, onPending{}, onRetain{};
    bool Matches(const Frame& next) const {
        return visible && !pending && glide::monitor::SameFrame(prepared, next);
    }
    bool Prepare(const Frame& next) {
        ++preparations;
        backend.trace.push_back("prepare");
        if (onPrepare) onPrepare();
        if (pending) return false;
        if (fault == Fault::PrepareBefore) { faultReached = true; return false; }
        prepared = next;
        if (fault == Fault::PrepareAfter) {
            visible = true; // A partial native effect must still be hidden.
            faultReached = true;
            return false;
        }
        return true;
    }
    bool Pending() {
        ++pendingChecks;
        if (onPending) onPending();
        return pending;
    }
    bool CanRetain(const glide::session::Snapshot& snapshot) {
        ++retentionChecks;
        if (onRetain) onRetain();
        const glide::session::Snapshot displayed{{1, 0, 0},
            {true, prepared.source, prepared.destination}};
        return retain && pending && visible && snapshot == displayed;
    }
    bool Show() {
        ++shows;
        backend.trace.push_back("show");
        Check(backend.state.visual.zoom == 1 && backend.state.input.enabled &&
              backend.state.input.source == prepared.source &&
              backend.state.input.destination == prepared.destination,
              "show occurs only after the matching input-only snapshot is observed");
        if (fault == Fault::ShowBefore) { faultReached = true; return false; }
        visible = true;
        if (fault == Fault::ShowAfter) { faultReached = true; return false; }
        return true;
    }
    void Hide() { ++hides; visible = pending = false; backend.trace.push_back("hide"); }
};

Frame Make(glide::Rect monitor, double zoom, double fraction = 0.5) {
    Frame result;
    const glide::Vec2 anchor{monitor.left + (monitor.right - monitor.left) * fraction,
                             monitor.top + (monitor.bottom - monitor.top) * fraction};
    Check(glide::monitor::MakeFrame(monitor, glide::AnchorZoom({}, zoom, anchor, monitor), result),
          "valid monitor fixture");
    return result;
}

void Run(glide::Rect monitor, double zoom, bool previouslyOwned, Fault fault) {
    ++scenarios;
    Backend backend;
    Renderer renderer{backend};
    Journal journal;
    glide::session::Session owner;
    if (previouslyOwned) {
        const auto previous = Make(monitor, 3.14159, 0.25);
        Check(Success(glide::monitor::Present(previous, renderer, owner, backend, journal).result),
              "establish old monitor frame");
        Check(renderer.visible && owner.OwnsState() && journal.armed, "old frame owns recoverable input mapping");
    }
    backend.RestartCounters();
    renderer.preparations = renderer.shows = renderer.hides = 0;
    backend.fault = renderer.fault = fault;
    if (fault == Fault::ForeignBefore) {
        backend.state.visual = {2.5f, -25, 81};
        backend.faultReached = true;
    }
    const auto frame = Make(monitor, zoom);
    const auto outcome = glide::monitor::Present(frame, renderer, owner, backend, journal);
    const bool local = fault == Fault::PrepareBefore || fault == Fault::PrepareAfter ||
                       fault == Fault::ShowBefore || fault == Fault::ShowAfter;
    Result expected = Result::Applied;
    if (local || fault == Fault::InputBefore) expected = Result::WriteFailure;
    if (fault >= Fault::Read1 && fault <= Fault::Read4) expected = Result::ReadFailure;
    if (fault == Fault::ForeignBefore || fault == Fault::ForeignAfter) expected = Result::Conflict;
    Check(outcome.result == expected && outcome.localFailure == local, "exact fault outcome and owner classification");
    Check(renderer.preparations == 1 && backend.trace.front() == "prepare", "prepare precedes global access");
    Check(backend.visualWrites == 0, "monitor presentation never sets global fullscreen visual transform");

    if (fault != Fault::None) {
        ++injected;
        Check(renderer.faultReached || backend.faultReached, "scheduled failure actually reached its boundary");
        ++reachedBoundaries;
    }
    if (Success(outcome.result)) {
        Check(renderer.visible && renderer.shows == 1 && renderer.hides == 0, "successful frame stays visible");
        Check(backend.state.input.source == frame.source && backend.state.input.destination == frame.destination,
              "only selected monitor crop is mapped");
        Check(owner.OwnsState() && journal.armed, "success is journaled for parent-crash recovery");
    } else {
        Check(!renderer.visible && renderer.hides == 1 && backend.trace.back() == "hide",
              "all failure paths hide even after partial local visual effects");
    }
    if (fault == Fault::PrepareBefore || fault == Fault::PrepareAfter) {
        Check(backend.reads == 0 && backend.inputWrites == 0 && renderer.shows == 0,
              "failed preparation never touches mapping or calls show");
    }
    if (fault == Fault::ShowBefore || fault == Fault::ShowAfter) {
        Check(backend.inputWrites == 1 && owner.NeedsRecovery() && journal.armed,
              "show failure leaves input mapping ownership and recovery record for caller reset");
    }
    const auto beforeReset = backend.state;
    const auto writesBeforeReset = backend.inputWrites;
    backend.fault = renderer.fault = Fault::None;
    renderer.Hide();
    const auto restored = owner.Reset(backend, journal);
    Check(Success(restored), "caller can complete conditional restoration after fault disarmed");
    if (expected == Result::Conflict) {
        Check(backend.state == beforeReset && backend.inputWrites == writesBeforeReset,
              "caller reset preserves the observed foreign transform without any write");
    } else Check(glide::session::IsIdentity(backend.state), "caller restores its input-only session to neutral");
    Check(!owner.NeedsRecovery() && !journal.armed && backend.visualWrites == 0,
          "conditional cleanup releases journal and never alters fullscreen visuals");
}

void CachedRefreshMatrix() {
    const glide::Rect bounds{-1920, 0, 0, 1080};
    for (const auto fault : {Fault::None, Fault::PrepareBefore, Fault::Read1,
                             Fault::ShowBefore, Fault::ShowAfter, Fault::ForeignBefore}) {
        ++scenarios;
        Backend backend;
        Renderer renderer{backend};
        Journal journal;
        glide::session::Session owner;
        const auto frame = Make(bounds, 4);
        Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result),
              "establish cached live refresh fixture");
        backend.RestartCounters();
        renderer.preparations = renderer.shows = renderer.hides = 0;
        backend.fault = renderer.fault = fault;
        if (fault == Fault::ForeignBefore) backend.state.input.destination.left += 1;
        const auto before = backend.state;
        const auto result = glide::monitor::Present(frame, renderer, owner, backend, journal);
        const Result expected = fault == Fault::Read1 ? Result::ReadFailure :
            fault == Fault::ForeignBefore ? Result::Conflict :
            fault == Fault::ShowBefore || fault == Fault::ShowAfter ? Result::WriteFailure :
            Result::Unchanged;
        Check(result.result == expected, "cached refresh retains exact ownership and local failure classifications");
        Check(renderer.preparations == 0 && backend.reads == 1 &&
              backend.inputWrites == 0 && backend.visualWrites == 0,
              "cached refresh uses one real ownership read and no geometry transaction or shared setters");
        Check(backend.state == before, "cached refresh cannot mutate an outside transform even on failure");
        if (Success(expected)) {
            Check(renderer.visible && renderer.shows == 1 && renderer.hides == 0,
                  "unchanged content still receives one live refresh");
            if (fault == Fault::PrepareBefore)
                Check(!renderer.faultReached, "unused preparation boundary is not falsely counted as injected");
        } else {
            Check(!renderer.visible && renderer.hides == 1, "failed cached refresh hides the stale overlay");
            Check(renderer.shows == (fault == Fault::Read1 || fault == Fault::ForeignBefore ? 0u : 1u),
                  "cached ownership failure prevents private display refresh");
        }
        if (fault == Fault::ForeignBefore)
            Check(!owner.OwnsState() && !journal.armed, "foreign cached state releases ownership instead of resetting it");
    }

    for (bool dropOwner : {false, true}) {
        ++scenarios;
        Backend backend;
        Renderer renderer{backend};
        Journal journal;
        glide::session::Session owner;
        const auto frame = Make(bounds, 2);
        Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result),
              "establish cache revocation fixture");
        if (dropOwner) Check(Success(owner.Reset(backend, journal)), "release global ownership while local cache still exists");
        else renderer.Hide();
        renderer.preparations = 0;
        Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result) &&
              renderer.preparations == 1 && renderer.visible,
              "either missing global ownership or hidden local frame requires full preparation again");
    }

    {
        ++scenarios;
        Backend backend;
        Renderer renderer{backend};
        Journal journal;
        glide::session::Session owner;
        const auto frame = Make(bounds, 2);
        const auto other = Make(bounds, 3);
        Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result),
              "establish previously verified cached frame");
        backend.RestartCounters();
        backend.fault = Fault::Read3;
        Check(owner.Apply({{1, 0, 0}, {true, other.source, other.destination}}, backend, journal) ==
              Result::ReadFailure && backend.state.input.source == other.source && owner.Expected().input.source == frame.source,
              "model a shared write that took effect before a failed postcondition read");
        backend.fault = Fault::None;
        backend.RestartCounters();
        renderer.preparations = 0;
        Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result) &&
              backend.state.input.source == frame.source && owner.Expected().input.source == frame.source &&
              renderer.preparations == 0 && backend.inputWrites == 1,
              "resolving an uncertain owned mapping repairs it before showing the cached frame");
    }

    ++scenarios;
    Backend backend;
    Renderer renderer{backend};
    Journal journal;
    glide::session::Session owner;
    const auto first = Make({0, 0, 1920, 1080}, 1.00001);
    const auto next = Make({0, 0, 1920, 1080}, 1.00002);
    Check(first.source == next.source && first.destination == next.destination &&
          first.transform.zoom != next.transform.zoom, "different native scales can share integer input mapping");
    Check(Success(glide::monitor::Present(first, renderer, owner, backend, journal).result), "present first scale");
    Check(Success(glide::monitor::Present(next, renderer, owner, backend, journal).result) &&
          renderer.preparations == 2 && renderer.prepared.transform.zoom == next.transform.zoom,
          "same input mapping cannot suppress a changed native scale");
}

void CaptureWarmupAndCancellation() {
    for (const glide::Rect bounds : {glide::Rect{0, 0, 1920, 1080}, glide::Rect{-1920, -1080, 0, 0}}) {
        ++scenarios;
        Backend backend;
        Renderer renderer{backend};
        Journal journal;
        glide::session::Session owner;
        const auto frame = Make(bounds, 4);
        renderer.pending = true;
        for (unsigned poll = 0; poll < 5; ++poll) {
            const auto result = glide::monitor::Present(frame, renderer, owner, backend, journal);
            Check(result.pending && !result.localFailure && result.result == Result::Unchanged,
                  "asynchronous first-frame wait is an explicit pending result");
            Check(!renderer.visible && renderer.hides == 0 && renderer.shows == 0 &&
                  backend.reads == 0 && backend.inputWrites == 0 && backend.visualWrites == 0 &&
                  !owner.OwnsState() && !journal.armed,
                  "initial wait neither cancels capture nor maps input beneath an absent image");
        }
        renderer.pending = false;
        const auto ready = glide::monitor::Present(frame, renderer, owner, backend, journal);
        Check(!ready.pending && !ready.localFailure && ready.result == Result::Applied &&
              renderer.visible && backend.inputWrites == 1 && owner.OwnsState(),
              "first complete frame acquires mapping and presents exactly once");

        // A capture recreation cannot claim an ordinary initial wait while the
        // preceding frame still owns transformed global input.
        renderer.pending = true;
        backend.RestartCounters();
        const auto lost = glide::monitor::Present(frame, renderer, owner, backend, journal);
        Check(!lost.pending && lost.localFailure && lost.result == Result::WriteFailure &&
              !renderer.visible && renderer.hides == 1 && backend.inputWrites == 0 &&
              owner.NeedsRecovery() && journal.armed,
              "lost image with existing mapping requests restoration instead of waiting invisibly");
        Check(Success(owner.Reset(backend, journal)) && glide::session::IsIdentity(backend.state),
              "caller restores mapping after capture recreation invalidates the active image");
    }

    {
        ++scenarios;
        Backend backend;
        Renderer renderer{backend};
        Journal journal;
        glide::session::Session owner;
        const auto frame = Make({0, 0, 1920, 1080}, 2);
        renderer.pending = true;
        Check(glide::monitor::Present(frame, renderer, owner, backend, journal).pending,
              "start initial capture wait before timeout");
        renderer.pending = false;
        renderer.fault = Fault::PrepareBefore;
        const auto timeout = glide::monitor::Present(frame, renderer, owner, backend, journal);
        Check(!timeout.pending && timeout.localFailure && timeout.result == Result::WriteFailure &&
              renderer.hides == 1 && backend.reads == 0 && backend.inputWrites == 0 && !journal.armed,
              "native warmup timeout becomes one real failure without acquiring input state");
    }

    // Exercise the actual lifecycle lease around both calls. Reentrant pause
    // may happen while starting capture or while querying its pending status.
    for (unsigned boundary = 0; boundary < 3; ++boundary) {
        ++scenarios;
        Backend backend;
        Renderer native{backend};
        Journal journal;
        glide::session::Session owner;
        glide::runtime::State lifecycle;
        Check(lifecycle.PrepareEnable() && lifecycle.Activate(), "activate warmup lifecycle");
        const auto frame = Make({0, 0, 1920, 1080}, 2);
        glide::frame::Lease lease{lifecycle};
        glide::frame::Renderer renderer{native, lease};
        glide::frame::Backend fencedBackend{backend, lease};
        native.pending = true;
        if (boundary == 0) lifecycle.Pause();
        if (boundary == 1) native.onPrepare = [&] { lifecycle.Pause(); };
        if (boundary == 2) native.onPending = [&] { lifecycle.Pause(); };
        const auto cancelled = glide::monitor::Present(frame, renderer, owner, fencedBackend, journal);
        Check(!cancelled.pending && cancelled.localFailure && cancelled.result == Result::WriteFailure,
              "cancelled initial capture cannot report a live pending operation");
        Check(native.hides == 1 && backend.reads == 0 && backend.inputWrites == 0 &&
              !journal.armed && !owner.OwnsState(),
              "cancelled capture hides privately without shared mutation");
        Check(native.preparations == (boundary == 0 ? 0u : 1u) &&
              native.pendingChecks == (boundary == 2 ? 1u : 0u),
              "cancellation prevents every later native preparation/pending boundary");
    }
}

void RetainedFrameCancellation() {
    // A retained GPU frame is a new native boundary. Cancellation can arrive
    // in its proof or in the shared-state read between the two proofs.
    for (unsigned boundary = 0; boundary != 6; ++boundary) {
        for (bool change : {false, true}) {
            ++scenarios;
            Backend backend;
            Renderer native{backend};
            Journal journal;
            glide::session::Session owner;
            glide::runtime::State lifecycle;
            Check(lifecycle.PrepareEnable() && lifecycle.Activate(), "activate retained-frame lifecycle");
            const auto previous = Make({-1920, -1080, 0, 0}, 2);
            const auto requested = change ? Make({-1920, -1080, 0, 0}, 4) : previous;
            Check(Success(glide::monitor::Present(previous, native, owner, backend, journal).result),
                  "establish an actual shown frame and its session-owned map");
            const auto displayed = backend.state;
            backend.RestartCounters();
            native.pending = native.retain = true;
            const auto pause = [&]() { lifecycle.Pause(); };
            if (boundary == 0) native.onPrepare = pause;
            if (boundary == 1) native.onPending = pause;
            if (boundary == 2) native.onRetain = pause;
            if (boundary == 3) backend.onRead = pause;
            if (boundary == 4) native.onRetain = [&]() {
                if (native.retentionChecks == 2) lifecycle.Pause();
            };
            if (boundary == 5) native.onRetain = [&]() {
                if (native.retentionChecks == 2) native.retain = false;
            };
            glide::frame::Lease lease{lifecycle};
            glide::frame::Renderer renderer{native, lease};
            glide::frame::Backend fencedBackend{backend, lease};
            const auto result = glide::monitor::Present(requested, renderer, owner, fencedBackend, journal);
            Check(!result.pending && !Success(result.result) && !native.visible && native.hides == 1,
                  "cancelled or invalidated retained image cannot remain pending/visible");
            Check(backend.state == displayed && backend.inputWrites == 0 && backend.visualWrites == 0 &&
                  native.shows == 1 && owner.NeedsRecovery() && journal.armed,
                  "no requested map or second presentation escapes retention cancellation");
            backend.onRead = {};
            Check(Success(owner.Reset(backend, journal)) && glide::session::IsIdentity(backend.state) &&
                  !owner.NeedsRecovery() && !journal.armed,
                  "outer recovery restores exactly the recognized old input mapping");
        }
    }
}

} // namespace

int Run_monitor_presenter() {
    RetainedFrameCancellation();
    CaptureWarmupAndCancellation();
    CachedRefreshMatrix();
    const std::vector<glide::Rect> monitors{
        {0, 0, 1920, 1080}, {-1920, 0, 0, 1080},
        {1920, -2160, 5760, 0}, {-1080, -1920, 0, 0}
    };
    const std::vector<Fault> faults{
        Fault::None, Fault::PrepareBefore, Fault::PrepareAfter, Fault::ShowBefore, Fault::ShowAfter,
        Fault::Read1, Fault::Read2, Fault::Read3, Fault::Read4,
        Fault::InputBefore, Fault::InputFalseAfter, Fault::ForeignBefore, Fault::ForeignAfter
    };
    for (const auto monitor : monitors) for (const double zoom : {1.05, 1.5, 2.0, 8.0, 16.0})
        for (const bool owned : {false, true}) for (const auto fault : faults) {
            // Acquisition has one additional native read. Do not call an
            // ineligible failure an exercised boundary.
            if (owned && fault == Fault::Read4) continue;
            Run(monitor, zoom, owned, fault);
        }

    for (const auto monitor : monitors) {
        ++scenarios;
        Backend backend;
        Renderer renderer{backend};
        Journal journal;
        glide::session::Session owner;
        const auto frame = Make(monitor, 1.05);
        Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result), "initial live frame");
        const unsigned oldInputWrites = backend.inputWrites;
        const unsigned oldReads = backend.reads;
        for (unsigned repeat = 0; repeat < 20; ++repeat) {
            const auto outcome = glide::monitor::Present(frame, renderer, owner, backend, journal);
            Check(outcome.result == Result::Unchanged && !outcome.localFailure, "unchanged mapping can be presented repeatedly");
        }
        Check(renderer.preparations == 1 && renderer.shows == 21 && backend.inputWrites == oldInputWrites,
              "static zoom refreshes live content without another geometry transaction or global setter");
        Check(backend.reads == oldReads + 20,
              "each unchanged refresh still verifies shared input ownership with one native snapshot");
        renderer.Hide(); // Windows destroys private windows when the parent exits.
        Check(Success(glide::session::Recover(journal.record, backend)), "guardian recovers last input-only presentation");
        Check(glide::session::IsIdentity(backend.state) && backend.visualWrites == 0,
              "guardian restores mapping without writing global visuals");
    }

    {
        ++scenarios;
        Backend backend;
        Renderer renderer{backend};
        Journal journal;
        glide::session::Session owner;
        const glide::session::Snapshot wide{{2, 0, 0},
            {true, {-960, 0, 1920, 540}, {-1920, 0, 3840, 1080}}};
        Check(Success(owner.Apply(wide, backend, journal)), "establish owned all-desktop mode");
        backend.RestartCounters();
        const auto frame = Make(monitors[0], 2);
        const auto result = glide::monitor::Present(frame, renderer, owner, backend, journal);
        Check(result.result == Result::Invalid && result.localFailure,
              "presenter cannot bypass mode-switch restoration");
        Check(backend.state == wide && backend.reads == 0 && backend.inputWrites == 0 &&
              backend.visualWrites == 0 && renderer.preparations == 0 && renderer.shows == 0,
              "unrestored all-desktop session is not mutated by monitor presenter");
        Check(renderer.hides == 1 && owner.NeedsRecovery() && journal.armed,
              "failed implicit mode switch retains original cleanup ownership");
        Check(Success(owner.Reset(backend, journal)) && glide::session::IsIdentity(backend.state),
              "caller can explicitly restore old all-desktop session");
    }

    for (unsigned invalid = 0; invalid < 6; ++invalid) {
        ++scenarios;
        Backend backend;
        Renderer renderer{backend};
        Journal journal;
        glide::session::Session owner;
        auto frame = Make(monitors[0], 2);
        if (invalid == 0) frame.transform.zoom = std::numeric_limits<double>::quiet_NaN();
        if (invalid == 1) frame.transform.offset.x = std::numeric_limits<double>::infinity();
        if (invalid == 2) frame.source.right = frame.source.left;
        if (invalid == 3) frame.source.left = frame.destination.left - 1;
        if (invalid == 4) frame.destination.bottom = frame.destination.top;
        if (invalid == 5) frame.transform.zoom = 0.99;
        const auto result = glide::monitor::Present(frame, renderer, owner, backend, journal);
        Check(result.result == Result::Invalid && result.localFailure, "invalid frame rejected locally");
        Check(renderer.preparations == 0 && renderer.shows == 0 && renderer.hides == 1 && backend.reads == 0,
              "invalid frame causes no preparation/global read/write/show");
        Check(!owner.OwnsState() && !journal.armed, "invalid frame acquires no session ownership");
    }

    std::cout << "PASS: " << assertions << " monitor-presenter assertions; " << scenarios
              << " scenarios; " << reachedBoundaries << '/' << injected
              << " injected boundaries reached; actual Present + Session, mocked renderer/native API\n";
    return 0;
}
