#include <stdexcept>
#include "../GlideZoom/monitor_magnifier.h"
#include "../GlideZoom/monitor_presenter.h"
#include "../GlideZoom/cursor_visibility.h"
#include "../GlideZoom/cursor_visibility_win32.h"
#include "../GlideZoom/frame_transaction.h"
#include "../GlideZoom/ui_text_cache.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Compile the actual native renderer against a private, deterministic SDK
// boundary. This verifies call ordering, partial effects, ownership and cache
// repair. It cannot measure Windows compositor latency or cursor appearance.
struct MockRendererWindow {
    HWND parent = nullptr;
    HWND predecessor = nullptr;
    WNDPROC procedure = nullptr;
    LONG_PTR userData = 0;
    RECT local{};
    DWORD style = 0, extendedStyle = 0;
    bool alive = true, visible = false, destroying = false, layered = false;
    bool excluded = false, dirty = false;
    unsigned invalidations = 0, paints = 0;
    RECT source{};
    MagnificationApi::WindowTransform matrix{};
};

namespace {
enum class Op {
    Register, ClassInfo, Unregister, CreateHost, CreateChild, UserData,
    Layered, Filter, HostPosition, ChildPosition, Show, Hide, Destroy, ZOrder, Raise,
    HostRect, ChildRect, OccluderRect, Source, Matrix, ReadSource, ReadMatrix, Invalidate, Paint,
    OpenDesktop, DesktopName, CloseDesktop, ReadVisual, ReadInput, Visual, Input,
    ReadCursor, HideCursor, ShowCursor
};
enum class Effect { Before, After, Mismatch, DesktopAfter };
struct Fault {
    Op op = Op::Register;
    unsigned ordinal = 0;
    Effect effect = Effect::Before;
    bool reached = false;
};
struct Boundary {
    std::vector<std::unique_ptr<MockRendererWindow>> windows;
    MockRendererWindow externalTopmost;
    bool externalAbove = false;
    bool persistentAbove = false;
    std::map<Op, unsigned> calls;
    std::vector<Op> trace;
    Fault fault;
    DWORD error = ERROR_SUCCESS;
    WNDCLASSEXW registered{};
    bool hasClass = false, foreignClass = false, inputDesktop = true;
    unsigned openDesktops = 0;
    unsigned childSizeCalls = 0, childResizeEffects = 0;
    std::vector<UINT> childPositionFlags;
    bool enforceInputBeforeShow = false;
    bool enforceCursorJournal = false, cursorRecovery = false;
    bool systemCursorVisible = true;
    DWORD cursorFlags = CURSOR_SHOWING;
    std::function<void()> callback;
    Op callbackOp = Op::Register;
    unsigned callbackOrdinal = 0;
    bool callbackAfter = false, callbackReached = false;
    glide::session::Snapshot global{};
    void ResetCounts() {
        calls.clear(); trace.clear(); fault = {};
        childSizeCalls = childResizeEffects = 0;
        childPositionFlags.clear();
    }
    bool Hit(Op op) {
        const unsigned number = ++calls[op];
        trace.push_back(op);
        Invoke(op, false);
        if (!fault.reached && fault.ordinal && fault.op == op && fault.ordinal == number) {
            fault.reached = true;
            error = ERROR_GEN_FAILURE;
            return true;
        }
        return false;
    }
    void Invoke(Op op, bool after) {
        if (callback && callbackOp == op && callbackAfter == after &&
            calls[op] == callbackOrdinal) {
            callbackReached = true;
            auto action = std::move(callback);
            callback = {};
            action();
        }
    }
    void Arm(Op op, Effect effect = Effect::Before, unsigned ordinal = 1) {
        ResetCounts();
        fault = {op, ordinal, effect, false};
    }
    HWND Host() const {
        for (const auto& window : windows)
            if (window->alive && !window->parent) return window.get();
        return nullptr;
    }
    HWND Child() const {
        for (const auto& window : windows)
            if (window->alive && window->parent) return window.get();
        return nullptr;
    }
    unsigned Alive() const {
        unsigned count = 0;
        for (const auto& window : windows) count += window->alive ? 1U : 0U;
        return count;
    }
} boundary;
unsigned assertions = 0, scenarios = 0, faultScenarios = 0, reachedFaults = 0;
unsigned reentryScenarios = 0, reachedReentries = 0;
unsigned textCacheScenarios = 0;

void Check(bool condition, const std::string& message) {
    ++assertions;
    if (!condition) {
        std::cerr << "FAIL: " << message << " (scenario " << scenarios << ")\n";
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}
bool Same(RECT a, RECT b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
RECT Rect(glide::session::Rect r) { return {r.left, r.top, r.right, r.bottom}; }
bool Succeeded(glide::session::Result result) {
    return result == glide::session::Result::Applied || result == glide::session::Result::Unchanged;
}
bool Visible(HWND window) {
    return window && window->alive && window->visible && (!window->parent || Visible(window->parent));
}
RECT ScreenRect(HWND window) {
    RECT result = window->local;
    if (window->parent) {
        const RECT parent = ScreenRect(window->parent);
        result.left += parent.left; result.right += parent.left;
        result.top += parent.top; result.bottom += parent.top;
    }
    return result;
}
bool Before(bool fail) { return fail && boundary.fault.effect == Effect::Before; }

BOOL WINAPI SetSource(HWND window, RECT source) {
    const bool fail = boundary.Hit(Op::Source);
    if (!window || !window->alive || Before(fail)) return FALSE;
    if (fail && boundary.fault.effect == Effect::Mismatch) return TRUE;
    window->source = source;
    boundary.Invoke(Op::Source, true);
    return fail ? FALSE : TRUE;
}
BOOL WINAPI GetSource(HWND window, RECT* source) {
    const bool fail = boundary.Hit(Op::ReadSource);
    if (!window || !window->alive || !source || Before(fail)) return FALSE;
    *source = window->source;
    boundary.Invoke(Op::ReadSource, true);
    if (fail && boundary.fault.effect == Effect::Mismatch) { ++source->left; return TRUE; }
    return fail ? FALSE : TRUE;
}
BOOL WINAPI SetMatrix(HWND window, MagnificationApi::WindowTransform* matrix) {
    const bool fail = boundary.Hit(Op::Matrix);
    if (!window || !window->alive || !matrix || Before(fail)) return FALSE;
    if (fail && boundary.fault.effect == Effect::Mismatch) return TRUE;
    window->matrix = *matrix;
    boundary.Invoke(Op::Matrix, true);
    return fail ? FALSE : TRUE;
}
BOOL WINAPI GetMatrix(HWND window, MagnificationApi::WindowTransform* matrix) {
    const bool fail = boundary.Hit(Op::ReadMatrix);
    if (!window || !window->alive || !matrix || Before(fail)) return FALSE;
    *matrix = window->matrix;
    boundary.Invoke(Op::ReadMatrix, true);
    if (fail && boundary.fault.effect == Effect::Mismatch) { matrix->v[0][0] += 0.5f; return TRUE; }
    return fail ? FALSE : TRUE;
}
BOOL WINAPI Filter(HWND window, DWORD mode, int count, HWND* excluded) {
    const bool fail = boundary.Hit(Op::Filter);
    if (Before(fail)) return FALSE;
    Check(window && window->alive && mode == 0 && count == 1 && excluded &&
          *excluded == window->parent, "renderer excludes its own host from magnification");
    window->excluded = true;
    return fail ? FALSE : TRUE;
}
BOOL WINAPI ReadVisual(float* zoom, int* x, int* y) {
    if (boundary.Hit(Op::ReadVisual)) return FALSE;
    *zoom = boundary.global.visual.zoom; *x = boundary.global.visual.x; *y = boundary.global.visual.y;
    boundary.Invoke(Op::ReadVisual, true);
    return TRUE;
}
BOOL WINAPI ReadInput(BOOL* enabled, RECT* source, RECT* destination) {
    if (boundary.Hit(Op::ReadInput)) return FALSE;
    *enabled = boundary.global.input.enabled;
    *source = Rect(boundary.global.input.source); *destination = Rect(boundary.global.input.destination);
    boundary.Invoke(Op::ReadInput, true);
    return TRUE;
}
BOOL WINAPI WriteVisual(float zoom, int x, int y) {
    const bool fail = boundary.Hit(Op::Visual);
    if (Before(fail)) return FALSE;
    boundary.global.visual = {zoom, x, y};
    boundary.Invoke(Op::Visual, true);
    return fail ? FALSE : TRUE;
}
BOOL WINAPI WriteInput(BOOL enabled, const RECT* source, const RECT* destination) {
    const bool fail = boundary.Hit(Op::Input);
    if (Before(fail)) return FALSE;
    if (fail && boundary.fault.effect == Effect::Mismatch) return TRUE;
    boundary.global.input = {};
    if (enabled && source && destination)
        boundary.global.input = {true, {source->left, source->top, source->right, source->bottom},
            {destination->left, destination->top, destination->right, destination->bottom}};
    boundary.Invoke(Op::Input, true);
    return fail ? FALSE : TRUE;
}
BOOL WINAPI WriteCursor(BOOL show) {
    const bool fail = boundary.Hit(show ? Op::ShowCursor : Op::HideCursor);
    if (!show && boundary.enforceCursorJournal)
        Check(boundary.cursorRecovery, "cursor recovery is journaled before every native hide call");
    if (Before(fail)) return FALSE;
    boundary.systemCursorVisible = show != FALSE;
    if (fail && boundary.fault.effect == Effect::DesktopAfter) {
        boundary.inputDesktop = false;
        return TRUE;
    }
    return fail ? FALSE : TRUE;
}
struct Journal {
    glide::session::RecoveryRecord record{};
    bool armed = false;
    unsigned publications = 0;
    void Publish(const glide::session::RecoveryRecord& next) {
        Check(glide::session::Valid(next), "every actual presenter publication is valid");
        record = next; armed = true; ++publications;
    }
    void Clear() { armed = false; }
    bool ArmCursorRecovery() {
        Check(!boundary.cursorRecovery, "cursor acquisition does not overwrite a pending recovery");
        boundary.cursorRecovery = true;
        return true;
    }
    void ClearCursorRecovery() { boundary.cursorRecovery = false; }
};
struct Fixture {
    MagnificationApi api;
    MonitorMagnifier renderer;
    Fixture() {
        Check(boundary.Alive() == 0, "previous fixture left no private window");
        boundary = {};
        api.SetWindowSource = SetSource; api.GetWindowSource = GetSource;
        api.SetWindowTransform = SetMatrix; api.GetWindowTransform = GetMatrix;
        api.SetWindowFilterList = Filter;
        api.GetFullscreenTransform = ReadVisual; api.GetInputTransform = ReadInput;
        api.SetFullscreenTransform = WriteVisual; api.SetInputTransform = WriteInput;
        api.ShowSystemCursor = WriteCursor;
        Check(renderer.Initialize(reinterpret_cast<HINSTANCE>(1), api), "initialize actual renderer");
    }
    ~Fixture() {
        boundary.fault = {};
        renderer.Destroy();
        Check(boundary.Alive() == 0, "destroy releases both private native windows");
        Check(boundary.openDesktops == 0, "all successfully opened input desktops are closed");
    }
};
glide::monitor::Frame Frame(glide::Rect monitor = {-1920, -240, 0, 840},
                            double zoom = 2.345, double fraction = 0.37) {
    glide::monitor::Frame result;
    const glide::Vec2 anchor{monitor.left + (monitor.right - monitor.left) * fraction,
                             monitor.top + (monitor.bottom - monitor.top) * (1 - fraction)};
    Check(glide::monitor::MakeFrame(monitor, glide::AnchorZoom({}, zoom, anchor, monitor), result),
          "fixture is a valid production frame");
    return result;
}
void Verify(const glide::monitor::Frame& frame, const MonitorMagnifier& renderer) {
    HWND host = boundary.Host(), child = boundary.Child();
    Check(host && child && boundary.Alive() == 2 && renderer.Active(), "one visible host and one child");
    Check(Same(ScreenRect(host), Rect(frame.destination)), "host has exact selected monitor extent");
    Check(child->local.left == frame.child.left && child->local.top == frame.child.top &&
          child->local.right >= frame.child.right && child->local.bottom >= frame.child.bottom,
          "stable child allocation retains exact output-pixel shift and covers the entire clipped monitor");
    Check(child->local.right - child->local.left <= frame.destination.right - frame.destination.left + 4096 &&
          child->local.bottom - child->local.top <= frame.destination.bottom - frame.destination.top + 4096,
          "private child allocation remains bounded even at the renderer's extreme zoom limit");
    Check(Same(child->source, Rect(frame.capture)), "capture origin and bounds match applied frame");
    Check(child->matrix.v[0][0] == static_cast<float>(frame.transform.zoom) &&
          child->matrix.v[1][1] == static_cast<float>(frame.transform.zoom) &&
          child->matrix.v[2][2] == 1.0f, "actual native matrix is uniform scale");
    Check(child->matrix.v[0][1] == 0 && child->matrix.v[0][2] == 0 &&
          child->matrix.v[1][0] == 0 && child->matrix.v[1][2] == 0 &&
          child->matrix.v[2][0] == 0 && child->matrix.v[2][1] == 0,
          "renderer does not introduce undocumented affine matrix terms");
    Check(host->layered && child->excluded && child->invalidations && boundary.calls[Op::Paint] == 0,
          "layered overlay excludes itself and requests repaint without forcing synchronous desktop capture");
    Check((child->style & 1) != 0, "magnifier remains responsible for the enlarged cursor");
    Check((host->extendedStyle & (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW)) ==
          (WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW),
          "overlay remains nonactivating, click-through, and absent from taskbar");
}

void SteadyAndLifecycle() {
    ++scenarios;
    Fixture f;
    const auto frame = Frame();
    Check(!f.renderer.Show(), "show cannot precede successful preparation");
    Check(f.renderer.Prepare(frame) && !f.renderer.Active(), "preparation does not show the overlay");
    Check(f.renderer.Show(), "show after successful preparation");
    Verify(frame, f.renderer);
    boundary.ResetCounts();
    for (unsigned i = 0; i < 120; ++i) {
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "unchanged active frame refresh succeeds");
        Verify(frame, f.renderer);
    }
    Check(boundary.calls[Op::HostPosition] == 0 && boundary.calls[Op::ChildPosition] == 0,
          "unchanged frames do not move or resize either native window");
    Check(boundary.calls[Op::Source] == 0 && boundary.calls[Op::Matrix] == 0,
          "unchanged frames do not repeat native source or scale setters");
    Check(boundary.calls[Op::Show] == 0 && boundary.calls[Op::Raise] == 0,
          "already visible refreshes do not repeatedly reposition the topmost host");
    Check(boundary.calls[Op::Invalidate] == 120,
          "every unchanged refresh still invalidates live magnified content");
    Check(boundary.calls[Op::Paint] == 0 && boundary.childSizeCalls == 0,
          "unchanged refreshes do not force paint or resize the magnifier surface");
    Check(boundary.calls[Op::Visual] == 0 && boundary.calls[Op::Input] == 0,
          "private renderer never mutates global visual or input transforms");
    HWND host = boundary.Host();
    Check(host->procedure(host, WM_NCHITTEST, 0, 0) == HTTRANSPARENT,
          "real host window procedure passes hit testing through");
    Check(host->procedure(host, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE,
          "real host window procedure refuses mouse activation");
    Check(host->procedure(host, WM_ERASEBKGND, 0, 0) == 1,
          "real host window procedure avoids background erase flicker");
    host->visible = false; // Simulate an external hide, without the owner's Hide.
    Check(f.renderer.Prepare(frame) && f.renderer.Show(), "external host hide is observed and repaired");
    Check(boundary.calls[Op::Show] == 1, "visibility cache cannot suppress a needed re-show");
    f.renderer.Hide();
    Check(!f.renderer.Active() && !f.renderer.Show(), "hidden state cannot be shown without prepare");
    Check(f.renderer.Prepare(frame) && f.renderer.Show(), "explicit hide may be resumed safely");
    HWND oldChild = boundary.Child();
    Check(DestroyWindow(oldChild), "simulate external child destruction");
    Check(!f.renderer.Active() && !f.renderer.Show(), "child destruction synchronously hides host");
    Check(f.renderer.Prepare(frame) && f.renderer.Show(), "lost native child recreates a complete overlay");
    Check(boundary.Child() != oldChild, "destroyed child handle is not reused as live state");
    Verify(frame, f.renderer);
    HWND oldHost = boundary.Host();
    Check(DestroyWindow(oldHost), "simulate external host destruction");
    Check(!f.renderer.Active(), "WM_NCDESTROY clears active owner state");
    Check(f.renderer.Prepare(frame) && f.renderer.Show(), "host destruction is recoverable on next frame");
    Verify(frame, f.renderer);
}

void StableAllocationAndQueuedRefresh() {
    const std::array<glide::Rect, 3> monitors{{{0, 0, 3840, 2160}, {-2560, -240, 0, 1200},
                                             {1920, 0, 3840, 1080}}};
    for (const auto bounds : monitors) for (double zoom : {1.05, 2.0, 2.345, 4.0, 8.0, 16.0}) {
        ++scenarios;
        Fixture f;
        const glide::Vec2 anchor{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2};
        const auto baseline = glide::AnchorZoom({}, zoom, anchor, bounds);
        glide::monitor::Frame previous;
        Check(glide::monitor::MakeFrame(bounds, baseline, previous) &&
              f.renderer.Prepare(previous) && f.renderer.Show(), "allocate one native surface before fractional panning");
        const auto allocated = boundary.Child()->local;
        const LONG width = allocated.right - allocated.left, height = allocated.bottom - allocated.top;
        Check(width == bounds.right - bounds.left + 64 && height == bounds.bottom - bounds.top + 64,
              "supported zoom range starts with bounded stable overscan allocation");
        boundary.ResetCounts();
        unsigned positionChanges = 0, sourceChanges = 0;
        for (unsigned step = 1; step <= 120; ++step) {
            auto requested = baseline;
            requested.offset.x += step * 0.1;
            requested.offset.y += step * 0.03;
            glide::monitor::Frame frame;
            Check(glide::monitor::MakeFrame(bounds, requested, frame), "fractional pan fixture remains valid");
            positionChanges += frame.child.left != previous.child.left || frame.child.top != previous.child.top;
            sourceChanges += !(frame.capture == previous.capture);
            Check(f.renderer.Prepare(frame) && f.renderer.Show(), "actual native renderer accepts fractional pan");
            Verify(frame, f.renderer);
            Check(boundary.Child()->local.right - boundary.Child()->local.left == width &&
                  boundary.Child()->local.bottom - boundary.Child()->local.top == height,
                  "fractional compensation translates the child without changing its native allocation");
            const auto nativePoint = [&](glide::Vec2 point) {
                const auto& child = *boundary.Child();
                return glide::Vec2{bounds.left + child.local.left + (point.x - child.source.left) * child.matrix.v[0][0],
                                   bounds.top + child.local.top + (point.y - child.source.top) * child.matrix.v[1][1]};
            };
            const auto source = glide::ScreenToSource(anchor, frame.transform);
            const auto projected = nativePoint(source);
            Check(std::abs(projected.x - anchor.x) < 0.000001 && std::abs(projected.y - anchor.y) < 0.000001,
                  "stable allocation preserves the independently calculated rendered affine");
            previous = frame;
        }
        Check(boundary.calls[Op::ChildPosition] == positionChanges && boundary.childSizeCalls == 0 &&
              boundary.childResizeEffects == 0, "each needed subpixel shift uses translation-only native positioning");
        for (const UINT flags : boundary.childPositionFlags)
            Check((flags & SWP_NOSIZE) != 0 && (flags & SWP_NOREDRAW) != 0,
                  "every fractional-pan position call explicitly suppresses resizing and immediate repaint");
        Check(boundary.calls[Op::Source] == sourceChanges && boundary.calls[Op::Matrix] == 0,
              "panning writes source only when its actual integer capture changes and never rewrites scale");
        Check(boundary.calls[Op::HostPosition] == 0 && boundary.calls[Op::Show] == 0 && boundary.calls[Op::Raise] == 0 &&
              boundary.calls[Op::Hide] == 0 && boundary.calls[Op::Paint] == 0,
              "panning never resizes/repositions the host, toggles visibility, or forces synchronous paint");
        Check(boundary.calls[Op::Invalidate] == 120 && boundary.Child()->dirty,
              "all pan updates request live content refresh through the private child's queued dirty region");
        // Model an ordinary queued WM_PAINT after several invalidations have
        // coalesced. This is deliberately outside Show; no graphics/timing claim.
        boundary.Child()->dirty = false;
        ++boundary.Child()->paints;
        boundary.ResetCounts();
        Check(f.renderer.Prepare(previous) && f.renderer.Show() && boundary.Child()->dirty,
              "an unchanged frame requests fresh live content after an earlier queued paint completed");
        Check(boundary.calls[Op::Invalidate] == 1 && boundary.calls[Op::Source] == 0 &&
              boundary.calls[Op::Matrix] == 0 && boundary.calls[Op::Paint] == 0,
              "live refresh does not require repeated geometry writes or forced native capture");
    }
    {
        ++scenarios;
        Fixture f;
        const glide::Rect bounds{0, 0, 3840, 2160};
        auto frame = Frame(bounds, 2.0, 0.5);
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "allocate before changing zoom levels");
        boundary.ResetCounts();
        for (double zoom : {1.01, 1.02, 1.05, 1.13, 2.0, 2.01, 3.14, 8.0, 15.99, 16.0, 8.0, 1.01}) {
            frame = Frame(bounds, zoom, 0.53);
            Check(f.renderer.Prepare(frame) && f.renderer.Show(), "direct supported zoom update succeeds");
            Verify(frame, f.renderer);
        }
        Check(boundary.childSizeCalls == 0 && boundary.childResizeEffects == 0 &&
              boundary.calls[Op::HostPosition] == 0 && boundary.calls[Op::Show] == 0 && boundary.calls[Op::Raise] == 0 &&
              boundary.calls[Op::Hide] == 0 && boundary.calls[Op::Paint] == 0,
              "supported direct zoom changes never resize, hide/show, reposition host, or force paint");
        Check(boundary.calls[Op::Invalidate] == 12 && boundary.calls[Op::Matrix] == 12,
              "each changed native scale has exactly one matrix write and queued repaint request");
    }
}

void AllocationGrowthAndRepair() {
    ++scenarios;
    Fixture f;
    const glide::Rect bounds{0, 0, 3840, 2160};
    auto normal = Frame(bounds, 2.0, 0.5);
    Check(f.renderer.Prepare(normal) && f.renderer.Show(), "start bounded native allocation");
    const auto highFrame = [&](double phase) {
        auto requested = glide::AnchorZoom({}, 128.0, {1920, 1080}, bounds);
        requested.offset.x += phase; requested.offset.y += phase;
        glide::monitor::Frame frame;
        Check(glide::monitor::MakeFrame(bounds, requested, frame), "create renderer-only large overscan fixture");
        return frame;
    };
    const auto first = highFrame(0.1);
    boundary.ResetCounts();
    Check(f.renderer.Prepare(first) && f.renderer.Show(), "larger verified extent can grow native surface");
    Check(boundary.childSizeCalls == 1 && boundary.childResizeEffects == 1,
          "first necessary allocation growth performs one native size change");
    const LONG grownWidth = boundary.Child()->local.right - boundary.Child()->local.left;
    const LONG grownHeight = boundary.Child()->local.bottom - boundary.Child()->local.top;
    boundary.ResetCounts();
    Check(f.renderer.Prepare(normal) && f.renderer.Show(), "return to normal zoom after growth");
    Check(boundary.childSizeCalls == 0 &&
          boundary.Child()->local.right - boundary.Child()->local.left == grownWidth &&
          boundary.Child()->local.bottom - boundary.Child()->local.top == grownHeight,
          "zooming back does not shrink and reallocate the same monitor's surface");
    f.renderer.Hide();
    boundary.ResetCounts();
    Check(f.renderer.Prepare(normal) && f.renderer.Show() && boundary.childSizeCalls == 0,
          "ordinary hide/show retains the known native allocation");
    const auto second = highFrame(0.9);
    boundary.Arm(Op::ChildPosition, Effect::After);
    Check(!f.renderer.Prepare(second) && boundary.fault.reached && !f.renderer.Active(),
          "failed native growth after effect hides the overlay and invalidates allocation cache");
    ++faultScenarios; ++reachedFaults;
    boundary.ResetCounts();
    Check(f.renderer.Prepare(second) && f.renderer.Show() && boundary.childSizeCalls == 1,
          "retry performs an explicit verified allocation repair after a partial size effect");
    Verify(second, f.renderer);
    boundary.ResetCounts();
    const auto retarget = Frame({-1920, -240, 0, 840}, 2.345);
    Check(f.renderer.Prepare(retarget) && f.renderer.Show() && boundary.childSizeCalls == 1 &&
          boundary.calls[Op::HostPosition] == 1, "monitor retarget resets surface allocation exactly once");
    Check(boundary.Child()->local.right - boundary.Child()->local.left == 1984 &&
          boundary.Child()->local.bottom - boundary.Child()->local.top == 1144,
          "retarget allocation uses only the new monitor extent and bounded padding");
    Verify(retarget, f.renderer);
}

void TopmostRepair() {
    {
        ++scenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "establish visible topmost host");
        auto& external = boundary.externalTopmost;
        external.visible = true;
        external.local = {frame.destination.left + 20, frame.destination.top + 30,
                          frame.destination.left + 420, frame.destination.top + 90};
        external.style = WS_POPUP | WS_VISIBLE;
        external.extendedStyle = WS_EX_TOPMOST;
        const RECT foreignRect = external.local;
        boundary.externalAbove = true;
        boundary.ResetCounts();
        Check(f.renderer.Prepare(frame) && f.renderer.Show() && !boundary.externalAbove,
              "new topmost tooltip/taskbar predecessor triggers one own-host re-raise");
        Check(boundary.calls[Op::ZOrder] == 2 && boundary.calls[Op::Raise] == 1 &&
              boundary.calls[Op::Show] == 0 && boundary.calls[Op::Hide] == 0,
              "occlusion repair reorders only the already visible host without hide/show churn");
        Check(external.alive && external.visible && Same(external.local, foreignRect) &&
              external.style == (WS_POPUP | WS_VISIBLE) && external.extendedStyle == WS_EX_TOPMOST,
              "foreign window visibility, geometry and styles remain untouched");
        for (unsigned i = 0; i < 120; ++i)
            Check(f.renderer.Prepare(frame) && f.renderer.Show(), "steady native frame after occlusion repair");
        Check(boundary.calls[Op::ZOrder] == 122 && boundary.calls[Op::Raise] == 1 &&
              boundary.calls[Op::HostPosition] == 0 && boundary.calls[Op::ChildPosition] == 0 &&
              boundary.calls[Op::Show] == 0 && boundary.calls[Op::Hide] == 0 && boundary.calls[Op::Paint] == 0,
              "one bounded predecessor query replaces repeated unconditional topmost mutation");
        boundary.externalAbove = true;
        external.visible = false;
        Check(f.renderer.Show() && boundary.calls[Op::Raise] == 1 && boundary.externalAbove && !external.visible,
              "hidden predecessor does not trigger needless own-host reordering");
        SetLastError(ERROR_GEN_FAILURE);
        Check(f.renderer.Show(), "stale last-error value cannot turn no-predecessor success into a failure");
    }
    for (const Op op : {Op::ZOrder, Op::Raise}) for (const Effect effect : {Effect::Before, Effect::After}) {
        if (op == Op::ZOrder && effect == Effect::After) continue;
        ++scenarios; ++faultScenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "establish host before occlusion failure");
        boundary.externalAbove = true;
        boundary.externalTopmost.visible = true;
        boundary.externalTopmost.local = Rect(frame.destination);
        boundary.Arm(op, effect);
        Check(!f.renderer.Show() && boundary.fault.reached && !f.renderer.Active(),
              "predecessor-query or own-host-raise failure fails closed even after a native effect");
        ++reachedFaults;
        Check(!f.renderer.ReplacementVisibleAt({-1000, 300}),
              "failed occlusion repair cannot justify hiding the ordinary cursor");
        boundary.ResetCounts();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "occlusion repair failure permits clean verified retry");
        Verify(frame, f.renderer);
    }
    for (unsigned action = 0; action < 3; ++action) {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "establish host before z-order query callback");
        boundary.ResetCounts();
        boundary.externalAbove = true;
        boundary.callbackOp = Op::ZOrder;
        boundary.callbackOrdinal = 1;
        boundary.callbackAfter = true;
        boundary.callback = [&]() {
            if (action == 0) f.renderer.Hide();
            else if (action == 1) DestroyWindow(boundary.Host());
            else boundary.inputDesktop = false;
        };
        Check(!f.renderer.Show() && boundary.callbackReached && !f.renderer.Active(),
              "query-time hide, destruction or desktop switch revokes native presentation");
        ++reachedReentries;
        if (action != 2)
            Check(boundary.calls[Op::Raise] == 0, "invalidated handles are never raised after the query callback");
        Check(boundary.calls[Op::Input] == 0 && boundary.calls[Op::Visual] == 0,
              "z-order inspection and private recovery never mutate global magnification");
        boundary.inputDesktop = true;
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "return from query callback supports fresh native frame");
    }
    for (bool after : {false, true}) {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        const auto frame = Frame();
        glide::runtime::State state;
        Check(state.PrepareEnable() && state.Activate(), "activate z-order cancellation fixture");
        glide::session::Session session;
        Journal journal;
        Check(Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result),
              "establish owned input mapping before z-order cancellation");
        boundary.ResetCounts();
        boundary.externalAbove = true;
        boundary.callbackOp = Op::ZOrder;
        boundary.callbackOrdinal = 1;
        boundary.callbackAfter = after;
        boundary.callback = [&]() { state.Pause(); state.RequestRestore(); };
        glide::frame::Lease lease(state);
        glide::frame::Backend backend(f.api, lease);
        glide::frame::Renderer renderer(f.renderer, lease);
        Check(!Succeeded(glide::monitor::Present(frame, renderer, session, backend, journal).result) &&
              boundary.callbackReached && !lease.Valid() && !f.renderer.Active(),
              "lifecycle cancellation inside z-order query cannot publish a stale outer frame");
        ++reachedReentries;
        Check(boundary.calls[Op::Input] == 0 && boundary.calls[Op::Visual] == 0,
              "cancelled steady presentation makes no global writes before deferred restoration");
        Check(Succeeded(session.Reset(f.api, journal)) && !journal.armed &&
              glide::session::IsIdentity(boundary.global), "deferred z-order cancellation restores only owned input mapping");
    }
}

