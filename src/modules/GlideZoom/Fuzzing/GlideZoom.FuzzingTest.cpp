// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "../GlideZoom/settings.h"
#include "../GlideZoom/touchpad_packets.h"
#include "../GlideZoom/touchpad_state.h"
#include "../Tests/HidTestApi.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>

void FuzzPowerToysSettings(const std::uint8_t* data, std::size_t size);
void FuzzRouting(const std::uint8_t* data, std::size_t size);

namespace
{
    struct Bytes
    {
        const std::uint8_t* data;
        std::size_t size;
        std::size_t offset = 0;

        template<typename T>
        T Take()
        {
            static_assert(std::is_trivially_copyable_v<T>);
            T result{};
            const auto count = (std::min)(sizeof(T), size - offset);
            if (count)
            {
                std::memcpy(&result, data + offset, count);
                offset += count;
            }
            return result;
        }

        bool Empty() const noexcept
        {
            return offset == size;
        }
    };

    void Require(bool condition)
    {
        if (!condition)
        {
            std::abort();
        }
    }

    void FuzzSettings(const std::uint8_t* data, std::size_t size)
    {
        glide::settings::Values original;
        original.zoomScope = 1;
        original.scopeIndicatorMode = 3;
        original.scopeIndicatorStyle = 2;
        original.scopeIndicatorDetails = false;
        original.wheelAcceleration = true;
        original.scopeIndicatorX = 0;
        original.scopeIndicatorY = 0;
        original.scopeIndicatorFade = 400;
        original.pointerSpeed = 137;
        original.overviewOpacity = 35;
        auto parsed = original;
        const std::string_view input(reinterpret_cast<const char*>(data), size);
        if (glide::settings::Parse(input, parsed))
        {
            Require(glide::settings::Valid(parsed));
            Require(parsed.zoomScope >= 0 && parsed.zoomScope <= 2);
            Require(parsed.scopeIndicatorStyle >= 0 && parsed.scopeIndicatorStyle <= 2);
            Require(parsed.pointerSpeed >= 25 && parsed.pointerSpeed <= 300 && parsed.overviewOpacity >= 0 && parsed.overviewOpacity <= 100);
            glide::settings::Values roundTrip{};
            Require(glide::settings::Parse(glide::settings::Serialize(parsed), roundTrip));
            Require(roundTrip == parsed);
            Require(std::isfinite(parsed.WheelFactor()) && parsed.DragDistance() > 0);
        }
        else
        {
            Require(parsed == original);
        }

        std::wstring field;
        field.reserve((std::min)(size, std::size_t{256}));
        for (std::size_t index = 0; index < (std::min)(size, std::size_t{256}); ++index)
        {
            field.push_back(static_cast<wchar_t>(data[index]));
        }
        int zoom = 37;
        const bool zoomValid = glide::settings::ParseZoomMultiplier(field, zoom);
        Require(zoomValid ? zoom >= 1 && zoom <= 100 : zoom == 37);
        int edge = 7;
        const bool edgeValid = glide::settings::ParseEdgePanMultiplier(field, edge);
        Require(edgeValid ? edge >= 1 && edge <= 10 : edge == 7);
        int speed = 137;
        const bool speedValid = glide::settings::ParsePointerSpeed(field, speed);
        Require(speedValid ? speed >= 25 && speed <= 300 : speed == 137);
    }

    void FuzzPacketPolicies(const std::uint8_t* data, std::size_t size)
    {
        using namespace glide::touchpad;
        Bytes bytes{data, size};
        const auto totalBytes = bytes.Take<std::size_t>();
        const auto offset = bytes.Take<std::size_t>();
        const auto reportBytes = bytes.Take<std::size_t>();
        const auto reports = bytes.Take<std::size_t>();
        if (ValidBatch(totalBytes, offset, reportBytes, reports))
        {
            Require(totalBytes <= MaxRawBytes && offset <= totalBytes && reportBytes > 0 &&
                    reportBytes <= MaxReportBytes && reports > 0 && reports <= MaxReportsPerPacket);
            Require(reports * reportBytes <= totalBytes - offset);
        }

        const auto bits = bytes.Take<std::uint32_t>();
        const auto bitSize = bytes.Take<std::uint8_t>();
        const auto minimum = bytes.Take<std::int32_t>();
        const auto maximum = bytes.Take<std::int32_t>();
        double coordinate = 0.5;
        if (Normalize(bits, bitSize, minimum, maximum, coordinate))
        {
            Require(std::isfinite(coordinate) && coordinate >= 0 && coordinate <= 1);
        }

        Assembler assembler;
        State gesture;
        gesture.SetEnabled(true);
        for (unsigned step = 0; step < 512 && !bytes.Empty(); ++step)
        {
            Report report;
            const auto action = bytes.Take<std::uint8_t>();
            const auto now = bytes.Take<std::uint64_t>();
            if (action & 1)
            {
                report.device = bytes.Take<std::uintptr_t>();
                report.scan = bytes.Take<std::uint16_t>();
                report.total = bytes.Take<std::uint8_t>();
                report.slots = bytes.Take<std::uint8_t>();
                report.button = (action & 2) != 0;
                for (auto& contact : report.contacts)
                {
                    contact.id = bytes.Take<std::uint32_t>();
                    contact.x = bytes.Take<double>();
                    contact.y = bytes.Take<double>();
                    const auto flags = bytes.Take<std::uint8_t>();
                    contact.tip = (flags & 1) != 0;
                    contact.confidence = (flags & 2) != 0;
                    contact.valid = (flags & 4) != 0;
                }
                const auto result = assembler.Push(report, now);
                if (result.complete)
                {
                    Require(result.frame.count <= MaxContacts);
                    gesture.Push(result.frame, now, (action & 4) != 0);
                }
            }
            else
            {
                assembler.Expire(now);
                gesture.Expire(now);
                if (action & 8)
                {
                    assembler.Reset();
                    gesture.Cancel();
                }
                if (action & 16)
                {
                    gesture.SetEnabled((action & 32) != 0);
                }
            }
            Require(assembler.Invariant() && gesture.Invariant());
        }
    }

