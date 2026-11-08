#pragma once

// Private SDK stand-in for pointer_native_tests.cpp only. It exercises the
// production NativeOps calls without moving a cursor or querying this desktop.
#include <cstdint>

using BOOL = int;
using DWORD = std::uint32_t;
using LONG = std::int32_t;
using DPI_AWARENESS_CONTEXT = std::intptr_t;
struct PointerMockDesktop;
struct PointerMockMonitor;
using HDESK = PointerMockDesktop*;
using HMONITOR = PointerMockMonitor*;
struct POINT
{
    LONG x, y;
};
struct RECT
{
    LONG left, top, right, bottom;
};
struct MONITORINFO
{
    DWORD cbSize;
    RECT rcMonitor, rcWork;
    DWORD dwFlags;
};

constexpr BOOL FALSE = 0, TRUE = 1;
constexpr DWORD ERROR_SUCCESS = 0, ERROR_ACCESS_DENIED = 5, ERROR_GEN_FAILURE = 31;
constexpr DWORD ERROR_INVALID_PARAMETER = 87, ERROR_DEVICE_NOT_CONNECTED = 1167;
constexpr int UOI_NAME = 2;
constexpr DWORD DESKTOP_READOBJECTS = 1, MONITOR_DEFAULTTONULL = 0;
constexpr int SM_XVIRTUALSCREEN = 76, SM_YVIRTUALSCREEN = 77;
constexpr int SM_CXVIRTUALSCREEN = 78, SM_CYVIRTUALSCREEN = 79;
constexpr DPI_AWARENESS_CONTEXT DPI_AWARENESS_CONTEXT_UNAWARE = -1;
constexpr DPI_AWARENESS_CONTEXT DPI_AWARENESS_CONTEXT_SYSTEM_AWARE = -2;
constexpr DPI_AWARENESS_CONTEXT DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE = -3;
constexpr DPI_AWARENESS_CONTEXT DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4;

DWORD GetLastError() noexcept;
DWORD GetCurrentThreadId() noexcept;
HDESK GetThreadDesktop(DWORD) noexcept;
HDESK OpenInputDesktop(DWORD, BOOL, DWORD) noexcept;
BOOL GetUserObjectInformationW(HDESK, int, void*, DWORD, DWORD*) noexcept;
BOOL CloseDesktop(HDESK) noexcept;
DPI_AWARENESS_CONTEXT GetThreadDpiAwarenessContext() noexcept;
BOOL AreDpiAwarenessContextsEqual(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT) noexcept;
BOOL GetClipCursor(RECT*) noexcept;
int GetSystemMetrics(int) noexcept;
HMONITOR MonitorFromRect(const RECT*, DWORD) noexcept;
BOOL GetMonitorInfoW(HMONITOR, MONITORINFO*) noexcept;
BOOL GetPhysicalCursorPos(POINT*) noexcept;
BOOL GetCursorPos(POINT*) noexcept;
HMONITOR MonitorFromPoint(POINT, DWORD) noexcept;
BOOL SetPhysicalCursorPos(int, int) noexcept;