void CachedFrameAndPaintIdentity() {
    ++scenarios;
    Fixture f;
    const auto frame = Frame();
    Check(!f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
          "unprepared renderer exposes neither a frame match nor a paint target");
    Check(f.renderer.Prepare(frame) && f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
          "verified but hidden frame is cached without exposing a paint target");
    Check(f.renderer.Show() && f.renderer.PaintWindow() == boundary.Child(),
          "completed visible preparation exposes only the actual private magnifier child");
    boundary.ResetCounts();
    for (unsigned repeat = 0; repeat < 1000; ++repeat)
        Check(f.renderer.Matches(frame) && f.renderer.PaintWindow() == boundary.Child(),
              "steady frame and private paint identity remain available without another preparation");
    Check(boundary.calls.empty(), "pure frame matching and visible handle lookup perform no geometry or magnification calls");
    for (unsigned field = 0; field < 11; ++field) {
        auto changed = frame;
        switch (field) {
        case 0: changed.transform.zoom += 0.01; break;
        case 1: changed.transform.offset.x += 0.01; break;
        case 2: changed.transform.offset.y += 0.01; break;
        case 3: ++changed.source.left; break;
        case 4: ++changed.source.right; break;
        case 5: ++changed.destination.top; break;
        case 6: ++changed.destination.bottom; break;
        case 7: ++changed.capture.left; break;
        case 8: ++changed.capture.bottom; break;
        case 9: --changed.child.left; break;
        case 10: --changed.child.bottom; break;
        }
        Check(!f.renderer.Matches(changed), "every input/render/affine component participates in exact frame matching");
    }
    f.renderer.Hide();
    Check(!f.renderer.Matches(frame) && !f.renderer.PaintWindow(), "hide invalidates both cached presentation eligibility and paint identity");
    Check(f.renderer.Prepare(frame) && f.renderer.Show(), "fresh preparation restores cache after hide");
    Check(DestroyWindow(boundary.Child()) && !f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
          "child destruction invalidates all exposed presentation state");
    Check(f.renderer.Prepare(frame) && f.renderer.Show(), "fresh private child can be presented");
    boundary.Arm(Op::Invalidate, Effect::After);
    Check(!f.renderer.Show() && boundary.fault.reached && !f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
          "partial repaint-request failure revokes frame reuse and the paint handle");
    ++faultScenarios; ++reachedFaults;
}

