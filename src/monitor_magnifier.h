#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <windows.h>
#include "magnification_api.h"
#include "monitor_geometry.h"

// Private windowed rendering only. The caller owns the global input transform
// through session::Session and must conditionally restore it if Prepare/Show
// fails. The caller also owns ordinary-cursor visibility: ReplacementVisibleAt
// identifies when this private window supplies the magnified replacement. This
// renderer never changes global cursor visibility, speed or visual settings.
class MonitorMagnifier
{
public:
    MonitorMagnifier() = default;
    MonitorMagnifier(const MonitorMagnifier&) = delete;
    MonitorMagnifier& operator=(const MonitorMagnifier&) = delete;
    ~MonitorMagnifier() noexcept
    {
        Destroy();
        if (host_)
            SetWindowLongPtrW(host_, GWLP_USERDATA, 0);
        if (ownsClass_ && instance_)
            UnregisterClassW(kClass, instance_);
    }

    // MagnificationApi::Initialize must already have succeeded on this thread.
    // Destroy the overlay before the caller uninitializes the magnification API.
    bool Initialize(HINSTANCE instance, MagnificationApi& api) noexcept
    {
        if (!instance || !api.WindowedAvailable())
        {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return false;
        }
        if (api_ && (api_ != &api || instance_ != instance))
        {
            SetLastError(ERROR_ALREADY_INITIALIZED);
            return false;
        }
        instance_ = instance;
        api_ = &api;
        return true;
    }

    bool Prepare(const glide::monitor::Frame& frame) noexcept
    {
        prepared_ = false;
        ++geometryRevision_;
        if (!api_ || !api_->WindowedAvailable())
            return Fail(ERROR_PROC_NOT_FOUND);
        if (!glide::OnInputDesktop())
            return Fail(ERROR_ACCESS_DENIED);
        if (!glide::monitor::ValidFrame(frame))
            return Fail(ERROR_INVALID_PARAMETER);
        if (!EnsureWindows())
            return Fail();

        const RECT destination = Native(frame.destination);
        const RECT source = Native(frame.capture);
        const RECT requestedChild = Native(frame.child);
        const bool destinationChanged = !Same(destination_, destination);
        if (destinationChanged)
            Hide();
        const int width = destination.right - destination.left;
        const int height = destination.bottom - destination.top;
        // Keep the native control's allocation stable while its subpixel
        // compensation moves. The logical frame right/bottom stay at the host
        // edge, so using that changing width would resize the control on every
        // fractional pan. The host clips the extra area: source capture, input
        // rectangles and the rendered affine are unchanged.
        const int requiredWidth = static_cast<int>(requestedChild.right - requestedChild.left);
        const int requiredHeight = static_cast<int>(requestedChild.bottom - requestedChild.top);
        const int childWidth = std::max(requiredWidth,
                                        destinationChanged ? width + kChildPadding : childWidth_);
        const int childHeight = std::max(requiredHeight,
                                         destinationChanged ? height + kChildPadding : childHeight_);
        const RECT child{ requestedChild.left, requestedChild.top, requestedChild.left + childWidth, requestedChild.top + childHeight };
        const bool resizeChild = childWidth != childWidth_ || childHeight != childHeight_;
        // Only the host owns monitor coordinates. The oversized child is
        // clipped to the host and shifts by OUTPUT pixels; this compensates
        // the integer capture origin without undocumented matrix translation.
        // Defer ordinary repaint until source, scale and input are all ready.
        if ((destinationChanged &&
             !SetWindowPos(host_, HWND_TOPMOST, destination.left, destination.top, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOREDRAW)) ||
            (!Same(child_, child) &&
             !SetWindowPos(magnifier_, nullptr, child.left, child.top, childWidth, childHeight, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOREDRAW | (resizeChild ? 0 : SWP_NOSIZE))))
            return Fail();

        const auto geometryRevision = geometryRevision_;

        RECT actualDestination{};
        if (!GetWindowRect(host_, &actualDestination))
            return Fail();
        if (!Same(actualDestination, destination))
            return Fail(ERROR_INVALID_DATA);
        RECT actualChild{};
        if (!GetWindowRect(magnifier_, &actualChild))
            return Fail();
        const RECT expectedChild{ destination.left + child.left, destination.top + child.top, destination.left + child.right, destination.top + child.bottom };
        if (!Same(actualChild, expectedChild))
            return Fail(ERROR_INVALID_DATA);

        MagnificationApi::WindowTransform matrix{};
        matrix.v[0][0] = matrix.v[1][1] = static_cast<float>(frame.transform.zoom);
        matrix.v[2][2] = 1.0f;
        if ((!nativeFrame_ || !Same(source_, source)) &&
            !api_->SetWindowSource(magnifier_, source))
            return Fail();
        if ((!nativeFrame_ || !Same(matrix_, matrix)) &&
            !api_->SetWindowTransform(magnifier_, &matrix))
            return Fail();

        RECT actualSource{};
        MagnificationApi::WindowTransform actualMatrix{};
        if (!api_->GetWindowSource(magnifier_, &actualSource) ||
            !api_->GetWindowTransform(magnifier_, &actualMatrix))
            return Fail();
        if (!Same(actualSource, source) || !Same(actualMatrix, matrix))
            return Fail(ERROR_INVALID_DATA);
        if (!glide::OnInputDesktop())
            return Fail(ERROR_ACCESS_DENIED);
        if (geometryRevision_ != geometryRevision)
            return Fail(ERROR_INVALID_STATE);

        destination_ = destination;
        child_ = child;
        childWidth_ = childWidth;
        childHeight_ = childHeight;
        source_ = source;
        matrix_ = matrix;
        frame_ = frame;
        nativeFrame_ = true;
        prepared_ = true;
        return true;
    }

