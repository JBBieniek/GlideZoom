#include "../src/window_fit.h"

#include <iostream>
#include <stdexcept>

namespace
{
    using namespace glide::window_fit;
    unsigned assertions = 0;
    void Check(bool passed, const char* message)
    {
        ++assertions;
        if (!passed)
            throw std::runtime_error(message);
    }
    const Request viewport{ { -1800, 120, -840, 660 }, { -1920, 0, 0, 1080 } };
    Observation Window()
    {
        Observation o;
        o.identity = { 42, 100, 200 };
        o.outer = { -1708, 72, -892, 688 };
        o.visible = { -1700, 80, -900, 680 };
        o.monitor = viewport.monitor;
        return o;
    }
    struct Native
    {
        Observation observation = Window();
        Reason failure = Reason::Ready;
        unsigned reads = 0, writes = 0;
        bool failWrite = false, changeOnReread = false;
        Reason queueFailure = Reason::Ready;
        Operation* cancelOperation = nullptr;
        unsigned cancelOnRead = 0;
        bool cancelOnQueue = false, cancelAfterQueue = false;
        bool* invalidateOnQueue = nullptr;
        Rect queued{};
        Identity queuedIdentity{};
        Reason Read(Identity, Observation& output, std::uint32_t& error) noexcept
        {
            ++reads;
            if (failure != Reason::Ready)
            {
                error = 5;
                return failure;
            }
            output = observation;
            if (changeOnReread && reads == 2)
                ++output.identity.window;
            if (cancelOperation && reads == cancelOnRead)
                cancelOperation->Abandon(Reason::UnavailableDesktop);
            return Reason::Ready;
        }
        Reason Queue(const Observation& expected, Rect rect, std::uint32_t& error, ValidityCallback valid, void* context) noexcept
        {
            if (queueFailure != Reason::Ready)
                return queueFailure;
            if (cancelOperation && cancelOnQueue)
                cancelOperation->Abandon(Reason::UnavailableDesktop);
            if (invalidateOnQueue)
                *invalidateOnQueue = false;
            if (valid && !valid(context))
                return Reason::ContextChanged;
            ++writes;
            queuedIdentity = expected.identity;
            queued = rect;
            if (failWrite)
                error = 5;
            if (cancelOperation && cancelAfterQueue)
                cancelOperation->Abandon(Reason::UnavailableDesktop);
            return failWrite ? Reason::NativeFailure : Reason::Ready;
        }
    };
}