void GeometryMessagesRevokeCache() {
    for (unsigned change = 0; change < 4; ++change) {
        ++scenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "establish geometry-notification fixture");
        const HWND host = boundary.Host();
        const RECT original = host->local;
        if (change == 0)
            Check(SetWindowPos(host, HWND_TOPMOST, original.left + 17, original.top - 11, 0, 0,
                               SWP_NOSIZE | SWP_NOACTIVATE), "simulate an outside host move");
        else if (change == 1)
            Check(SetWindowPos(host, HWND_TOPMOST, 0, 0, original.right - original.left + 31,
                               original.bottom - original.top + 23, SWP_NOMOVE | SWP_NOACTIVATE),
                  "simulate an outside host resize");
        else if (change == 2) {
            boundary.Child()->local.right += 13;
            host->procedure(host, WM_DPICHANGED, 0, 0);
        } else
            Check(SetWindowPos(host, HWND_TOPMOST, 0, 0, 0, 0,
                               SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE),
                  "simulate a Z-order-only native notification");
        if (change == 3) {
            Check(f.renderer.Matches(frame) && f.renderer.PaintWindow() == boundary.Child(),
                  "Z-order-only notification retains the verified geometry and paint identity");
            continue;
        }
        Check(!f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
              "host move/size/DPI notification revokes cached geometry before another frame can reuse it");
        boundary.ResetCounts();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "geometry notification permits complete verified repair");
        Check(boundary.calls[Op::HostPosition] == 1 && boundary.calls[Op::ChildPosition] == 1 &&
              boundary.calls[Op::Source] == 1 && boundary.calls[Op::Matrix] == 1 &&
              f.renderer.Matches(frame) && f.renderer.PaintWindow() == boundary.Child(),
              "repair reestablishes the exact native frame and commits its cache after positioning notifications");
        Verify(frame, f.renderer);
    }
}

void PreparationInvalidationFence() {
    for (unsigned action = 0; action < 4; ++action) {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "establish frame before preparation invalidation callback");
        boundary.ResetCounts();
        boundary.callbackOp = action == 3 ? Op::ReadSource : Op::ReadMatrix;
        boundary.callbackOrdinal = 1;
        boundary.callbackAfter = true;
        boundary.callback = [&]() {
            const HWND host = boundary.Host();
            if (action == 0) {
                const RECT old = host->local;
                Check(SetWindowPos(host, HWND_TOPMOST, old.left + 19, old.top, 0, 0,
                                   SWP_NOSIZE | SWP_NOACTIVATE), "move host during source/scale verification");
            } else if (action == 1) {
                boundary.Child()->local.right += 9;
                host->procedure(host, WM_DPICHANGED, 0, 0);
            } else if (action == 2) {
                f.renderer.Hide();
            } else {
                auto transform = frame.transform;
                transform.offset.x += 1;
                glide::monitor::Frame nested;
                Check(glide::monitor::MakeFrame(glide::monitor::AsRect(frame.destination), transform, nested) &&
                      f.renderer.Prepare(nested), "nested source-only preparation can complete at a native callback boundary");
            }
        };
        Check(!f.renderer.Prepare(frame) && boundary.callbackReached && !f.renderer.Active() &&
              !f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
              "geometry/DPI/hide/nested-preparation invalidation cannot be overwritten by stale outer preparation success");
        ++reachedReentries;
        boundary.ResetCounts();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "new preparation repairs a cancelled private transaction");
        Verify(frame, f.renderer);
    }
}

