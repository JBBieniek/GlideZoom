// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#pragma once

#include <cmath>
#include <limits>
#include <string>
#include <string_view>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#include "settings.h"

namespace glide::settings
{
    // Keep the PowerToys Settings UI JSON contract separate from the original
    // standalone INI format. A complete candidate is validated before use.
    inline bool ParsePowerToys(std::string_view bytes, Values& output) noexcept
    {
        using namespace winrt::Windows::Data::Json;
        if (bytes.size() > 16384 || bytes.find('\0') != std::string_view::npos)
        {
            return false;
        }

        try
        {
            if (bytes.substr(0, 3) == "\xef\xbb\xbf")
            {
                bytes.remove_prefix(3);
            }
            const auto root = JsonObject::Parse(winrt::to_hstring(bytes));
            const auto version = root.GetNamedString(L"version");
            if (root.GetNamedString(L"name") != L"GlideZoom" || (version != L"1" && version != L"1.0"))
            {
                return false;
            }
            const auto properties = root.GetNamedObject(L"properties");
            Values candidate;
            // Missing scope belongs to the original settings contract. Keep its
            // single-monitor behavior while fresh configurations default to 2.
            candidate.zoomScope = 0;
            const auto integer = [&](const wchar_t* name, int& value) {
                const double number = properties.GetNamedObject(name).GetNamedNumber(L"value");
                if (!std::isfinite(number) || std::trunc(number) != number ||
                    number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max())
                {
                    return false;
                }
                value = static_cast<int>(number);
                return true;
            };
            if (!integer(L"WheelModifier", candidate.modifier) ||
                !integer(L"MaximumZoom", candidate.maxZoom) ||
                !integer(L"Sensitivity", candidate.sensitivity) ||
                !integer(L"ZoomMultiplier", candidate.zoomMultiplier) ||
                !integer(L"EdgePanMultiplier", candidate.edgePanMultiplier))
            {
                return false;
            }
            // Existing PowerToys settings have eleven fields. An omitted scope
            // preserves their selected-monitor behavior; a present scope is strict.
            if (properties.HasKey(L"ZoomScope") && !integer(L"ZoomScope", candidate.zoomScope))
            {
                return false;
            }
            const auto optionalInteger = [&](const wchar_t* name, int& value) {
                return !properties.HasKey(name) || integer(name, value);
            };
            if (!optionalInteger(L"ScopeIndicatorMode", candidate.scopeIndicatorMode) ||
                !optionalInteger(L"ScopeIndicatorStyle", candidate.scopeIndicatorStyle) ||
                !optionalInteger(L"ScopeIndicatorSize", candidate.scopeIndicatorSize) ||
                !optionalInteger(L"ScopeIndicatorOpacity", candidate.scopeIndicatorOpacity) ||
                !optionalInteger(L"ScopeIndicatorBorder", candidate.scopeIndicatorBorder) ||
                !optionalInteger(L"ScopeIndicatorDuration", candidate.scopeIndicatorDuration) ||
                !optionalInteger(L"WheelAccelerationLimit", candidate.wheelAccelerationLimit) ||
                !optionalInteger(L"ScopeIndicatorX", candidate.scopeIndicatorX) ||
                !optionalInteger(L"ScopeIndicatorY", candidate.scopeIndicatorY) ||
                !optionalInteger(L"ScopeIndicatorFade", candidate.scopeIndicatorFade) ||
                !optionalInteger(L"PointerSpeed", candidate.pointerSpeed))
            {
                return false;
            }
            candidate.overviewOpacity = candidate.scopeIndicatorOpacity;
            if (!optionalInteger(L"OverviewOpacity", candidate.overviewOpacity))
                return false;
            const auto boolean = [&](const wchar_t* name) {
                return properties.GetNamedObject(name).GetNamedBoolean(L"value");
            };
            candidate.keyboardGesture = boolean(L"DoubleControl");
            candidate.mouseGesture = boolean(L"ModifiedDoubleClick");
            candidate.reverse = boolean(L"ReverseDirection");
            candidate.centerFollow = boolean(L"CenterFollow");
            candidate.touchpadGesture = boolean(L"ThreeFingerTouchpad");
            candidate.pointerScaling = boolean(L"ZoomScaledPointer");
            if (properties.HasKey(L"ScopeIndicatorDetails"))
                candidate.scopeIndicatorDetails = boolean(L"ScopeIndicatorDetails");
            if (properties.HasKey(L"WheelAcceleration"))
                candidate.wheelAcceleration = boolean(L"WheelAcceleration");
            if (!Valid(candidate))
            {
                return false;
            }
            output = candidate;
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    inline std::string SerializePowerToys(const Values& values)
    {
        using namespace winrt::Windows::Data::Json;
        if (!Valid(values))
        {
            return {};
        }
        JsonObject properties;
        const auto integer = [&](const wchar_t* name, int value) {
            JsonObject property;
            property.Insert(L"value", JsonValue::CreateNumberValue(value));
            properties.Insert(name, property);
        };
        const auto boolean = [&](const wchar_t* name, bool value) {
            JsonObject property;
            property.Insert(L"value", JsonValue::CreateBooleanValue(value));
            properties.Insert(name, property);
        };
        integer(L"WheelModifier", values.modifier);
        integer(L"MaximumZoom", values.maxZoom);
        integer(L"Sensitivity", values.sensitivity);
        integer(L"ZoomMultiplier", values.zoomMultiplier);
        integer(L"EdgePanMultiplier", values.edgePanMultiplier);
        integer(L"ZoomScope", values.zoomScope);
        integer(L"ScopeIndicatorMode", values.scopeIndicatorMode);
        integer(L"ScopeIndicatorStyle", values.scopeIndicatorStyle);
        integer(L"PointerSpeed", values.pointerSpeed);
        integer(L"OverviewOpacity", values.overviewOpacity);
        integer(L"ScopeIndicatorSize", values.scopeIndicatorSize);
        integer(L"ScopeIndicatorOpacity", values.scopeIndicatorOpacity);
        integer(L"ScopeIndicatorBorder", values.scopeIndicatorBorder);
        integer(L"ScopeIndicatorDuration", values.scopeIndicatorDuration);
        integer(L"WheelAccelerationLimit", values.wheelAccelerationLimit);
        integer(L"ScopeIndicatorX", values.scopeIndicatorX);
        integer(L"ScopeIndicatorY", values.scopeIndicatorY);
        integer(L"ScopeIndicatorFade", values.scopeIndicatorFade);
        boolean(L"DoubleControl", values.keyboardGesture);
        boolean(L"ModifiedDoubleClick", values.mouseGesture);
        boolean(L"ReverseDirection", values.reverse);
        boolean(L"CenterFollow", values.centerFollow);
        boolean(L"ThreeFingerTouchpad", values.touchpadGesture);
        boolean(L"ZoomScaledPointer", values.pointerScaling);
        boolean(L"ScopeIndicatorDetails", values.scopeIndicatorDetails);
        boolean(L"WheelAcceleration", values.wheelAcceleration);
        JsonObject root;
        root.Insert(L"name", JsonValue::CreateStringValue(L"GlideZoom"));
        root.Insert(L"version", JsonValue::CreateStringValue(L"1.0"));
        root.Insert(L"properties", properties);
        return winrt::to_string(root.Stringify());
    }
}