    // Call only after the matching global input transform was verified.
    bool Show() noexcept
    {
        if (!prepared_ || !host_ || !magnifier_)
            return Fail(ERROR_INVALID_STATE);
        const HWND presentedHost = host_, presentedChild = magnifier_;
        const auto samePreparedWindows = [&]() noexcept {
            return prepared_ && nativeFrame_ && host_ == presentedHost &&
                   magnifier_ == presentedChild;
        };
        if (!glide::OnInputDesktop())
            return Fail(ERROR_ACCESS_DENIED);
        // Reclaim an overlapping visible predecessor once. Hidden windows and
        // windows on another monitor do not obscure this overlay. A persistent
        // higher-band window (or another app constantly reclaiming topmost)
        // must not cause a fresh Z-order mutation on every presentation tick.
        // A blocked or continuously reclaimed view fails closed so the caller
        // restores its input/cursor leases instead of mapping an unmagnified
        // foreground overlay. A later clear observation rearms the attempt.
        const bool visible = IsWindowVisible(presentedHost) != FALSE;
        bool raise = !visible;
        HWND above = nullptr;
        if (visible)
        {
            if (!OverlappingPredecessor(presentedHost, above))
                return Fail();
            if (above && above == lastRaiseAgainst_)
                return Fail(ERROR_ACCESS_DENIED);
            raise = above && above != lastRaiseAgainst_;
            if (!above)
                lastRaiseAgainst_ = nullptr;
        }
        if (!samePreparedWindows())
            return Fail(ERROR_INVALID_STATE);
        // ShowWindow's return value is the old visibility, not success.
        if (raise &&
            !SetWindowPos(presentedHost, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | (visible ? 0 : SWP_SHOWWINDOW)))
            return Fail();
        if (raise)
            lastRaiseAgainst_ = above;
        if (!IsWindowVisible(presentedHost) || !IsWindowVisible(presentedChild) ||
            !samePreparedWindows())
            return Fail(ERROR_INVALID_STATE);
        if (raise)
        {
            HWND remaining = nullptr;
            if (!OverlappingPredecessor(presentedHost, remaining))
                return Fail();
            if (!samePreparedWindows())
                return Fail(ERROR_INVALID_STATE);
            if (remaining)
                return Fail(ERROR_ACCESS_DENIED);
        }
        // Keep content live with a queued repaint of this child only. Let
        // Windows coalesce painting; forcing synchronous WM_PAINT every frame
        // adds blocking work while source/scale and window layout are changing.
        // Do not erase the background. Preserve explicit child identity so
        // a reentrant destruction can never turn InvalidateRect into a global
        // null-HWND invalidation. The caller fences lifecycle cancellation.
        if (!InvalidateRect(presentedChild, nullptr, FALSE))
            return Fail();
        // Native calls can still dispatch destruction or hide messages.
        if (!IsWindowVisible(presentedHost) || !IsWindowVisible(presentedChild) ||
            !samePreparedWindows())
            return Fail(ERROR_INVALID_STATE);
        if (!glide::OnInputDesktop())
            return Fail(ERROR_ACCESS_DENIED);
        if (!samePreparedWindows())
            return Fail(ERROR_INVALID_STATE);
        return true;
    }

