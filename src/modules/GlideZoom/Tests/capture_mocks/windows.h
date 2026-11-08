#pragma once

// Private SDK stand-in for capture_native_tests only. The application always
// compiles against the real Windows SDK. Types match the fields used by the
// production capture adapter; this header does not simulate Windows graphics.
#include <cstddef>
#include <cstdint>

#define CALLBACK
#define TRUE 1
#define FALSE 0
using BOOL = int;
using BYTE = std::uint8_t;
using WORD = std::uint16_t;
using DWORD = std::uint32_t;
using UINT = std::uint32_t;
using LONG = std::int32_t;
using SIZE_T = std::size_t;
using LPARAM = std::intptr_t;
using HANDLE = void*;
using HGDIOBJ = void*;
using HMONITOR = void*;
struct MockDC;
struct MockBitmap;
struct MockGlobal;
struct MockWindow;
using HDC = MockDC*;
using HBITMAP = MockBitmap*;
using HGLOBAL = MockGlobal*;
using HWND = MockWindow*;
struct RECT { LONG left, top, right, bottom; };
using LPRECT = RECT*;
using MONITORENUMPROC = BOOL (CALLBACK*)(HMONITOR, HDC, LPRECT, LPARAM);
struct BITMAPINFOHEADER {
    DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
};
struct RGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; };
struct BITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; };
struct CIEXYZ { LONG ciexyzX, ciexyzY, ciexyzZ; };
struct CIEXYZTRIPLE { CIEXYZ ciexyzRed, ciexyzGreen, ciexyzBlue; };
struct BITMAPV5HEADER {
    DWORD bV5Size; LONG bV5Width, bV5Height; WORD bV5Planes, bV5BitCount;
    DWORD bV5Compression, bV5SizeImage; LONG bV5XPelsPerMeter, bV5YPelsPerMeter;
    DWORD bV5ClrUsed, bV5ClrImportant, bV5RedMask, bV5GreenMask, bV5BlueMask, bV5AlphaMask, bV5CSType;
    CIEXYZTRIPLE bV5Endpoints;
    DWORD bV5GammaRed, bV5GammaGreen, bV5GammaBlue, bV5Intent, bV5ProfileData, bV5ProfileSize, bV5Reserved;
};
static_assert(sizeof(BITMAPINFOHEADER) == 40 && sizeof(BITMAPV5HEADER) == 124, "SDK bitmap layout");
#define HGDI_ERROR reinterpret_cast<HGDIOBJ>(static_cast<std::intptr_t>(-1))
constexpr DWORD ERROR_SUCCESS = 0, ERROR_INVALID_HANDLE = 6, ERROR_NOT_ENOUGH_MEMORY = 8;
constexpr DWORD ERROR_INVALID_DATA = 13, ERROR_GEN_FAILURE = 31, ERROR_NOT_SUPPORTED = 50;
constexpr DWORD ERROR_INVALID_PARAMETER = 87, ERROR_BUSY = 170, ERROR_CANCELLED = 1223, ERROR_INVALID_STATE = 5023;
constexpr DWORD BI_RGB = 0, BI_BITFIELDS = 3, SRCCOPY = 0x00cc0020, CAPTUREBLT = 0x40000000;
constexpr DWORD LCS_sRGB = 0x73524742, LCS_GM_IMAGES = 4;
constexpr UINT DIB_RGB_COLORS = 0, GMEM_MOVEABLE = 2, CF_DIBV5 = 17;

DWORD GetLastError() noexcept;
void SetLastError(DWORD) noexcept;
BOOL EnumDisplayMonitors(HDC, const RECT*, MONITORENUMPROC, LPARAM) noexcept;
HDC CreateCompatibleDC(HDC) noexcept;
HBITMAP CreateDIBSection(HDC, const BITMAPINFO*, UINT, void**, HANDLE, DWORD) noexcept;
HGDIOBJ SelectObject(HDC, HGDIOBJ) noexcept;
BOOL DeleteDC(HDC) noexcept;
BOOL DeleteObject(HGDIOBJ) noexcept;
HGLOBAL GlobalAlloc(UINT, SIZE_T) noexcept;
void* GlobalLock(HGLOBAL) noexcept;
BOOL GlobalUnlock(HGLOBAL) noexcept;
HGLOBAL GlobalFree(HGLOBAL) noexcept;
HDC GetDC(HWND) noexcept;
int ReleaseDC(HWND, HDC) noexcept;
BOOL BitBlt(HDC, int, int, int, int, HDC, int, int, DWORD) noexcept;
BOOL GdiFlush() noexcept;
DWORD GetWindowThreadProcessId(HWND, DWORD*) noexcept;
DWORD GetCurrentThreadId() noexcept;
DWORD GetCurrentProcessId() noexcept;
BOOL OpenClipboard(HWND) noexcept;
BOOL EmptyClipboard() noexcept;
HANDLE SetClipboardData(UINT, HANDLE) noexcept;
BOOL CloseClipboard() noexcept;