void BoundedTopmostObservation() {
    for (bool persistent : {false, true}) {
        ++scenarios;
        Fixture f;
        const auto frame = Frame();
        glide::session::Session session;
        Journal journal;
        glide::cursor::Lease cursor;
        glide::cursor::NativeBackend cursorBackend(f.api);
        Check(Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result) &&
              cursor.Update(true, cursorBackend, journal) && !boundary.systemCursorVisible,
              "start a real owned presentation and cursor lease before topmost contention");
        boundary.externalAbove = true;
        boundary.persistentAbove = persistent;
        auto& external = boundary.externalTopmost;
        external.visible = true;
        external.local = Rect(frame.destination);
        const RECT foreignRect = external.local;
        boundary.ResetCounts();
        if (!persistent) {
            Check(Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result) &&
                  !boundary.externalAbove, "first overlap can be repaired and verified once");
            boundary.externalAbove = true; // Same contender reclaims before the next observation.
        }
        const auto outcome = glide::monitor::Present(frame, f.renderer, session, f.api, journal);
        Check(!Succeeded(outcome.result) && outcome.localFailure && !f.renderer.Active() &&
              !f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
              "persistent or continuously reclaimed overlap revokes presentation rather than leaving mapping behind an unmagnified window");
        Check(boundary.calls[Op::Raise] == 1 && boundary.calls[Op::Input] == 0 &&
              boundary.calls[Op::Visual] == 0 && external.visible && Same(external.local, foreignRect),
              "one attempted own-host raise never becomes a Z-order fight or a mutation of foreign windows/global mapping");
        Check(session.NeedsRecovery() && journal.armed && boundary.cursorRecovery,
              "failed overlap verification retains all owned recovery responsibilities for the caller");
        Check(cursor.Release(cursorBackend, journal) && Succeeded(session.Reset(f.api, journal)) &&
              boundary.systemCursorVisible && !boundary.cursorRecovery && !journal.armed &&
              glide::session::IsIdentity(boundary.global),
              "caller recovery releases the ordinary cursor and recognized mapping after a blocked presentation");
    }
    for (const bool firstVisible : {false, true}) {
        ++scenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "start skipped-predecessor fixture");
        MockRendererWindow overlap;
        overlap.visible = true; overlap.local = Rect(frame.destination);
        boundary.externalTopmost.visible = firstVisible;
        boundary.externalTopmost.local = {20000, 20000, 20200, 20200};
        boundary.externalTopmost.predecessor = &overlap;
        boundary.externalAbove = true;
        boundary.ResetCounts();
        Check(f.renderer.Show() && !boundary.externalAbove && boundary.calls[Op::ZOrder] == 3 &&
              boundary.calls[Op::Raise] == 1,
              "bounded walk finds a true overlap behind a hidden or off-monitor predecessor");
        Check(overlap.visible && Same(overlap.local, Rect(frame.destination)),
              "observed foreign windows remain unchanged");
    }
    {
        ++scenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "start bounded-chain fixture");
        std::array<MockRendererWindow, 24> hidden{};
        boundary.externalAbove = true;
        boundary.externalTopmost.predecessor = &hidden[0];
        for (std::size_t i = 1; i < hidden.size(); ++i) hidden[i - 1].predecessor = &hidden[i];
        boundary.ResetCounts();
        Check(!f.renderer.Show() && boundary.calls[Op::ZOrder] == 16 && boundary.calls[Op::Raise] == 0 &&
              !f.renderer.Active() && !f.renderer.PaintWindow(),
              "an exhausted bounded predecessor walk cannot claim that the view is unobscured");
        boundary.externalAbove = false;
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "restore a verified view before cyclic-order fixture");
        boundary.externalAbove = true;
        boundary.externalTopmost.predecessor = &boundary.externalTopmost;
        boundary.ResetCounts();
        Check(!f.renderer.Show() && boundary.calls[Op::ZOrder] == 2 && !f.renderer.Active() &&
              !f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
              "a changing/cyclic native order cannot loop or expose stale presentation state");
    }
    for (const Effect effect : {Effect::Before, Effect::After}) {
        ++scenarios; ++faultScenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "start foreign-geometry fault fixture");
        boundary.externalAbove = true;
        boundary.externalTopmost.visible = true;
        boundary.externalTopmost.local = Rect(frame.destination);
        boundary.Arm(Op::OccluderRect, effect);
        Check(!f.renderer.Show() && boundary.fault.reached && !f.renderer.Active(),
              "an observable live occluder geometry failure fails closed");
        ++reachedFaults;
        boundary.ResetCounts();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "foreign geometry failure permits a fresh verified retry");
    }
    for (unsigned action = 0; action < 3; ++action) {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "start occluder-read reentry fixture");
        boundary.externalAbove = true;
        boundary.externalTopmost.visible = true;
        boundary.externalTopmost.local = Rect(frame.destination);
        boundary.ResetCounts();
        boundary.callbackOp = Op::OccluderRect;
        boundary.callbackOrdinal = 1;
        boundary.callbackAfter = action != 2;
        boundary.callback = [&]() {
            if (action == 0) f.renderer.Hide();
            else if (action == 1) DestroyWindow(boundary.Host());
            else boundary.externalTopmost.alive = false;
        };
        const bool shown = f.renderer.Show();
        Check(boundary.callbackReached && boundary.calls[Op::Raise] == 0,
              "foreign geometry reentry never positions invalidated handles");
        ++reachedReentries;
        Check(!shown && !f.renderer.Active() && !f.renderer.PaintWindow(),
              "local invalidation or an unresolvable foreign-geometry race cannot claim an unobscured presentation");
    }
}

void TopmostPostRaiseVerification() {
    {
        ++scenarios; ++faultScenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "establish view before failed post-raise observation");
        boundary.externalAbove = true;
        boundary.externalTopmost.visible = true;
        boundary.externalTopmost.local = Rect(frame.destination);
        boundary.Arm(Op::ZOrder, Effect::Before, 2);
        Check(!f.renderer.Show() && boundary.fault.reached && boundary.calls[Op::Raise] == 1 &&
              !f.renderer.Active() && !f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
              "successful native raise is insufficient when its postcondition cannot be observed");
        ++reachedFaults;
        Check(boundary.calls[Op::Invalidate] == 0 && boundary.calls[Op::Input] == 0 && boundary.calls[Op::Visual] == 0,
              "failed post-raise verification does not refresh or mutate shared transforms");
    }
    for (bool after : {false, true}) for (bool stopping : {false, true}) {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        const auto frame = Frame();
        glide::runtime::State state;
        Check(state.PrepareEnable() && state.Activate(), "activate post-raise reentry fixture");
        glide::session::Session session;
        Journal journal;
        Check(Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result),
              "establish owned mapping before a post-raise callback");
        boundary.externalAbove = true;
        boundary.externalTopmost.visible = true;
        boundary.externalTopmost.local = Rect(frame.destination);
        boundary.ResetCounts();
        boundary.callbackOp = Op::ZOrder;
        boundary.callbackOrdinal = 2;
        boundary.callbackAfter = after;
        boundary.callback = [&]() {
            if (stopping) state.Stop(); else state.Pause();
            state.RequestRestore();
        };
        glide::frame::Lease lease(state);
        glide::frame::Backend backend(f.api, lease);
        glide::frame::Renderer renderer(f.renderer, lease);
        Check(!Succeeded(glide::monitor::Present(frame, renderer, session, backend, journal).result) &&
              boundary.callbackReached && boundary.calls[Op::Raise] == 1 && !lease.Valid() && !f.renderer.Active(),
              "post-raise observation reentry cannot publish stale successful presentation");
        ++reachedReentries;
        Check(boundary.calls[Op::Input] == 0 && boundary.calls[Op::Visual] == 0 &&
              !f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
              "post-raise cancellation retains recovery without another shared write or paint handle");
        Check(Succeeded(session.Reset(f.api, journal)) && !journal.armed && glide::session::IsIdentity(boundary.global),
              "caller recovers owned input after the post-raise callback unwinds");
    }
}

void PrepareFault(Op op, Effect effect, bool existing, unsigned ordinal = 1) {
    ++scenarios; ++faultScenarios;
    Fixture f;
    if (existing) {
        const auto old = Frame({0, 0, 1920, 1080}, 1.5, 0.23);
        Check(f.renderer.Prepare(old) && f.renderer.Show(), "establish previous visible native frame");
    }
    const auto frame = Frame({-2560, -170, 0, 1270}, 3.219, 0.63);
    boundary.Arm(op, effect, ordinal);
    Check(!f.renderer.Prepare(frame), "native preparation fault propagates as failure");
    Check(boundary.fault.reached, "scheduled native preparation boundary was actually reached");
    ++reachedFaults;
    Check(!f.renderer.Active(), "preparation failure never leaves the old or partial frame visible");
    Check(GetLastError() != ERROR_SUCCESS, "native failure keeps a useful error");
    boundary.ResetCounts();
    Check(!f.renderer.Show(), "failed preparation cannot expose cached pixels");
    Check(f.renderer.Prepare(frame) && f.renderer.Show(), "failure invalidates caches and allows exact retry");
    Check(boundary.calls[Op::Source] == 1 && boundary.calls[Op::Matrix] == 1,
          "retry replays both source and scale even after partial native effects");
    Verify(frame, f.renderer);
}

void ShowFault(Op op, Effect effect, unsigned ordinal = 1) {
    ++scenarios; ++faultScenarios;
    Fixture f;
    const auto frame = Frame();
    Check(f.renderer.Prepare(frame), "prepare before injected show failure");
    boundary.Arm(op, effect, ordinal);
    Check(!f.renderer.Show(), "native show fault propagates as failure");
    Check(boundary.fault.reached, "scheduled native show boundary was actually reached");
    ++reachedFaults;
    Check(!f.renderer.Active(), "show failure hides partial visual effects");
    boundary.ResetCounts();
    Check(!f.renderer.Show(), "failed show revokes prepared state");
    Check(f.renderer.Prepare(frame) && f.renderer.Show(), "show failure is retryable after preparation");
    Verify(frame, f.renderer);
}

void FailureMatrix() {
    for (const Op op : {Op::Register, Op::CreateHost, Op::CreateChild, Op::UserData,
                       Op::Layered, Op::Filter})
        PrepareFault(op, Effect::Before, false);
    for (const Op op : {Op::Layered, Op::Filter}) PrepareFault(op, Effect::After, false);
    for (const bool existing : {false, true}) {
        for (const Op op : {Op::HostPosition, Op::ChildPosition, Op::Source, Op::Matrix}) {
            PrepareFault(op, Effect::Before, existing);
            PrepareFault(op, Effect::After, existing);
            PrepareFault(op, Effect::Mismatch, existing);
        }
        for (const Op op : {Op::HostRect, Op::ChildRect, Op::ReadSource, Op::ReadMatrix}) {
            PrepareFault(op, Effect::Before, existing);
            PrepareFault(op, Effect::Mismatch, existing);
        }
        PrepareFault(Op::OpenDesktop, Effect::Before, existing, 1);
        PrepareFault(Op::OpenDesktop, Effect::Before, existing, 2);
        PrepareFault(Op::DesktopName, Effect::Before, existing, 1);
        PrepareFault(Op::DesktopName, Effect::Before, existing, 2);
    }
    ShowFault(Op::Show, Effect::Before);
    ShowFault(Op::Show, Effect::After);
    ShowFault(Op::Invalidate, Effect::Before);
    ShowFault(Op::Invalidate, Effect::After);
    ShowFault(Op::OpenDesktop, Effect::Before, 1);
    ShowFault(Op::OpenDesktop, Effect::Before, 2);
}

void CleanupAndIsolation() {
    {
        ++scenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "setup hide failure");
        boundary.Arm(Op::Hide);
        f.renderer.Hide();
        Check(boundary.fault.reached && !f.renderer.Active() && boundary.Alive() == 0,
              "failed native hide destroys private overlay as fallback");
        ++faultScenarios; ++reachedFaults;
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "hide fallback supports later recreation");
    }
    for (const Effect effect : {Effect::Before, Effect::After}) {
        ++scenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "setup native destruction fault");
        boundary.Arm(Op::Destroy, effect);
        f.renderer.Destroy();
        Check(boundary.fault.reached && !f.renderer.Active(), "failed destruction still hides private overlay");
        ++faultScenarios; ++reachedFaults;
        Check(boundary.Alive() == (effect == Effect::Before ? 2U : 0U),
              "failed destruction retains live handles only when destruction did not occur");
        boundary.ResetCounts();
        f.renderer.Destroy();
        Check(boundary.Alive() == 0, "retained failed-destroy handles can be retried");
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "destruction failure can later recreate cleanly");
    }
    {
        ++scenarios;
        Fixture f;
        const auto frame = Frame();
        boundary.foreignClass = true;
        Check(!f.renderer.Prepare(frame) && boundary.Alive() == 0,
              "existing unrelated class procedure cannot be adopted");
        Check(GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "class conflict remains explicit");
        boundary.foreignClass = false;
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "class conflict failure is recoverable");
        const auto valid = frame;
        auto invalid = frame;
        invalid.transform.zoom = std::nan("");
        Check(!f.renderer.Prepare(invalid) && !f.renderer.Active(),
              "invalid numeric frame hides an existing overlay before native mutation");
        Check(f.renderer.Prepare(valid) && f.renderer.Show(), "valid frame after numeric rejection succeeds");
        boundary.inputDesktop = false;
        Check(!f.renderer.Prepare(valid) && !f.renderer.Active(),
              "other input desktop blocks and hides native rendering");
        boundary.inputDesktop = true;
        Check(f.renderer.Prepare(valid) && f.renderer.Show(), "return to own desktop permits fresh preparation");
    }
}

