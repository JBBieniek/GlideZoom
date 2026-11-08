#include <stdexcept>
#include "../GlideZoom/zoom_session.h"

#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
using namespace glide::session;
int assertions = 0;
void Check(bool result, const std::string& message) {
    ++assertions;
    if (!result) {
        std::cerr << "FAIL: " << message << '\n';
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}

Snapshot Zoom(float factor = 2, int offset = 100) {
    return {{factor, offset, offset},
            {true, {offset, offset, offset + 800, offset + 450},
                   {0, 0, 1600, 900}}};
}
const Snapshot identity{};
const Snapshot foreign = Zoom(5, 700);

struct Journal {
    RecoveryRecord record{};
    std::vector<RecoveryRecord> history;
    bool armed = false;
    int clears = 0;
    void Publish(const RecoveryRecord& next) {
        Check(Valid(next), "every published recovery record is valid");
        record = next;
        history.push_back(next);
        armed = true;
    }
    void Clear() { armed = false; ++clears; }
};

enum class WriteEffect { Success, FailNoChange, FailAfterChange, SucceedNoChange, ForeignAfterChange };
struct Backend {
    Snapshot state{};
    Journal* journal = nullptr;
    int reads = 0, writes = 0;
    int failRead = 0, effectWrite = 0;
    WriteEffect effect = WriteEffect::Success;
    std::vector<char> order;
    std::function<void(int, Snapshot&)> beforeRead;

    bool Read(Snapshot& result) {
        ++reads;
        if (beforeRead) beforeRead(reads, state);
        if (reads == failRead) return false;
        result = state;
        return true;
    }
    bool SetVisual(Visual v) {
        Snapshot next = state;
        next.visual = v;
        order.push_back('v');
        return Write(next);
    }
    bool SetInput(Input input) {
        Snapshot next = state;
        next.input = input;
        order.push_back('i');
        return Write(next);
    }
    bool Write(Snapshot next) {
        ++writes;
        if (journal) {
            Check(journal->armed, "journal is armed before native mutation");
            Check(journal->record.before == state, "journal captures complete prewrite state");
            Check(journal->record.after == next, "journal captures complete intended postwrite state");
        }
        const WriteEffect action = writes == effectWrite ? effect : WriteEffect::Success;
        if (action == WriteEffect::FailNoChange) return false;
        if (action == WriteEffect::SucceedNoChange) return true;
        state = action == WriteEffect::ForeignAfterChange ? foreign : next;
        return action != WriteEffect::FailAfterChange;
    }
};

void HappyPath() {
    Journal journal;
    Backend backend;
    backend.journal = &journal;
    Session session;
    Check(session.Reset(backend, journal) == Result::Unchanged, "idle reset is a no-op");
    Check(backend.reads == 0 && backend.writes == 0, "idle session never touches global state");
    Check(session.Apply(Zoom(), backend, journal) == Result::Applied, "acquire neutral global state");
    Check(session.OwnsState() && session.NeedsRecovery() && journal.armed, "active ownership is recoverable");
    Check(backend.state == Zoom(), "both visual and input reach desired state");
    Check(backend.order == std::vector<char>({'v', 'i'}), "apply changes visual then input");
    const int priorWrites = backend.writes;
    Check(session.Apply(Zoom(), backend, journal) == Result::Unchanged, "unchanged frame still observes ownership");
    Check(backend.writes == priorWrites, "unchanged frame does not write");
    Check(session.Apply(Zoom(3, 200), backend, journal) == Result::Applied, "owned state can advance");
    Check(session.Check(backend, journal) == Result::Unchanged, "owned state can be checked without mutation");
    backend.order.clear();
    Check(session.Reset(backend, journal) == Result::Applied, "reset restores baseline");
    Check(backend.state == identity && !session.OwnsState() && !journal.armed, "successful reset releases ownership");
    Check(backend.order == std::vector<char>({'i', 'v'}), "reset disables input before restoring visual");
    Check(session.Reset(backend, journal) == Result::Unchanged, "reset is idempotent");
    Check(session.Apply(identity, backend, journal) == Result::Unchanged, "identity request stays idle");
}

void ValidationAndAcquisition() {
    for (const Snapshot state : {foreign, Snapshot{{1, 0, 0}, Zoom().input},
                                 Snapshot{Zoom().visual, {}}}) {
        Session session;
        Journal journal;
        Backend backend;
        backend.state = state;
        backend.journal = &journal;
        Check(session.Apply(Zoom(), backend, journal) == Result::Conflict,
              "initial acquisition refuses any nonidentity owner");
        Check(!session.OwnsState() && backend.writes == 0 && !journal.armed,
              "failed acquisition changes nothing");
        Check(session.Reset(backend, journal) == Result::Unchanged && backend.state == state,
              "idle cleanup preserves another owner");
    }
    std::vector<Snapshot> invalid;
    for (float zoom : {0.0f, 0.99f, 4097.0f, std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::quiet_NaN()}) {
        Snapshot value = Zoom(); value.visual.zoom = zoom; invalid.push_back(value);
    }
    Snapshot value = Zoom(); value.visual.x = 262145; invalid.push_back(value);
    value = Zoom(); value.input.source.right = value.input.source.left; invalid.push_back(value);
    value = Zoom(); value.input.destination.bottom = -1; invalid.push_back(value);
    for (const auto& bad : invalid) {
        Session session;
        Backend backend;
        Journal journal;
        Check(session.Apply(bad, backend, journal) == Result::Invalid, "invalid request is rejected");
        Check(backend.reads == 0 && backend.writes == 0, "invalid request is side-effect free");
    }
    Backend backend;
    Journal journal;
    Session session;
    backend.state.visual = {1, 31, -22};
    backend.state.input.source = {8, 9, -1, -2}; // Disabled rectangles are ignored.
    Check(session.Apply(Zoom(), backend, journal) == Result::Applied, "inactive native fields do not block acquisition");
    Check(session.Reset(backend, journal) == Result::Applied && backend.state == identity,
          "inactive native fields do not block restoration");
}

void ForeignOwnerAtEveryBoundary() {
    // Acquire has five read boundaries: acquire, pre/post visual, pre/post input.
    for (int boundary = 1; boundary <= 5; ++boundary) {
        Session session;
        Journal journal;
        Backend backend;
        backend.journal = &journal;
        backend.beforeRead = [boundary](int n, Snapshot& state) { if (n == boundary) state = foreign; };
        Check(session.Apply(Zoom(), backend, journal) == Result::Conflict,
              "foreign acquisition-boundary change is detected");
        const int writes = backend.writes;
        Check(!session.OwnsState() && !journal.armed && backend.state == foreign,
              "conflict relinquishes ownership without touching foreign state");
        Check(session.Reset(backend, journal) == Result::Unchanged && backend.writes == writes,
              "cleanup after detected conflict cannot reset foreign state");
    }
    for (bool resetting : {false, true}) {
        // Existing ownership has four read boundaries around two mutations.
        for (int boundary = 1; boundary <= 4; ++boundary) {
            Session session;
            Journal journal;
            Backend backend;
            backend.journal = &journal;
            Check(session.Apply(Zoom(), backend, journal) == Result::Applied, "prepare owned state");
            backend.reads = 0;
            backend.beforeRead = [boundary](int n, Snapshot& state) { if (n == boundary) state = foreign; };
            const Result result = resetting ? session.Reset(backend, journal) :
                                             session.Apply(Zoom(3, 200), backend, journal);
            Check(result == Result::Conflict, "foreign update/reset-boundary change is detected");
            const int writes = backend.writes;
            Check(!session.OwnsState() && !journal.armed && backend.state == foreign,
                  "update/reset conflict preserves foreign complete snapshot");
            session.Reset(backend, journal);
            Check(backend.writes == writes && backend.state == foreign, "repeated cleanup is harmless after transfer");
        }
    }
    for (int write = 1; write <= 2; ++write) {
        Session session;
        Journal journal;
        Backend backend;
        backend.journal = &journal;
        backend.effectWrite = write;
        backend.effect = WriteEffect::ForeignAfterChange;
        Check(session.Apply(Zoom(), backend, journal) == Result::Conflict,
              "foreign change immediately after either setter is detected");
        Check(backend.writes == write && backend.state == foreign && !journal.armed,
              "no later component is written after a foreign setter result");
    }
    Session session;
    Journal journal;
    Backend backend;
    backend.journal = &journal;
    session.Apply(Zoom(), backend, journal);
    backend.state.input = foreign.input; // Only input ownership changes.
    Check(session.Check(backend, journal) == Result::Conflict, "timer check detects input-only interference");
    Check(!session.OwnsState() && !journal.armed, "input-only conflict also relinquishes visual ownership");
}

void ReadFailures() {
    for (int boundary = 1; boundary <= 5; ++boundary) {
        Session session;
        Journal journal;
        Backend backend;
        backend.journal = &journal;
        backend.failRead = boundary;
        Check(session.Apply(Zoom(), backend, journal) == Result::ReadFailure, "every acquire read can fail safely");
        const int writes = backend.writes;
        Check(writes <= 2, "read failure stops the current operation");
        backend.failRead = 0;
        const Result reset = session.Reset(backend, journal);
        Check((reset == Result::Applied || reset == Result::Unchanged) && backend.state == identity,
              "known partial acquisition can reset after reads recover");
        Check(!journal.armed && !session.OwnsState(), "read recovery releases journal after full reset");
    }
    for (bool resetting : {false, true}) {
        for (int boundary = 1; boundary <= 4; ++boundary) {
            Session session;
            Journal journal;
            Backend backend;
            backend.journal = &journal;
            session.Apply(Zoom(), backend, journal);
            backend.reads = 0;
            backend.failRead = boundary;
            const Result result = resetting ? session.Reset(backend, journal) :
                                             session.Apply(Zoom(3, 200), backend, journal);
            Check(result == Result::ReadFailure && session.NeedsRecovery() && journal.armed,
                  "failed owned read retains the published recovery record");
            backend.failRead = 0;
            Check(session.Reset(backend, journal) == Result::Applied || backend.state == identity,
                  "partial update/reset can finish after read recovery");
            Check(backend.state == identity && !session.OwnsState() && !journal.armed,
                  "recovered read failure leaves complete baseline");
        }
    }
    // A read failure before a new setter must not authorize an old journal's
    // `before` state, because that old mutation was already verified complete.
    Session session;
    Journal journal;
    Backend backend;
    backend.journal = &journal;
    session.Apply(Zoom(), backend, journal);
    const Snapshot staleBefore = journal.history[journal.history.size() - 2].before;
    backend.failRead = backend.reads + 1;
    Check(session.Check(backend, journal) == Result::ReadFailure, "nonmutating ownership check can fail");
    backend.state = staleBefore;
    backend.failRead = 0;
    const int writes = backend.writes;
    Check(session.Reset(backend, journal) == Result::Conflict && backend.writes == writes,
          "read failure does not broaden established ownership to stale values");

    // If another owner appears while reads are unavailable, the next successful
    // observation must relinquish ownership instead of attempting repair.
    for (int boundary = 2; boundary <= 5; ++boundary) {
        Session pending;
        Journal pendingJournal;
        Backend pendingBackend;
        pendingBackend.journal = &pendingJournal;
        pendingBackend.failRead = boundary;
        pending.Apply(Zoom(), pendingBackend, pendingJournal);
        pendingBackend.state = foreign;
        pendingBackend.failRead = 0;
        const int oldWrites = pendingBackend.writes;
        Check(pending.Reset(pendingBackend, pendingJournal) == Result::Conflict,
              "foreign state after an unavailable read is never repaired");
        Check(pendingBackend.writes == oldWrites && pendingBackend.state == foreign && !pendingJournal.armed,
              "unknown state after a read outage is preserved");
    }
}

void WriteFailures() {
    for (WriteEffect effect : {WriteEffect::FailNoChange, WriteEffect::SucceedNoChange,
                               WriteEffect::FailAfterChange}) {
        for (int write = 1; write <= 2; ++write) {
            Session session;
            Journal journal;
            Backend backend;
            backend.journal = &journal;
            backend.effectWrite = write;
            backend.effect = effect;
            const Result result = session.Apply(Zoom(), backend, journal);
            Check(result == (effect == WriteEffect::FailAfterChange ? Result::Applied : Result::WriteFailure),
                  "native return code cannot substitute for observed postcondition");
            Check(journal.armed && session.NeedsRecovery(), "failed or completed write remains journaled while owned");
            backend.effectWrite = 0;
            const Result reset = session.Reset(backend, journal);
            Check((reset == Result::Applied || reset == Result::Unchanged) && backend.state == identity,
                  "partial write failure can restore recognized baseline");
        }
    }
    for (int write = 1; write <= 2; ++write) {
        Session session;
        Journal journal;
        Backend backend;
        backend.journal = &journal;
        session.Apply(Zoom(), backend, journal);
        backend.effectWrite = backend.writes + write;
        backend.effect = WriteEffect::FailNoChange;
        Check(session.Reset(backend, journal) == Result::WriteFailure && journal.armed,
              "failed reset keeps recovery armed");
        if (write == 1) Check(backend.state == Zoom(), "failed input-disable leaves visible zoom in place");
        backend.effectWrite = 0;
        Check(session.Reset(backend, journal) == Result::Applied && backend.state == identity && !journal.armed,
              "repeated reset finishes its known partial state");
    }
}

void CrashRecovery() {
    Session session;
    Journal journal;
    Backend backend;
    backend.journal = &journal;
    session.Apply(Zoom(), backend, journal);
    session.Apply(Zoom(3, 200), backend, journal);
    session.Reset(backend, journal);
    Check(journal.history.size() == 12, "each of six native writes has intent and verified-outcome records");
    for (const RecoveryRecord& record : journal.history) {
        // Parent may die immediately before or immediately after every setter.
        for (Snapshot atCrash : {record.before, record.after}) {
            Backend recovery;
            recovery.state = atCrash;
            const Result result = Recover(record, recovery);
            Check((result == Result::Applied || result == Result::Unchanged) && recovery.state == identity,
                  "every published crash boundary restores only recognized state");
        }
        for (Snapshot other : {foreign, Snapshot{record.after.visual, foreign.input},
                               Snapshot{foreign.visual, record.after.input}}) {
            Backend recovery;
            recovery.state = other;
            Check(Recover(record, recovery) == Result::Conflict && recovery.writes == 0 && recovery.state == other,
                  "watchdog refuses complete or component-only foreign state");
        }
        Backend unavailable;
        unavailable.state = record.after;
        unavailable.failRead = 1;
        Check(Recover(record, unavailable) == Result::ReadFailure && unavailable.writes == 0,
              "watchdog cannot write when current state is unreadable");

        Backend normalRecovery;
        normalRecovery.state = record.after;
        Recover(record, normalRecovery);
        for (int boundary = 1; boundary <= normalRecovery.reads; ++boundary) {
            Backend retry;
            Journal pending;
            pending.record = record;
            pending.armed = true;
            retry.journal = &pending;
            retry.state = record.after;
            retry.failRead = boundary;
            Check(Recover(record, retry, pending) == Result::ReadFailure,
                  "each recovery read failure preserves a pending record");
            retry.failRead = 0;
            const Result result = Recover(pending.record, retry, pending);
            Check((result == Result::Applied || result == Result::Unchanged) && retry.state == identity,
                  "recovery retries use the last intent or verified outcome");
            Check(!pending.armed, "successful recovery retry clears pending ownership");
        }
        for (int write = 1; write <= normalRecovery.writes; ++write) {
            Backend retry;
            Journal pending;
            pending.record = record;
            pending.armed = true;
            retry.journal = &pending;
            retry.state = record.after;
            retry.effectWrite = write;
            retry.effect = WriteEffect::FailNoChange;
            Check(Recover(record, retry, pending) == Result::WriteFailure && pending.armed,
                  "recovery setter failure keeps its latest recognized state");
            retry.effectWrite = 0;
            Check(Recover(pending.record, retry, pending) == Result::Applied && retry.state == identity,
                  "recovery setter retry can restore a known partial state");
        }
    }
    RecoveryRecord invalid = journal.history.front();
    invalid.baseline = foreign;
    Backend recovery;
    recovery.state = invalid.after;
    Check(Recover(invalid, recovery) == Result::Invalid && recovery.writes == 0,
          "invalid or nonneutral recovery baseline is rejected");
    invalid = {identity, identity, Zoom()};
    Check(Recover(invalid, recovery) == Result::Invalid && recovery.writes == 0,
          "a recovery record cannot claim two independent mutations happened atomically");
}

void ShortEventSequences() {
    // Exhaustively exercise 7^5 short command/event sequences. Each event is
    // observable from outside the class; after interference, it must stay
    // relinquished until the global state becomes neutral again.
    constexpr int alphabet = 7, length = 5;
    int count = 1;
    for (int i = 0; i < length; ++i) count *= alphabet;
    for (int sequence = 0; sequence < count; ++sequence) {
        Session session;
        Journal journal;
        Backend backend;
        backend.journal = &journal;
        int code = sequence;
        for (int step = 0; step < length; ++step, code /= alphabet) {
            const int event = code % alphabet;
            const int priorWrites = backend.writes;
            const Snapshot prior = backend.state;
            const bool previouslyOwned = session.OwnsState();
            Result result = Result::Unchanged;
            switch (event) {
            case 0: result = session.Apply(Zoom(), backend, journal); break;
            case 1: result = session.Apply(Zoom(3, 200), backend, journal); break;
            case 2: result = session.Reset(backend, journal); break;
            case 3: result = session.Check(backend, journal); break;
            case 4: backend.state = foreign; break;
            case 5: backend.state = identity; break;
            case 6:
                backend.failRead = backend.reads + 1;
                result = session.Check(backend, journal);
                backend.failRead = 0;
                break;
            }
            if (result == Result::Conflict) {
                Check(!session.OwnsState() && !journal.armed, "model: conflict relinquishes all ownership");
                Check(backend.state == prior && backend.writes == priorWrites,
                      "model: observable conflict is never overwritten");
            }
            if (event <= 3 && !previouslyOwned && prior == foreign) {
                Check(backend.state == foreign && backend.writes == priorWrites,
                      "model: idle session never takes a foreign owner");
            }
            if (result == Result::Applied) {
                Check(Valid(backend.state), "model: completed operation leaves valid state");
                Check(session.OwnsState() ? backend.state == session.Expected() : backend.state == identity,
                      "model: successful operation agrees with external state");
            }
            if (!session.OwnsState()) Check(!journal.armed, "model: released sessions never retain armed journal");
        }
    }
}
} // namespace

int Run_zoom_session() {
    HappyPath();
    ValidationAndAcquisition();
    ForeignOwnerAtEveryBoundary();
    ReadFailures();
    WriteFailures();
    CrashRecovery();
    ShortEventSequences();
    std::cout << "All " << assertions << " session assertions passed (including 16,807 event sequences).\n";
    return 0;
}
