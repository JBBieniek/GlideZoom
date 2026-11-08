#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cwchar>

#include "settings_ui_model.h"

namespace glide::settings::ui::detail
{
    inline constexpr int ModeId = 2100;
    inline constexpr int StyleId = 2101;

    struct EmptyDialogTemplate
    {
        DLGTEMPLATE dialog{};
        WORD menu = 0;
        WORD windowClass = 0;
        WORD title = 0;
    };
    static_assert(offsetof(EmptyDialogTemplate, menu) == sizeof(DLGTEMPLATE));

    struct IntegerControl
    {
        HWND label = nullptr;
        HWND edit = nullptr;
    };

    struct AdvancedDialog
    {
        HINSTANCE instance = nullptr;
        HWND owner = nullptr;
        HWND window = nullptr;
        HWND content = nullptr;
        HWND save = nullptr;
        HWND cancel = nullptr;
        HWND footer = nullptr;
        HWND zoomHeading = nullptr;
        HWND overviewHeading = nullptr;
        HWND indicatorHeading = nullptr;
        HWND wheelAcceleration = nullptr;
        HWND modeLabel = nullptr;
        HWND mode = nullptr;
        HWND styleLabel = nullptr;
        HWND style = nullptr;
        HWND details = nullptr;
        HWND zoomHelp = nullptr;
        HWND overviewHelp = nullptr;
        HWND indicatorHelp = nullptr;
        std::array<IntegerControl, IntegerFields.size()> integers{};
        Values current{};
        Values result{};
        HFONT font = nullptr;
        HFONT headingFont = nullptr;
        UINT dpi = 96;
        int scroll = 0;
        int wheelRemainder = 0;
        bool ready = false;
        bool accepted = false;

        int Scale(int value) const noexcept { return MulDiv(value, static_cast<int>(dpi), 96); }

        ~AdvancedDialog()
        {
            if (font)
                DeleteObject(font);
            if (headingFont)
                DeleteObject(headingFont);
        }
    };

    inline AdvancedDialog* State(HWND window) noexcept
    {
        return reinterpret_cast<AdvancedDialog*>(GetWindowLongPtrW(window, DWLP_USER));
    }

    inline void Place(const AdvancedDialog& state, HWND control, int x, int y, int width, int height) noexcept
    {
        MoveWindow(control, state.Scale(x), state.Scale(y) - state.scroll,
                   state.Scale(width), state.Scale(height), FALSE);
    }

    inline void LayoutContent(AdvancedDialog& state) noexcept
    {
        if (!state.ready)
            return;
        RECT bounds{};
        GetClientRect(state.content, &bounds);
        const int logicalWidth = MulDiv(bounds.right, 96, static_cast<int>(state.dpi));
        const bool twoColumns = logicalWidth >= 660;
        const int margin = 16;
        const int gap = 24;
        const int columnWidth = (std::max)(180, twoColumns ? (logicalWidth - margin * 2 - gap) / 2 : logicalWidth - margin * 2);
        const int secondX = twoColumns ? margin + columnWidth + gap : margin;
        const int secondY = twoColumns ? 0 : 500;
        const int virtualHeight = state.Scale(twoColumns ? 500 : 978);
        const int maximumScroll = (std::max)(0L, virtualHeight - bounds.bottom);
        state.scroll = (std::clamp)(state.scroll, 0, maximumScroll);
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
        info.nMin = 0;
        info.nMax = virtualHeight - 1;
        info.nPage = static_cast<UINT>((std::max)(0L, bounds.bottom));
        info.nPos = state.scroll;
        SetScrollInfo(state.content, SB_VERT, &info, TRUE);

        const auto integer = [&](std::size_t index, int x, int y) {
            Place(state, state.integers[index].label, x, y + 4, columnWidth - 82, 28);
            Place(state, state.integers[index].edit, x + columnWidth - 76, y, 76, 26);
        };
        Place(state, state.zoomHeading, margin, 16, columnWidth, 24);
        integer(0, margin, 50);
        integer(1, margin, 86);
        Place(state, state.wheelAcceleration, margin, 124, columnWidth, 26);
        integer(2, margin, 160);
        Place(state, state.zoomHelp, margin, 198, columnWidth, 82);
        Place(state, state.overviewHeading, margin, 282, columnWidth, 24);
        integer(3, margin, 316);
        integer(8, margin, 352);
        integer(9, margin, 388);
        integer(5, margin, 424);
        Place(state, state.overviewHelp, margin, 460, columnWidth, 36);

        Place(state, state.indicatorHeading, secondX, secondY + 16, columnWidth, 24);
        Place(state, state.modeLabel, secondX, secondY + 50, columnWidth, 20);
        Place(state, state.mode, secondX, secondY + 74, columnWidth, 156);
        Place(state, state.styleLabel, secondX, secondY + 112, columnWidth, 20);
        Place(state, state.style, secondX, secondY + 136, columnWidth, 132);
        integer(4, secondX, secondY + 178);
        integer(6, secondX, secondY + 214);
        integer(7, secondX, secondY + 250);
        integer(10, secondX, secondY + 286);
        Place(state, state.details, secondX, secondY + 324, columnWidth, 26);
        Place(state, state.indicatorHelp, secondX, secondY + 364, columnWidth, 110);
        InvalidateRect(state.content, nullptr, TRUE);
    }