void ExternalCorruption() {
    // Cached writes are safe only while observed native state still matches.
    // An external move, DPI-sized child, or modified native source/scale must
    // invalidate the complete cache, not silently continue with stale pixels.
    for (unsigned corruption = 0; corruption < 4; ++corruption) {
        ++scenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "establish frame before external native change");
        switch (corruption) {
        case 0: ++boundary.Host()->local.left; break;
        case 1: ++boundary.Child()->local.bottom; break;
        case 2: ++boundary.Child()->source.left; break;
        case 3: boundary.Child()->matrix.v[1][1] += 1.0f; break;
        }
        Check(!f.renderer.Prepare(frame) && !f.renderer.Active(),
              "same-frame verification detects external native state corruption");
        Check(!f.renderer.ReplacementVisibleAt({-1000, 300}),
              "mismatched native state cannot justify hiding the system cursor");
        boundary.ResetCounts();
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "invalidated cache repairs all native state");
        Check(boundary.calls[Op::HostPosition] == 1 && boundary.calls[Op::ChildPosition] == 1 &&
              boundary.calls[Op::Source] == 1 && boundary.calls[Op::Matrix] == 1,
              "external corruption forces complete geometry, capture and matrix repair");
        Verify(frame, f.renderer);
    }
}

void ReplacementCursorGeometry() {
    const std::array<glide::Rect, 4> monitors{{{0, 0, 1920, 1080}, {-2560, 0, 0, 1440},
                                             {0, -2160, 3840, 0}, {1920, -240, 3840, 840}}};
    const std::array<double, 6> zooms{{1.001, 1.05, 2.0, 3.141, 16.0, 63.999}};
    const std::array<double, 7> fractions{{-0.1, -0.001, 0.001, 0.5, 0.999, 1.001, 1.1}};
    for (const auto monitor : monitors) for (double zoom : zooms) {
        ++scenarios;
        Fixture f;
        const auto frame = Frame(monitor, zoom, 0.73);
        const auto sourceAt = [&](double x, double y) {
            const glide::Vec2 screen{monitor.left + (monitor.right - monitor.left) * x,
                                     monitor.top + (monitor.bottom - monitor.top) * y};
            return glide::ScreenToSource(screen, frame.transform);
        };
        const auto center = sourceAt(0.5, 0.5);
        Check(!f.renderer.ReplacementVisibleAt(center), "no replacement cursor exists before native preparation");
        Check(f.renderer.Prepare(frame), "prepare cursor geometry fixture");
        Check(!f.renderer.ReplacementVisibleAt(center), "hidden prepared overlay cannot justify cursor hiding");
        Check(f.renderer.Show(), "show cursor geometry fixture");
        for (double x : fractions) for (double y : fractions) {
            const bool inVisibleOutput = x >= 0 && x < 1 && y >= 0 && y < 1;
            Check(f.renderer.ReplacementVisibleAt(sourceAt(x, y)) == inVisibleOutput,
                  "replacement hotspot visibility matches independent source-to-output geometry");
        }
        Check(!f.renderer.ReplacementVisibleAt({std::nan(""), center.y}) &&
              !f.renderer.ReplacementVisibleAt({center.x, std::numeric_limits<double>::infinity()}),
              "nonfinite samples never hide the system cursor");
        Check(!f.renderer.ReplacementVisibleAt({monitor.left - 10, center.y}) &&
              !f.renderer.ReplacementVisibleAt({monitor.right + 10, center.y}),
              "pointer on an adjacent monitor keeps the ordinary cursor");
        Check(!f.renderer.ReplacementVisibleAt({static_cast<double>(frame.capture.left) - 1, center.y}) &&
              !f.renderer.ReplacementVisibleAt({static_cast<double>(frame.capture.right), center.y}) &&
              !f.renderer.ReplacementVisibleAt({center.x, static_cast<double>(frame.capture.top) - 1}) &&
              !f.renderer.ReplacementVisibleAt({center.x, static_cast<double>(frame.capture.bottom)}),
              "all four half-open capture boundaries preserve ordinary cursor outside captured source");
        Check(f.renderer.ReplacementVisibleAt({static_cast<double>(frame.capture.left), center.y}) ==
                  (frame.child.left == 0) &&
              f.renderer.ReplacementVisibleAt({center.x, static_cast<double>(frame.capture.top)}) ==
                  (frame.child.top == 0),
              "overscan pixels captured beyond visible left/top crop do not justify hiding system cursor");
        boundary.Child()->visible = false;
        Check(!f.renderer.ReplacementVisibleAt(center), "externally hidden native child revokes cursor replacement");
        boundary.Child()->visible = true;
        Check(f.renderer.ReplacementVisibleAt(center), "restored native child permits replacement inside visible crop");
        f.renderer.Hide();
        Check(!f.renderer.ReplacementVisibleAt(center), "explicit overlay hide immediately revokes cursor replacement");
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "reprepare cursor geometry after hide");
        boundary.Arm(Op::ReadMatrix, Effect::Mismatch);
        Check(!f.renderer.Prepare(frame) && !f.renderer.ReplacementVisibleAt(center),
              "verification failure revokes previously valid cursor replacement");
        Check(boundary.fault.reached, "replacement failure test reaches matrix read boundary");
        ++faultScenarios; ++reachedFaults;
    }
}

void ActualPresenterFusion() {
    const std::array<glide::Rect, 4> monitors{{{0, 0, 1920, 1080}, {-2560, 0, 0, 1440},
                                             {0, -2160, 3840, 0}, {1920, -240, 3840, 840}}};
    const std::array<double, 5> zooms{{1.001, 1.05, 2.0, 3.141, 16.0}};
    for (const auto monitor : monitors) for (double zoom : zooms) {
        ++scenarios;
        Fixture f;
        glide::session::Session session;
        Journal journal;
        boundary.enforceInputBeforeShow = true;
        const auto frame = Frame(monitor, zoom);
        Check(Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result),
              "real presenter, session, native adapter and renderer agree");
        Check(session.OwnsState() && journal.armed, "visible native frame has a recoverable input lease");
        Verify(frame, f.renderer);
        boundary.ResetCounts();
        Check(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result ==
              glide::session::Result::Unchanged, "identical fusion frame leaves global state unchanged");
        Check(boundary.calls[Op::Input] == 0 && boundary.calls[Op::Visual] == 0 &&
              boundary.calls[Op::Show] == 0 && boundary.calls[Op::Invalidate] == 1,
              "steady fusion refresh only observes global state and repaints once");
        Check(boundary.calls[Op::ReadVisual] == 1 && boundary.calls[Op::ReadInput] == 1 &&
              boundary.calls[Op::HostRect] == 0 && boundary.calls[Op::ChildRect] == 0 &&
              boundary.calls[Op::ReadSource] == 0 && boundary.calls[Op::ReadMatrix] == 0 &&
              boundary.calls[Op::Source] == 0 && boundary.calls[Op::Matrix] == 0,
              "steady native presentation performs one owned global snapshot and no redundant private geometry transaction");
        f.renderer.Hide();
        Check(Succeeded(session.Reset(f.api, journal)) && !journal.armed &&
              glide::session::IsIdentity(boundary.global), "fusion teardown returns owned global input to identity");
    }
    for (const Op op : {Op::Input, Op::Show, Op::Invalidate})
        for (const Effect effect : {Effect::Before, Effect::After, Effect::Mismatch}) {
        if (op == Op::Invalidate && effect == Effect::Mismatch) continue;
        ++scenarios; ++faultScenarios;
        Fixture f;
        glide::session::Session session;
        Journal journal;
        const auto frame = Frame();
        boundary.enforceInputBeforeShow = true;
        boundary.Arm(op, effect);
        const auto outcome = glide::monitor::Present(frame, f.renderer, session, f.api, journal);
        Check(boundary.fault.reached, "fusion fault reached actual native setter/show/invalidation");
        ++reachedFaults;
        // Session uses the verified postcondition: a setter which reports FALSE
        // after applying the requested mapping is safe to accept.
        const bool accepted = op == Op::Input && effect == Effect::After;
        Check(Succeeded(outcome.result) == accepted, "fusion uses observed global write outcome");
        Check(f.renderer.Active() == accepted, "unverified local presentation cannot remain visible");
        Check(journal.armed && session.OwnsState(), "uncertain or applied mapping remains recoverable");
        boundary.ResetCounts();
        f.renderer.Hide();
        Check(Succeeded(session.Reset(f.api, journal)) && !journal.armed &&
              glide::session::IsIdentity(boundary.global), "every fusion fault restores only the owned mapping");
    }
}

void CachedPresenterOwnershipAndReentry() {
    for (const Op op : {Op::ReadVisual, Op::ReadInput}) {
        ++scenarios; ++faultScenarios;
        Fixture f;
        const auto frame = Frame();
        glide::session::Session session;
        Journal journal;
        Check(Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result),
              "establish cached presentation before ownership read failure");
        boundary.Arm(op);
        const auto result = glide::monitor::Present(frame, f.renderer, session, f.api, journal);
        Check(result.result == glide::session::Result::ReadFailure && boundary.fault.reached &&
              !f.renderer.Active() && !f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
              "cached private frame never bypasses failed global ownership observation");
        ++reachedFaults;
        Check(boundary.calls[Op::HostRect] == 0 && boundary.calls[Op::ReadSource] == 0 &&
              boundary.calls[Op::Input] == 0 && boundary.calls[Op::Visual] == 0 &&
              boundary.calls[Op::Invalidate] == 0,
              "failed unchanged ownership read performs neither geometry work nor presentation writes");
        boundary.ResetCounts();
        Check(Succeeded(session.Reset(f.api, journal)) && !journal.armed,
              "ownership read failure retains sufficient recovery state for a clean reset");
    }
    for (bool visual : {false, true}) {
        ++scenarios;
        Fixture f;
        const auto frame = Frame();
        glide::session::Session session;
        Journal journal;
        Check(Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result),
              "establish cache before foreign global takeover");
        if (visual) boundary.global.visual = {3.0f, 21, -19};
        else boundary.global.input = {};
        const auto foreign = boundary.global;
        boundary.ResetCounts();
        Check(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result ==
              glide::session::Result::Conflict && !f.renderer.Active() &&
              !session.OwnsState() && !journal.armed,
              "cached frame observes and relinquishes a foreign global transform");
        Check(boundary.global == foreign && boundary.calls[Op::Input] == 0 && boundary.calls[Op::Visual] == 0 &&
              boundary.calls[Op::Invalidate] == 0 && boundary.calls[Op::ReadSource] == 0,
              "foreign takeover is neither overwritten nor hidden behind a stale private refresh");
    }
    for (const Op op : {Op::ReadVisual, Op::ReadInput, Op::ZOrder, Op::Invalidate})
        for (bool after : {false, true}) for (bool stopping : {false, true}) {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        const auto frame = Frame();
        glide::runtime::State state;
        Check(state.PrepareEnable() && state.Activate(), "activate cached reentry fixture");
        glide::session::Session session;
        Journal journal;
        Check(Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result),
              "establish same frame before cached-path callback");
        boundary.ResetCounts();
        boundary.callbackOp = op;
        boundary.callbackOrdinal = 1;
        boundary.callbackAfter = after;
        boundary.callback = [&]() {
            if (stopping) state.Stop(); else state.Pause();
            state.RequestRestore();
        };
        glide::frame::Lease lease(state);
        glide::frame::Backend backend(f.api, lease);
        glide::frame::Renderer renderer(f.renderer, lease);
        Check(!Succeeded(glide::monitor::Present(frame, renderer, session, backend, journal).result) &&
              boundary.callbackReached && !lease.Valid() && !f.renderer.Active(),
              "unchanged-frame cancellation remains fenced at the actual read and refresh boundaries");
        ++reachedReentries;
        Check(boundary.calls[Op::Input] == 0 && boundary.calls[Op::Visual] == 0 &&
              boundary.calls[Op::HostRect] == 0 && boundary.calls[Op::ReadSource] == 0 &&
              !f.renderer.Matches(frame) && !f.renderer.PaintWindow(),
              "cancelled cached path performs no redundant geometry/global writes and exposes no stale paint handle");
        Check(Succeeded(session.Reset(f.api, journal)) && !journal.armed &&
              glide::session::IsIdentity(boundary.global),
              "deferred recovery restores recognized global state after cached-path cancellation");
    }
}

