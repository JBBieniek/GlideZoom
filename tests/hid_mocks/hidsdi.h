#pragma once
#include "windows.h"

using USAGE = std::uint16_t;
using NTSTATUS = std::int32_t;
inline constexpr NTSTATUS HIDP_STATUS_SUCCESS = 0x00110000;
inline constexpr NTSTATUS HIDP_STATUS_FAILURE = -1;
enum HIDP_REPORT_TYPE { HidP_Input, HidP_Output, HidP_Feature };
struct _HIDP_PREPARSED_DATA { std::uint32_t device; };
using PHIDP_PREPARSED_DATA = _HIDP_PREPARSED_DATA*;

struct HIDP_CAPS {
    USAGE Usage = 0, UsagePage = 0;
    USHORT InputReportByteLength = 0, NumberInputValueCaps = 0, NumberInputButtonCaps = 0;
};
struct HIDP_VALUE_CAPS {
    USAGE UsagePage = 0;
    UCHAR ReportID = 0;
    bool IsAlias = false;
    USHORT BitSize = 0, ReportCount = 0, LinkCollection = 0;
    bool IsRange = false, IsAbsolute = true;
    LONG LogicalMin = 0, LogicalMax = 0;
    struct { USAGE UsageMin = 0, UsageMax = 0; USHORT DataIndexMin = 0, DataIndexMax = 0; } Range;
    struct { USAGE Usage = 0; USHORT DataIndex = 0; } NotRange;
};
struct HIDP_BUTTON_CAPS {
    USAGE UsagePage = 0;
    UCHAR ReportID = 0;
    USHORT LinkCollection = 0;
    bool IsRange = false;
    struct { USAGE UsageMin = 0, UsageMax = 0; } Range;
    struct { USAGE Usage = 0; } NotRange;
};

NTSTATUS HidP_GetCaps(PHIDP_PREPARSED_DATA, HIDP_CAPS*);
NTSTATUS HidP_GetValueCaps(HIDP_REPORT_TYPE, HIDP_VALUE_CAPS*, USHORT*, PHIDP_PREPARSED_DATA);
NTSTATUS HidP_GetButtonCaps(HIDP_REPORT_TYPE, HIDP_BUTTON_CAPS*, USHORT*, PHIDP_PREPARSED_DATA);
NTSTATUS HidP_GetUsageValue(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE, ULONG*, PHIDP_PREPARSED_DATA, PCHAR, ULONG);
NTSTATUS HidP_GetUsages(HIDP_REPORT_TYPE, USAGE, USHORT, USAGE*, ULONG*, PHIDP_PREPARSED_DATA, PCHAR, ULONG);
