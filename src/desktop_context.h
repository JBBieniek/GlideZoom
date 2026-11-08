#pragma once
#include <windows.h>
#include <cwchar>

namespace glide
{
    enum class InputDesktop
    {
        Current,
        Other,
        Unknown
    };
    // This only queries the desktop receiving input. Never switch to, open windows
    // on, or change magnification on a secure/login desktop.
    inline InputDesktop QueryInputDesktop() noexcept
    {
        wchar_t ours[256]{}, active[256]{};
        DWORD bytes = 0;
        if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, ours, sizeof(ours), &bytes))
            return InputDesktop::Unknown;
        HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (!input)
            return InputDesktop::Unknown;
        const bool read = GetUserObjectInformationW(input, UOI_NAME, active, sizeof(active), &bytes) != FALSE;
        CloseDesktop(input);
        if (!read)
            return InputDesktop::Unknown;
        return std::wcscmp(ours, active) == 0 ? InputDesktop::Current : InputDesktop::Other;
    }
    inline bool OnInputDesktop() noexcept
    {
        return QueryInputDesktop() == InputDesktop::Current;
    }
} // namespace glide
