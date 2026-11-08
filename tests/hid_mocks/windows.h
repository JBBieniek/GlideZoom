#pragma once

// Host-side API contract shim, not a replacement Windows SDK. Production is
// separately compiled against the actual Windows headers and import libraries.
#include <cstdint>

using BYTE = std::uint8_t;
using UCHAR = std::uint8_t;
using USHORT = std::uint16_t;
using UINT = std::uint32_t;
using DWORD = std::uint32_t;
using ULONG = std::uint32_t;
using LONG = std::int32_t;
using HANDLE = void*;
using HRAWINPUT = void*;
using PCHAR = char*;

struct RAWINPUTHEADER {
    DWORD dwType = 0, dwSize = 0;
    HANDLE hDevice = nullptr;
    std::uintptr_t wParam = 0;
};
struct RAWHID { DWORD dwSizeHid = 0, dwCount = 0; BYTE bRawData[1]{}; };
struct RAWINPUT { RAWINPUTHEADER header{}; union Data { RAWHID hid; Data() : hid{} {} } data; };

inline constexpr UINT RID_INPUT = 0x10000003;
inline constexpr UINT RIDI_PREPARSEDDATA = 0x20000005;
inline constexpr DWORD RIM_TYPEHID = 2;

UINT GetRawInputData(HRAWINPUT, UINT, void*, UINT*, UINT);
UINT GetRawInputDeviceInfoW(HANDLE, UINT, void*, UINT*);
