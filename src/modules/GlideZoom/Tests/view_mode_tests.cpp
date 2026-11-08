#include <stdexcept>
#include "../GlideZoom/view_mode.h"
#include "../GlideZoom/monitor_geometry.h"
#include "../GlideZoom/runtime_state.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace
{
    unsigned assertions = 0, scenarios = 0;
    void Check(bool condition, const std::string& message)
    {
        ++assertions;
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            throw std::runtime_error("GlideZoom assertion failed; see captured output");
        }
    }
    struct Journal
    {
        glide::session::RecoveryRecord record{};
        bool armed = false;
        void Publish(const glide::session::RecoveryRecord& value)
        {
            record = value;
            armed = true;
        }
        void Clear() { armed = false; }
    };
    struct Backend
    {
        glide::session::Snapshot state{};
        bool failRead = false, failWrite = false;
        unsigned visualWrites = 0, inputWrites = 0;
        bool Read(glide::session::Snapshot& output)
        {
            if (failRead)
                return false;
            output = state;
            return true;
        }
        bool SetVisual(glide::session::Visual value)
        {
            ++visualWrites;
            if (failWrite)
                return false;
            state.visual = value;
            return true;
        }
        bool SetInput(glide::session::Input value)
        {
            ++inputWrites;
            if (failWrite)
                return false;
            state.input = value;
            return true;
        }
    };
}

int Run_view_mode()
{
    using namespace glide;
    using session::Result;
    using view::Scope;
    view::State initial;
    Check(initial.Current() == Scope::Monitor, "default scope is one monitor");
    for (const Scope start : { Scope::Monitor, Scope::Desktop, Scope::Independent })
        for (const Scope requested : { Scope::Monitor, Scope::Desktop, Scope::Independent, static_cast<Scope>(100) })
            for (const Result result : { Result::Applied, Result::Unchanged, Result::Conflict, Result::ReadFailure, Result::WriteFailure, Result::Invalid })
                for (bool desktop : { false, true })
                    for (bool pending : { false, true })
                        for (bool terminal : { false, true })
                        {
                            ++scenarios;
                            view::State mode;
                            Check(mode.CommitSwitch(start, Result::Unchanged, true), "establish old scope");
                            const bool expected = (requested == Scope::Monitor || requested == Scope::Desktop || requested == Scope::Independent) &&
                                                  (result == Result::Applied || result == Result::Unchanged) &&
                                                  desktop && !pending && !terminal;
                            Check(mode.CommitSwitch(requested, result, desktop, pending, terminal) == expected,
                                  "mode switch commits only after completed safe restoration");
                            Check(mode.Current() == (expected ? requested : start),
                                  "failed/deferred/conflicting mode switch preserves old scope");
                        }

    const Rect left{ -1920, 0, 0, 1080 };
    monitor::Frame frame{};
    Check(monitor::MakeFrame(left, AnchorZoom({}, 2, { -960, 540 }, left), frame), "monitor fixture");
    const session::Snapshot monitorDesired{ {}, { true, frame.source, frame.destination } };
    const session::Snapshot desktopDesired{ { 2, 0, 0 }, { true, { -960, 0, 1920, 540 }, { -1920, 0, 3840, 1080 } } };

    // Real shared-transform Session composed with the production mode gate.
    // The window visual is deliberately not a global visual transform.
    {
        Backend backend;
        Journal journal;
        session::Session session;
        view::State mode;
        Check(session.Apply(monitorDesired, backend, journal) == Result::Applied, "acquire monitor input mapping");
        Check(backend.visualWrites == 0, "monitor mode never writes the all-desktop visual scale");
        Check(backend.state.visual.zoom == 1 && backend.state.input.destination == frame.destination,
              "only selected monitor destination is mapped");
        const auto result = session.Reset(backend, journal);
        Check(mode.CommitSwitch(Scope::Desktop, result, true, session.NeedsRecovery()), "commit desktop after reset");
        Check(!journal.armed && session::IsIdentity(backend.state), "old global mapping released before new scope");
        Check(session.Apply(desktopDesired, backend, journal) == Result::Applied, "acquire desktop transform afterward");
        Check(backend.state.visual.zoom == 2 && backend.visualWrites == 1, "desktop mode preserves old wide-screen effect");
        Check(session.Reset(backend, journal) == Result::Applied, "desktop reset");
    }

    for (const Result expected : { Result::ReadFailure, Result::WriteFailure, Result::Conflict })
    {
        ++scenarios;
        Backend backend;
        Journal journal;
        session::Session session;
        view::State mode;
        Check(session.Apply(monitorDesired, backend, journal) == Result::Applied, "acquire before interrupted switch");
        if (expected == Result::ReadFailure)
            backend.failRead = true;
        if (expected == Result::WriteFailure)
            backend.failWrite = true;
        if (expected == Result::Conflict)
            backend.state.visual = { 3, 90, 30 };
        const auto before = backend.state;
        const auto result = session.Reset(backend, journal);
        Check(result == expected, "injected restoration result reached");
        Check(!mode.CommitSwitch(Scope::Desktop, result, true, session.NeedsRecovery()),
              "restore failure cannot silently change backend scope");
        Check(mode.Current() == Scope::Monitor, "old scope survives interrupted switch");
        if (expected == Result::Conflict)
        {
            Check(backend.state == before && !session.NeedsRecovery(), "foreign transform untouched and relinquished");
        }
        else
        {
            Check(session.NeedsRecovery() && journal.armed, "retry retains recovery ownership");
            backend.failRead = backend.failWrite = false;
            const auto restored = session.Reset(backend, journal);
            Check(mode.CommitSwitch(Scope::Desktop, restored, true, session.NeedsRecovery()),
                  "new explicit request can commit after successful retry");
        }
    }

    {
        ++scenarios;
        Backend backend;
        Journal journal;
        session::Session session;
        Check(session.Apply(monitorDesired, backend, journal) == Result::Applied, "monitor before parent crash");
        Check(session::Recover(journal.record, backend) == Result::Applied, "existing guardian restores input-only snapshot");
        Check(session::IsIdentity(backend.state) && backend.visualWrites == 0,
              "guardian needs no window handle and no global visual mutation in monitor mode");
    }

    {
        ++scenarios;
        runtime::State runtime;
        view::State mode;
        Check(runtime.PrepareEnable() && runtime.Activate(), "runtime ready before switch");
        const auto oldEpoch = runtime.Epoch();
        runtime.Cancel();
        Check(mode.CommitSwitch(Scope::Desktop, Result::Unchanged, true), "switch at identity");
        Check(!runtime.Accept(oldEpoch) && runtime.Accept(runtime.Epoch()), "queued old drag packets cannot cross scope switch");
        runtime.Suspend();
        runtime.RequestRestore();
        Check(!mode.CommitSwitch(Scope::Monitor, Result::Unchanged, false, runtime.NeedsRestore()),
              "unavailable desktop keeps old scope");
        Check(!mode.CommitSwitch(Scope::Monitor, Result::Unchanged, true, runtime.NeedsRestore()),
              "desktop return alone cannot bypass pending restore");
        Check(mode.Current() == Scope::Desktop, "no deferred mode request executes on desktop return");
        runtime.RestoreComplete();
        runtime.Fault();
        Check(!mode.CommitSwitch(Scope::Monitor, Result::Unchanged, true, false, runtime.Terminal()),
              "terminal runtime cannot switch mode");
    }

    std::cout << "PASS: " << assertions << " view-mode assertions; " << scenarios
              << " mode/restore conditions; actual Session, runtime epoch and guardian recovery composition\n";
    return 0;
}
