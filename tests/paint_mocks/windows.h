#pragma once

#include <cstdint>

struct MockPaintWindow;
using HWND = MockPaintWindow*;
using BOOL = int;
using UINT = unsigned int;
using DWORD = std::uint32_t;
using WPARAM = std::uintptr_t;
using LPARAM = std::intptr_t;
using LRESULT = std::intptr_t;
using LONG = std::int32_t;

struct POINT { LONG x = 0, y = 0; };
struct MSG {
    HWND hwnd = nullptr;
    UINT message = 0;
    WPARAM wParam = 0;
    LPARAM lParam = 0;
    DWORD time = 0;
    POINT pt{};
};

constexpr BOOL FALSE = 0, TRUE = 1;
constexpr UINT WM_PAINT = 0x000F, WM_QUIT = 0x0012;
constexpr UINT WM_KEYDOWN = 0x0100, WM_MOUSEMOVE = 0x0200, WM_TIMER = 0x0113;
constexpr UINT PM_REMOVE = 0x0001;

BOOL IsWindow(HWND window);
BOOL PeekMessageW(MSG* message, HWND window, UINT first, UINT last, UINT remove);
LRESULT DispatchMessageW(const MSG* message);