    void Hide() noexcept
    {
        prepared_ = false;
        ++geometryRevision_;
        lastRaiseAgainst_ = nullptr;
        if (host_)
        {
            ShowWindow(host_, SW_HIDE);
            // Hiding is normally immediate for this thread's own window. If
            // visibility persists, drop the private window rather than leave
            // a stale overlay while the caller restores its input transform.
            if (IsWindowVisible(host_))
                Destroy();
        }
    }

    void Destroy() noexcept
    {
        prepared_ = false;
        ++geometryRevision_;
        if (host_)
        {
            const HWND old = host_;
            if (!DestroyWindow(old) && IsWindow(old))
            {
                // Retain the handle for a later cleanup attempt if Windows
                // refuses destruction; never silently lose a live overlay.
                const DWORD error = GetLastError();
                ShowWindow(old, SW_HIDE);
                SetLastError(error);
                return;
            }
        }
        host_ = magnifier_ = nullptr;
        ForgetFrame();
    }

    bool Active() const noexcept { return host_ && IsWindowVisible(host_); }

    // This is a private-window cache, not proof of global input ownership.
    // The presenter must still check the Session before taking this path.
    // Every field of the validated native frame participates, so an input-only
    // change cannot accidentally reuse the preceding presentation.
    bool Matches(const glide::monitor::Frame& frame) const noexcept
    {
        return prepared_ && nativeFrame_ && host_ && magnifier_ &&
               glide::monitor::SameFrame(frame, frame_);
    }

    bool Pending() const noexcept { return false; } // Legacy synchronous renderer.
    bool CanRetain(const glide::session::Snapshot&) const noexcept { return false; }

    // The UI loop may explicitly retrieve this child's queued WM_PAINT after
    // the frame transaction has unwound. It must recheck its lifecycle fence
    // and this identity before dispatching; never synthesize WM_PAINT or pass
    // a null HWND to a paint/invalidation API.
    HWND PaintWindow() const noexcept
    {
        const HWND host = host_, child = magnifier_;
        if (!prepared_ || !nativeFrame_ || !host || !child ||
            !IsWindowVisible(host) || !IsWindowVisible(child))
            return nullptr;
        return prepared_ && nativeFrame_ && host_ == host && magnifier_ == child ? child : nullptr;
    }

