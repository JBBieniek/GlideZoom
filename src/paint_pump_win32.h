#pragma once

#include <windows.h>

namespace glide::paint
{

    enum class Kind
    {
        Idle,
        Painted,
        Cancelled,
        Quit,
        Failed
    };

    struct Result
    {
        Kind kind = Kind::Idle;
        int quitCode = 0;
    };

    namespace detail
    {
        // Shared across callback/template types. Sent messages and a paint procedure
        // can reenter the app; nested service must never recurse into another paint.
        inline thread_local bool servicing = false;
        struct ServiceScope
        {
            ServiceScope() noexcept { servicing = true; }
            ~ServiceScope() noexcept { servicing = false; }
            ServiceScope(const ServiceScope&) = delete;
            ServiceScope& operator=(const ServiceScope&) = delete;
        };
    } // namespace detail

    // Service at most one ALREADY pending paint for the caller's private child.
    // This does not invalidate any window or manufacture WM_PAINT. The caller must
    // own the HWND on this thread and make Current validate its frame/lifetime lease
    // and continued ownership of this exact handle. Native calls can dispatch sent
    // messages; the app must defer destructive recovery until this call unwinds.
    // Painted means the window procedure was called, not that DWM presented pixels.
    template<class Current>
    Result Service(HWND window, Current current) noexcept
    {
        if (!window)
            return { Kind::Idle, 0 }; // Never turn a null HWND into a global filter.
        if (detail::servicing)
            return { Kind::Cancelled, 0 };
        detail::ServiceScope scope;
        try
        {
            const auto validate = [&]() {
                if (!current())
                    return Kind::Cancelled;
                const bool live = IsWindow(window) != FALSE;
                if (!current())
                    return Kind::Cancelled;
                return live ? Kind::Idle : Kind::Failed;
            };
            auto state = validate();
            if (state != Kind::Idle)
                return { state, 0 };

            MSG message{};
            const bool available = PeekMessageW(&message, window, WM_PAINT, WM_PAINT, PM_REMOVE) != FALSE;
            // WM_QUIT bypasses the filter. It may have been removed even if sent
            // messages cancelled this frame; never lose the thread's quit request.
            if (available && message.message == WM_QUIT)
                return { Kind::Quit, static_cast<int>(message.wParam) };
            state = validate();
            if (state != Kind::Idle)
                return { state, 0 };
            if (!available)
                return { Kind::Idle, 0 };
            // A HWND filter can also match descendants. This adapter dispatches
            // only its exact private control, never an unrelated child or input.
            // Native WM_PAINT has unused zero parameters; some controls interpret
            // a nonzero wParam as a caller-supplied HDC, which is not our contract.
            if (message.hwnd != window || message.message != WM_PAINT ||
                message.wParam != 0 || message.lParam != 0)
                return { Kind::Failed, 0 };

            // The window procedure's return value is not a native success flag.
            (void)DispatchMessageW(&message);
            state = validate();
            return { state == Kind::Idle ? Kind::Painted : state, 0 };
        }
        catch (...)
        {
            return { Kind::Failed, 0 };
        }
    }

} // namespace glide::paint
