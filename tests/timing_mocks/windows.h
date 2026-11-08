#pragma once

// Private SDK stand-in for latency_native_tests only. The application's native
// adapter compiles against the real Windows SDK; this file models calls and
// resource ownership, not Windows scheduling or actual timer precision.
#include <cstdint>

#define TRUE 1
#define FALSE 0
using BOOL = int;
using DWORD = std::uint32_t;
using LONG = std::int32_t;
using LONGLONG = std::int64_t;
struct LARGE_INTEGER { LONGLONG QuadPart; };
struct MockTimer;
using HANDLE = MockTimer*;
using PTIMERAPCROUTINE = void (*)(void*, DWORD, DWORD);

constexpr DWORD ERROR_SUCCESS = 0, ERROR_ACCESS_DENIED = 5, ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr DWORD ERROR_NOT_SUPPORTED = 50, ERROR_INVALID_PARAMETER = 87;
constexpr DWORD SYNCHRONIZE = 0x00100000, TIMER_MODIFY_STATE = 0x0002;

BOOL QueryPerformanceFrequency(LARGE_INTEGER*) noexcept;
BOOL QueryPerformanceCounter(LARGE_INTEGER*) noexcept;
HANDLE CreateWaitableTimerExW(void*, const wchar_t*, DWORD, DWORD) noexcept;
DWORD GetLastError() noexcept;
BOOL CancelWaitableTimer(HANDLE) noexcept;
BOOL CloseHandle(HANDLE) noexcept;
BOOL SetWaitableTimer(HANDLE, const LARGE_INTEGER*, LONG, PTIMERAPCROUTINE, void*, BOOL) noexcept;