    // The control draws its cursor only inside its capture rectangle, and the
    // host clips that cursor after scaling/child translation. Hide the ordinary
    // cursor only when the replacement's hotspot is actually in the visible
    // monitor. This conservative test keeps a normal cursor on other displays
    // and whenever a sample lies outside the visible crop.
    bool ReplacementVisibleAt(glide::Vec2 point) const noexcept
    {
        if (!prepared_ || !nativeFrame_ || !Active() || !magnifier_ ||
            !IsWindowVisible(magnifier_) || !glide::detail::Finite(point) ||
            point.x < source_.left || point.x >= source_.right ||
            point.y < source_.top || point.y >= source_.bottom)
            return false;
        const double zoom = matrix_.v[0][0];
        const double x = destination_.left + child_.left + (point.x - source_.left) * zoom;
        const double y = destination_.top + child_.top + (point.y - source_.top) * zoom;
        return x >= destination_.left && x < destination_.right &&
               y >= destination_.top && y < destination_.bottom;
    }

private:
    static constexpr wchar_t kClass[] = L"GlideZoom.MonitorMagnifier.1";
    static constexpr wchar_t kMagnifierClass[] = L"Magnifier"; // WC_MAGNIFIER.
    static constexpr DWORD kShowMagnifiedCursor = 0x0001; // MS_SHOWMAGNIFIEDCURSOR.
    static constexpr DWORD kFilterExclude = 0; // MW_FILTERMODE_EXCLUDE.
    // A 64-pixel reserve covers every current production zoom level. It avoids
    // child resizing during ordinary pan/zoom; larger valid geometry grows only when
    // required. For one monitor the extent is at most (width + 4096) by
    // (height + 4096), and for production zoom is (width + 64) by
    // (height + 64). Retargeting resets the allocation; Hide retains it, while
    // destruction or a partial native failure discards the verified cache.
    static constexpr int kChildPadding = 64;
    HINSTANCE instance_ = nullptr;
    MagnificationApi* api_ = nullptr;
    HWND host_ = nullptr, magnifier_ = nullptr;
    RECT destination_{}, child_{}, source_{};
    int childWidth_ = 0, childHeight_ = 0;
    MagnificationApi::WindowTransform matrix_{};
    glide::monitor::Frame frame_{};
    HWND lastRaiseAgainst_ = nullptr;
    std::uint64_t geometryRevision_ = 0;
    bool prepared_ = false, nativeFrame_ = false, ownsClass_ = false, classReady_ = false;

    void ForgetFrame() noexcept
    {
        ++geometryRevision_;
        prepared_ = nativeFrame_ = false;
        destination_ = child_ = source_ = {};
        childWidth_ = childHeight_ = 0;
        matrix_ = {};
        frame_ = {};
        lastRaiseAgainst_ = nullptr;
    }

