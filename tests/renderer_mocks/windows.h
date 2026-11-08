#pragma once

// Private SDK stand-in for monitor_native_tests. Production uses the Windows
// SDK. This supplies only the ABI surface used by the real renderer; it does
// not claim to emulate the compositor, cursor graphics, or native timing.
#include <cstddef>
#include <cstdint>

#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
using BOOL = int;
using BYTE = std::uint8_t;
using WORD = std::uint16_t;
using DWORD = std::uint32_t;
using UINT = std::uint32_t;
using LONG = std::int32_t;
using LONG_PTR = std::intptr_t;
using LRESULT = std::intptr_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using HANDLE = void*;
using HINSTANCE = void*;
using HMODULE = void*;
using HDESK = void*;
using HMENU = void*;
using HICON = void*;
using HCURSOR = void*;
using HBRUSH = void*;
using FARPROC = void(*)();
struct MockRendererWindow;
using HWND = MockRendererWindow*;
struct RECT { LONG left, top, right, bottom; };
struct POINT { LONG x, y; };
struct CURSORINFO { DWORD cbSize, flags; HCURSOR hCursor; POINT ptScreenPos; };
using WNDPROC = LRESULT(CALLBACK*)(HWND, UINT, WPARAM, LPARAM);
struct WNDCLASSEXW {
    UINT cbSize, style;
    WNDPROC lpfnWndProc;
    int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    const wchar_t* lpszMenuName;
    const wchar_t* lpszClassName;
    HICON hIconSm;
};
struct CREATESTRUCTW {
    void* lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy, cx, y, x;
    LONG style;
    const wchar_t* lpszName;
    const wchar_t* lpszClass;
    DWORD dwExStyle;
};
struct WINDOWPOS { HWND hwnd, hwndInsertAfter; int x, y, cx, cy; UINT flags; };

#define HWND_TOPMOST reinterpret_cast<HWND>(static_cast<std::intptr_t>(-1))
constexpr WORD LOWORD(WPARAM value) noexcept { return static_cast<WORD>(value); }
constexpr DWORD ERROR_SUCCESS = 0, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_HANDLE = 6;
constexpr DWORD ERROR_INVALID_DATA = 13, ERROR_GEN_FAILURE = 31;
constexpr DWORD ERROR_INVALID_PARAMETER = 87, ERROR_PROC_NOT_FOUND = 127;
constexpr DWORD ERROR_ALREADY_INITIALIZED = 1247, ERROR_CLASS_ALREADY_EXISTS = 1410;
constexpr DWORD ERROR_INVALID_STATE = 5023;
constexpr DWORD LOAD_LIBRARY_SEARCH_SYSTEM32 = 0x00000800;
constexpr int UOI_NAME = 2, GWLP_USERDATA = -21;
constexpr UINT GW_HWNDPREV = 3;
constexpr DWORD DESKTOP_READOBJECTS = 1;
constexpr UINT WM_DESTROY = 2, WM_ERASEBKGND = 0x0014;
constexpr UINT WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082, WM_NCHITTEST = 0x0084;
constexpr UINT WM_MOUSEACTIVATE = 0x0021, WM_PARENTNOTIFY = 0x0210;
constexpr UINT WM_WINDOWPOSCHANGED = 0x0047, WM_DPICHANGED = 0x02E0;
constexpr LRESULT HTTRANSPARENT = -1, MA_NOACTIVATE = 3;
constexpr int SW_HIDE = 0, SW_SHOW = 5;
constexpr UINT SWP_NOSIZE = 0x0001, SWP_NOMOVE = 0x0002, SWP_NOZORDER = 0x0004;
constexpr UINT SWP_NOREDRAW = 0x0008, SWP_NOACTIVATE = 0x0010;
constexpr UINT SWP_SHOWWINDOW = 0x0040, SWP_NOOWNERZORDER = 0x0200;
constexpr DWORD WS_POPUP = 0x80000000, WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000;
constexpr DWORD WS_CLIPCHILDREN = 0x02000000;
constexpr DWORD WS_EX_TOPMOST = 0x00000008, WS_EX_TRANSPARENT = 0x00000020;
constexpr DWORD WS_EX_TOOLWINDOW = 0x00000080, WS_EX_LAYERED = 0x00080000;
constexpr DWORD WS_EX_NOACTIVATE = 0x08000000, LWA_ALPHA = 0x00000002;
constexpr DWORD CURSOR_SHOWING = 0x00000001, CURSOR_SUPPRESSED = 0x00000002;

DWORD GetLastError() noexcept;
void SetLastError(DWORD) noexcept;
DWORD GetCurrentThreadId() noexcept;
HDESK GetThreadDesktop(DWORD) noexcept;
BOOL GetUserObjectInformationW(HANDLE, int, void*, DWORD, DWORD*) noexcept;
HDESK OpenInputDesktop(DWORD, BOOL, DWORD) noexcept;
BOOL CloseDesktop(HDESK) noexcept;
HMODULE LoadLibraryExW(const wchar_t*, HANDLE, DWORD) noexcept;
FARPROC GetProcAddress(HMODULE, const char*) noexcept;
BOOL FreeLibrary(HMODULE) noexcept;
WORD RegisterClassExW(const WNDCLASSEXW*) noexcept;
BOOL GetClassInfoExW(HINSTANCE, const wchar_t*, WNDCLASSEXW*) noexcept;
BOOL UnregisterClassW(const wchar_t*, HINSTANCE) noexcept;
LONG_PTR GetWindowLongPtrW(HWND, int) noexcept;
LONG_PTR SetWindowLongPtrW(HWND, int, LONG_PTR) noexcept;
HWND CreateWindowExW(DWORD, const wchar_t*, const wchar_t*, DWORD,
                     int, int, int, int, HWND, HMENU, HINSTANCE, void*) noexcept;
BOOL DestroyWindow(HWND) noexcept;
BOOL IsWindow(HWND) noexcept;
BOOL IsWindowVisible(HWND) noexcept;
BOOL ShowWindow(HWND, int) noexcept;
BOOL SetWindowPos(HWND, HWND, int, int, int, int, UINT) noexcept;
BOOL GetWindowRect(HWND, RECT*) noexcept;
HWND GetWindow(HWND, UINT) noexcept;
BOOL SetLayeredWindowAttributes(HWND, DWORD, BYTE, DWORD) noexcept;
BOOL InvalidateRect(HWND, const RECT*, BOOL) noexcept;
BOOL UpdateWindow(HWND) noexcept;
BOOL GetCursorInfo(CURSORINFO*) noexcept;
LRESULT DefWindowProcW(HWND, UINT, WPARAM, LPARAM) noexcept;