void NativeCursorBoundary() {
    for (const DWORD flags : {DWORD{0}, CURSOR_SHOWING, CURSOR_SUPPRESSED, CURSOR_SHOWING | CURSOR_SUPPRESSED}) {
        ++scenarios;
        Fixture f;
        glide::cursor::NativeBackend native(f.api);
        boundary.cursorFlags = flags;
        bool showing = true;
        Check(native.CursorShowing(showing) && showing == (flags == CURSOR_SHOWING),
              "actual cursor adapter distinguishes visible, hidden, and touch/pen-suppressed baseline");
        glide::cursor::Lease lease;
        Journal journal;
        boundary.enforceCursorJournal = true;
        Check(lease.Update(true, native, journal), "cursor acquisition handles all observable baseline states");
        Check(boundary.calls[Op::HideCursor] == (flags == CURSOR_SHOWING ? 1U : 0U),
              "already hidden or suppressed baseline never receives a hide write");
        Check(lease.Release(native, journal) && !boundary.cursorRecovery,
              "cursor baseline test releases any owned lease");
        Check(boundary.calls[Op::ShowCursor] == (flags == CURSOR_SHOWING ? 1U : 0U),
              "unowned baseline never receives a show write");
    }
    for (const Effect effect : {Effect::Before, Effect::DesktopAfter}) {
        ++scenarios; ++faultScenarios;
        Fixture f;
        glide::cursor::NativeBackend native(f.api);
        boundary.Arm(Op::ReadCursor, effect);
        bool showing = true;
        Check(!native.CursorShowing(showing) && !showing && boundary.fault.reached,
              "cursor snapshot read failure or desktop transition returns no visibility claim");
        ++reachedFaults;
        Check(boundary.calls[Op::HideCursor] == 0 && boundary.calls[Op::ShowCursor] == 0,
              "cursor query failure cannot mutate cursor visibility");
    }
    for (const bool show : {false, true}) for (const Effect effect : {Effect::Before, Effect::After, Effect::DesktopAfter}) {
        ++scenarios; ++faultScenarios;
        Fixture f;
        glide::cursor::NativeBackend native(f.api);
        boundary.systemCursorVisible = !show;
        boundary.Arm(show ? Op::ShowCursor : Op::HideCursor, effect);
        Check(!native.ShowSystemCursor(show) && boundary.fault.reached,
              "native adapter reports failed or desktop-ambiguous visibility writes");
        ++reachedFaults;
        Check(boundary.systemCursorVisible == (effect == Effect::Before ? !show : show),
              "test distinguishes no-effect failures from native effects despite failure");
    }
    {
        ++scenarios;
        Fixture f;
        glide::cursor::NativeBackend native(f.api);
        boundary.inputDesktop = false;
        bool showing = true;
        Check(!native.CursorShowing(showing) && !showing && !native.ShowSystemCursor(false),
              "other desktop blocks the actual native cursor boundary");
        Check(boundary.calls[Op::ReadCursor] == 0 && boundary.calls[Op::HideCursor] == 0,
              "secure-desktop rejection happens before any cursor API access");
        boundary.inputDesktop = true;
        f.api.ShowSystemCursor = nullptr;
        Check(!native.ShowSystemCursor(false) && boundary.calls[Op::HideCursor] == 0,
              "missing MagShowSystemCursor export fails without a native call");
        Check(!f.api.WindowedAvailable(), "renderer availability includes cursor visibility capability");
    }
    for (const Effect effect : {Effect::Before, Effect::After, Effect::DesktopAfter}) {
        ++scenarios; ++faultScenarios;
        Fixture f;
        glide::cursor::NativeBackend native(f.api);
        glide::cursor::Lease lease;
        Journal journal;
        boundary.enforceCursorJournal = true;
        boundary.Arm(Op::HideCursor, effect);
        Check(!lease.Update(true, native, journal) && boundary.fault.reached,
              "lease detects actual adapter hide failure");
        ++reachedFaults;
        const bool deferred = effect == Effect::DesktopAfter;
        Check(lease.NeedsRecovery() == deferred && boundary.cursorRecovery == deferred,
              "post-hide desktop transition retains recovery; ordinary failures restore immediately");
        Check(boundary.systemCursorVisible != deferred,
              "native hiding is undone immediately when desktop remains accessible");
        boundary.inputDesktop = true;
        boundary.ResetCounts();
        Check(lease.Release(native, journal) && !boundary.cursorRecovery &&
              boundary.systemCursorVisible, "deferred native cursor restoration succeeds on return");
        Check(boundary.calls[Op::ShowCursor] == (deferred ? 1U : 0U),
              "release only writes when cursor recovery remains owned");
    }
    for (const Effect effect : {Effect::Before, Effect::After, Effect::DesktopAfter}) {
        ++scenarios; ++faultScenarios;
        Fixture f;
        glide::cursor::NativeBackend native(f.api);
        glide::cursor::Lease lease;
        Journal journal;
        boundary.enforceCursorJournal = true;
        Check(lease.Update(true, native, journal) && lease.Hidden(), "establish cursor lease before restore fault");
        boundary.Arm(Op::ShowCursor, effect);
        Check(!lease.Release(native, journal) && boundary.fault.reached &&
              lease.NeedsRecovery() && boundary.cursorRecovery,
              "uncertain native restore always retains recovery responsibility");
        ++reachedFaults;
        boundary.inputDesktop = true;
        boundary.ResetCounts();
        Check(lease.Release(native, journal) && boundary.systemCursorVisible &&
              !lease.NeedsRecovery() && !boundary.cursorRecovery,
              "repeat boolean show safely resolves uncertain native restoration");
        Check(boundary.calls[Op::ShowCursor] == 1 && boundary.calls[Op::HideCursor] == 0,
              "retry never hides the cursor while restoring an ambiguous write");
    }
}

void NativeCursorRendererFusion() {
    for (const auto monitor : std::array<glide::Rect, 3>{{{-1920, 0, 0, 1080}, {0, 0, 1920, 1080},
                                                       {1920, -240, 3840, 840}}}) {
        ++scenarios;
        Fixture f;
        glide::session::Session session;
        Journal journal;
        glide::cursor::NativeBackend native(f.api);
        glide::cursor::Lease cursor;
        boundary.enforceInputBeforeShow = boundary.enforceCursorJournal = true;
        const auto frame = Frame(monitor, 3.219);
        Check(Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result),
              "cursor fusion starts with real native renderer and verified input mapping");
        const auto center = glide::ScreenToSource({(monitor.left + monitor.right) / 2,
                                                   (monitor.top + monitor.bottom) / 2}, frame.transform);
        for (unsigned crossing = 0; crossing < 32; ++crossing) {
            const bool inside = crossing % 2 == 0;
            const glide::Vec2 point = inside ? center : glide::Vec2{monitor.right + 10, center.y};
            const bool replacement = f.renderer.ReplacementVisibleAt(point);
            Check(replacement == inside, "monitor crossing changes actual replacement visibility");
            Check(cursor.Update(replacement, native, journal), "monitor crossing updates native cursor lease");
            Check(static_cast<unsigned>(replacement) + static_cast<unsigned>(boundary.systemCursorVisible) == 1,
                  "model has exactly one visible cursor at each settled inside/outside monitor position");
            Check(cursor.NeedsRecovery() == inside && boundary.cursorRecovery == inside,
                  "cursor journal ownership follows replacement visibility");
            Check(Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result),
                  "cursor changes do not corrupt native zoom or input ownership");
        }
        Check(cursor.Update(f.renderer.ReplacementVisibleAt(center), native, journal) && cursor.Hidden(),
              "reacquire before presentation failure");
        boundary.Arm(Op::Invalidate, Effect::After);
        Check(!Succeeded(glide::monitor::Present(frame, f.renderer, session, f.api, journal).result) &&
              !f.renderer.ReplacementVisibleAt(center) && boundary.fault.reached,
              "actual native rendering failure revokes replacement cursor eligibility");
        ++faultScenarios; ++reachedFaults;
        Check(cursor.Release(native, journal) && boundary.systemCursorVisible && !boundary.cursorRecovery,
              "presentation failure restores the ordinary cursor through the actual native adapter");
        Check(Succeeded(session.Reset(f.api, journal)) && glide::session::IsIdentity(boundary.global),
              "cursor and input restorations complete independently without losing either lease");
    }
}

void Ready(glide::runtime::State& state) {
    Check(state.PrepareEnable() && state.Activate() && state.Ready(), "activate frame transaction fixture");
}

void FrameLeaseBasics() {
    {
        ++scenarios;
        glide::runtime::State state;
        glide::frame::Lease initiallyBlocked(state);
        unsigned calls = 0;
        Check(!initiallyBlocked.Call([&]() { ++calls; return true; }) && calls == 0,
              "not-ready lease cannot enter a native operation");
        Ready(state);
        Check(!initiallyBlocked.Valid(), "a lease created while inactive never acquires later ownership");
        glide::frame::Lease lease(state);
        Check(lease.Call([&]() { ++calls; return true; }) && calls == 1,
              "current ready lease permits one native operation");
        Check(!lease.Call([&]() { state.RequestRestore(); return false; }),
              "native failure still observes cancellation after its call");
        state.RestoreComplete();
        Check(state.Ready() && !lease.Valid() &&
              !lease.Call([&]() { ++calls; return true; }) && calls == 1,
              "observed temporary restore cannot revive an invalidated lease in the same epoch");
    }
    {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        glide::runtime::State state;
        Ready(state);
        glide::frame::Lease lease(state);
        glide::frame::Backend backend(f.api, lease);
        const glide::session::Snapshot sentinel{{3.0f, 23, -17}, {}};
        auto observed = sentinel;
        boundary.callbackOp = Op::ReadVisual;
        boundary.callbackOrdinal = 1;
        boundary.callbackAfter = true;
        boundary.callback = [&]() { state.Pause(); state.RequestRestore(); };
        Check(!backend.Read(observed) && observed == sentinel && boundary.callbackReached,
              "native read invalidated during callback never publishes its temporary snapshot");
        ++reachedReentries;
        boundary.ResetCounts();
        Check(!backend.SetVisual({2.0f, 1, 2}) &&
              !backend.SetInput({true, {0, 0, 960, 540}, {0, 0, 1920, 1080}}),
              "cancelled native backend rejects all following writes");
        Check(boundary.calls[Op::Visual] == 0 && boundary.calls[Op::Input] == 0,
              "cancelled setters are blocked before entering the native API");
    }
}

void FrameReentryFusion() {
    // Each callback represents a synchronous sent-message reset/pause/desktop
    // change/exit request. The intent invalidates the lifecycle immediately;
    // actual native restoration is deliberately deferred until Present returns.
    const std::array<Op, 9> boundaries{{Op::Source, Op::Matrix, Op::ReadSource, Op::ReadMatrix,
                                      Op::ReadVisual, Op::ReadInput, Op::Input, Op::Show, Op::Invalidate}};
    for (unsigned intent = 0; intent < 4; ++intent) for (bool previouslyOwned : {false, true})
        for (const Op boundaryOp : boundaries) for (bool after : {false, true}) {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        glide::runtime::State state;
        Ready(state);
        glide::session::Session session;
        Journal journal;
        boundary.enforceInputBeforeShow = true;
        if (previouslyOwned) {
            const auto previous = Frame({0, 0, 1920, 1080}, 1.731, 0.19);
            Check(Succeeded(glide::monitor::Present(previous, f.renderer, session, f.api, journal).result),
                  "establish old native frame before sent-message cancellation");
        }
        boundary.ResetCounts();
        glide::frame::Lease lease(state);
        glide::frame::Backend backend(f.api, lease);
        glide::frame::Renderer renderer(f.renderer, lease);
        const auto next = Frame({-2560, -170, 0, 1270}, 4.321, 0.73);
        std::size_t invalidatedAt = 0;
        boundary.callbackOp = boundaryOp;
        boundary.callbackOrdinal = 1;
        boundary.callbackAfter = after;
        boundary.callback = [&]() {
            switch (intent) {
            case 0: state.Cancel(); break;
            case 1: state.Pause(); break;
            case 2: state.Suspend(); break;
            case 3: state.Stop(); break;
            }
            state.RequestRestore();
            invalidatedAt = boundary.trace.size();
        };
        const auto outcome = glide::monitor::Present(next, renderer, session, backend, journal);
        Check(boundary.callbackReached, "scheduled synchronous native reentry was reached");
        ++reachedReentries;
        Check(!Succeeded(outcome.result) && !lease.Valid() && !f.renderer.Active(),
              "cancelled outer frame cannot report success or leave a private overlay visible");
        Check(!f.renderer.ReplacementVisibleAt({-1000, 300}),
              "cancelled outer frame cannot justify a new cursor hide");
        for (std::size_t i = invalidatedAt; i < boundary.trace.size(); ++i)
            Check(boundary.trace[i] != Op::Visual && boundary.trace[i] != Op::Input,
                  "no subsequent global setter begins after the in-flight native callback invalidates the lease");
        // This is the same conditional drain used by the application after its
        // busy stack unwinds. A setter already in progress may have applied its
        // effect despite cancellation, so preserve and resolve its journal.
        f.renderer.Hide();
        Check(Succeeded(session.Reset(f.api, journal)) &&
              glide::session::IsIdentity(boundary.global) && !session.NeedsRecovery() && !journal.armed,
              "deferred drain recovers every recognized in-flight effect without recursive Session access");
        Check(!lease.Valid(), "drained old frame cannot become valid for publication again");
        Check(boundary.calls[Op::Visual] == 0,
              "monitor reentry and recovery never touch the full-screen visual transform");
    }
}

