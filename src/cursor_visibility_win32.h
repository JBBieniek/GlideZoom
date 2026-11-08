#pragma once

#include <windows.h>
#include "desktop_context.h"
#include "magnification_api.h"

namespace glide::cursor
{

    // MagShowSystemCursor controls the ordinary cursor without changing the
    // ShowCursor display counter or the cursor drawn by a magnifier control.
    // Windows exposes no ownership token for this boolean. Only the journaled
    // lease may call this adapter; a hidden/suppressed baseline is left alone.
    class NativeBackend
    {
    public:
        explicit NativeBackend(MagnificationApi& api) noexcept :
            api_(api) {}

        bool OnInputDesktop() const noexcept { return glide::OnInputDesktop(); }

        bool CursorShowing(bool& showing) const noexcept
        {
            showing = false;
            if (!OnInputDesktop())
                return false;
            CURSORINFO info{};
            info.cbSize = sizeof(info);
            if (!GetCursorInfo(&info) || !OnInputDesktop())
                return false;
            showing = (info.flags & CURSOR_SHOWING) != 0 &&
                      (info.flags & CURSOR_SUPPRESSED) == 0;
            return true;
        }

        bool ShowSystemCursor(bool show) const noexcept
        {
            return api_.ShowSystemCursor && OnInputDesktop() &&
                   api_.ShowSystemCursor(show ? TRUE : FALSE) != FALSE &&
                   OnInputDesktop();
        }

    private:
        MagnificationApi& api_;
    };

} // namespace glide::cursor
