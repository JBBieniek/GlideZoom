// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "../GlideZoom/powertoys_runtime.h"
#include "../GlideZoom/powertoys_settings.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

// This translation unit uses the real Windows SDK. The HID boundary fuzzer
// deliberately compiles in a different translation unit with private API mocks.
void FuzzPowerToysSettings(const std::uint8_t* data, std::size_t size)
{
    static glide::powertoys::Apartment apartment;
    if (!apartment.Ready())
    {
        std::abort();
    }
    glide::settings::Values original;
    original.zoomScope = 1;
    original.scopeIndicatorMode = 3;
    original.scopeIndicatorStyle = 2;
    original.scopeIndicatorSize = 35;
    original.scopeIndicatorOpacity = 90;
    original.scopeIndicatorDetails = false;
    original.wheelAcceleration = true;
    original.wheelAccelerationLimit = 4;
    original.scopeIndicatorX = 0;
    original.scopeIndicatorY = 0;
    original.scopeIndicatorFade = 400;
    original.pointerSpeed = 137;
    original.overviewOpacity = 35;
    auto parsed = original;
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    if (glide::settings::ParsePowerToys(input, parsed))
    {
        glide::settings::Values roundTrip;
        if (!glide::settings::Valid(parsed) || parsed.zoomScope < 0 || parsed.zoomScope > 2 ||
            parsed.scopeIndicatorStyle < 0 || parsed.scopeIndicatorStyle > 2 ||
            parsed.pointerSpeed < 25 || parsed.pointerSpeed > 300 || parsed.overviewOpacity < 0 || parsed.overviewOpacity > 100 ||
            !glide::settings::ParsePowerToys(glide::settings::SerializePowerToys(parsed), roundTrip) ||
            !(roundTrip == parsed))
        {
            std::abort();
        }
    }
    else if (!(parsed == original))
    {
        std::abort();
    }

    std::wstring processText;
    processText.reserve((std::min)(size, std::size_t{256}));
    for (std::size_t index = 0; index < (std::min)(size, std::size_t{256}); ++index)
    {
        processText.push_back(static_cast<wchar_t>(data[index]));
    }
    DWORD processId = 37;
    const bool valid = glide::powertoys::ParseProcessId(processText, processId);
    if ((valid && processId == 0) || (!valid && processId != 37))
    {
        std::abort();
    }
}
