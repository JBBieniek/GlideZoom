#include <stdexcept>
#include "../GlideZoom/frame_clock_win32.h"
#include "../GlideZoom/zoom_animation.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

// Exercise the real NativeClock class through isolated SDK call stand-ins.
// This verifies error/ownership/arming behavior; it does not emulate the OS
// message pump, DWM presentation, timer accuracy or real display latency.
struct MockTimer {
    bool live = true, armed = false, canceled = false;
    DWORD flags = 0;
    LONGLONG due = 0;
};

namespace {
using glide::timing::NativeClock;
using Poll = NativeClock::Poll;
std::uint64_t assertions = 0, scenarios = 0, injected = 0;
void Check(bool value, const char* message) {
    ++assertions;
    if (!value) {
        std::cerr << "FAIL native timing: " << message << " [scenario=" << scenarios << "]\n";
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}
void Near(double actual, double expected, const char* message) noexcept {
    Check(std::isfinite(actual) && std::abs(actual - expected) < 1e-12, message);
}
enum class Op { Frequency, Counter, Create, Arm, Cancel, Close };
struct World {
    LONGLONG frequency = 10000000, now = 1000000;
    bool failFrequency = false, failCounter = false, failArm = false, failCancel = false;
    std::array<DWORD, 2> createErrors{{ERROR_SUCCESS, ERROR_SUCCESS}};
    unsigned frequencyCalls = 0, counterCalls = 0, createCalls = 0, armCalls = 0, cancelCalls = 0, closeCalls = 0;
    DWORD error = ERROR_SUCCESS;
    std::array<MockTimer, 4> handles{};
    unsigned nextHandle = 0, live = 0;
    std::vector<DWORD> flags;
    std::vector<Op> operations;
    void Clean() const noexcept { Check(live == 0, "native wrapper leaves no timer handles alive after scope"); }
} world;

void Reset() {
    world.Clean();
    world = World{};
    ++scenarios;
}

void CreateAndCleanup() {
    Reset();
    {
        NativeClock clock;
        double dt = 99;
        Check(clock.Take(dt) == Poll::Failed && dt == 0 && !clock.Arm() && !clock.SetRate(30) && !clock.Rebase(),
              "stopped native clock cannot report a frame or arm a wait");
        Check(clock.Start(144), "normal start acquires a high-resolution timer");
        Check(world.frequencyCalls == 1 && world.counterCalls == 1 && world.createCalls == 1,
              "start samples clock once and creates exactly one timer");
        Check(world.flags.front() == 2 && world.live == 1 && clock.Handle(),
              "high-resolution timer is the sole owned handle");
        Check(clock.Take(dt) == Poll::Frame && dt == 0, "first frame is due without manufacturing elapsed time");
        Check(clock.Take(dt) == Poll::Idle && dt == 0, "same-time polling cannot replay a first frame");
        clock.Stop();
        Check(!clock.Handle() && world.live == 0 && world.cancelCalls == 1 && world.closeCalls == 1,
              "explicit stop cancels and closes the owned timer");
        const auto count = world.operations.size();
        clock.Stop();
        Check(world.operations.size() == count, "repeated stop never closes the same handle twice");
    }
    world.Clean();

    Reset();
    {
        NativeClock clock;
        Check(clock.Start() && clock.Start(240), "restart can replace an existing clock");
        Check(world.createCalls == 2 && world.cancelCalls == 1 && world.closeCalls == 1 && world.live == 1,
              "restart closes old native timer before retaining one new timer");
        Check(world.operations[world.operations.size() - 4] == Op::Close,
              "restart releases prior handle before querying and creating its replacement");
    }
    Check(world.cancelCalls == 2 && world.closeCalls == 2, "destructor releases the restarted timer exactly once");
    world.Clean();

    Reset();
    {
        NativeClock clock;
        Check(clock.Start(), "cancel-failure fixture starts");
        world.failCancel = true;
        clock.Stop();
        Check(world.cancelCalls == 1 && world.closeCalls == 1 && !clock.Handle(),
              "cancellation failure never prevents handle cleanup");
    }
    world.Clean();
}

void UnsupportedAndCreationFailures() {
    Reset();
    world.createErrors[0] = ERROR_INVALID_PARAMETER;
    {
        NativeClock clock;
        Check(clock.Start(144), "unsupported high-resolution flag falls back to an ordinary timer");
        Check(world.flags.size() == 2 && world.flags[0] == 2 && world.flags[1] == 0 && world.live == 1,
              "fallback changes only the unsupported high-resolution flag");
        double dt = 0;
        Check(clock.Take(dt) == Poll::Frame && clock.Arm() && clock.Handle()->due == -69445,
              "ordinary timer fallback preserves requested cadence without claiming wake accuracy");
    }
    world.Clean();

    for (DWORD error : {ERROR_ACCESS_DENIED, ERROR_NOT_ENOUGH_MEMORY, ERROR_NOT_SUPPORTED}) {
        Reset();
        world.createErrors[0] = error;
        {
            NativeClock clock;
            Check(!clock.Start() && !clock.Handle() && world.createCalls == 1,
                  "unrelated creation failure is surfaced without an unjustified fallback");
            double dt = 99;
            Check(clock.Take(dt) == Poll::Failed && dt == 0 && !clock.Arm(),
                  "failed start cannot use a configured schedule without its timer");
        }
        Check(world.cancelCalls == 0 && world.closeCalls == 0, "failed creation does not release an unowned handle");
        world.Clean();
    }
    Reset();
    world.createErrors = {{ERROR_INVALID_PARAMETER, ERROR_ACCESS_DENIED}};
    {
        NativeClock clock;
        Check(!clock.Start() && world.createCalls == 2 && !clock.Handle(), "ordinary fallback failure cleanly fails start");
    }
    world.Clean();
}

void QueryFailuresAndClockAnomalies() {
    for (unsigned failure = 0; failure < 5; ++failure) {
        Reset();
        if (failure == 0) world.failFrequency = true;
        if (failure == 1) world.frequency = 0;
        if (failure == 2) world.frequency = -1;
        if (failure == 3) world.failCounter = true;
        if (failure == 4) world.now = -1;
        {
            NativeClock clock;
            Check(!clock.Start() && !clock.Handle() && world.createCalls == 0,
                  "bad native frequency/counter fails before acquiring any handle");
        }
        world.Clean();
    }
    for (unsigned operation = 0; operation < 3; ++operation)
    for (bool negative : {false, true}) {
        Reset();
        {
            NativeClock clock;
            Check(clock.Start(), "runtime query-failure fixture starts");
            if (negative) world.now = -1;
            else world.failCounter = true;
            double dt = 99;
            if (operation == 0) Check(clock.Take(dt) == Poll::Failed && dt == 0, "Take rejects failing or negative QPC before elapsed conversion");
            if (operation == 1) Check(!clock.Arm() && world.armCalls == 0, "Arm rejects failing or negative QPC before a native wait call");
            if (operation == 2) Check(!clock.SetRate(30), "rate change rejects failing or negative QPC without reusing stale time");
        }
        world.Clean();
    }

    Reset();
    {
        NativeClock clock;
        Check(clock.Start(50), "rollback fixture starts");
        double dt = 0;
        Check(clock.Take(dt) == Poll::Frame, "rollback fixture consumes first frame");
        world.now += 200000;
        Check(clock.Take(dt) == Poll::Frame, "next frame samples positive elapsed time");
        Near(dt, 0.020, "QPC elapsed duration has fractional-second precision");
        world.now -= 50000;
        Check(clock.Take(dt) == Poll::Frame && dt == 0 && clock.Take(dt) == Poll::Idle,
              "backward QPC rebases once with zero elapsed and no repeated frames");
        world.now += 200000;
        Check(clock.Take(dt) == Poll::Frame, "normal timing resumes after backward counter");
        Near(dt, 0.020, "rollback does not accrue old clock time into the next frame");
    }
    world.Clean();
}

void RelativeArmingAndCoalescing() {
    Reset();
    {
        NativeClock clock;
        Check(clock.Start(144) && clock.Arm(), "initial due frame can arm a short wake-up");
        Check(clock.Handle()->due == -1 && clock.Handle()->armed,
              "already-due wake is a negative relative 100 ns one-shot");
        double dt = 0;
        Check(clock.Take(dt) == Poll::Frame && clock.Arm(), "consumed frame arms its next cadence deadline");
        Check(clock.Handle()->due == -69445, "144Hz display request arms its own fractional refresh period");
        world.now += 69400;
        Check(clock.Take(dt) == Poll::Idle && clock.Arm() && clock.Handle()->due == -45,
              "early/native-message wake arms only the remaining fractional period");
        const auto arms = world.armCalls;
        world.failArm = true;
        Check(!clock.Arm() && world.armCalls == arms + 1, "SetWaitableTimer failure is surfaced to the caller");
        world.failArm = false;
        world.now += 20000000; // A UI stall spanning hundreds of frame deadlines.
        Check(clock.Take(dt) == Poll::Frame, "late native wake processes one current frame");
        Near(dt, 2.00694, "elapsed duration exposes the real stall to app stale-input handling");
        for (unsigned i = 0; i < 1000; ++i)
            Check(clock.Take(dt) == Poll::Idle && dt == 0, "late wake cannot replay a backlog on repeated queue polls");
        Check(clock.Arm() && clock.Handle()->due < 0 && clock.Handle()->due >= -69445,
              "stalled clock rearms one future slot without a catch-up burst");
    }
    world.Clean();
}

void RefreshAndRestartAfterFailure() {
    Reset();
    {
        NativeClock clock;
        Check(clock.Start(30), "refresh-change fixture starts");
        double dt = 0;
        Check(clock.Take(dt) == Poll::Frame, "initial rate frame consumed");
        const auto counters = world.counterCalls;
        Check(clock.SetRate(30) && world.counterCalls == counters, "unchanged monitor rate does not reset the current schedule");
        world.now += 1000;
        Check(clock.SetRate(240) && clock.Take(dt) == Poll::Frame && clock.Arm(),
              "monitor rate change becomes eligible immediately and rearms one new schedule");
        Check(clock.Handle()->due == -41667 && world.createCalls == 1,
              "rate change retains one handle and switches to its new precise period");
        Check(clock.SetRate(1000000), "large display frequency is safely capped");
        Check(clock.Take(dt) == Poll::Idle && clock.Arm() && clock.Handle()->due == -41667,
              "rate capped to the current value does not manufacture another frame");
        for (double invalid : {0.0, -1.0, std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity()}) {
            Check(clock.SetRate(invalid), "invalid monitor frequency uses the safe default cadence");
            const auto result = clock.Take(dt);
            Check(result != Poll::Failed && clock.Arm() && clock.Handle()->due == -166667,
                  "default refresh cadence remains bounded and usable");
        }
        world.failCounter = true;
        Check(!clock.SetRate(30), "faulted refresh change is visible to caller");
        world.failCounter = false;
        world.now += 166667;
        Check(clock.Take(dt) == Poll::Frame, "failed refresh query preserves the previous usable cadence until caller cleanup");
        world.failFrequency = true;
        Check(!clock.Start() && !clock.Handle() && world.live == 0,
              "failed restart first releases the old timer and leaves a stopped clock");
        world.failFrequency = false;
        Check(clock.Start(144), "a stopped clock can start cleanly after an earlier failure");
    }
    world.Clean();
}

struct RateCase { double request; LONGLONG interval; };
// Exact expected 100ns periods at the fixture's 10MHz QPC frequency. Fractional
// display rates are rounded up in QPC units so a frame is never scheduled early.
constexpr std::array<RateCase, 12> rates{{
    {0.5, 10000000}, {1.0, 10000000}, {30.0, 333334}, {59.94, 166834},
    {60.0, 166667}, {90.0, 111112}, {119.88, 83417}, {120.0, 83334},
    {144.0, 69445}, {165.0, 60607}, {240.0, 41667}, {1000000.0, 41667}
}};

void SelectedMonitorCadence() {
    for (const auto rate : rates) {
        Reset();
        {
            NativeClock clock;
            Check(clock.Start(rate.request), "production clock accepts the selected display refresh request");
            double dt = 0;
            Check(clock.Take(dt) == Poll::Frame && clock.Arm() && clock.Handle()->due == -rate.interval,
                  "Start follows selected refresh between1Hz and240Hz, including fractional requests");
            const auto counters = world.counterCalls;
            Check(clock.SetRate(rate.request) && world.counterCalls == counters,
                  "Start and SetRate use the same normalization without resetting an unchanged deadline");
            const double different = rate.request == 30.0 ? 50.0 : 30.0;
            const LONGLONG differentInterval = rate.request == 30.0 ? 200000 : 333334;
            Check(clock.SetRate(different) && clock.Take(dt) == Poll::Frame && clock.Arm() &&
                  clock.Handle()->due == -differentInterval,
                  "moving to a different display changes one current schedule");
            Check(clock.SetRate(rate.request) && clock.Take(dt) == Poll::Frame && clock.Arm() &&
                  clock.Handle()->due == -rate.interval,
                  "moving back restores the selected display cadence without replacing the timer");
            Check(world.createCalls == 1 && world.live == 1, "monitor changes reuse the one owned timer");
            if (rate.request > 60.0)
                Check(-clock.Handle()->due < 166667, "high refresh is not silently limited to the old60Hz period");
        }
        world.Clean();
    }
}

void HighRefreshSkipsObsoleteFrames() {
    for (const auto rate : {RateCase{120.0, 83334}, RateCase{144.0, 69445}, RateCase{240.0, 41667}}) {
        Reset();
        {
            NativeClock clock;
            Check(clock.Start(rate.request), "high-refresh stall fixture starts");
            double dt = 0;
            Check(clock.Take(dt) == Poll::Frame, "consume first high-refresh frame");
            const auto origin = world.now;
            for (LONGLONG frame = 1; frame <= 120; ++frame) {
                world.now = origin + frame * rate.interval - 1;
                Check(clock.Take(dt) == Poll::Idle && dt == 0,
                      "queue wake one QPC tick before deadline never submits early");
                Check(clock.Arm() && clock.Handle()->due == -1,
                      "early high-refresh wake arms the remaining100ns without resetting phase");
                ++world.now;
                Check(clock.Take(dt) == Poll::Frame, "every selected high-refresh deadline is eligible");
                Near(dt, static_cast<double>(rate.interval) / 10000000.0,
                     "high-refresh elapsed interval reflects current cadence");
                Check(clock.Take(dt) == Poll::Idle && dt == 0,
                      "repeated message polling cannot duplicate the same high-refresh frame");
            }
            world.now += 937 * rate.interval + rate.interval / 3;
            Check(clock.Take(dt) == Poll::Frame, "multi-second high-refresh stall emits one current frame");
            Near(dt, static_cast<double>(937 * rate.interval + rate.interval / 3) / 10000000.0,
                 "late frame reports actual stall duration for stale-input cancellation");
            for (unsigned poll = 0; poll != 1000; ++poll)
                Check(clock.Take(dt) == Poll::Idle && dt == 0,
                      "hundreds of missed high-refresh slots never become a queued replay burst");
            const auto remaining = rate.interval - rate.interval / 3;
            Check(clock.Arm() && clock.Handle()->due == -remaining,
                  "late high-refresh wake retains phase and arms only the next future slot");
            world.now += remaining;
            Check(clock.Take(dt) == Poll::Frame && clock.Arm() && clock.Handle()->due == -rate.interval,
                  "normal cadence resumes immediately after skipped backlog");
            Check(world.createCalls == 1 && world.live == 1, "stall recovery does not create extra timers");
        }
        world.Clean();
    }
}

void CaptureRebase() {
    for (const auto rate : {RateCase{60.0, 166667}, RateCase{120.0, 83334},
                            RateCase{144.0, 69445}, RateCase{240.0, 41667}}) {
      Reset();
      {
        NativeClock clock;
        Check(clock.Start(rate.request), "capture rebase fixture starts");
        double dt = 0;
        Check(clock.Take(dt) == Poll::Frame && clock.Arm(), "capture fixture establishes a live prior deadline");
        world.now += 20000000; // App paused ServiceFrame during screenshot work.
        Check(clock.Rebase(), "capture completion starts a fresh elapsed-time origin");
        Check(clock.Take(dt) == Poll::Frame && dt == 0 && clock.Take(dt) == Poll::Idle,
              "capture-sized interval is discarded once with no queued frames");
        const double selected = glide::animation::Quantize(glide::animation::StepZoom(1, 120, 16), 16);
        Check(selected == 1.01, "fresh key command reaches its exact0.01x step on the first resumed frame");
        Check(clock.Arm() && clock.Handle()->due == -rate.interval,
              "capture rebasing preserves the selected high-refresh period");
        world.now += rate.interval;
        Check(clock.Take(dt) == Poll::Frame, "first normal post-capture interval produces a frame");
        Near(dt, static_cast<double>(rate.interval) / 10000000.0,
             "post-capture elapsed time is one selected refresh interval");
        Check(glide::animation::Quantize(selected, 16) == selected,
              "later post-capture frames cannot animate past the already-selected endpoint");
        Check(world.createCalls == 1 && world.live == 1, "capture rebasing does not replace or leak the native timer");
      }
      world.Clean();
    }

    for (bool negative : {false, true}) {
        Reset();
        {
            NativeClock clock;
            Check(clock.Start(50), "failed-rebase fixture starts");
            double dt = 0;
            Check(clock.Take(dt) == Poll::Frame, "failed-rebase fixture consumes initial frame");
            if (negative) world.now = -1;
            else world.failCounter = true;
            Check(!clock.Rebase(), "rebase reports failing/negative native clock to its capture caller");
            world.failCounter = false;
            world.now = 1200000;
            Check(clock.Take(dt) == Poll::Frame, "a failed rebase did not secretly replace its previous valid schedule");
            Near(dt, 0.020, "failed rebase preserves the last known elapsed origin");
        }
        world.Clean();
    }
}
} // namespace

BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency) noexcept {
    world.operations.push_back(Op::Frequency);
    ++world.frequencyCalls;
    Check(frequency != nullptr, "frequency call supplies writable storage");
    if (world.failFrequency) { ++injected; return FALSE; }
    frequency->QuadPart = world.frequency;
    return TRUE;
}
BOOL QueryPerformanceCounter(LARGE_INTEGER* counter) noexcept {
    world.operations.push_back(Op::Counter);
    ++world.counterCalls;
    Check(counter != nullptr, "counter call supplies writable storage");
    if (world.failCounter) { ++injected; return FALSE; }
    counter->QuadPart = world.now;
    return TRUE;
}
HANDLE CreateWaitableTimerExW(void* security, const wchar_t* name, DWORD flags, DWORD access) noexcept {
    world.operations.push_back(Op::Create);
    Check(!security && !name, "timer is private, unnamed and does not request inheritable security attributes");
    Check(flags == 0 || flags == 2, "timer is auto-reset and never uses the manual-reset flag");
    Check(access == (TIMER_MODIFY_STATE | SYNCHRONIZE), "timer requests only modify-state and synchronization access");
    world.flags.push_back(flags);
    const auto call = world.createCalls++;
    const DWORD error = call < world.createErrors.size() ? world.createErrors[call] : ERROR_SUCCESS;
    if (error != ERROR_SUCCESS) { ++injected; world.error = error; return nullptr; }
    Check(world.nextHandle < world.handles.size(), "test handle storage remains bounded");
    auto* handle = &world.handles[world.nextHandle++];
    *handle = MockTimer{};
    handle->flags = flags;
    ++world.live;
    return handle;
}
DWORD GetLastError() noexcept { return world.error; }
BOOL CancelWaitableTimer(HANDLE handle) noexcept {
    world.operations.push_back(Op::Cancel);
    ++world.cancelCalls;
    Check(handle && handle->live, "only live owned timer handles are canceled");
    handle->canceled = true;
    handle->armed = false;
    if (world.failCancel) { ++injected; return FALSE; }
    return TRUE;
}
BOOL CloseHandle(HANDLE handle) noexcept {
    world.operations.push_back(Op::Close);
    ++world.closeCalls;
    Check(handle && handle->live && handle->canceled, "timer is canceled before its single close");
    handle->live = false;
    --world.live;
    return TRUE;
}
BOOL SetWaitableTimer(HANDLE handle, const LARGE_INTEGER* due, LONG period,
                      PTIMERAPCROUTINE callback, void* context, BOOL resume) noexcept {
    world.operations.push_back(Op::Arm);
    ++world.armCalls;
    Check(handle && handle->live && due, "arming uses a live handle and valid due-time storage");
    Check(due->QuadPart < 0 && due->QuadPart >= -10000000,
          "native due time is a bounded relative wait rather than UTC wall time");
    Check(period == 0 && !callback && !context && resume == FALSE,
          "native timer is one-shot without APCs, worker callbacks or waking a sleeping computer");
    if (world.failArm) { ++injected; return FALSE; }
    handle->armed = true;
    handle->due = due->QuadPart;
    return TRUE;
}

int Run_latency_native() {
    CreateAndCleanup(); UnsupportedAndCreationFailures(); QueryFailuresAndClockAnomalies();
    RelativeArmingAndCoalescing(); RefreshAndRestartAfterFailure(); SelectedMonitorCadence();
    HighRefreshSkipsObsoleteFrames(); CaptureRebase();
    world.Clean();
    std::cout << "PASS: " << assertions << " native clock assertions; " << scenarios
              << " isolated scenarios; " << injected << " injected API failures; actual production NativeClock\n";
    return 0;
}