    inline void ScrollTo(AdvancedDialog& state, int position) noexcept
    {
        state.scroll = position;
        LayoutContent(state);
    }

    inline void ScrollCommand(AdvancedDialog& state, WPARAM command) noexcept
    {
        SCROLLINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = SIF_ALL;
        GetScrollInfo(state.content, SB_VERT, &info);
        int position = state.scroll;
        switch (LOWORD(command))
        {
        case SB_TOP: position = 0; break;
        case SB_BOTTOM: position = info.nMax; break;
        case SB_LINEUP: position -= state.Scale(28); break;
        case SB_LINEDOWN: position += state.Scale(28); break;
        case SB_PAGEUP: position -= static_cast<int>(info.nPage); break;
        case SB_PAGEDOWN: position += static_cast<int>(info.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: position = info.nTrackPos; break;
        default: return;
        }
        ScrollTo(state, position);
    }

    inline void MouseWheel(AdvancedDialog& state, WPARAM wParam) noexcept
    {
        state.wheelRemainder += static_cast<short>(HIWORD(wParam));
        const int steps = state.wheelRemainder / WHEEL_DELTA;
        state.wheelRemainder %= WHEEL_DELTA;
        ScrollTo(state, state.scroll - steps * state.Scale(42));
    }

    inline void RevealControl(AdvancedDialog& state, HWND control) noexcept
    {
        if (!state.ready || !control)
            return;
        RECT bounds{};
        RECT viewport{};
        GetWindowRect(control, &bounds);
        MapWindowPoints(nullptr, state.content, reinterpret_cast<POINT*>(&bounds), 2);
        GetClientRect(state.content, &viewport);
        if (bounds.top < 0)
            ScrollTo(state, state.scroll + bounds.top - state.Scale(8));
        else if (bounds.bottom > viewport.bottom)
            ScrollTo(state, state.scroll + bounds.bottom - viewport.bottom + state.Scale(8));
    }

    inline INT_PTR CALLBACK ContentProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_INITDIALOG)
        {
            SetWindowLongPtrW(window, DWLP_USER, lParam);
            return TRUE;
        }
        auto* state = State(window);
        if (!state)
            return FALSE;
        switch (message)
        {
        case WM_COMMAND:
            if (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == CBN_SETFOCUS || HIWORD(wParam) == BN_SETFOCUS)
                RevealControl(*state, reinterpret_cast<HWND>(lParam));
            SendMessageW(GetParent(window), message, wParam, lParam);
            return TRUE;
        case WM_VSCROLL:
            ScrollCommand(*state, wParam);
            return TRUE;
        case WM_MOUSEWHEEL:
            MouseWheel(*state, wParam);
            return TRUE;
        case WM_SIZE:
            LayoutContent(*state);
            return TRUE;
        }
        return FALSE;
    }

    inline HWND Control(AdvancedDialog& state, const wchar_t* type, const wchar_t* text,
                        DWORD style, int id = 0, HWND parent = nullptr) noexcept
    {
        const DWORD extended = wcscmp(type, L"EDIT") == 0 ? WS_EX_CLIENTEDGE : 0;
        HWND control = CreateWindowExW(extended, type, text, WS_CHILD | WS_VISIBLE | style,
                                       0, 0, 1, 1, parent ? parent : state.content,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), state.instance, nullptr);
        if (control)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), FALSE);
        return control;
    }

    inline bool Integer(AdvancedDialog& state, std::size_t index) noexcept
    {
        const auto& field = IntegerFields[index];
        wchar_t text[16]{};
        swprintf_s(text, L"%d", state.current.*(field.member));
        auto& controls = state.integers[index];
        controls.label = Control(state, L"STATIC", field.label, SS_LEFT);
        controls.edit = Control(state, L"EDIT", text, WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                                2200 + static_cast<int>(index));
        // Keep at least one invalid extra character in the control. A long paste
        // must not be truncated to a valid 1-4 digit prefix before validation.
        if (controls.edit)
            SendMessageW(controls.edit, EM_SETLIMITTEXT, 16, 0);
        return controls.label && controls.edit;
    }

    inline BOOL CALLBACK SetChildFont(HWND control, LPARAM font)
    {
        SendMessageW(control, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
        return TRUE;
    }

    inline bool UpdateFont(AdvancedDialog& state) noexcept
    {
        HFONT font = CreateFontW(-MulDiv(9, static_cast<int>(state.dpi), 72), 0, 0, 0, FW_NORMAL,
                                 FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT heading = CreateFontW(-MulDiv(9, static_cast<int>(state.dpi), 72), 0, 0, 0, FW_BOLD,
                                    FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font || !heading)
        {
            if (font) DeleteObject(font);
            if (heading) DeleteObject(heading);
            return false;
        }
        SendMessageW(state.window, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
        EnumChildWindows(state.window, SetChildFont, reinterpret_cast<LPARAM>(font));
        for (HWND control : {state.zoomHeading, state.overviewHeading, state.indicatorHeading})
            if (control)
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(heading), TRUE);
        if (state.font) DeleteObject(state.font);
        if (state.headingFont) DeleteObject(state.headingFont);
        state.font = font;
        state.headingFont = heading;
        return true;
    }

    inline void EnableDependentControls(AdvancedDialog& state) noexcept
    {
        const auto mode = SendMessageW(state.mode, CB_GETCURSEL, 0, 0);
        const auto style = SendMessageW(state.style, CB_GETCURSEL, 0, 0);
        const bool enabled = mode >= 1 && mode <= 3;
        const bool overview = enabled && (style == 0 || style == 2);
        const bool outline = enabled && (style == 1 || style == 2);
        EnableWindow(state.style, enabled);
        EnableWindow(state.styleLabel, enabled);
        EnableWindow(state.details, enabled);
        for (std::size_t index = 0; index < state.integers.size(); ++index)
        {
            bool available = true;
            if (index == 2)
                available = SendMessageW(state.wheelAcceleration, BM_GETCHECK, 0, 0) == BST_CHECKED;
            else if (index == 3 || index == 5 || index == 8 || index == 9)
                available = overview;
            else if (index == 4)
                available = outline;
            else if (index == 6 || index == 7 || index == 10)
                available = enabled;
            EnableWindow(state.integers[index].label, available);
            EnableWindow(state.integers[index].edit, available);
        }
    }

    inline bool CreateControls(AdvancedDialog& state) noexcept
    {
        state.zoomHeading = Control(state, L"STATIC", L"Zoom and wheel", SS_LEFT);
        if (!Integer(state, 0) || !Integer(state, 1))
            return false;
        state.wheelAcceleration = Control(state, L"BUTTON", L"Boost fast wheel scrolling", WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY);
        if (!Integer(state, 2))
            return false;
        state.zoomHelp = Control(state, L"STATIC",
            L"Zoom gain affects keys and wheel, not drag sensitivity. Edge pan requires automatic pointer scaling "
            L"with the pointer free to move. Wheel boost is optional.", SS_LEFT);
        state.overviewHeading = Control(state, L"STATIC", L"Monitor overview", SS_LEFT);
        if (!Integer(state, 3) || !Integer(state, 8) || !Integer(state, 9) || !Integer(state, 5))
            return false;
        state.overviewHelp = Control(state, L"STATIC",
            L"Position: 0 is left/top, 100 is right/bottom. Opacity 0 hides the overview.", SS_LEFT);
        state.indicatorHeading = Control(state, L"STATIC", L"View indicator", SS_LEFT);
        state.modeLabel = Control(state, L"STATIC", L"Visibility", SS_LEFT);
        state.mode = Control(state, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, ModeId);
        state.styleLabel = Control(state, L"STATIC", L"Style", SS_LEFT);
        state.style = Control(state, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, StyleId);
        if (!Integer(state, 4) || !Integer(state, 6) || !Integer(state, 7) || !Integer(state, 10))
            return false;
        state.details = Control(state, L"BUTTON", L"Show source position, size and zoom", WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY);
        state.indicatorHelp = Control(state, L"STATIC",
            L"The overview shows the entire monitor and its source crop. The screen outline marks that crop at its "
            L"original position. Both share visibility and fade timing. Ctrl+Alt+V reveals an enabled indicator. "
            L"Indicators appear in selected and independent monitor modes.", SS_LEFT);
        state.footer = Control(state, L"STATIC", L"Save applies the complete settings update.", SS_LEFT, 0, state.window);
        state.save = Control(state, L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK, state.window);
        state.cancel = Control(state, L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL, state.window);
        for (HWND control : {state.zoomHeading, state.overviewHeading, state.indicatorHeading, state.wheelAcceleration,
                             state.modeLabel, state.mode, state.styleLabel, state.style, state.details, state.zoomHelp,
                             state.overviewHelp, state.indicatorHelp, state.footer, state.save, state.cancel})
            if (!control)
                return false;
        for (const auto* text : {L"Off", L"While moving or zooming", L"Shortcut only", L"Always while zoomed"})
            if (SendMessageW(state.mode, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text)) < 0)
                return false;
        for (const auto* text : {L"Monitor overview", L"Full-screen scope outline", L"Overview and screen outline"})
            if (SendMessageW(state.style, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text)) < 0)
                return false;
        SendMessageW(state.mode, CB_SETCURSEL, state.current.scopeIndicatorMode, 0);
        SendMessageW(state.style, CB_SETCURSEL, state.current.scopeIndicatorStyle, 0);
        SendMessageW(state.wheelAcceleration, BM_SETCHECK, state.current.wheelAcceleration ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(state.details, BM_SETCHECK, state.current.scopeIndicatorDetails ? BST_CHECKED : BST_UNCHECKED, 0);
        for (HWND control : {state.zoomHeading, state.overviewHeading, state.indicatorHeading})
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.headingFont), FALSE);
        EnableDependentControls(state);
        return true;
    }

    inline RECT WorkArea(HWND window) noexcept
    {
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor))
            return monitor.rcWork;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        return work;
    }

    inline void LayoutWindow(AdvancedDialog& state) noexcept
    {
        if (!state.ready)
            return;
        RECT bounds{};
        GetClientRect(state.window, &bounds);
        const int footerHeight = state.Scale(62);
        const int buttonWidth = state.Scale(88);
        const int buttonHeight = state.Scale(28);
        const int margin = state.Scale(16);
        const int gap = state.Scale(10);
        const int viewport = (std::max)(1L, bounds.bottom - footerHeight);
        MoveWindow(state.content, 0, 0, bounds.right, viewport, TRUE);
        MoveWindow(state.cancel, bounds.right - margin - buttonWidth, viewport + margin,
                   buttonWidth, buttonHeight, TRUE);
        MoveWindow(state.save, bounds.right - margin - buttonWidth * 2 - gap, viewport + margin,
                   buttonWidth, buttonHeight, TRUE);
        MoveWindow(state.footer, margin, viewport + margin + state.Scale(5),
                   (std::max)(0L, bounds.right - margin * 3 - buttonWidth * 2 - gap), state.Scale(38), TRUE);
        LayoutContent(state);
    }

    inline void InitialSize(AdvancedDialog& state) noexcept
    {
        const RECT work = WorkArea(state.owner ? state.owner : state.window);
        RECT extent{0, 0, state.Scale(760), state.Scale(562)};
        AdjustWindowRectExForDpi(&extent, static_cast<DWORD>(GetWindowLongPtrW(state.window, GWL_STYLE)),
                                 FALSE, static_cast<DWORD>(GetWindowLongPtrW(state.window, GWL_EXSTYLE)), state.dpi);
        const int margin = state.Scale(8);
        const int width = (std::min)(extent.right - extent.left, work.right - work.left - margin * 2);
        const int height = (std::min)(extent.bottom - extent.top, work.bottom - work.top - margin * 2);
        RECT owner = work;
        if (state.owner)
            GetWindowRect(state.owner, &owner);
        const int x = (std::clamp)(owner.left + (owner.right - owner.left - width) / 2,
                                   work.left, work.right - width);
        const int y = (std::clamp)(owner.top + (owner.bottom - owner.top - height) / 2,
                                   work.top, work.bottom - height);
        SetWindowPos(state.window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        LayoutWindow(state);
    }

    inline void Invalid(AdvancedDialog& state, std::size_t field) noexcept
    {
        HWND control = nullptr;
        wchar_t message[256]{};
        if (field < IntegerFields.size())
        {
            const auto& descriptor = IntegerFields[field];
            swprintf_s(message, L"%ls: enter a complete whole number from %d to %d.",
                       descriptor.label, descriptor.minimum, descriptor.maximum);
            control = state.integers[field].edit;
        }
        else if (field == InvalidMode || field == InvalidStyle)
        {
            wcscpy_s(message, L"Choose a valid indicator visibility and style.");
            control = field == InvalidMode ? state.mode : state.style;
        }
        else
            wcscpy_s(message, L"The complete configuration is invalid. Cancel and reload settings before trying again.");
        MessageBoxW(state.window, message, L"GlideZoom advanced settings", MB_OK | MB_ICONWARNING);
        if (control)
        {
            // A value edited before its group was disabled still needs repair.
            EnableWindow(control, TRUE);
            RevealControl(state, control);
            SetFocus(control);
            if (field < IntegerFields.size())
                SendMessageW(control, EM_SETSEL, 0, -1);
        }
    }

    inline void Accept(AdvancedDialog& state) noexcept
    {
        AdvancedInput input;
        std::array<std::array<wchar_t, 6>, IntegerFields.size()> text{};
        for (std::size_t index = 0; index < state.integers.size(); ++index)
        {
            const HWND edit = state.integers[index].edit;
            const int length = GetWindowTextLengthW(edit);
            if (length <= 0 || length > 4)
            {
                Invalid(state, index);
                return;
            }
            const int copied = GetWindowTextW(edit, text[index].data(), static_cast<int>(text[index].size()));
            if (copied != length || GetWindowTextLengthW(edit) != copied)
            {
                Invalid(state, index);
                return;
            }
            input.integers[index] = {text[index].data(), static_cast<std::size_t>(copied)};
        }
        input.indicatorMode = static_cast<int>(SendMessageW(state.mode, CB_GETCURSEL, 0, 0));
        input.indicatorStyle = static_cast<int>(SendMessageW(state.style, CB_GETCURSEL, 0, 0));
        input.wheelAcceleration = SendMessageW(state.wheelAcceleration, BM_GETCHECK, 0, 0) == BST_CHECKED;
        input.showDetails = SendMessageW(state.details, BM_GETCHECK, 0, 0) == BST_CHECKED;
        std::size_t invalid = NoInvalidField;
        if (!ApplyAdvanced(state.current, input, state.result, invalid))
        {
            Invalid(state, invalid);
            return;
        }
        state.accepted = true;
        EndDialog(state.window, IDOK);
    }

    inline INT_PTR CALLBACK DialogProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_INITDIALOG)
        {
            auto& state = *reinterpret_cast<AdvancedDialog*>(lParam);
            state.window = window;
            SetWindowLongPtrW(window, DWLP_USER, lParam);
            SetWindowTextW(window, L"GlideZoom advanced settings");
            state.dpi = GetDpiForWindow(window);
            if (!state.dpi) state.dpi = 96;
            if (!UpdateFont(state))
            {
                EndDialog(window, IDCANCEL);
                return TRUE;
            }
            alignas(DWORD) EmptyDialogTemplate contentTemplate;
            contentTemplate.dialog.style = WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_VSCROLL | DS_CONTROL;
            contentTemplate.dialog.dwExtendedStyle = WS_EX_CONTROLPARENT;
            contentTemplate.dialog.cx = 100;
            contentTemplate.dialog.cy = 100;
            state.content = CreateDialogIndirectParamW(state.instance, &contentTemplate.dialog, window,
                                                        ContentProcedure, reinterpret_cast<LPARAM>(&state));
            if (!state.content || !CreateControls(state))
            {
                EndDialog(window, IDCANCEL);
                return TRUE;
            }
            state.ready = true;
            InitialSize(state);
            SetFocus(state.integers[0].edit);
            return FALSE;
        }
        auto* state = State(window);
        if (!state)
            return FALSE;
        switch (message)
        {
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK)
            {
                Accept(*state);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(window, IDCANCEL);
                return TRUE;
            }
            if ((LOWORD(wParam) == ModeId || LOWORD(wParam) == StyleId) && HIWORD(wParam) == CBN_SELCHANGE)
                EnableDependentControls(*state);
            else if (reinterpret_cast<HWND>(lParam) == state->wheelAcceleration && HIWORD(wParam) == BN_CLICKED)
                EnableDependentControls(*state);
            return TRUE;
        case WM_CLOSE:
            EndDialog(window, IDCANCEL);
            return TRUE;
        case WM_SIZE:
            LayoutWindow(*state);
            return TRUE;
        case WM_MOUSEWHEEL:
            MouseWheel(*state, wParam);
            return TRUE;
        case WM_DPICHANGED:
        {
            state->dpi = HIWORD(wParam);
            UpdateFont(*state);
            const RECT suggested = *reinterpret_cast<const RECT*>(lParam);
            RECT work = WorkArea(window);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromRect(&suggested, MONITOR_DEFAULTTONEAREST), &monitor))
                work = monitor.rcWork;
            const LONG width = (std::min)(suggested.right - suggested.left, work.right - work.left);
            const LONG height = (std::min)(suggested.bottom - suggested.top, work.bottom - work.top);
            const LONG x = (std::clamp)(suggested.left, work.left, work.right - width);
            const LONG y = (std::clamp)(suggested.top, work.top, work.bottom - height);
            SetWindowPos(window, nullptr, x, y, width, height,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutWindow(*state);
            return TRUE;
        }
        case WM_GETMINMAXINFO:
        {
            const RECT work = WorkArea(window);
            auto& info = *reinterpret_cast<MINMAXINFO*>(lParam);
            info.ptMinTrackSize.x = (std::min)(static_cast<LONG>(state->Scale(340)), work.right - work.left);
            info.ptMinTrackSize.y = (std::min)(static_cast<LONG>(state->Scale(240)), work.bottom - work.top);
            return TRUE;
        }
        }
        return FALSE;
    }
}

namespace glide::settings
{
    inline bool EditAdvancedSettings(HWND owner, HINSTANCE instance, const Values& current, Values& output)
    {
        ui::detail::AdvancedDialog state;
        state.instance = instance;
        state.owner = owner;
        state.current = current;
        alignas(DWORD) ui::detail::EmptyDialogTemplate dialogTemplate;
        dialogTemplate.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | DS_MODALFRAME;
        dialogTemplate.dialog.dwExtendedStyle = WS_EX_CONTROLPARENT;
        dialogTemplate.dialog.cx = 380;
        dialogTemplate.dialog.cy = 280;
        // Native modal dispatch keeps the owner's timer/service messages alive.
        // DialogBox owns disabling/restoring the owner and Escape/Tab navigation.
        const auto result = DialogBoxIndirectParamW(instance, &dialogTemplate.dialog, owner,
                                                    ui::detail::DialogProcedure, reinterpret_cast<LPARAM>(&state));
        if (result != IDOK || !state.accepted)
            return false;
        output = state.result;
        return true;
    }
}
