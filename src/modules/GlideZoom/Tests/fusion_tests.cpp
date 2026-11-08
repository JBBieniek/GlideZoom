#include <stdexcept>
#include "fusion_harness.h"

#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

namespace {
using namespace fusion;

struct Outcome {
    std::string id, group, title;
    std::string inputFixture, queueSchedule, interruption, nativeFault;
    int initialZoom = 1;
    std::string status = "passed", finalPhase, error;
    bool faultReached = false, cleanupVerified = false;
    unsigned reachedBoundaries = 0, reads = 0, writes = 0, dropped = 0;
    std::vector<std::string> trace;
};

std::string Id(unsigned index) {
    std::ostringstream out;
    out << 'F' << std::setfill('0') << std::setw(5) << index;
    return out.str();
}

void Capture(Outcome& outcome, const Harness& harness) {
    outcome.faultReached = harness.backend.Reached();
    outcome.reachedBoundaries = harness.backend.ReachedCount();
    outcome.reads = harness.backend.reads;
    outcome.writes = harness.backend.writes;
    outcome.dropped = harness.dropped;
    outcome.finalPhase = Name(harness.runtime.Current());
}

Outcome Matrix(unsigned index, Fixture fixture, int zoom, Schedule schedule,
               Interruption interruption, Fault fault) {
    Outcome out;
    out.id = Id(index);
    out.group = "matrix";
    out.inputFixture = FixtureNames[static_cast<unsigned>(fixture)];
    out.initialZoom = zoom;
    out.queueSchedule = ScheduleNames[static_cast<unsigned>(schedule)];
    out.interruption = InterruptionNames[static_cast<unsigned>(interruption)];
    out.nativeFault = FaultNames[static_cast<unsigned>(fault)];
    std::unique_ptr<Harness> harness;
    try {
        harness = std::make_unique<Harness>(zoom);
        auto& h = *harness;
        h.Start(fixture);
        if (schedule == Schedule::BeginConsumed) h.Drain(1);
        if (schedule == Schedule::FullyConsumed) { h.Drain(); h.Frame(); }
        const auto settingsBefore = h.settings;
        const auto owedKeysBefore = h.owedKeys;
        const auto owedButtonsBefore = h.owedButtons;
        h.backend.Arm(fault);
        h.Interrupt(interruption);
        h.Tick();
        h.Check();

        if (interruption != Interruption::DesktopOther) {
            Require(h.owedKeys == owedKeysBefore && h.owedButtons == owedButtonsBefore,
                    "interruption erased a physical release obligation");
        } else {
            for (bool owed : h.owedKeys) Require(!owed, "new desktop retained old key obligation");
            for (bool owed : h.owedButtons) Require(!owed, "new desktop retained old button obligation");
        }
        if (interruption == Interruption::DesktopUnknown || interruption == Interruption::DesktopOther) {
            Require(h.backend.reads == 0 && h.backend.writes == 0,
                    "unavailable desktop did not gate native calls");
            Require(!h.runtime.Ready(), "unavailable desktop remained command-ready");
        }
        if (interruption == Interruption::SettingsInvalid || interruption == Interruption::SaveFailure)
            Require(h.settings == settingsBefore && !h.saveCommitted,
                    "rejected/failed save partially applied settings");
        if (interruption == Interruption::SettingsValid)
            Require(h.saveCommitted == h.saveAttempted,
                    "a completed successful save was not committed atomically");
        if (interruption == Interruption::GuardianLoss)
            Require(h.runtime.Current() == Phase::Faulted && h.input.Active() == Gesture::None,
                    "guardian loss did not leave a terminal, noncapturing phase");
        if (interruption == Interruption::Focus)
            Require(!h.dragging && h.input.Active() == Gesture::None,
                    "focus cancellation left input capture or a renderer drag active");
        if (h.backend.foreignActive)
            Require(h.backend.state == h.backend.lastForeign && h.backend.writesAfterForeign == 0,
                    "observable foreign ownership was overwritten");

        // Fault coverage is captured before disarmed cleanup. A fault which was
        // gated off by the desktop/lifecycle is explicitly reported as unreached.
        Capture(out, h);
        h.Cleanup();
        out.cleanupVerified = true;
        out.dropped = h.dropped;
    } catch (const std::exception& error) {
        out.status = "failed";
        out.error = error.what();
        if (harness) {
            Capture(out, *harness);
            out.trace = harness->trace.entries;
        }
    }
    return out;
}

struct Regression {
    const char* id;
    const char* title;
    int zoom;
    std::function<void(Harness&)> run;
    bool knownLimit = false;
};

std::vector<Regression> Regressions() {
    using namespace glide::input;
    std::vector<Regression> cases;
    cases.push_back({"FR01a", "held Space + pause on Unknown desktop + restoration on Current", 2,
        [](Harness& h) {
            h.Start(Fixture::Space);
            h.Drain();
            h.SetDesktop(Desktop::Unknown);
            h.Pause();
            Require(h.runtime.Current() == Phase::Paused && h.runtime.NeedsRestore() &&
                    h.session.OwnsState() && !h.runtime.Ready(),
                    "paused unavailable reset lost its restoration intent");
            Require(h.input.SwallowedKey(vk::Space), "unknown desktop erased Space release");
            const auto writes = h.backend.writes;
            h.SetDesktop(Desktop::Current);
            h.Tick();
            Require(!h.runtime.NeedsRestore() && !h.session.OwnsState() &&
                    glide::session::IsIdentity(h.backend.state) && h.backend.writes > writes,
                    "returning Current did not retry restoration in Paused");
            Require(h.runtime.Current() == Phase::Paused, "deferred pause unexpectedly resumed");
            Require(h.Key(vk::Space, false), "reserved release was lost across deferred restore");
        }});
    cases.push_back({"FR01b", "settings save on Unknown desktop aborts commit and retains restore", 4,
        [](Harness& h) {
            h.Start(Fixture::Mouse);
            const auto old = h.settings;
            h.SetDesktop(Desktop::Unknown);
            h.Save(true, true);
            Require(h.runtime.Current() == Phase::Paused && h.runtime.NeedsRestore() &&
                    !h.saveAttempted && h.settings == old,
                    "settings operation continued while restoration was deferred");
            Require(h.input.SwallowedButton(Button::Left), "deferred save erased mouse release");
            h.SetDesktop(Desktop::Current);
            h.Tick();
            Require(glide::session::IsIdentity(h.backend.state) && !h.runtime.NeedsRestore(),
                    "deferred settings restoration did not complete");
            Require(h.ButtonEvent(Button::Left, false), "reserved mouse UP did not survive save");
        }});
    for (bool stopping : {false, true}) {
        cases.push_back({stopping ? "FR02b" : "FR02a",
            stopping ? "Stopping + queued reset + foreign conflict stays terminal" :
                       "Faulted + queued reset + foreign conflict stays terminal", 2,
            [stopping](Harness& h) {
                h.Start(Fixture::Space);
                Command reset;
                reset.kind = Kind::Reset;
                h.Push(reset);
                if (stopping) h.runtime.Stop();
                else h.runtime.Fault();
                h.Sync();
                h.backend.ExternalWrite();
                h.Reset();
                h.Drain();
                const auto phase = stopping ? Phase::Stopping : Phase::Faulted;
                Require(h.runtime.Current() == phase && !h.runtime.Ready() &&
                        !h.runtime.NeedsRestore() && !h.session.OwnsState(),
                        "foreign conflict demoted terminal phase");
                Require(!h.runtime.PrepareEnable() && !h.runtime.Activate(),
                        "terminal phase permitted reactivation");
                Require(h.Key(vk::Space, false), "terminal conflict erased reserved UP");
            }});
    }
    cases.push_back({"FR03", "display change on Unknown desktop restores before refresh/resume", 4,
        [](Harness& h) {
            h.Start(Fixture::Mouse);
            h.Drain(1);
            h.SetDesktop(Desktop::Unknown);
            h.DisplayChange();
            Require(h.refreshPending && h.runtime.NeedsRestore() &&
                    h.runtime.Current() == Phase::Suspended && h.runtime.ResumeRequested(),
                    "unknown display change lost pending restore/refresh/resume intent");
            const auto firstReturnStep = h.trace.entries.size();
            h.SetDesktop(Desktop::Current);
            h.Tick();
            Require(!h.refreshPending && !h.runtime.NeedsRestore() && h.refreshes == 1 &&
                    h.desktop.left == -1280 && h.runtime.Ready() &&
                    glide::session::IsIdentity(h.backend.state),
                    "display return did not restore, refresh and re-enable in order");
            std::size_t clear = h.trace.entries.size(), refresh = h.trace.entries.size();
            for (std::size_t i = firstReturnStep; i < h.trace.entries.size(); ++i) {
                if (h.trace.entries[i] == "journal.clear") clear = std::min(clear, i);
                if (h.trace.entries[i] == "display.refresh") refresh = std::min(refresh, i);
            }
            Require(clear < refresh, "geometry refresh preceded restoration completion");
            Require(!h.dragging && h.input.Active() == Gesture::None,
                    "display return revived old held gesture");
        }});
    for (unsigned schedule = 0; schedule < 3; ++schedule) {
        static constexpr std::array<const char*, 3> ids{"FR04a", "FR04b", "FR04c"};
        cases.push_back({ids[schedule], "reset invalidates queued begin/delta/end at a selected drain cut", 2,
            [schedule](Harness& h) {
                h.Start(Fixture::Space);
                if (schedule == 1) h.Drain(1);
                if (schedule == 2) { h.Drain(); h.Frame(); }
                const auto oldEpoch = h.runtime.Epoch();
                h.Reset();
                const auto writesAfterReset = h.backend.writes;
                h.Drain();
                h.Frame();
                Require(h.runtime.Epoch() != oldEpoch && !h.dragging && h.target == 1 &&
                        h.backend.writes == writesAfterReset && !h.session.OwnsState(),
                        "stale queue resurrected zoom after reset");
                Require(h.Key(vk::Space, false), "epoch cancellation lost owed Space UP");
            }});
    }
    cases.push_back({"FR05", "queue overflow + held Space + foreign magnifier + guardian fault policy", 2,
        [](Harness& h) {
            h.Start(Fixture::Space);
            Command delta;
            delta.kind = Kind::Delta;
            delta.amount = -1;
            delta.travel = 1;
            for (unsigned i = 0; i < 32 && !h.queueFault; ++i) h.Push(delta);
            Require(h.queueFault && h.input.Active() == Gesture::None &&
                    h.input.SwallowedKey(vk::Space), "overflow did not preserve only owed releases");
            h.backend.ExternalWrite();
            h.Tick();
            Require(h.runtime.Current() == Phase::Faulted && h.backend.writesAfterForeign == 0,
                    "overflow recovery overwrote foreign state or re-enabled");
            Require(h.Key(vk::Space, false), "overflow lost reserved Space UP");
        }});
    for (unsigned fixture = 0; fixture < 3; ++fixture) {
        static constexpr std::array<const char*, 3> ids{"FR06a", "FR06b", "FR06c"};
        static constexpr std::array<Fixture, 3> fixtures{Fixture::Space, Fixture::Control, Fixture::Mouse};
        cases.push_back({ids[fixture], "held gesture reaches 1x then reverses without a new activation", 2,
            [fixture](Harness& h) {
                h.Start(fixtures[fixture]);
                h.Drain();
                const auto gesture = h.input.Active();
                const auto epoch = h.runtime.Epoch();
                h.Delta(1000);
                h.Drain();
                h.Frame(10);
                Require(h.target == 1 && h.transform.zoom == 1 && !h.session.OwnsState() &&
                        h.dragging && h.input.Active() == gesture && h.runtime.Epoch() == epoch,
                        "reaching 1x cancelled the still-held gesture");
                h.Delta(-2000);
                h.Drain();
                h.Frame(10);
                Require(h.transform.zoom > 1 && h.session.OwnsState() &&
                        h.input.Active() == gesture && h.runtime.Epoch() == epoch,
                        "reversing held motion at 1x did not reacquire zoom");
            }});
    }
    cases.push_back({"FR07a", "partial visual write + two read failures + parent loss + journal recovery", 1,
        [](Harness& h) {
            h.Start(Fixture::Space);
            h.Drain();
            h.backend.Arm({{Boundary::Read, 3, Effect::FailRead},
                           {Boundary::Read, 4, Effect::FailRead}});
            const auto desired = Desired(glide::AnchorZoom({}, 2, {800, 450}, h.desktop), h.desktop);
            Require(h.session.Apply(desired, h.backend, h.journal) == Result::ReadFailure,
                    "post-visual read failure was not reached");
            const auto record = h.journal.record;
            Require(h.backend.state.visual.zoom == 2 && !h.backend.state.input.enabled &&
                    h.journal.armed && glide::session::Recognized(record, h.backend.state),
                    "partial visual write was not recoverable");
            Require(h.session.Check(h.backend, h.journal) == Result::ReadFailure &&
                    h.journal.record.before == record.before && h.journal.record.after == record.after,
                    "second failed read broadened or discarded the recovery pair");
            Require(h.backend.ReachedCount() == 2, "both scripted failed reads must execute");
            h.session = {}; // Parent is gone; only the published record survives.
            const auto result = glide::session::Recover(record, h.backend, h.journal);
            Require(result == Result::Applied && !h.journal.armed &&
                    glide::session::IsIdentity(h.backend.state), "guardian-style recovery failed");
        }});
    cases.push_back({"FR07b", "partial reset disables mapping + read failure + parent loss + recovery", 4,
        [](Harness& h) {
            h.Start(Fixture::Mouse);
            h.backend.Arm({{Boundary::Read, 2, Effect::FailRead}});
            Require(h.session.Reset(h.backend, h.journal) == Result::ReadFailure,
                    "reset post-input read failure was not reached");
            const auto record = h.journal.record;
            Require(h.backend.state.visual.zoom == 4 && !h.backend.state.input.enabled &&
                    glide::session::Recognized(record, h.backend.state),
                    "partial reset did not retain its recoverable input-first state");
            h.session = {};
            const auto result = glide::session::Recover(record, h.backend, h.journal);
            Require(result == Result::Applied && glide::session::IsIdentity(h.backend.state) &&
                    !h.journal.armed, "partial reset recovery failed");
        }});
    cases.push_back({"FR07c", "parent loss after partial reset + foreign takeover makes recovery relinquish", 4,
        [](Harness& h) {
            h.backend.Arm({{Boundary::Read, 2, Effect::FailRead}});
            Require(h.session.Reset(h.backend, h.journal) == Result::ReadFailure,
                    "partial reset failure was not reached");
            const auto record = h.journal.record;
            h.session = {};
            h.backend.ExternalWrite();
            const auto writes = h.backend.writes;
            Require(glide::session::Recover(record, h.backend, h.journal) == Result::Conflict &&
                    h.backend.writes == writes && h.backend.state == h.backend.lastForeign &&
                    !h.journal.armed, "recovery overwrote foreign state after parent loss");
        }});
    cases.push_back({"FR08", "known limit: foreign write between final read and native setter", 2,
        [](Harness& h) {
            h.backend.allowForeignOverwrite = true;
            h.backend.Arm({{Boundary::BeforeWrite, 1, Effect::Foreign}});
            const auto desired = Desired(glide::AnchorZoom(h.transform, 4, {800, 450}, h.desktop), h.desktop);
            const auto result = h.session.Apply(desired, h.backend, h.journal);
            Require(h.backend.Reached() && h.backend.writesAfterForeign == 1 &&
                    h.backend.state.visual == desired.visual &&
                    h.backend.state.input == h.backend.lastForeign.input &&
                    result == Result::Conflict && !h.session.OwnsState() && !h.journal.armed,
                    "final-read/setter exposure demonstration no longer matches the API model");
            // This is evidence of a remaining non-atomic API limit, not a claim
            // that zero interference was achieved. Do not overwrite the mixed
            // state again to manufacture a clean teardown.
            h.runtime.Stop();
            h.Sync();
            h.Check();
        }, true});
    cases.push_back({"FR09", "failed settings save + held mouse + stale queue + successful explicit retry", 2,
        [](Harness& h) {
            h.Start(Fixture::Mouse);
            const auto old = h.settings;
            h.Save(true, false);
            Require(h.saveAttempted && !h.saveCommitted && h.settings == old && h.runtime.Ready(),
                    "failed settings transaction changed config or prevented safe re-enable");
            h.Drain();
            h.Frame();
            Require(!h.dragging && !h.session.OwnsState() && h.input.SwallowedButton(Button::Left),
                    "old queued gesture or changed config escaped failed save");
            Require(h.ButtonEvent(Button::Left, false), "save failure lost reserved mouse release");
            h.Save(true, true);
            Require(h.saveCommitted && h.settings.reverse && !h.settings.mouseGesture &&
                    h.settings.maxZoom == 16, "explicit retry failed to commit complete new settings");
        }});
    cases.push_back({"FR10", "focus change with queued Begin/End cancels tap and new movement", 2,
        [](Harness& h) {
            h.Key(vk::LeftControl, true);
            h.Key(vk::LeftAlt, true);
            h.Key(vk::Space, true); // Begin queued, no movement yet.
            const auto epoch = h.runtime.Epoch();
            h.Interrupt(Interruption::Focus);
            h.Drain();
            Require(h.runtime.Epoch() == epoch && !h.dragging && h.target == 2,
                    "focus cancellation changed epoch or toggled a canceled tap");
            const auto queued = h.queued;
            h.Delta(-200);
            Require(h.queued == queued && h.input.Active() == Gesture::None,
                    "new movement entered the canceled foreground gesture");
            Require(h.Key(vk::Space, false), "focus change lost reserved key release");
        }});
    cases.push_back({"FR11", "gesture timeout + queued motion + pause + owed release", 2,
        [](Harness& h) {
            h.Start(Fixture::Space);
            h.Drain(1);
            h.now += 30001;
            h.Emit(h.input.Expire(h.now));
            Require(h.input.Active() == Gesture::None && h.input.SwallowedKey(vk::Space),
                    "gesture timeout dropped release obligation");
            h.Pause();
            h.Drain();
            Require(!h.dragging && !h.session.OwnsState() && h.target == 1,
                    "timed-out queued motion escaped pause");
            Require(h.Key(vk::Space, false), "timeout/pause lost reserved key release");
        }});
    cases.push_back({"FR12", "direct held-at-1x restore loses desktop then rejects old queued motion", 2,
        [](Harness& h) {
            h.Start(Fixture::Space);
            h.Drain(1); // Renderer has begun; its Delta remains queued.
            const auto oldEpoch = h.runtime.Epoch();
            h.SetDesktop(Desktop::Unknown);
            Require(!h.RestoreOwned() && h.runtime.NeedsRestore() &&
                    h.runtime.Epoch() != oldEpoch && !h.dragging,
                    "direct deferred restore preserved the old gesture epoch");
            h.SetDesktop(Desktop::Current);
            h.Tick();
            Require(h.runtime.Ready() && !h.runtime.NeedsRestore() &&
                    h.target == 1 && h.transform.zoom == 1 && !h.session.OwnsState() &&
                    !h.dragging && h.input.Active() == Gesture::None && h.dropped > 0,
                    "desktop return revived queued motion from a deferred 1x restore");
            Require(h.Key(vk::Space, false), "deferred 1x restore lost reserved Space UP");
        }});
    return cases;
}

Outcome Run(const Regression& regression) {
    Outcome out;
    out.id = regression.id;
    out.group = "regression";
    out.title = regression.title;
    out.initialZoom = regression.zoom;
    std::unique_ptr<Harness> harness;
    try {
        harness = std::make_unique<Harness>(regression.zoom);
        harness->backend.Arm(Fault::None);
        regression.run(*harness);
        Capture(out, *harness);
        if (regression.knownLimit) out.status = "known_limit_demonstrated";
        else {
            harness->Cleanup();
            out.cleanupVerified = true;
        }
        // Targeted traces are small enough to retain, including successful
        // regressions and the known-limit demonstration, for review/replay.
        out.trace = harness->trace.entries;
        out.dropped = harness->dropped;
    } catch (const std::exception& error) {
        out.status = "failed";
        out.error = error.what();
        if (harness) {
            Capture(out, *harness);
            out.trace = harness->trace.entries;
        }
    }
    return out;
}

std::string Json(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char character : value) {
        switch (character) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (character < 0x20)
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') <<
                       static_cast<unsigned>(character) << std::dec;
            else out << character;
        }
    }
    out << '"';
    return out.str();
}