    // Use the unit tests' bounded Windows API shims to exercise the production
    // HidReader's header, descriptor and report handling without real devices.
    // The shims model the HidP API contract; they do not parse binary descriptors.
    void FuzzNativeHid(const std::uint8_t* data, std::size_t size)
    {
        using namespace glide::touchpad;
        Bytes bytes{data, size};
        const auto slots = 1u + bytes.Take<std::uint8_t>() % MaxContacts;
        const auto count = 1u + bytes.Take<std::uint8_t>() % MaxReportsPerPacket;
        auto& device = mock::Standard(1, static_cast<unsigned>(slots));
        auto packet = mock::Input(1, static_cast<unsigned>(count));
        packet.fault = bytes.Take<std::uint8_t>() % 7;
        device.preparsedFault = bytes.Take<std::uint8_t>() % 7;
        device.capsFault = bytes.Take<std::uint8_t>() % 2;
        device.valuesFault = bytes.Take<std::uint8_t>() % 3;
        device.buttonsFault = bytes.Take<std::uint8_t>() % 3;
        device.valueFault = bytes.Take<std::uint8_t>() % 2;
        device.usagesFault = bytes.Take<std::uint8_t>() % 3;

        const auto mode = bytes.Take<std::uint8_t>();
        if (mode & 1)
        {
            auto& descriptor = device.values[bytes.Take<std::uint8_t>() % device.values.size()];
            descriptor.BitSize = bytes.Take<std::uint8_t>();
            descriptor.LogicalMin = bytes.Take<std::int32_t>();
            descriptor.LogicalMax = bytes.Take<std::int32_t>();
            descriptor.IsRange = (mode & 2) != 0;
            descriptor.IsAlias = (mode & 4) != 0;
            descriptor.IsAbsolute = (mode & 8) != 0;
            descriptor.Range.UsageMin = bytes.Take<std::uint16_t>();
            descriptor.Range.UsageMax = bytes.Take<std::uint16_t>();
            descriptor.Range.DataIndexMin = bytes.Take<std::uint16_t>();
            descriptor.Range.DataIndexMax = bytes.Take<std::uint16_t>();
        }

        // Keep the actual allocation/query size truthful while allowing hostile
        // header lengths/counts. Only valid mock device tokens reach the shims.
        const auto mutationOffset = (mode & 16) ? std::size_t{0} : mock::Offset;
        for (std::size_t index = mutationOffset; index < packet.bytes && !bytes.Empty(); ++index)
        {
            packet.Data()[index] ^= bytes.Take<std::uint8_t>();
        }
        packet.DeviceHandle(mock::Handle(1));

        HidReader reader;
        Assembler assembler;
        State gesture;
        gesture.SetEnabled(true);
        reader.Read(&packet, [&](const Report& report) {
            Require(report.total <= MaxContacts && report.slots <= MaxContacts);
            const auto result = assembler.Push(report, report.scan);
            if (result.complete)
            {
                gesture.Push(result.frame, report.scan);
            }
            Require(assembler.Invariant() && gesture.Invariant());
        });
        reader.Reset();
    }
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    FuzzRouting(data, size);
    FuzzSettings(data, size);
    FuzzPowerToysSettings(data, size);
    FuzzPacketPolicies(data, size);
    FuzzNativeHid(data, size);
    return 0;
}

#ifndef GLIDEZOOM_LIBFUZZER
int main()
{
    // Debug builds provide a deterministic harness smoke run. Release|x64 uses
    // libFuzzer's main and ASan, with generated inputs and corpus support.
    for (const int scope : {0, 1, 2})
    {
        for (const int style : {0, 1, 2})
        {
            glide::settings::Values values;
            values.zoomScope = scope;
            values.scopeIndicatorStyle = style;
            const auto settings = glide::settings::Serialize(values);
            LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(settings.data()), settings.size());
        }
    }
    std::uint8_t bytes[512]{};
    for (unsigned value = 0; value <= 255; ++value)
    {
        std::fill(std::begin(bytes), std::end(bytes), static_cast<std::uint8_t>(value));
        LLVMFuzzerTestOneInput(bytes, sizeof(bytes));
    }
}
#endif