int main()
{
    using namespace glide::window_fit;
    {
        Rect output{ 1, 2, 3, 4 };
        Check(MakeOuter(Window().outer, Window().visible, viewport.source, output), "physical DWM inset conversion succeeds");
        Check(output == Rect{ -1808, 112, -832, 668 }, "negative-origin source maps to visible frame without DPI scaling twice");
        const Rect previous = output;
        Check(!MakeOuter({ 0, 0, 100, 100 }, { -1, 0, 100, 100 }, viewport.source, output) && output == previous,
              "inconsistent frame measurements cannot mutate plan output");
        Check(!MakeOuter({ -1000, -1000, 1000, 1000 }, { 0, 0, 100, 100 }, viewport.source, output) && output == previous,
              "unbounded custom frame inset rejected atomically");
        Check(Valid(viewport), "source viewport contained on negative-origin monitor");
        Check(!Valid(Request{ viewport.monitor, viewport.monitor }), "identity viewport cannot resize a window fullscreen");
        Check(!Valid(Request{ { -2000, 0, -1000, 500 }, viewport.monitor }), "source cannot cross monitor boundary");
        Check(!Valid(Rect{ INT32_MIN, 0, INT32_MAX, 100 }), "malformed geometry rejected without signed overflow");
    }
    {
        const Observation original = Window();
        Check(Eligible(original) == Reason::Ready, "normal movable resizable root allowed");
        for (unsigned flag = 0; flag < 19; ++flag)
        {
            Observation o = original;
            Reason expected = Reason::NotResizable;
            switch (flag)
            {
            case 0:
                o.currentDesktop = false;
                expected = Reason::UnavailableDesktop;
                break;
            case 1:
                o.sameSession = false;
                expected = Reason::UnavailableDesktop;
                break;
            case 2:
                o.ownWindow = true;
                expected = Reason::OwnWindow;
                break;
            case 3:
                o.foreground = false;
                expected = Reason::NoWindow;
                break;
            case 4:
                o.visibleWindow = false;
                break;
            case 5:
                o.cloaked = true;
                break;
            case 6:
                o.iconic = true;
                break;
            case 7:
                o.rootWindow = false;
                break;
            case 8:
                o.ownedWindow = true;
                break;
            case 9:
                o.thickFrame = false;
                break;
            case 10:
                o.caption = false;
                break;
            case 11:
                o.enabled = false;
                break;
            case 12:
                o.moveAllowed = false;
                break;
            case 13:
                o.sizeAllowed = false;
                break;
            case 14:
                o.maximized = true;
                expected = Reason::Maximized;
                break;
            case 15:
                o.hung = true;
                expected = Reason::Hung;
                break;
            case 16:
                o.sameUser = false;
                expected = Reason::UnavailableDesktop;
                break;
            case 17:
                o.identity.process = 0;
                expected = Reason::NoWindow;
                break;
            case 18:
                o.identity.thread = 0;
                expected = Reason::NoWindow;
                break;
            }
            Check(Eligible(o) == expected, "eligibility flag prevents native request");
            Native native;
            native.observation = o;
            Operation operation;
            Check(operation.Begin(native, viewport, 100).reason == expected && native.writes == 0,
                  "ineligible window never reaches native position request");
        }
        for (bool borderless : { false, true })
        {
            auto o = original;
            o.visible = viewport.monitor;
            o.outer = viewport.monitor;
            o.caption = !borderless;
            o.thickFrame = !borderless;
            Check(Eligible(o) == Reason::Fullscreen, "monitor-filling windows rejected with or without standard frame styles");
        }
        Check(CoversMonitor({ -1918, 2, -2, 1078 }, viewport.monitor), "fullscreen classification tolerates two physical edge pixels");
        Check(!CoversMonitor({ -1917, 3, -3, 1077 }, viewport.monitor), "nonfullscreen gap outside tolerance retained");
    }
    {
        Native native;
        Operation operation;
        const Result begun = operation.Begin(native, viewport, 100);
        Check(begun.reason == Reason::Pending && operation.Pending() && native.writes == 1 && native.reads == 2,
              "coherent double observation queues exactly one async resize");
        Check(native.queued == Rect{ -1808, 112, -832, 668 }, "native receives outer HWND bounds, not visible or destination rectangle");
        Check(native.queuedIdentity == native.observation.identity, "only selected foreground identity may be changed");
        Check(operation.Begin(native, viewport, 101).reason == Reason::Busy && native.writes == 1,
              "repeat shortcut cannot queue competing mutation");
        Check(operation.Poll(native, 599).reason == Reason::Pending && native.writes == 1, "polling does not force application size");
        native.observation.outer = native.queued;
        native.observation.visible = viewport.source;
        const Result fitted = operation.Poll(native, 600);
        Check(fitted.reason == Reason::Fitted && fitted.actualKnown && fitted.actualVisible == viewport.source && !operation.Pending(),
              "success requires actual visible source-sized frame, even exactly at deadline");
        Check(operation.Poll(native, 999).reason == Reason::Fitted && native.writes == 1, "terminal polling cannot resize again");
    }
    {
        Native native;
        Operation operation;
        native.changeOnReread = true;
        Check(operation.Begin(native, viewport, 100).reason == Reason::ContextChanged && native.writes == 0,
              "foreground identity change before mutation has no side effect");
        native = {};
        native.failWrite = true;
        Check(operation.Begin(native, viewport, 100).reason == Reason::NativeFailure && native.writes == 1 && !operation.Pending(),
              "native failure ends ownership without automatic retry");
        Check(operation.Poll(native, 101).reason == Reason::NativeFailure && native.writes == 1,
              "failed native write never triggers rollback or resend");
    }
    for (const Reason rejected : { Reason::ContextChanged, Reason::UnavailableDesktop, Reason::NoWindow })
    {
        Native native;
        native.queueFailure = rejected;
        Operation operation;
        const Result result = operation.Begin(native, viewport, 100);
        Check(result.reason == rejected && !result.actualKnown && native.reads == 2 && native.writes == 0 && !operation.Pending(),
              "last-moment native identity/desktop/fullscreen guard can reject without mutation or retry");
    }
    for (unsigned cancelledRead : { 1u, 2u })
    {
        Native native;
        Operation operation;
        native.cancelOperation = &operation;
        native.cancelOnRead = cancelledRead;
        const Result result = operation.Begin(native, viewport, 100);
        Check(result.reason == Reason::UnavailableDesktop && !result.actualKnown && !operation.Pending() && native.writes == 0,
              "emergency abandonment during either Begin read prevents queued resize and preserves cancellation result");
    }
    {
        Native native;
        Operation operation;
        native.cancelOperation = &operation;
        native.cancelOnRead = 3;
        Check(operation.Begin(native, viewport, 100).reason == Reason::Pending, "poll cancellation scenario queued once");
        native.observation.visible = viewport.source;
        const Result result = operation.Poll(native, 110);
        Check(result.reason == Reason::UnavailableDesktop && !result.actualKnown && !operation.Pending() && native.writes == 1,
              "abandonment dispatched by Poll read cannot be overwritten by a stale successful geometry read");
    }
    for (bool afterWrite : { false, true })
    {
        Native native;
        Operation operation;
        native.cancelOperation = &operation;
        native.cancelOnQueue = !afterWrite;
        native.cancelAfterQueue = afterWrite;
        const Result result = operation.Begin(native, viewport, 100);
        Check(result.reason == Reason::UnavailableDesktop && !result.actualKnown && !operation.Pending() &&
                  native.writes == (afterWrite ? 1u : 0u),
              "native Queue checks cancellation before write and never recreates pending ownership after reentrant abandonment");
    }
    {
        struct Validity
        {
            bool valid = true;
            unsigned calls = 0;
            Operation* cancel = nullptr;
        } validity;
        const auto check = [](void* context) noexcept {
            auto& state = *static_cast<Validity*>(context);
            ++state.calls;
            if (state.cancel)
                state.cancel->Abandon(Reason::UnavailableDesktop);
            return state.valid;
        };
        Native native;
        Operation operation;
        validity.valid = false;
        Check(operation.Begin(native, viewport, 100, check, &validity).reason == Reason::ContextChanged && native.reads == 0 && native.writes == 0,
              "invalid stack-only frame lease denies initial fit before native reads");
        validity.valid = true;
        native.invalidateOnQueue = &validity.valid;
        Check(operation.Begin(native, viewport, 100, check, &validity).reason == Reason::ContextChanged && native.writes == 0 && !operation.Pending(),
              "frame lease loss in final native queue guard prevents SetWindowPos");
        native = {};
        validity.valid = true;
        validity.cancel = &operation;
        Check(operation.Begin(native, viewport, 100, check, &validity).reason == Reason::UnavailableDesktop && native.writes == 0,
              "predicate-triggered abandonment is rechecked after predicate returns true");
        validity.cancel = nullptr;
        Check(operation.Begin(native, viewport, 100, check, &validity).reason == Reason::Pending, "fresh fit after abandonment starts under new revision");
        const unsigned calls = validity.calls;
        validity.valid = false;
        native.observation.visible = viewport.source;
        Check(operation.Poll(native, 110).reason == Reason::Fitted && validity.calls == calls,
              "Poll retains geometry/identity only and never invokes expired Begin frame-lease callback");
    }
    for (unsigned outcome = 0; outcome < 6; ++outcome)
    {
        Native native;
        Operation operation;
        Check(operation.Begin(native, viewport, 100).reason == Reason::Pending, "failure scenario begins with one request");
        Reason expected = Reason::TimedOut;
        switch (outcome)
        {
        case 0:
            break;
        case 1:
            native.observation.visible.right -= 100;
            expected = Reason::AppConstrained;
            break;
        case 2:
            ++native.observation.identity.thread;
            expected = Reason::ContextChanged;
            break;
        case 3:
            native.observation.currentDesktop = false;
            expected = Reason::UnavailableDesktop;
            break;
        case 4:
            native.failure = Reason::NoWindow;
            expected = Reason::NoWindow;
            break;
        case 5:
            native.observation.foreground = false;
            expected = Reason::NoWindow;
            break;
        }
        const auto result = operation.Poll(native, 600);
        Check(result.reason == expected && !operation.Pending() && native.writes == 1,
              "refusal, focus loss, desktop loss and identity loss never force or restore foreign geometry");
        operation.Poll(native, 1000);
        Check(native.writes == 1, "no mutation follows failed verification");
    }
    {
        Native native;
        Operation operation;
        operation.Begin(native, viewport, 100);
        Check(operation.Poll(native, 99).reason == Reason::TimedOut && !operation.Pending(), "backward timestamp cannot extend pending lifetime");
        native = {};
        native.observation.visible = viewport.source;
        native.observation.outer = viewport.source;
        Check(operation.Begin(native, viewport, 100).reason == Reason::Fitted && native.writes == 0,
              "already exact visible frame needs no native resize");
    }
    std::cout << "PASS window_fit: " << assertions << " assertions\n";
    return 0;
}