struct Summary {
    unsigned matrix = 0, regression = 0, passed = 0, failed = 0, knownLimit = 0;
    unsigned requestedFault = 0, reachedFault = 0, unreachedFault = 0, noFault = 0;
    std::array<unsigned, 7> reachedByFault{}, unreachedByFault{};
};

Summary Summarize(const std::vector<Outcome>& outcomes) {
    Summary summary;
    for (const auto& outcome : outcomes) {
        if (outcome.status == "failed") ++summary.failed;
        else if (outcome.status == "known_limit_demonstrated") ++summary.knownLimit;
        else ++summary.passed;
        if (outcome.group == "regression") { ++summary.regression; continue; }
        ++summary.matrix;
        if (outcome.nativeFault == "none") { ++summary.noFault; continue; }
        ++summary.requestedFault;
        for (unsigned i = 1; i < FaultNames.size(); ++i) {
            if (outcome.nativeFault != FaultNames[i]) continue;
            if (outcome.faultReached) { ++summary.reachedFault; ++summary.reachedByFault[i]; }
            else { ++summary.unreachedFault; ++summary.unreachedByFault[i]; }
        }
    }
    return summary;
}

template<std::size_t Size>
void JsonArray(std::ostream& out, const std::array<const char*, Size>& values) {
    out << '[';
    for (unsigned i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << Json(values[i]);
    }
    out << ']';
}