    static RECT Native(glide::session::Rect rect) noexcept
    {
        return { rect.left, rect.top, rect.right, rect.bottom };
    }
    static bool Same(RECT a, RECT b) noexcept
    {
        return a.left == b.left && a.top == b.top &&
               a.right == b.right && a.bottom == b.bottom;
    }
    static bool Same(const MagnificationApi::WindowTransform& a,
                     const MagnificationApi::WindowTransform& b) noexcept
    {
        for (unsigned row = 0; row < 3; ++row)
            for (unsigned column = 0; column < 3; ++column)
                if (a.v[row][column] != b.v[row][column])
                    return false;
        return true;
    }
    bool OverlappingPredecessor(HWND host, HWND& found) noexcept
    {
        found = nullptr;
        // Native window order can change during a query. Bound this walk and
        // reject cycles rather than assuming a stable foreign window list.
        std::array<HWND, 16> visited{};
        HWND current = host;
        for (std::size_t count = 0; count < visited.size(); ++count)
        {
            SetLastError(ERROR_SUCCESS);
            const HWND previous = GetWindow(current, GW_HWNDPREV);
            if (!previous)
                return GetLastError() == ERROR_SUCCESS;
            if (previous == host ||
                std::find(visited.begin(), visited.begin() + count, previous) != visited.begin() + count)
            {
                SetLastError(ERROR_INVALID_DATA);
                return false;
            }
            visited[count] = previous;
            if (IsWindowVisible(previous))
            {
                RECT rectangle{};
                if (!GetWindowRect(previous, &rectangle))
                {
                    // Do not chase a vanished foreign handle or assume that
                    // its disappearance proves the rest of the order clear.
                    if (!IsWindow(previous))
                        SetLastError(ERROR_INVALID_HANDLE);
                    return false;
                }
                if (rectangle.right > rectangle.left && rectangle.bottom > rectangle.top &&
                    rectangle.left < destination_.right && rectangle.right > destination_.left &&
                    rectangle.top < destination_.bottom && rectangle.bottom > destination_.top)
                {
                    found = previous;
                    return true;
                }
            }
            current = previous;
        }
        // A truncated walk does not prove that the view is unobscured. Keep
        // work bounded and revoke presentation instead of retaining mapping
        // beneath an unobserved topmost overlay.
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    bool Fail(DWORD error = ERROR_SUCCESS) noexcept
    {
        if (!error)
            error = GetLastError();
        Hide();
        // Native operations may have taken effect before a later call failed.
        // No cached position/scale may suppress a repair on the next attempt.
        ForgetFrame();
        SetLastError(error ? error : ERROR_GEN_FAILURE);
        return false;
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
    {
        auto* owner = reinterpret_cast<MonitorMagnifier*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            const auto* creation = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            owner = static_cast<MonitorMagnifier*>(creation->lpCreateParams);
            SetLastError(ERROR_SUCCESS);
            if (!SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner)) &&
                GetLastError() != ERROR_SUCCESS)
                return FALSE;
        }
        if (owner && message == WM_NCDESTROY)
        {
            if (owner->host_ == hwnd)
            {
                owner->host_ = owner->magnifier_ = nullptr;
                owner->ForgetFrame();
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        if (owner && owner->host_ == hwnd && message == WM_WINDOWPOSCHANGED && lParam)
        {
            const auto* position = reinterpret_cast<const WINDOWPOS*>(lParam);
            if (!(position->flags & SWP_NOMOVE) || !(position->flags & SWP_NOSIZE))
            {
                // A window manager can relocate our host independently of a
                // zoom command. Revoke the exact-frame cache and force a full
                // verified repair. Our own Prepare commits after its native
                // positioning calls return; Z-order-only calls stay reusable.
                owner->ForgetFrame();
            }
        }
        if (owner && owner->host_ == hwnd && message == WM_DPICHANGED)
            owner->ForgetFrame();
        if (owner && message == WM_PARENTNOTIFY && LOWORD(wParam) == WM_DESTROY &&
            reinterpret_cast<HWND>(lParam) == owner->magnifier_)
        {
            owner->magnifier_ = nullptr;
            owner->Hide();
        }
        if (message == WM_NCHITTEST)
            return HTTRANSPARENT;
        if (message == WM_MOUSEACTIVATE)
            return MA_NOACTIVATE;
        if (message == WM_ERASEBKGND)
            return 1;
        // This is an auxiliary window: destroying it must not quit the app.
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool EnsureWindows() noexcept
    {
        if (host_ && magnifier_ && IsWindow(host_) && IsWindow(magnifier_))
            return true;
        if (host_)
        {
            Destroy();
            if (host_)
                return false;
        }
        if (!classReady_)
        {
            WNDCLASSEXW windowClass{};
            windowClass.cbSize = sizeof(windowClass);
            windowClass.hInstance = instance_;
            windowClass.lpfnWndProc = WindowProc;
            windowClass.lpszClassName = kClass;
            if (RegisterClassExW(&windowClass))
                ownsClass_ = true;
            else
            {
                if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                    return false;
                WNDCLASSEXW existing{};
                existing.cbSize = sizeof(existing);
                if (!GetClassInfoExW(instance_, kClass, &existing) ||
                    existing.lpfnWndProc != WindowProc)
                {
                    SetLastError(ERROR_CLASS_ALREADY_EXISTS);
                    return false;
                }
            }
            classReady_ = true;
        }
        host_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                                    WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                kClass,
                                L"GlideZoom monitor zoom",
                                WS_POPUP | WS_CLIPCHILDREN,
                                0,
                                0,
                                1,
                                1,
                                nullptr,
                                nullptr,
                                instance_,
                                this);
        if (!host_)
            return false;
        if (!SetLayeredWindowAttributes(host_, 0, 255, LWA_ALPHA))
        {
            const DWORD error = GetLastError();
            Destroy();
            SetLastError(error);
            return false;
        }
        magnifier_ = CreateWindowExW(0, kMagnifierClass, L"GlideZoom magnified content", WS_CHILD | WS_VISIBLE | kShowMagnifiedCursor, 0, 0, 1, 1, host_, nullptr, instance_, nullptr);
        if (!magnifier_)
        {
            const DWORD error = GetLastError();
            Destroy();
            SetLastError(error);
            return false;
        }
        HWND excluded = host_;
        if (!api_->SetWindowFilterList(magnifier_, kFilterExclude, 1, &excluded))
        {
            const DWORD error = GetLastError();
            Destroy();
            SetLastError(error);
            return false;
        }
        return true;
    }
};
