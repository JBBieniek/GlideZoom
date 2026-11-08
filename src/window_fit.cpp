#include "window_fit.h"
#include "desktop_context.h"

#include <dwmapi.h>
#include <cwchar>

namespace glide::window_fit
{
    namespace
    {
        Rect AsRect(const RECT& r) noexcept
        {
            return { r.left, r.top, r.right, r.bottom };
        }
        class PhysicalCoordinates
        {
        public:
            PhysicalCoordinates() noexcept :
                previous_(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {}
            ~PhysicalCoordinates()
            {
                if (previous_)
                    SetThreadDpiAwarenessContext(previous_);
            }
            bool Valid() const noexcept { return previous_ != nullptr; }

        private:
            DPI_AWARENESS_CONTEXT previous_;
        };
        bool SameDesktop(DWORD thread) noexcept
        {
            wchar_t ours[256]{}, theirs[256]{};
            DWORD bytes = 0;
            return GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, ours, sizeof(ours), &bytes) &&
                   GetUserObjectInformationW(GetThreadDesktop(thread), UOI_NAME, theirs, sizeof(theirs), &bytes) &&
                   std::wcscmp(ours, theirs) == 0;
        }
        bool CommandAllowed(HMENU menu, UINT command) noexcept
        {
            if (!menu)
                return false;
            const UINT state = GetMenuState(menu, command, MF_BYCOMMAND);
            return state != static_cast<UINT>(-1) && !(state & (MF_DISABLED | MF_GRAYED));
        }
        bool SameUser(DWORD process) noexcept
        {
            const HANDLE target = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process);
            if (!target)
                return false;
            HANDLE targetToken = nullptr, ownToken = nullptr;
            bool equal = false;
            if (OpenProcessToken(target, TOKEN_QUERY, &targetToken) &&
                OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ownToken))
            {
                alignas(TOKEN_USER) BYTE targetUser[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
                alignas(TOKEN_USER) BYTE ownUser[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
                DWORD targetBytes = 0, ownBytes = 0;
                if (GetTokenInformation(targetToken, TokenUser, targetUser, sizeof(targetUser), &targetBytes) &&
                    GetTokenInformation(ownToken, TokenUser, ownUser, sizeof(ownUser), &ownBytes))
                    equal = EqualSid(reinterpret_cast<const TOKEN_USER*>(targetUser)->User.Sid,
                                     reinterpret_cast<const TOKEN_USER*>(ownUser)->User.Sid) != FALSE;
            }
            if (ownToken)
                CloseHandle(ownToken);
            if (targetToken)
                CloseHandle(targetToken);
            CloseHandle(target);
            return equal;
        }
        class Native
        {
        public:
            Native(HWND owner, DWORD ownProcess) noexcept :
                owner_(owner), ownProcess_(ownProcess) {}

            Reason Read(Identity expected, Observation& output, std::uint32_t& error) const noexcept
            {
                if (!OnInputDesktop())
                    return Reason::UnavailableDesktop;
                const HWND foreground = GetForegroundWindow();
                const HWND window = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
                if (!window)
                    return Reason::NoWindow;
                DWORD process = 0;
                const DWORD thread = GetWindowThreadProcessId(window, &process);
                if (!thread || !process)
                    return Reason::NoWindow;
                const Identity identity{ reinterpret_cast<std::uintptr_t>(window), process, thread };
                if (expected.window && expected != identity)
                    return Reason::ContextChanged;
                Observation observed{};
                observed.identity = identity;
                observed.ownWindow = process == ownProcess_ || window == owner_;
                DWORD session = 0, ownSession = 0;
                observed.sameSession = ProcessIdToSessionId(process, &session) &&
                                       ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) && session == ownSession;
                observed.currentDesktop = SameDesktop(thread);
                observed.sameUser = observed.ownWindow || SameUser(process);
                if (observed.ownWindow || !observed.sameSession || !observed.currentDesktop || !observed.sameUser)
                {
                    output = observed;
                    return Reason::Ready;
                }
                observed.rootWindow = foreground == window;
                observed.ownedWindow = GetWindow(window, GW_OWNER) != nullptr;
                observed.visibleWindow = IsWindowVisible(window) != FALSE;
                observed.iconic = IsIconic(window) != FALSE;
                observed.maximized = IsZoomed(window) != FALSE;
                observed.enabled = IsWindowEnabled(window) != FALSE;
                observed.hung = IsHungAppWindow(window) != FALSE;
                SetLastError(ERROR_SUCCESS);
                const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
                if (!style && GetLastError() != ERROR_SUCCESS)
                {
                    error = GetLastError();
                    return Reason::NativeFailure;
                }
                observed.thickFrame = (style & WS_THICKFRAME) != 0 && (style & WS_CHILD) == 0;
                observed.caption = (style & WS_CAPTION) == WS_CAPTION && (style & WS_SYSMENU) != 0;
                const HMENU systemMenu = GetSystemMenu(window, FALSE);
                observed.moveAllowed = CommandAllowed(systemMenu, SC_MOVE);
                observed.sizeAllowed = CommandAllowed(systemMenu, SC_SIZE);
                DWORD cloaked = 0;
                const HRESULT cloakResult = DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
                RECT outer{}, visible{};
                const HRESULT frameResult = DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &visible, sizeof(visible));
                MONITORINFO info{ sizeof(info) };
                if (FAILED(cloakResult) || FAILED(frameResult) || !GetWindowRect(window, &outer) ||
                    !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONULL), &info))
                {
                    error = GetLastError();
                    return Reason::UnavailableGeometry;
                }
                observed.cloaked = cloaked != 0;
                observed.outer = AsRect(outer);
                observed.visible = AsRect(visible);
                observed.monitor = AsRect(info.rcMonitor);
                output = observed;
                return Reason::Ready;
            }

            Reason Queue(const Observation& expected, Rect desired, std::uint32_t& error, ValidityCallback valid, void* context) const noexcept
            {
                Observation fresh{};
                const Reason read = Read(expected.identity, fresh, error);
                if (valid && !valid(context))
                    return Reason::ContextChanged;
                if (read != Reason::Ready)
                    return read;
                if (Eligible(fresh) != Reason::Ready || fresh.identity != expected.identity ||
                    fresh.outer != expected.outer || fresh.visible != expected.visible || fresh.monitor != expected.monitor)
                    return Reason::ContextChanged;
                const HWND window = reinterpret_cast<HWND>(expected.identity.window);
                DWORD process = 0;
                const DWORD thread = GetWindowThreadProcessId(window, &process);
                if (!OnInputDesktop())
                    return Reason::UnavailableDesktop;
                if (GetForegroundWindow() != window || process != expected.identity.process || thread != expected.identity.thread ||
                    IsIconic(window) || IsZoomed(window) || !IsWindowVisible(window))
                    return Reason::ContextChanged;
                if (valid && !valid(context))
                    return Reason::ContextChanged;
                // ASYNC avoids waiting on a foreign message queue. Do not attach
                // input queues or suppress WM_WINDOWPOSCHANGING: its normal
                // DefWindowProc path enforces WM_GETMINMAXINFO, and apps may refuse.
                const UINT flags = SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;
                if (!SetWindowPos(window, nullptr, desired.left, desired.top, desired.right - desired.left, desired.bottom - desired.top, flags))
                {
                    error = GetLastError();
                    return Reason::NativeFailure;
                }
                return Reason::Ready;
            }

        private:
            HWND owner_;
            DWORD ownProcess_;
        };
    } // namespace

    Result NativeOperation::Begin(HWND owner, DWORD ownProcess, const RECT& source, const RECT& monitor, ULONGLONG now, ValidityCallback valid, void* context) noexcept
    {
        if (operation_.Pending())
        {
            Result result = operation_.Last();
            result.reason = Reason::Busy;
            return result;
        }
        PhysicalCoordinates coordinates;
        if (!coordinates.Valid())
            return operation_.Abandon(Reason::UnavailableGeometry);
        owner_ = owner;
        ownProcess_ = ownProcess ? ownProcess : GetCurrentProcessId();
        Native native(owner_, ownProcess_);
        return operation_.Begin(native, { AsRect(source), AsRect(monitor) }, now, valid, context);
    }
    Result NativeOperation::Poll(ULONGLONG now) noexcept
    {
        if (!operation_.Pending())
            return operation_.Last();
        PhysicalCoordinates coordinates;
        if (!coordinates.Valid())
            return operation_.Abandon(Reason::UnavailableGeometry);
        Native native(owner_, ownProcess_);
        return operation_.Poll(native, now);
    }
    const wchar_t* ReasonText(Reason reason) noexcept
    {
        switch (reason)
        {
        case Reason::Pending:
            return L"Fitting the active window...";
        case Reason::Fitted:
            return L"Active window fitted to the magnified viewport.";
        case Reason::Busy:
            return L"A window fit is already pending.";
        case Reason::InvalidViewport:
            return L"Zoom in on a monitor before fitting a window.";
        case Reason::UnavailableDesktop:
            return L"Window fit is unavailable on this desktop.";
        case Reason::NoWindow:
            return L"No active application window to fit.";
        case Reason::OwnWindow:
            return L"GlideZoom windows cannot be fitted.";
        case Reason::NotResizable:
            return L"The active window cannot be moved and resized.";
        case Reason::Fullscreen:
            return L"Leave fullscreen in the application before fitting its window.";
        case Reason::Maximized:
            return L"Restore the maximized window before fitting it.";
        case Reason::Hung:
            return L"The active application is not responding.";
        case Reason::UnavailableGeometry:
            return L"The window's physical frame could not be read safely.";
        case Reason::ContextChanged:
            return L"The active window changed; window fit stopped.";
        case Reason::NativeFailure:
            return L"Windows refused the window fit.";
        case Reason::AppConstrained:
            return L"The application adjusted the requested size or position.";
        case Reason::TimedOut:
            return L"The application did not confirm the requested window fit.";
        default:
            return L"Window fit is unavailable.";
        }
    }
} // namespace glide::window_fit