void Report(const std::string& path, const std::vector<Outcome>& outcomes, const Summary& summary,
            unsigned definedRegressions) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    Require(static_cast<bool>(out), "could not open JSON report: " + path);
    out << "{\n\"schema_version\":1,\n"
        << "\"scope\":\"Portable composition of production input::State, SpscQueue, runtime::State, "
           "session::Session, settings codec and zoom math; scripted native backend and event/UI adapter. "
           "No native Windows hooks, message loop, filesystem, security desktop or guardian process executed.\",\n"
        << "\"schedule_definition\":{\"queued\":\"all fixture commands pending\","
           "\"begin_consumed\":\"consume at most the first command, with no frame; for wheel this is the wheel command, "
           "and for idle the cut is vacuous\",\"fully_consumed\":\"all fixture commands consumed plus one frame before "
           "fault injection is armed\"},\n"
        << "\"fault_window\":\"Armed after fixture/schedule setup; covers interruption and one UI tick. "
           "Reached flags and native-call counters are captured before faults are disarmed for cleanup. "
           "Unreached means no matching native boundary occurred in this window, including lifecycle gates "
           "and no-op operations.\",\n"
        << "\"phase_definition\":\"final_phase is measured after the scripted work, before separate cleanup; "
           "dropped_commands includes cleanup draining stale commands.\",\n"
        << "\"constraints\":[],\n\"defined_matrix_rows\":3780,\n\"defined_regressions\":"
        << definedRegressions << ",\n\"factors\":{\"input_fixture\":";
    JsonArray(out, FixtureNames);
    out << ",\"initial_zoom\":[1,2,4],\"queue_schedule\":";
    JsonArray(out, ScheduleNames);
    out << ",\"interruption\":";
    JsonArray(out, InterruptionNames);
    out << ",\"native_fault\":";
    JsonArray(out, FaultNames);
    out << "},\n\"summary\":{\"matrix_executed\":" << summary.matrix
        << ",\"regressions_executed\":" << summary.regression
        << ",\"passed\":" << summary.passed << ",\"failed\":" << summary.failed
        << ",\"known_limits_demonstrated\":" << summary.knownLimit
        << ",\"matrix_no_fault_requested\":" << summary.noFault
        << ",\"matrix_fault_requested\":" << summary.requestedFault
        << ",\"matrix_fault_reached\":" << summary.reachedFault
        << ",\"matrix_fault_unreached\":" << summary.unreachedFault << "},\n"
        << "\"fault_counts\":[";
    for (unsigned i = 1; i < FaultNames.size(); ++i) {
        if (i > 1) out << ',';
        out << "{\"native_fault\":" << Json(FaultNames[i]) << ",\"reached\":"
            << summary.reachedByFault[i] << ",\"unreached\":" << summary.unreachedByFault[i] << '}';
    }
    out << "],\n\"scenarios\":[\n";
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        const auto& row = outcomes[i];
        if (i) out << ",\n";
        out << "{\"id\":" << Json(row.id) << ",\"group\":" << Json(row.group)
            << ",\"status\":" << Json(row.status) << ",\"initial_zoom\":" << row.initialZoom;
        if (row.group == "matrix")
            out << ",\"input_fixture\":" << Json(row.inputFixture)
                << ",\"queue_schedule\":" << Json(row.queueSchedule)
                << ",\"interruption\":" << Json(row.interruption)
                << ",\"native_fault\":" << Json(row.nativeFault);
        else out << ",\"title\":" << Json(row.title);
        out << ",\"fault_reached\":" << (row.faultReached ? "true" : "false")
            << ",\"reached_boundaries\":" << row.reachedBoundaries
            << ",\"reads_after_arm\":" << row.reads << ",\"writes_after_arm\":" << row.writes
            << ",\"dropped_commands\":" << row.dropped << ",\"final_phase\":" << Json(row.finalPhase)
            << ",\"cleanup_verified\":" << (row.cleanupVerified ? "true" : "false");
        if (!row.error.empty()) out << ",\"error\":" << Json(row.error);
        if (!row.trace.empty()) {
            out << ",\"trace\":[";
            for (std::size_t step = 0; step < row.trace.size(); ++step) {
                if (step) out << ',';
                out << Json(row.trace[step]);
            }
            out << ']';
        }
        out << '}';
    }
    out << "\n]}\n";
    out.close();
    Require(static_cast<bool>(out), "could not finish JSON report: " + path);
}
} // namespace

