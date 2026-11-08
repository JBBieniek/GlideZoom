#include <stdexcept>
#include "../GlideZoom/paint_pump_win32.h"
#include "../GlideZoom/frame_transaction.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

// Run the production queue adapter through isolated SDK stand-ins. These model
// documented filtering/dirty-region behavior, not Windows scheduling or DWM.
struct MockPaintWindow {
    bool live = true, dirty = false, internal = false;
};

namespace {
using glide::paint::Kind;
using glide::paint::Result;
std::uint64_t assertions = 0, scenarios = 0, reachedReentries = 0;

void Check(bool value, const char* message) {
    ++assertions;
    if (!value) {
        std::cerr << "FAIL paint service: " << message << " [scenario=" << scenarios << "]\n";
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}

enum class Op { IsWindow, Peek, Dispatch };
struct World {
    MockPaintWindow own{}, other{}, descendant{};
    glide::runtime::State lifecycle;
    bool current = true, forceMessage = false, invalidateDuringPaint = false;
    MSG forced{};
    LRESULT dispatchResult = 0;
    unsigned isCalls = 0, peekCalls = 0, dispatchCalls = 0, currentCalls = 0;
    std::vector<MSG> queue;
    std::function<void(Op, unsigned)> after;
    std::function<void()> onCurrent;
} world;

void Reset() {
    world = World{};
    Check(world.lifecycle.PrepareEnable() && world.lifecycle.Activate(), "fixture activates a real lifecycle");
    ++scenarios;
}

// Match a runtime callback boundary. LTCG must not specialize the production
// adapter around a fixture callback that is known to always succeed or throw.
// The real callback can change during sent-message reentry.
#ifdef _MSC_VER
__declspec(noinline)
#endif
bool Current() {
    ++world.currentCalls;
    if (world.onCurrent) world.onCurrent();
    return world.current;
}

Result Service() {
    glide::frame::Lease lease{world.lifecycle};
    return glide::paint::Service(&world.own, [&]() { return Current() && lease.Valid(); });
}

void After(Op operation, unsigned count) {
    // Copy so a reentrant action can replace itself without destroying the
    // callable currently executing on its own stack.
    const auto action = world.after;
    if (action) action(operation, count);
}

void AddInput(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
        MSG message{};
        message.hwnd = i % 2 ? &world.other : &world.own;
        message.message = i % 2 ? WM_KEYDOWN : WM_MOUSEMOVE;
        message.wParam = i;
        world.queue.push_back(message);
    }
}

void BasicDispatchAndFiltering() {
    Reset();
    const auto nullResult = glide::paint::Service(nullptr, []() { return Current(); });
    Check(nullResult.kind == Kind::Idle && world.isCalls == 0 && world.peekCalls == 0 &&
          world.dispatchCalls == 0 && world.currentCalls == 0,
          "null HWND never evaluates the frame callback or broadens the native filter");

    Reset();
    world.current = false;
    Check(Service().kind == Kind::Cancelled && world.isCalls == 0 && world.peekCalls == 0,
          "a cancelled frame cannot enter a native boundary");

    Reset();
    world.own.live = false;
    Check(Service().kind == Kind::Failed && world.isCalls == 1 && world.peekCalls == 0,
          "a dead private HWND fails before queue removal");

    Reset();
    world.other.dirty = true;
    AddInput(4096);
    Check(Service().kind == Kind::Idle && world.peekCalls == 1 && world.dispatchCalls == 0,
          "foreign paint and busy input never cause an invented private paint");
    Check(world.queue.size() == 4096 && world.other.dirty,
          "idle service preserves every unrelated input and dirty window");
    world.own.dirty = true;
    Check(Service().kind == Kind::Painted && world.peekCalls == 2 && world.dispatchCalls == 1,
          "paint-only filter services the dirty child despite a busy input queue");
    Check(!world.own.dirty && world.other.dirty && world.queue.size() == 4096,
          "only the requested private child's pending paint is processed");
    Check(Service().kind == Kind::Idle && world.dispatchCalls == 1,
          "a clean child is not repainted merely because a frame was serviced");

    for (LRESULT result : {LRESULT{0}, LRESULT{-1}, std::numeric_limits<LRESULT>::max()}) {
        Reset();
        world.own.dirty = true;
        world.dispatchResult = result;
        Check(Service().kind == Kind::Painted && world.dispatchCalls == 1,
              "window-procedure return value is never interpreted as native success/failure");
    }

    Reset();
    world.own.internal = true;
    Check(Service().kind == Kind::Painted && !world.own.internal && world.dispatchCalls == 1,
          "one already-pending internal paint is dispatched without inventing invalidation");
    Check(Service().kind == Kind::Idle && world.dispatchCalls == 1,
          "internal paint cannot turn into a private repaint loop");

    Reset();
    world.own.dirty = true;
    world.invalidateDuringPaint = true;
    for (unsigned frame = 0; frame < 1000; ++frame) {
        Check(Service().kind == Kind::Painted && world.own.dirty,
              "a control that remains dirty still gets only one paint per service");
        Check(world.peekCalls == frame + 1 && world.dispatchCalls == frame + 1,
              "new dirty work never creates a catch-up dispatch loop");
    }
    world.invalidateDuringPaint = false;
    Check(Service().kind == Kind::Painted && !world.own.dirty,
          "latest paint can clear the accumulated update region");
    Check(Service().kind == Kind::Idle, "no cached paint count survives a clean update region");
}

void UnexpectedMessagesAndQuit() {
    for (unsigned fault = 0; fault < 6; ++fault) {
        Reset();
        world.forceMessage = true;
        world.forced.hwnd = &world.own;
        world.forced.message = WM_PAINT;
        if (fault == 0) world.forced.hwnd = &world.other;
        if (fault == 1) world.forced.hwnd = nullptr;
        if (fault == 2) world.forced.message = WM_TIMER;
        if (fault == 3) world.forced.message = WM_KEYDOWN;
        if (fault == 4) world.forced.wParam = 1;
        if (fault == 5) world.forced.lParam = 1;
        Check(Service().kind == Kind::Failed && world.peekCalls == 1 && world.dispatchCalls == 0,
              "unexpected HWND/message/paint parameters are never dispatched");
    }
    Reset();
    world.descendant.dirty = true;
    Check(Service().kind == Kind::Failed && world.dispatchCalls == 0 && world.descendant.dirty,
          "a descendant returned by a native HWND filter is not mistaken for the owned control");

    for (int code : {0, 1, -1, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()})
    for (bool cancel : {false, true}) {
        Reset();
        AddInput(32);
        MSG quit{};
        quit.message = WM_QUIT;
        quit.wParam = static_cast<WPARAM>(code);
        world.queue.push_back(quit);
        world.own.dirty = true;
        if (cancel) world.after = [](Op op, unsigned) {
            if (op == Op::Peek) { world.lifecycle.Stop(); ++reachedReentries; }
        };
        const auto result = Service();
        Check(result.kind == Kind::Quit && result.quitCode == code,
              "WM_QUIT bypasses the paint filter and retains its exact exit code");
        Check(world.peekCalls == 1 && world.dispatchCalls == 0 && world.queue.size() == 32 && world.own.dirty,
              "removed quit is reported even when sent-message reentry cancels the frame");
    }
}

void CancellationMatrix() {
    struct Site { Op operation; unsigned ordinal; };
    const std::vector<Site> sites{{Op::IsWindow, 1}, {Op::Peek, 1},
                                 {Op::IsWindow, 2}, {Op::Dispatch, 1}, {Op::IsWindow, 3}};
    for (unsigned input : {0u, 1u, 64u, 4096u})
    for (bool dirty : {false, true})
    for (unsigned action = 0; action < 5; ++action)
    for (const auto site : sites) {
        Reset();
        AddInput(input);
        world.own.dirty = dirty;
        world.other.dirty = true;
        bool reached = false;
        world.after = [&](Op op, unsigned ordinal) {
            if (reached || op != site.operation || ordinal != site.ordinal) return;
            reached = true;
            ++reachedReentries;
            if (action == 0) world.lifecycle.Cancel();
            if (action == 1) world.lifecycle.Pause();
            if (action == 2) world.lifecycle.Suspend();
            if (action == 3) world.lifecycle.Fault();
            if (action == 4) world.lifecycle.Stop();
        };
        const auto result = Service();
        Check(result.kind == (reached ? Kind::Cancelled : (dirty ? Kind::Painted : Kind::Idle)),
              "real lifecycle epoch/phase cancellation fences each reached native boundary");
        const bool reachedDispatch = dirty && (site.operation == Op::Dispatch ||
                                                (site.operation == Op::IsWindow && site.ordinal == 3));
        Check(world.dispatchCalls == (reachedDispatch ? 1u : 0u),
              "cancelled calls never dispatch a later private paint");
        Check(world.peekCalls <= 1 && world.dispatchCalls <= 1 && world.queue.size() == input && world.other.dirty,
              "mixed cancellation preserves bounded work and every outside input/window");
    }

    for (Op site : {Op::IsWindow, Op::Peek, Op::Dispatch}) {
        Reset();
        world.own.dirty = true;
        world.after = [site](Op op, unsigned) {
            if (op == site) { world.current = false; world.own.live = false; }
        };
        Check(Service().kind == Kind::Cancelled, "lifetime callback cancellation wins over a dead HWND");
    }

    for (Op site : {Op::Peek, Op::Dispatch}) {
        Reset();
        world.own.dirty = true;
        world.after = [site](Op op, unsigned) { if (op == site) world.own.live = false; };
        Check(Service().kind == Kind::Failed,
              "a destroyed HWND with an erroneously current callback is still rejected");
    }

    Reset();
    world.own.dirty = true;
    world.after = [](Op op, unsigned) {
        if (op != Op::Peek) return;
        // The owner can return to Ready during sent-message reentry, but the
        // outer frame's captured epoch must not become valid again.
        world.lifecycle.Pause();
        Check(world.lifecycle.PrepareEnable() && world.lifecycle.Activate(), "reentry returns to Ready");
    };
    Check(Service().kind == Kind::Cancelled && world.dispatchCalls == 0,
          "a newer Ready generation cannot revive the cancelled outer paint");
}

void ReentryAndExceptionRecovery() {
    for (unsigned site = 0; site < 4; ++site) {
        Reset();
        world.own.dirty = world.other.dirty = true;
        bool called = false;
        const auto nested = [&]() {
            if (called) return;
            called = true;
            ++reachedReentries;
            const auto beforeIs = world.isCalls, beforePeek = world.peekCalls, beforeDispatch = world.dispatchCalls;
            const auto beforeCurrent = world.currentCalls;
            const auto inner = glide::paint::Service(&world.other, []() { return Current(); });
            Check(inner.kind == Kind::Cancelled && world.isCalls == beforeIs &&
                  world.peekCalls == beforePeek && world.dispatchCalls == beforeDispatch &&
                  world.currentCalls == beforeCurrent,
                  "nested service with a different callback type performs no callback or native work");
        };
        if (site == 0) world.onCurrent = nested;
        else world.after = [&, site](Op op, unsigned) {
            if ((site == 1 && op == Op::IsWindow) || (site == 2 && op == Op::Peek) ||
                (site == 3 && op == Op::Dispatch)) nested();
        };
        Check(Service().kind == Kind::Painted && called && world.peekCalls == 1 && world.dispatchCalls == 1,
              "outer paint completes once after bounded reentry");
        Check(world.other.dirty, "nested service never paints another control");
        world.onCurrent = {};
        world.after = {};
        world.own.dirty = true;
        Check(Service().kind == Kind::Painted && world.dispatchCalls == 2,
              "scope exit releases the per-thread guard for a later frame");
    }

    for (unsigned site = 0; site < 4; ++site) {
        Reset();
        world.own.dirty = true;
        if (site == 0) world.onCurrent = []() { throw std::runtime_error("current fault"); };
        else world.after = [site](Op op, unsigned) {
            if ((site == 1 && op == Op::IsWindow) || (site == 2 && op == Op::Peek) ||
                (site == 3 && op == Op::Dispatch)) throw std::runtime_error("native boundary fault");
        };
        Check(Service().kind == Kind::Failed, "exceptions cannot escape the noexcept adapter");
        world.onCurrent = {};
        world.after = {};
        world.own.dirty = true;
        Check(Service().kind == Kind::Painted, "exception unwinding does not latch a stuck service guard");
    }
}
} // namespace

