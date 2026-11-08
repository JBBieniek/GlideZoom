#pragma once

#include "touchpad_packets.h"

#include <windows.h>
#include <hidsdi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace glide::touchpad
{

    enum class DecodeResult
    {
        Ignored,
        Parsed,
        Invalid,
        Unsupported
    };

    // Read-only HID adapter. No CreateFile, output/feature report, driver change,
    // device claim, input injection or gesture-system setting is performed here.
    class HidReader
    {
    public:
        void Removed(HANDLE device) noexcept
        {
            for (auto& entry : entries_)
                if (entry.device == device)
                    entry = Entry{};
        }
        void Reset() noexcept
        {
            for (auto& entry : entries_)
                entry = Entry{};
        }

        template<class Emit>
        DecodeResult Read(HRAWINPUT handle, Emit emit)
        {
            UINT bytes = 0;
            if (GetRawInputData(handle, RID_INPUT, nullptr, &bytes, sizeof(RAWINPUTHEADER)) != 0 ||
                bytes < offsetof(RAWINPUT, data) + offsetof(RAWHID, bRawData) || bytes > MaxRawBytes)
                return DecodeResult::Invalid;
            // ULONG storage provides the required DWORD alignment.
            std::vector<ULONG> storage((bytes + sizeof(ULONG) - 1) / sizeof(ULONG));
            const UINT expected = bytes;
            if (GetRawInputData(handle, RID_INPUT, storage.data(), &bytes, sizeof(RAWINPUTHEADER)) != expected ||
                bytes != expected)
                return DecodeResult::Invalid;
            // This private report copy remains mutable for the Windows HidP
            // signatures, which accept PCHAR even for report-reading operations.
            auto* input = reinterpret_cast<RAWINPUT*>(storage.data());
            if (input->header.dwSize != bytes || input->header.dwType != RIM_TYPEHID)
                return DecodeResult::Invalid;
            if (!input->header.hDevice)
                return DecodeResult::Unsupported;
            auto& hid = input->data.hid;
            constexpr std::size_t offset = offsetof(RAWINPUT, data) + offsetof(RAWHID, bRawData);
            if (!ValidBatch(bytes, offset, hid.dwSizeHid, hid.dwCount))
                return DecodeResult::Invalid;
            auto& entry = EntryFor(input->header.hDevice);
            if (!entry.usable)
                return DecodeResult::Unsupported;
            if (hid.dwSizeHid != entry.reportBytes)
                return DecodeResult::Invalid;
            bool parsed = false;
            for (DWORD i = 0; i < hid.dwCount; ++i)
            {
                auto* data = reinterpret_cast<unsigned char*>(hid.bRawData) + static_cast<std::size_t>(i) * hid.dwSizeHid;
                const Format* format = nullptr;
                for (unsigned j = 0; j < entry.formats; ++j)
                {
                    if (entry.format[j].report == data[0])
                    {
                        format = &entry.format[j];
                        break;
                    }
                }
                if (!format)
                    continue; // Another report ID in the same touchpad TLC.
                Report report;
                if (!Decode(entry, *format, data, hid.dwSizeHid, report))
                    return DecodeResult::Invalid;
                emit(report);
                parsed = true;
            }
            return parsed ? DecodeResult::Parsed : DecodeResult::Ignored;
        }

    private:
        struct Value
        {
            USAGE page = 0, usage = 0;
            USHORT link = 0, bits = 0, index = 0;
            LONG minimum = 0, maximum = 0;
            bool set = false;
        };
        struct Finger
        {
            USHORT link = 0;
            Value id{}, x{}, y{};
            bool tip = false, confidence = false;
        };
        struct Format
        {
            UCHAR report = 0;
            Value count{}, scan{};
            std::array<Finger, MaxContacts> fingers{};
            unsigned slots = 0;
            bool buttons = false;
        };
        struct Entry
        {
            HANDLE device = nullptr;
            std::uint64_t age = 0;
            bool usable = false;
            USHORT reportBytes = 0;
            std::vector<ULONG> preparsed{};
            std::array<Format, 16> format{};
            unsigned formats = 0;
            PHIDP_PREPARSED_DATA Data() { return reinterpret_cast<PHIDP_PREPARSED_DATA>(preparsed.data()); }
        };

        Entry& EntryFor(HANDLE device)
        {
            for (auto& entry : entries_)
                if (entry.device == device)
                {
                    entry.age = ++age_;
                    return entry;
                }
            auto* selected = &entries_[0];
            for (auto& entry : entries_)
            {
                if (!entry.device)
                {
                    selected = &entry;
                    break;
                }
                if (entry.age < selected->age)
                    selected = &entry;
            }
            *selected = Entry{};
            selected->device = device;
            selected->age = ++age_;
            selected->usable = Load(*selected);
            return *selected;
        }
        static Format* FindFormat(Entry& entry, UCHAR report)
        {
            for (unsigned i = 0; i < entry.formats; ++i)
                if (entry.format[i].report == report)
                    return &entry.format[i];
            if (entry.formats == entry.format.size())
                return nullptr;
            auto& format = entry.format[entry.formats++];
            format.report = report;
            return &format;
        }
        static Finger* FindFinger(Format& format, USHORT link)
        {
            if (!link)
                return nullptr;
            for (unsigned i = 0; i < format.slots; ++i)
                if (format.fingers[i].link == link)
                    return &format.fingers[i];
            if (format.slots == MaxContacts)
                return nullptr;
            auto& finger = format.fingers[format.slots++];
            finger.link = link;
            return &finger;
        }
        static bool Includes(const HIDP_VALUE_CAPS& caps, USAGE usage)
        {
            return caps.IsRange ? usage >= caps.Range.UsageMin && usage <= caps.Range.UsageMax : caps.NotRange.Usage == usage;
        }
        static bool Includes(const HIDP_BUTTON_CAPS& caps, USAGE usage)
        {
            return caps.IsRange ? usage >= caps.Range.UsageMin && usage <= caps.Range.UsageMax : caps.NotRange.Usage == usage;
        }
        static bool Assign(Value& value, const HIDP_VALUE_CAPS& caps, USAGE usage)
        {
            if (value.set || caps.IsAlias || !caps.IsAbsolute || !caps.BitSize || caps.BitSize > 32 ||
                (!caps.IsRange && caps.ReportCount != 1))
                return false;
            unsigned index = caps.NotRange.DataIndex;
            if (caps.IsRange)
            {
                if (caps.Range.UsageMax < caps.Range.UsageMin ||
                    caps.Range.DataIndexMax < caps.Range.DataIndexMin ||
                    caps.Range.DataIndexMax - caps.Range.DataIndexMin != caps.Range.UsageMax - caps.Range.UsageMin)
                    return false;
                index = caps.Range.DataIndexMin + usage - caps.Range.UsageMin;
            }
            value = { caps.UsagePage, usage, caps.LinkCollection, caps.BitSize, static_cast<USHORT>(index), caps.LogicalMin, caps.LogicalMax, true };
            return true;
        }
        static bool Load(Entry& entry)
        {
            UINT bytes = 0;
            if (GetRawInputDeviceInfoW(entry.device, RIDI_PREPARSEDDATA, nullptr, &bytes) != 0 ||
                !bytes || bytes > MaxRawBytes)
                return false;
            entry.preparsed.resize((bytes + sizeof(ULONG) - 1) / sizeof(ULONG));
            const UINT expected = bytes;
            if (GetRawInputDeviceInfoW(entry.device, RIDI_PREPARSEDDATA, entry.preparsed.data(), &bytes) != expected ||
                bytes != expected)
                return false;
            HIDP_CAPS caps{};
            if (HidP_GetCaps(entry.Data(), &caps) != HIDP_STATUS_SUCCESS || caps.UsagePage != 0x0D || caps.Usage != 0x05 ||
                !caps.InputReportByteLength || caps.InputReportByteLength > MaxReportBytes ||
                !caps.NumberInputValueCaps || caps.NumberInputValueCaps > 256 ||
                !caps.NumberInputButtonCaps || caps.NumberInputButtonCaps > 128)
                return false;
            entry.reportBytes = caps.InputReportByteLength;
            std::array<HIDP_VALUE_CAPS, 256> values{};
            USHORT valueCount = caps.NumberInputValueCaps;
            if (HidP_GetValueCaps(HidP_Input, values.data(), &valueCount, entry.Data()) != HIDP_STATUS_SUCCESS ||
                valueCount > values.size())
                return false;
            // Discover slots by contact ID, independent of X/Y capability order.
            for (unsigned i = 0; i < valueCount; ++i)
            {
                const auto& value = values[i];
                if (value.UsagePage != 0x0D || !Includes(value, 0x51))
                    continue;
                auto* format = FindFormat(entry, value.ReportID);
                auto* finger = format ? FindFinger(*format, value.LinkCollection) : nullptr;
                if (!finger || !Assign(finger->id, value, 0x51))
                    return false;
            }
            if (!entry.formats)
                return false;
            for (unsigned f = 0; f < entry.formats; ++f)
            {
                auto& format = entry.format[f];
                // Capability arrays may enumerate controls in a different order.
                // HID data indices retain descriptor order; unused parallel slots
                // must never be promoted ahead of the frame's first valid slots.
                for (unsigned i = 1; i < format.slots; ++i)
                {
                    for (unsigned j = i; j && format.fingers[j].id.index < format.fingers[j - 1].id.index; --j)
                        std::swap(format.fingers[j], format.fingers[j - 1]);
                }
                for (unsigned i = 1; i < format.slots; ++i)
                    if (format.fingers[i - 1].id.index == format.fingers[i].id.index)
                        return false;
                for (unsigned i = 0; i < valueCount; ++i)
                {
                    const auto& value = values[i];
                    if (value.ReportID != format.report)
                        continue;
                    if (value.UsagePage == 0x0D && value.LinkCollection == 0)
                    {
                        if (Includes(value, 0x54) && !Assign(format.count, value, 0x54))
                            return false;
                        if (Includes(value, 0x56) && !Assign(format.scan, value, 0x56))
                            return false;
                    }
                    for (unsigned j = 0; j < format.slots; ++j)
                    {
                        auto& finger = format.fingers[j];
                        if (value.LinkCollection != finger.link || value.UsagePage != 0x01)
                            continue;
                        if (Includes(value, 0x30) && !Assign(finger.x, value, 0x30))
                            return false;
                        if (Includes(value, 0x31) && !Assign(finger.y, value, 0x31))
                            return false;
                    }
                }
            }
            std::array<HIDP_BUTTON_CAPS, 128> buttons{};
            USHORT buttonCount = caps.NumberInputButtonCaps;
            if (HidP_GetButtonCaps(HidP_Input, buttons.data(), &buttonCount, entry.Data()) != HIDP_STATUS_SUCCESS ||
                buttonCount > buttons.size())
                return false;
            for (unsigned f = 0; f < entry.formats; ++f)
            {
                auto& format = entry.format[f];
                for (unsigned i = 0; i < buttonCount; ++i)
                {
                    const auto& button = buttons[i];
                    if (button.ReportID != format.report)
                        continue;
                    if (button.UsagePage == 0x09)
                        format.buttons = true;
                    if (button.UsagePage != 0x0D)
                        continue;
                    for (unsigned j = 0; j < format.slots; ++j)
                    {
                        auto& finger = format.fingers[j];
                        if (button.LinkCollection != finger.link)
                            continue;
                        finger.tip = finger.tip || Includes(button, 0x42);
                        finger.confidence = finger.confidence || Includes(button, 0x47);
                    }
                }
                if (!format.count.set || !format.scan.set || !format.slots)
                    return false;
                for (unsigned j = 0; j < format.slots; ++j)
                {
                    const auto& finger = format.fingers[j];
                    if (!finger.id.set || !finger.x.set || !finger.y.set || !finger.tip || !finger.confidence ||
                        finger.x.maximum <= finger.x.minimum || finger.y.maximum <= finger.y.minimum)
                        return false;
                }
            }
            return true;
        }
        static bool Get(Entry& entry, const Value& value, unsigned char* data, ULONG bytes, ULONG& result)
        {
            return value.set && HidP_GetUsageValue(HidP_Input, value.page, value.link, value.usage, &result, entry.Data(), reinterpret_cast<PCHAR>(data), bytes) == HIDP_STATUS_SUCCESS;
        }
        static bool Usages(Entry& entry, USAGE page, USHORT link, unsigned char* data, ULONG bytes, std::array<USAGE, 128>& usages, ULONG& count)
        {
            count = static_cast<ULONG>(usages.size());
            return HidP_GetUsages(HidP_Input, page, link, usages.data(), &count, entry.Data(), reinterpret_cast<PCHAR>(data), bytes) == HIDP_STATUS_SUCCESS && count <= usages.size();
        }
        static bool Decode(Entry& entry, const Format& format, unsigned char* data, ULONG bytes, Report& report)
        {
            ULONG count = 0, scan = 0;
            if (!Get(entry, format.count, data, bytes, count) || !Get(entry, format.scan, data, bytes, scan) ||
                count > MaxContacts || scan > 0xFFFF)
                return false;
            report.device = reinterpret_cast<std::uintptr_t>(entry.device);
            report.total = count;
            report.scan = static_cast<std::uint16_t>(scan);
            report.slots = format.slots;
            std::array<USAGE, 128> usages{};
            ULONG usagesCount = 0;
            if (format.buttons)
            {
                if (!Usages(entry, 0x09, 0, data, bytes, usages, usagesCount))
                    return false;
                for (ULONG i = 0; i < usagesCount; ++i)
                    if (usages[i] >= 1 && usages[i] <= 3)
                        report.button = true;
            }
            for (unsigned i = 0; i < format.slots; ++i)
            {
                const auto& finger = format.fingers[i];
                auto& contact = report.contacts[i];
                ULONG id = 0, x = 0, y = 0;
                contact.valid = Get(entry, finger.id, data, bytes, id) && Get(entry, finger.x, data, bytes, x) &&
                                Get(entry, finger.y, data, bytes, y) &&
                                Normalize(x, finger.x.bits, finger.x.minimum, finger.x.maximum, contact.x) &&
                                Normalize(y, finger.y.bits, finger.y.minimum, finger.y.maximum, contact.y) &&
                                Usages(entry, 0x0D, finger.link, data, bytes, usages, usagesCount);
                contact.id = id;
                if (!contact.valid)
                    continue; // Unused parallel slots are ignored by Assembler.
                for (ULONG j = 0; j < usagesCount; ++j)
                {
                    contact.tip = contact.tip || usages[j] == 0x42;
                    contact.confidence = contact.confidence || usages[j] == 0x47;
                }
            }
            return true;
        }

        std::array<Entry, 4> entries_{};
        std::uint64_t age_ = 0;
    };

} // namespace glide::touchpad