void FullscreenFrameReentry() {
    const glide::session::Snapshot desired{{2.5f, -311, 47},
        {true, {-311, 47, 457, 479}, {0, 0, 1920, 1080}}};
    for (const Op op : {Op::Visual, Op::Input}) for (bool after : {false, true})
        for (bool previouslyOwned : {false, true}) for (bool exiting : {false, true}) {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        glide::runtime::State state;
        Ready(state);
        glide::session::Session session;
        Journal journal;
        if (previouslyOwned) {
            const glide::session::Snapshot previous{{1.5f, -10, 20},
                {true, {-10, 20, 1270, 740}, {0, 0, 1920, 1080}}};
            Check(Succeeded(session.Apply(previous, f.api, journal)), "establish owned full-screen frame");
        }
        boundary.ResetCounts();
        glide::frame::Lease lease(state);
        glide::frame::Backend backend(f.api, lease);
        boundary.callbackOp = op;
        boundary.callbackOrdinal = 1;
        boundary.callbackAfter = after;
        boundary.callback = [&]() {
            if (exiting) state.Stop(); else state.Cancel();
            state.RequestRestore();
        };
        Check(!Succeeded(session.Apply(desired, backend, journal)) && boundary.callbackReached && !lease.Valid(),
              "full-screen native visual/input write cancellation cannot publish success");
        ++reachedReentries;
        Check(Succeeded(session.Reset(f.api, journal)) && !journal.armed &&
              glide::session::IsIdentity(boundary.global),
              "deferred full-screen recovery handles either in-flight visual or input effect");
    }
    for (const Op op : {Op::ReadInput, Op::Input}) for (bool previouslyOwned : {false, true}) {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        glide::runtime::State state;
        Ready(state);
        glide::session::Session session;
        Journal journal;
        const glide::session::Snapshot foreign{{3.5f, 19, -71}, {}};
        if (previouslyOwned) Check(Succeeded(session.Apply(desired, f.api, journal)), "own state before external conflict");
        boundary.ResetCounts();
        glide::frame::Lease lease(state);
        glide::frame::Backend backend(f.api, lease);
        boundary.callbackOp = op;
        boundary.callbackOrdinal = 1;
        boundary.callbackAfter = true;
        boundary.callback = [&]() {
            state.Pause(); state.RequestRestore();
            boundary.global = foreign;
        };
        const glide::session::Snapshot next{{4.0f, 23, -53},
            {true, {23, -53, 503, 217}, {0, 0, 1920, 1080}}};
        Check(!Succeeded(session.Apply(next, backend, journal)) && boundary.callbackReached,
              "cancellation with foreign global takeover blocks stale success");
        ++reachedReentries;
        const auto recovered = session.Reset(f.api, journal);
        Check((recovered == glide::session::Result::Conflict || recovered == glide::session::Result::Unchanged) &&
              !session.NeedsRecovery() && !journal.armed && boundary.global == foreign,
              "deferred drain relinquishes foreign state instead of restoring over another owner");
    }
}

void PrivateInvalidationReentry() {
    for (const Op op : {Op::Invalidate}) for (unsigned action = 0; action < 7; ++action) {
        ++scenarios; ++reentryScenarios;
        Fixture f;
        const auto frame = Frame();
        Check(f.renderer.Prepare(frame), "prepare before private invalidation reentry");
        boundary.callbackOp = op;
        boundary.callbackOrdinal = 1;
        boundary.callbackAfter = true;
        boundary.callback = [&]() {
            switch (action) {
            case 0: ShowWindow(boundary.Host(), SW_HIDE); break;
            case 1: ShowWindow(boundary.Child(), SW_HIDE); break;
            case 2: DestroyWindow(boundary.Host()); break;
            case 3: DestroyWindow(boundary.Child()); break;
            case 4: f.renderer.Hide(); break;
            case 5: f.renderer.Destroy(); break;
            case 6:
                f.renderer.Destroy();
                Check(f.renderer.Prepare(Frame({0, 0, 1920, 1080}, 1.73)) && f.renderer.Show(),
                      "nested native callback replaces the original overlay handles");
                break;
            }
        };
        Check(!f.renderer.Show() && boundary.callbackReached,
              "invalidation cannot report success after native hide, destruction, or handle replacement");
        ++reachedReentries;
        Check(!f.renderer.Active() && !f.renderer.ReplacementVisibleAt({-1000, 300}),
              "post-invalidation validation hides any stale replacement and revokes cursor eligibility");
        f.renderer.Destroy();
        Check(boundary.Alive() == 0, "invalidation cancellation retains all handles needed for teardown");
        Check(f.renderer.Prepare(frame) && f.renderer.Show(), "native invalidation reentry supports fresh safe recreation");
        Verify(frame, f.renderer);
    }
}

enum class TextFailure { None, ReturnBefore, ReturnAfter, ThrowBefore, ThrowAfter };
struct NativeTextTarget {
    std::wstring visible;
    unsigned writes = 0;
    TextFailure failure = TextFailure::None;
    std::function<void()> callback;
    bool Set(std::wstring_view text) {
        ++writes;
        if (failure == TextFailure::ThrowBefore) throw std::runtime_error("text setter before effect");
        if (failure == TextFailure::ReturnBefore) return false;
        visible.assign(text);
        if (callback) {
            auto invoke = std::move(callback);
            callback = {};
            invoke();
        }
        if (failure == TextFailure::ThrowAfter) throw std::runtime_error("text setter after effect");
        return failure != TextFailure::ReturnAfter;
    }
};

void NativeTextCache() {
    {
        ++scenarios; ++textCacheScenarios;
        std::array<glide::ui::TextCache, 3> caches;
        std::array<NativeTextTarget, 3> native;
        const std::array<std::wstring, 3> values{{L"Ready | 2.00x | One monitor",
            L"Three-finger gesture: contact reports detected", L"GlideZoom - Ctrl+Alt+Esc to reset"}};
        for (unsigned slot = 0; slot < caches.size(); ++slot) {
            const auto owner = static_cast<std::uintptr_t>(slot + 1);
            Check(caches[slot].Update(owner, values[slot], [&]() { return native[slot].Set(values[slot]); }),
                  "first status/touchpad/tray text invokes its native setter");
        }
        for (unsigned tick = 0; tick < 1000; ++tick) for (unsigned slot = 0; slot < caches.size(); ++slot) {
            const auto owner = static_cast<std::uintptr_t>(slot + 1);
            Check(caches[slot].Update(owner, values[slot], [&]() { return native[slot].Set(values[slot]); }),
                  "unchanged UI text remains a successful cache hit");
        }
        for (unsigned slot = 0; slot < caches.size(); ++slot)
            Check(native[slot].writes == 1 && native[slot].visible == values[slot],
                  "1000 unchanged ticks perform zero extra native label or notification updates");
        const std::wstring changedStatus = L"Ready | 2.01x | One monitor";
        const std::wstring changedTip = values[2];
        Check(caches[0].Update(1, changedStatus, [&]() { return native[0].Set(changedStatus); }) &&
              caches[1].Update(2, values[1], [&]() { return native[1].Set(values[1]); }) &&
              caches[2].Update(3, changedTip, [&]() { return native[2].Set(changedTip); }),
              "changed zoom updates numeric main status while active tray and touchpad text stay stable");
        Check(native[0].writes == 2 && native[1].writes == 1 && native[2].writes == 1,
              "one changed zoom makes one label write and no notification update");
    }
    {
        ++scenarios; ++textCacheScenarios;
        glide::ui::TextCache cache;
        NativeTextTarget native;
        const std::wstring value = L"Paused";
        Check(!cache.Update(0, value, [&]() { return native.Set(value); }) && native.writes == 0,
              "missing HWND/icon owner is rejected without a native call");
        Check(cache.Update(11, value, [&]() { return native.Set(value); }) && native.writes == 1,
              "cache establishes a successful owner/value pair");
        Check(cache.Update(22, value, [&]() { return native.Set(value); }) && native.writes == 2,
              "different native owner receives text even when string is unchanged");
        cache.Invalidate();
        Check(cache.Update(22, value, [&]() { return native.Set(value); }) && native.writes == 3,
              "recreated control or Explorer icon with reused owner gets a fresh native write");
        Check(cache.Update(22, value, [&]() { return native.Set(value); }) && native.writes == 3,
              "the recreated owner then caches its successfully written value");
    }
    for (const TextFailure failure : {TextFailure::ReturnBefore, TextFailure::ReturnAfter,
                                     TextFailure::ThrowBefore, TextFailure::ThrowAfter}) {
        ++scenarios; ++textCacheScenarios;
        glide::ui::TextCache cache;
        NativeTextTarget native;
        const std::wstring old = L"Ready", changed = L"Paused";
        Check(cache.Update(1, old, [&]() { return native.Set(old); }), "establish UI cache before native failure");
        native.failure = failure;
        bool success = false, threw = false;
        try { success = cache.Update(1, changed, [&]() { return native.Set(changed); }); }
        catch (const std::runtime_error&) { threw = true; }
        const bool throws = failure == TextFailure::ThrowBefore || failure == TextFailure::ThrowAfter;
        const bool effect = failure == TextFailure::ReturnAfter || failure == TextFailure::ThrowAfter;
        Check(!success && threw == throws && native.writes == 2 && native.visible == (effect ? changed : old),
              "UI failure tests reach and distinguish effects, failure returns and exceptions");
        native.failure = TextFailure::None;
        Check(cache.Update(1, old, [&]() { return native.Set(old); }) && native.writes == 3 && native.visible == old,
              "failed or throwing write invalidates old cache and releases busy guard for an explicit repair");
        Check(cache.Update(1, old, [&]() { return native.Set(old); }) && native.writes == 3,
              "only the verified repair becomes cacheable");
        Check(cache.Update(1, changed, [&]() { return native.Set(changed); }) && native.writes == 4 &&
              native.visible == changed, "changed text retries successfully after any native failure boundary");
    }
    for (unsigned action = 0; action < 3; ++action) {
        ++scenarios; ++textCacheScenarios;
        glide::ui::TextCache cache;
        NativeTextTarget native;
        const std::wstring old = L"Ready", changed = L"Paused", replacement = L"Waiting to restore zoom";
        Check(cache.Update(1, old, [&]() { return native.Set(old); }), "establish cache before native callback");
        bool callbackReached = false, nestedResult = false;
        unsigned forbiddenNestedWrites = 0;
        native.callback = [&]() {
            callbackReached = true;
            if (action == 0) cache.Invalidate();
            else nestedResult = cache.Update(action == 1 ? 1 : 2, replacement, [&]() {
                ++forbiddenNestedWrites;
                return native.Set(replacement);
            });
        };
        Check(!cache.Update(1, changed, [&]() { return native.Set(changed); }) && callbackReached,
              "sent-message invalidation or nested update prevents stale outer cache publication");
        Check(!nestedResult && forbiddenNestedWrites == 0 && native.writes == 2 && native.visible == changed,
              "reentrant UI update does not recursively call the native setter");
        const auto owner = static_cast<std::uintptr_t>(action == 2 ? 2 : 1);
        Check(cache.Update(owner, replacement, [&]() { return native.Set(replacement); }) &&
              native.writes == 3 && native.visible == replacement,
              "cancelled outer update leaves a clean retry path for the current owner and value");
        Check(cache.Update(owner, replacement, [&]() { return native.Set(replacement); }) && native.writes == 3,
              "current successful retry is retained without another native write");
    }
    {
        ++scenarios; ++textCacheScenarios;
        glide::ui::TextCache cache;
        NativeTextTarget native;
        std::wstring input = L"Ready";
        const std::wstring written = input;
        Check(cache.Update(1, input, [&]() {
            const bool result = native.Set(input);
            input = L"Paused";
            return result;
        }), "cache snapshots requested text before entering a native callback");
        Check(cache.Update(1, written, [&]() { return native.Set(written); }) && native.writes == 1,
              "later input-buffer mutation cannot corrupt the successfully written cache value");
        Check(cache.Update(1, input, [&]() { return native.Set(input); }) && native.writes == 2 &&
              native.visible == input, "mutated input buffer is treated as a distinct future update");
    }
}
} // namespace