BOOL IsWindow(HWND window) {
    ++world.isCalls;
    const bool known = window == &world.own || window == &world.other || window == &world.descendant;
    const BOOL result = known && window->live ? TRUE : FALSE;
    After(Op::IsWindow, world.isCalls);
    return result;
}

BOOL PeekMessageW(MSG* message, HWND window, UINT first, UINT last, UINT remove) {
    ++world.peekCalls;
    Check(message && window == &world.own && first == WM_PAINT && last == WM_PAINT && remove == PM_REMOVE,
          "native queue access is one exact private-window paint-only filter");
    After(Op::Peek, world.peekCalls); // Sent messages may run even with no matching queued paint.
    if (world.forceMessage) { *message = world.forced; return TRUE; }
    const auto queued = std::find_if(world.queue.begin(), world.queue.end(), [&](const MSG& candidate) {
        return candidate.message == WM_QUIT ||
            (candidate.hwnd == window && candidate.message >= first && candidate.message <= last);
    });
    if (queued != world.queue.end()) {
        *message = *queued;
        world.queue.erase(queued);
        return TRUE;
    }
    for (HWND candidate : {window, &world.descendant}) {
        if (candidate->dirty || candidate->internal) {
            *message = MSG{};
            message->hwnd = candidate;
            message->message = WM_PAINT;
            // A nonempty update region remains pending until the paint handler
            // validates it. An internal/empty-region paint is removed by Peek.
            if (!candidate->dirty) candidate->internal = false;
            return TRUE;
        }
    }
    return FALSE;
}

LRESULT DispatchMessageW(const MSG* message) {
    ++world.dispatchCalls;
    Check(message && message->hwnd == &world.own && message->message == WM_PAINT &&
          message->wParam == 0 && message->lParam == 0,
          "only exact private native paint messages reach dispatch");
    world.own.dirty = world.invalidateDuringPaint;
    world.own.internal = false;
    After(Op::Dispatch, world.dispatchCalls);
    return world.dispatchResult;
}

int Run_paint_native() {
    BasicDispatchAndFiltering();
    UnexpectedMessagesAndQuit();
    CancellationMatrix();
    ReentryAndExceptionRecovery();
    std::cout << "PASS: " << assertions << " native paint assertions; " << scenarios
              << " scenarios; " << reachedReentries
              << " reached reentry/cancellation cases; actual production private-paint queue adapter\n";
    return 0;
}