int Run_fusion(int argc, char** argv) {
    std::string path, selected;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if ((argument == "--json" || argument == "--case") && i + 1 < argc) {
            (argument == "--json" ? path : selected) = argv[++i];
        } else {
            std::cerr << "Usage: GlideZoomFusionTests [--json PATH] [--case F00000|FR01a|...]\n";
            return 2;
        }
    }
    try {
        std::vector<Outcome> outcomes;
        unsigned index = 0;
        for (unsigned fixture = 0; fixture < FixtureNames.size(); ++fixture)
            for (int zoom : {1, 2, 4})
                for (unsigned schedule = 0; schedule < ScheduleNames.size(); ++schedule)
                    for (unsigned interruption = 0; interruption < InterruptionNames.size(); ++interruption)
                        for (unsigned fault = 0; fault < FaultNames.size(); ++fault, ++index) {
                            if (!selected.empty() && selected != Id(index)) continue;
                            outcomes.push_back(Matrix(index, static_cast<Fixture>(fixture), zoom,
                                static_cast<Schedule>(schedule), static_cast<Interruption>(interruption),
                                static_cast<Fault>(fault)));
                        }
        Require(index == 3780, "Cartesian matrix definition changed without updating manifest");
        const auto regressions = Regressions();
        for (const auto& regression : regressions)
            if (selected.empty() || selected == regression.id) outcomes.push_back(Run(regression));
        if (outcomes.empty()) {
            std::cerr << "Unknown scenario ID: " << selected << '\n';
            return 2;
        }
        const auto summary = Summarize(outcomes);
        if (!path.empty()) Report(path, outcomes, summary, static_cast<unsigned>(regressions.size()));
        for (const auto& outcome : outcomes) {
            if (outcome.status != "failed") continue;
            std::cerr << "FAIL " << outcome.id << ": " << outcome.error << '\n';
            for (const auto& step : outcome.trace) std::cerr << "  " << step << '\n';
        }
        std::cout << "Fusion: " << summary.matrix << " matrix rows, " << summary.regression
                  << " targeted scenarios; " << summary.passed << " passed, " << summary.failed
                  << " failed, " << summary.knownLimit << " known API limit demonstrated.\n"
                  << "Matrix faults: " << summary.requestedFault << " requested; "
                  << summary.reachedFault << " reached, " << summary.unreachedFault
                  << " unreached within injection window; " << summary.noFault << " no-fault rows.\n";
        return summary.failed == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "Fusion runner error: " << error.what() << '\n';
        return 2;
    }
}