DWORD GetLastError() noexcept { return boundary.error; }
void SetLastError(DWORD error) noexcept { boundary.error = error; }
DWORD GetCurrentThreadId() noexcept { return 1; }
HDESK GetThreadDesktop(DWORD) noexcept { return reinterpret_cast<HDESK>(1); }
BOOL GetUserObjectInformationW(HANDLE desktop, int index, void* output, DWORD bytes, DWORD* needed) noexcept {
    if (boundary.Hit(Op::DesktopName)) return FALSE;
    const wchar_t* name = desktop == reinterpret_cast<HDESK>(1) || boundary.inputDesktop ? L"Default" : L"Winlogon";
    const auto size = static_cast<DWORD>((std::wcslen(name) + 1) * sizeof(wchar_t));
    if (needed) *needed = size;
    if (index != UOI_NAME || !output || bytes < size) return FALSE;
    std::wmemcpy(static_cast<wchar_t*>(output), name, std::wcslen(name) + 1);
    return TRUE;
}
HDESK OpenInputDesktop(DWORD, BOOL, DWORD access) noexcept {
    Check(access == DESKTOP_READOBJECTS, "desktop checks request read-only access");
    if (boundary.Hit(Op::OpenDesktop)) return nullptr;
    ++boundary.openDesktops;
    return reinterpret_cast<HDESK>(2);
}
BOOL CloseDesktop(HDESK desktop) noexcept {
    boundary.Hit(Op::CloseDesktop);
    Check(desktop == reinterpret_cast<HDESK>(2) && boundary.openDesktops,
          "close exactly the input-desktop handle that was opened");
    --boundary.openDesktops;
    return TRUE;
}
HMODULE LoadLibraryExW(const wchar_t*, HANDLE, DWORD) noexcept { return nullptr; }
FARPROC GetProcAddress(HMODULE, const char*) noexcept { return nullptr; }
BOOL FreeLibrary(HMODULE) noexcept { return TRUE; }
WORD RegisterClassExW(const WNDCLASSEXW* windowClass) noexcept {
    if (boundary.Hit(Op::Register)) return 0;
    if (boundary.hasClass || boundary.foreignClass) { SetLastError(ERROR_CLASS_ALREADY_EXISTS); return 0; }
    boundary.registered = *windowClass;
    boundary.hasClass = true;
    return 1;
}
BOOL GetClassInfoExW(HINSTANCE, const wchar_t*, WNDCLASSEXW* windowClass) noexcept {
    if (boundary.Hit(Op::ClassInfo)) return FALSE;
    *windowClass = boundary.registered;
    if (boundary.foreignClass) windowClass->lpfnWndProc = DefWindowProcW;
    return boundary.hasClass || boundary.foreignClass;
}
BOOL UnregisterClassW(const wchar_t*, HINSTANCE) noexcept {
    boundary.Hit(Op::Unregister);
    Check(boundary.Alive() == 0, "class is unregistered only after every private window is destroyed");
    boundary.hasClass = false;
    return TRUE;
}
LONG_PTR GetWindowLongPtrW(HWND window, int) noexcept { return window ? window->userData : 0; }
LONG_PTR SetWindowLongPtrW(HWND window, int, LONG_PTR data) noexcept {
    if (boundary.Hit(Op::UserData)) return 0;
    const LONG_PTR old = window->userData;
    window->userData = data;
    return old;
}
HWND CreateWindowExW(DWORD ex, const wchar_t* name, const wchar_t*, DWORD style,
                     int x, int y, int width, int height, HWND parent, HMENU menu,
                     HINSTANCE instance, void* parameter) noexcept {
    if (boundary.Hit(parent ? Op::CreateChild : Op::CreateHost)) return nullptr;
    Check(parent ? std::wcscmp(name, L"Magnifier") == 0 : boundary.hasClass,
          "host uses its registered class and child uses the magnifier class");
    auto value = std::make_unique<MockRendererWindow>();
    HWND window = value.get();
    value->parent = parent; value->style = style; value->extendedStyle = ex;
    value->visible = (style & WS_VISIBLE) != 0;
    value->local = {x, y, x + width, y + height};
    value->procedure = parent ? nullptr : boundary.registered.lpfnWndProc;
    boundary.windows.push_back(std::move(value));
    if (window->procedure) {
        CREATESTRUCTW creation{parameter, instance, menu, parent, height, width, y, x,
            static_cast<LONG>(style), nullptr, name, ex};
        if (!window->procedure(window, WM_NCCREATE, 0, reinterpret_cast<LPARAM>(&creation))) {
            window->procedure(window, WM_NCDESTROY, 0, 0);
            window->alive = false;
            return nullptr;
        }
    }
    return window;
}
BOOL DestroyWindow(HWND window) noexcept {
    const bool fail = boundary.Hit(Op::Destroy);
    if (!window || !window->alive || Before(fail)) return FALSE;
    if (window->destroying) return TRUE;
    window->destroying = true;
    for (const auto& child : boundary.windows)
        if (child->alive && child->parent == window) DestroyWindow(child.get());
    if (window->parent && window->parent->alive && window->parent->procedure)
        window->parent->procedure(window->parent, WM_PARENTNOTIFY, WM_DESTROY, reinterpret_cast<LPARAM>(window));
    if (window->procedure) window->procedure(window, WM_NCDESTROY, 0, 0);
    window->visible = false; window->alive = false; window->destroying = false;
    return fail ? FALSE : TRUE;
}
BOOL IsWindow(HWND window) noexcept { return window && window->alive; }
BOOL IsWindowVisible(HWND window) noexcept { return Visible(window); }
BOOL ShowWindow(HWND window, int command) noexcept {
    const bool fail = boundary.Hit(command == SW_HIDE ? Op::Hide : Op::Show);
    if (!window || !window->alive || Before(fail)) return FALSE;
    const bool old = Visible(window);
    window->visible = command != SW_HIDE;
    return old;
}
BOOL SetWindowPos(HWND window, HWND after, int x, int y, int width, int height, UINT flags) noexcept {
    const Op op = (flags & SWP_SHOWWINDOW) ? Op::Show : window->parent ? Op::ChildPosition :
        ((flags & (SWP_NOMOVE | SWP_NOSIZE)) == (SWP_NOMOVE | SWP_NOSIZE) ? Op::Raise : Op::HostPosition);
    Check(window != &boundary.externalTopmost, "renderer never positions another application's window");
    if (window->parent) {
        boundary.childPositionFlags.push_back(flags);
        if (!(flags & SWP_NOSIZE)) ++boundary.childSizeCalls;
    }
    const bool fail = boundary.Hit(op);
    if (!window || !window->alive || Before(fail)) return FALSE;
    if (fail && boundary.fault.effect == Effect::Mismatch) return TRUE;
    Check((flags & SWP_NOACTIVATE) != 0, "every native positioning operation avoids activation");
    if (!window->parent) Check(after == HWND_TOPMOST, "host positioning retains its topmost role");
    if (!window->parent && after == HWND_TOPMOST && !boundary.persistentAbove) boundary.externalAbove = false;
    if (!(flags & SWP_NOSIZE)) {
        if (window->parent && (width != window->local.right - window->local.left ||
                              height != window->local.bottom - window->local.top)) ++boundary.childResizeEffects;
        window->local.right = window->local.left + width; window->local.bottom = window->local.top + height;
    }
    if (!(flags & SWP_NOMOVE)) {
        const LONG w = window->local.right - window->local.left, h = window->local.bottom - window->local.top;
        window->local = {x, y, x + w, y + h};
    }
    if (flags & SWP_SHOWWINDOW) {
        if (boundary.enforceInputBeforeShow)
            Check(boundary.global.visual.zoom == 1.0f && boundary.global.input.enabled &&
                  Same(Rect(boundary.global.input.destination), ScreenRect(window)),
                  "native visibility occurs only after a matching verified input transform");
        window->visible = true;
    }
    if (window->procedure) {
        WINDOWPOS position{window, after, x, y, width, height, flags};
        window->procedure(window, WM_WINDOWPOSCHANGED, 0, reinterpret_cast<LPARAM>(&position));
    }
    boundary.Invoke(op, true);
    return fail ? FALSE : TRUE;
}
BOOL GetWindowRect(HWND window, RECT* rectangle) noexcept {
    const Op op = window && window->parent ? Op::ChildRect : window == boundary.Host() ? Op::HostRect : Op::OccluderRect;
    const bool fail = boundary.Hit(op);
    if (!window || !window->alive || !rectangle || Before(fail)) return FALSE;
    *rectangle = ScreenRect(window);
    boundary.Invoke(op, true);
    if (fail && boundary.fault.effect == Effect::Mismatch) { ++rectangle->right; return TRUE; }
    return fail ? FALSE : TRUE;
}
HWND GetWindow(HWND window, UINT command) noexcept {
    const bool fail = boundary.Hit(Op::ZOrder);
    Check(window && command == GW_HWNDPREV,
          "renderer queries only the bounded topmost predecessor chain");
    if (fail) return nullptr;
    HWND previous = window == boundary.Host() ?
        (boundary.externalAbove ? &boundary.externalTopmost : nullptr) : window->predecessor;
    boundary.Invoke(Op::ZOrder, true);
    return previous;
}
BOOL SetLayeredWindowAttributes(HWND window, DWORD, BYTE alpha, DWORD flags) noexcept {
    const bool fail = boundary.Hit(Op::Layered);
    if (!window || !window->alive || Before(fail)) return FALSE;
    Check(alpha == 255 && flags == LWA_ALPHA, "overlay is explicitly fully opaque");
    window->layered = true;
    return fail ? FALSE : TRUE;
}
BOOL InvalidateRect(HWND window, const RECT*, BOOL erase) noexcept {
    Check(window != nullptr, "renderer never invalidates a null HWND and therefore the whole desktop");
    const bool fail = boundary.Hit(Op::Invalidate);
    if (!window || !window->alive || Before(fail)) return FALSE;
    Check(erase == FALSE, "renderer invalidation avoids erasing a visible background");
    window->dirty = true;
    ++window->invalidations;
    boundary.Invoke(Op::Invalidate, true);
    return fail ? FALSE : TRUE;
}
BOOL UpdateWindow(HWND window) noexcept {
    Check(window != nullptr, "renderer only flushes its explicit private child handle");
    const bool fail = boundary.Hit(Op::Paint);
    if (!window || !window->alive || Before(fail)) return FALSE;
    Check(window->dirty, "synchronous native paint always follows explicit invalidation");
    window->dirty = false;
    ++window->paints;
    boundary.Invoke(Op::Paint, true);
    return fail ? FALSE : TRUE;
}
BOOL GetCursorInfo(CURSORINFO* info) noexcept {
    const bool fail = boundary.Hit(Op::ReadCursor);
    if (Before(fail)) return FALSE;
    Check(info && info->cbSize == sizeof(CURSORINFO), "native cursor query provides the required structure size");
    info->flags = boundary.cursorFlags;
    if (!boundary.systemCursorVisible) info->flags &= ~CURSOR_SHOWING;
    info->hCursor = reinterpret_cast<HCURSOR>(1);
    info->ptScreenPos = {0, 0};
    if (fail && boundary.fault.effect == Effect::DesktopAfter) {
        boundary.inputDesktop = false;
        return TRUE;
    }
    return fail ? FALSE : TRUE;
}
LRESULT DefWindowProcW(HWND, UINT message, WPARAM, LPARAM) noexcept {
    return message == WM_NCCREATE ? TRUE : 0;
}

int Run_monitor_native() {
    SteadyAndLifecycle();
    StableAllocationAndQueuedRefresh();
    CachedFrameAndPaintIdentity();
    GeometryMessagesRevokeCache();
    PreparationInvalidationFence();
    BoundedTopmostObservation();
    TopmostPostRaiseVerification();
    AllocationGrowthAndRepair();
    TopmostRepair();
    FailureMatrix();
    CleanupAndIsolation();
    ExternalCorruption();
    ReplacementCursorGeometry();
    ActualPresenterFusion();
    CachedPresenterOwnershipAndReentry();
    NativeCursorBoundary();
    NativeCursorRendererFusion();
    FrameLeaseBasics();
    FrameReentryFusion();
    FullscreenFrameReentry();
    PrivateInvalidationReentry();
    NativeTextCache();
    Check(boundary.Alive() == 0 && boundary.openDesktops == 0, "final state has no leaked native boundary resources");
    Check(faultScenarios == reachedFaults, "every counted injected boundary was reached");
    Check(reentryScenarios == reachedReentries, "every counted synchronous reentry boundary was reached");
    std::cout << "monitor_native: " << assertions << " assertions, " << scenarios << " scenarios, "
              << faultScenarios << " reached native fault injections, " << reentryScenarios
              << " reached reentry scenarios, " << textCacheScenarios
              << " text-cache scenarios; actual production renderer and presenter\n";
    return 0;
}
