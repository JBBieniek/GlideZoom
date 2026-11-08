#pragma once
#include <array>
#include <charconv>
#include <string>
#include <string_view>

namespace glide::settings
{
    struct Values
    {
        int modifier = 0;
        int maxZoom = 8;
        int sensitivity = 1;
        bool keyboardGesture = false;
        bool mouseGesture = false;
        bool reverse = false;
        bool centerFollow = false;
        bool touchpadGesture = false;
        bool pointerScaling = true;
        int zoomMultiplier = 1;
        int edgePanMultiplier = 1;
        int zoomScope = 2; // 0: one monitor; 1: linked desktop; 2: independent monitors.
        int scopeIndicatorMode = 0; // Off, while moving, shortcut only, or always.
        int scopeIndicatorStyle = 0; // Overview, screen crop outline, or both.
        int scopeIndicatorSize = 20;
        int scopeIndicatorOpacity = 55;
        int scopeIndicatorBorder = 2;
        int scopeIndicatorDuration = 1500;
        bool scopeIndicatorDetails = true;
        bool wheelAcceleration = false;
        int wheelAccelerationLimit = 3;
        int scopeIndicatorX = 100;
        int scopeIndicatorY = 100;
        int scopeIndicatorFade = 120;
        int pointerSpeed = 100;
        int overviewOpacity = 55;

        double WheelFactor() const { return std::array<double, 3>{ 1.06, 1.12, 1.22 }[sensitivity]; }
        double DragDistance() const { return std::array<double, 3>{ 360, 220, 140 }[sensitivity]; }
    };
    inline bool Valid(const Values& v) noexcept
    {
        return v.modifier >= 0 && v.modifier <= 3 && v.sensitivity >= 0 && v.sensitivity <= 2 &&
               v.zoomMultiplier >= 1 && v.zoomMultiplier <= 100 &&
               v.edgePanMultiplier >= 1 && v.edgePanMultiplier <= 10 &&
               v.zoomScope >= 0 && v.zoomScope <= 2 &&
               v.scopeIndicatorMode >= 0 && v.scopeIndicatorMode <= 3 &&
               v.scopeIndicatorStyle >= 0 && v.scopeIndicatorStyle <= 2 &&
               v.scopeIndicatorSize >= 10 && v.scopeIndicatorSize <= 35 &&
               v.scopeIndicatorOpacity >= 10 && v.scopeIndicatorOpacity <= 90 &&
               v.scopeIndicatorBorder >= 1 && v.scopeIndicatorBorder <= 8 &&
               v.scopeIndicatorDuration >= 500 && v.scopeIndicatorDuration <= 5000 &&
               v.wheelAccelerationLimit >= 1 && v.wheelAccelerationLimit <= 4 &&
               v.scopeIndicatorX >= 0 && v.scopeIndicatorX <= 100 &&
               v.scopeIndicatorY >= 0 && v.scopeIndicatorY <= 100 &&
               v.scopeIndicatorFade >= 0 && v.scopeIndicatorFade <= 400 &&
               v.pointerSpeed >= 25 && v.pointerSpeed <= 300 &&
               v.overviewOpacity >= 0 && v.overviewOpacity <= 100 &&
               (v.maxZoom == 2 || v.maxZoom == 4 || v.maxZoom == 8 || v.maxZoom == 16);
    }
    inline bool operator==(const Values& a, const Values& b) noexcept
    {
        return a.modifier == b.modifier && a.maxZoom == b.maxZoom && a.sensitivity == b.sensitivity &&
               a.keyboardGesture == b.keyboardGesture && a.mouseGesture == b.mouseGesture &&
               a.reverse == b.reverse && a.centerFollow == b.centerFollow &&
               a.touchpadGesture == b.touchpadGesture && a.pointerScaling == b.pointerScaling &&
               a.zoomMultiplier == b.zoomMultiplier && a.edgePanMultiplier == b.edgePanMultiplier &&
               a.zoomScope == b.zoomScope && a.scopeIndicatorMode == b.scopeIndicatorMode &&
               a.scopeIndicatorStyle == b.scopeIndicatorStyle && a.scopeIndicatorSize == b.scopeIndicatorSize &&
               a.scopeIndicatorOpacity == b.scopeIndicatorOpacity && a.scopeIndicatorBorder == b.scopeIndicatorBorder &&
               a.scopeIndicatorDuration == b.scopeIndicatorDuration && a.scopeIndicatorDetails == b.scopeIndicatorDetails &&
               a.wheelAcceleration == b.wheelAcceleration && a.wheelAccelerationLimit == b.wheelAccelerationLimit &&
               a.scopeIndicatorX == b.scopeIndicatorX && a.scopeIndicatorY == b.scopeIndicatorY &&
               a.scopeIndicatorFade == b.scopeIndicatorFade && a.pointerSpeed == b.pointerSpeed &&
               a.overviewOpacity == b.overviewOpacity;
    }

    // EDIT controls can receive pasted text that their keyboard style would not
    // admit. Parse the whole short field, not a numeric prefix; never change the
    // candidate on a malformed or out-of-range value.
    inline bool ParseZoomMultiplier(std::wstring_view text, int& output) noexcept
    {
        if (text.empty() || text.size() > 3)
            return false;
        int value = 0;
        for (const wchar_t character : text)
        {
            if (character < L'0' || character > L'9')
                return false;
            value = value * 10 + static_cast<int>(character - L'0');
        }
        if (value < 1 || value > 100)
            return false;
        output = value;
        return true;
    }

    inline bool ParseEdgePanMultiplier(std::wstring_view text, int& output) noexcept
    {
        if (text.empty() || text.size() > 2)
            return false;
        int value = 0;
        for (const wchar_t character : text)
        {
            if (character < L'0' || character > L'9')
                return false;
            value = value * 10 + static_cast<int>(character - L'0');
        }
        if (value < 1 || value > 10)
            return false;
        output = value;
        return true;
    }

    inline bool ParsePointerSpeed(std::wstring_view text, int& output) noexcept
    {
        if (text.empty() || text.size() > 3)
            return false;
        int value = 0;
        for (const wchar_t character : text)
        {
            if (character < L'0' || character > L'9')
                return false;
            value = value * 10 + static_cast<int>(character - L'0');
        }
        if (value < 25 || value > 300)
            return false;
        output = value;
        return true;
    }

    inline std::string_view Trim(std::string_view s) noexcept
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
            s.remove_suffix(1);
        return s;
    }

    // Parse a complete candidate before modifying the caller. Unknown fields,
    // duplicate fields, truncated files and malformed values reject the whole file.
    // Versionless complete v0.1 files remain readable. ASCII and UTF-8 BOM accepted.
    inline bool Parse(std::string_view bytes, Values& output) noexcept
    {
        if (bytes.size() > 16384 || bytes.find('\0') != std::string_view::npos)
            return false;
        if (bytes.substr(0, 3) == "\xef\xbb\xbf")
            bytes.remove_prefix(3);
        constexpr std::array<std::string_view, 28> names{
            "WheelModifier", "MaximumZoom", "Sensitivity", "DoubleControl", "ModifiedDoubleClick", "ReverseDirection", "CenterFollow", "Version", "ThreeFingerTouchpad", "ExtraPointerPrecision", "ZoomScaledPointer", "ZoomMultiplier", "EdgePanMultiplier", "ZoomScope",
            "ScopeIndicatorMode", "ScopeIndicatorStyle", "ScopeIndicatorSize", "ScopeIndicatorOpacity", "ScopeIndicatorBorder", "ScopeIndicatorDuration", "ScopeIndicatorDetails", "WheelAcceleration", "WheelAccelerationLimit", "ScopeIndicatorX", "ScopeIndicatorY", "ScopeIndicatorFade", "PointerSpeed", "OverviewOpacity"
        };
        std::array<int, 28> values{};
        unsigned seen = 0;
        bool section = false;
        while (!bytes.empty())
        {
            const auto end = bytes.find('\n');
            const auto line = Trim(bytes.substr(0, end));
            if (end == std::string_view::npos)
                bytes = {};
            else
                bytes.remove_prefix(end + 1);
            if (line.empty() || line.front() == ';' || line.front() == '#')
                continue;
            if (line == "[GlideZoom]")
            {
                if (section)
                    return false;
                section = true;
                continue;
            }
            if (!section)
                return false;
            const auto equals = line.find('=');
            if (equals == std::string_view::npos)
                return false;
            const auto name = Trim(line.substr(0, equals));
            const auto value = Trim(line.substr(equals + 1));
            std::size_t index = 0;
            for (; index < names.size() && names[index] != name; ++index)
            {
            }
            if (index == names.size() || (seen & (1u << index)) || value.empty())
                return false;
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), values[index]);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
                return false;
            seen |= 1u << index;
        }
        if ((seen & 0x7fu) != 0x7fu)
            return false;
        const bool version4 = (seen & 0x80u) && values[7] == 4;
        const bool version5 = (seen & 0x80u) && values[7] == 5;
        const bool version6 = (seen & 0x80u) && values[7] == 6;
        const bool version7 = (seen & 0x80u) && values[7] == 7;
        const bool version8 = (seen & 0x80u) && values[7] == 8;
        const bool version9 = (seen & 0x80u) && values[7] == 9;
        const bool version10 = (seen & 0x80u) && values[7] == 10;
        const bool version11 = (seen & 0x80u) && values[7] == 11;
        const bool indicatorSchema = version10 || version11;
        const bool scopeSchema = version8 || version9 || indicatorSchema;
        const bool edgeSchema = version7 || scopeSchema;
        const bool multiplierSchema = version6 || edgeSchema;
        const bool pointerSchema = version5 || multiplierSchema;
        const bool touchpadSchema = (seen & 0x80u) && (values[7] == 3 || version4 || pointerSchema);
        if (touchpadSchema)
        {
            if (!(seen & 0x100u) || (values[8] != 0 && values[8] != 1))
                return false;
        }
        else if ((seen & 0x100u) || ((seen & 0x80u) && values[7] != 2))
            return false;
        if (version4)
        {
            if (!(seen & 0x200u) || (values[9] != 0 && values[9] != 1))
                return false;
        }
        else if (seen & 0x200u)
            return false;
        if (pointerSchema)
        {
            if (!(seen & 0x400u) || (values[10] != 0 && values[10] != 1))
                return false;
        }
        else if (seen & 0x400u)
            return false;
        if (multiplierSchema)
        {
            if (!(seen & 0x800u) || values[11] < 1 || values[11] > 100)
                return false;
        }
        else if (seen & 0x800u)
            return false;
        if (edgeSchema)
        {
            if (!(seen & 0x1000u) || values[12] < 1 || values[12] > 10)
                return false;
        }
        else if (seen & 0x1000u)
            return false;
        if (scopeSchema)
        {
            if (!(seen & 0x2000u) || values[13] < 0 || values[13] > (version8 ? 1 : 2))
                return false;
        }
        else if (seen & 0x2000u)
            return false;
        for (std::size_t i = 3; i < 7; ++i)
            if (values[i] != 0 && values[i] != 1)
                return false;
        constexpr unsigned indicatorFields = 0x3ffc000u; // Fields 14 through 25.
        if (indicatorSchema)
        {
            if ((seen & indicatorFields) != indicatorFields ||
                (values[20] != 0 && values[20] != 1) || (values[21] != 0 && values[21] != 1))
                return false;
        }
        else if (seen & indicatorFields)
            return false;
        if (version11)
        {
            if (!(seen & (1u << 26)) || values[26] < 25 || values[26] > 300 ||
                !(seen & (1u << 27)) || values[27] < 0 || values[27] > 100)
                return false;
        }
        else if (seen & ((1u << 26) | (1u << 27)))
            return false;
        Values candidate{ values[0], values[1], values[2], values[3] != 0, values[4] != 0, values[5] != 0, values[6] != 0, touchpadSchema && values[8] != 0, !pointerSchema || values[10] != 0, multiplierSchema ? values[11] : 1, edgeSchema ? values[12] : 1, scopeSchema ? values[13] : 0 };
        if (indicatorSchema)
        {
            candidate.scopeIndicatorMode = values[14];
            candidate.scopeIndicatorStyle = values[15];
            candidate.scopeIndicatorSize = values[16];
            candidate.scopeIndicatorOpacity = values[17];
            candidate.scopeIndicatorBorder = values[18];
            candidate.scopeIndicatorDuration = values[19];
            candidate.scopeIndicatorDetails = values[20] != 0;
            candidate.wheelAcceleration = values[21] != 0;
            candidate.wheelAccelerationLimit = values[22];
            candidate.scopeIndicatorX = values[23];
            candidate.scopeIndicatorY = values[24];
            candidate.scopeIndicatorFade = values[25];
        }
        candidate.overviewOpacity = candidate.scopeIndicatorOpacity;
        if (version11)
        {
            candidate.pointerSpeed = values[26];
            candidate.overviewOpacity = values[27];
        }
        // v0.5.1 fixes the ordinary zoom-speed default. Older schemas described an
        // experimental *extra* gain, so migrate either old value to the new normal
        // scaling mode. A v5+ explicit opt-out remains an opt-out on every reload.
        // Earlier schemas have no key/wheel multiplier and keep their 0.01x step.
        // Edge acceleration defaults to the previous behavior when upgrading v1-v6.
        // Configurations before v8 keep selected-monitor zoom as their default scope.
        // Before v11, pointer speed is 100 and overview opacity keeps the prior
        // shared indicator opacity rather than replacing a customized preview.
        if (!Valid(candidate))
            return false;
        output = candidate;
        return true;
    }
    inline std::string Serialize(const Values& v)
    {
        if (!Valid(v))
            return {};
        return "[GlideZoom]\r\nVersion=11\r\nWheelModifier=" + std::to_string(v.modifier) +
               "\r\nMaximumZoom=" + std::to_string(v.maxZoom) +
               "\r\nSensitivity=" + std::to_string(v.sensitivity) +
               "\r\nDoubleControl=" + std::to_string(v.keyboardGesture ? 1 : 0) +
               "\r\nModifiedDoubleClick=" + std::to_string(v.mouseGesture ? 1 : 0) +
               "\r\nReverseDirection=" + std::to_string(v.reverse ? 1 : 0) +
               "\r\nCenterFollow=" + std::to_string(v.centerFollow ? 1 : 0) +
               "\r\nThreeFingerTouchpad=" + std::to_string(v.touchpadGesture ? 1 : 0) +
               "\r\nZoomScaledPointer=" + std::to_string(v.pointerScaling ? 1 : 0) +
               "\r\nZoomMultiplier=" + std::to_string(v.zoomMultiplier) +
               "\r\nEdgePanMultiplier=" + std::to_string(v.edgePanMultiplier) +
               "\r\nZoomScope=" + std::to_string(v.zoomScope) +
               "\r\nScopeIndicatorMode=" + std::to_string(v.scopeIndicatorMode) +
               "\r\nScopeIndicatorStyle=" + std::to_string(v.scopeIndicatorStyle) +
               "\r\nScopeIndicatorSize=" + std::to_string(v.scopeIndicatorSize) +
               "\r\nScopeIndicatorOpacity=" + std::to_string(v.scopeIndicatorOpacity) +
               "\r\nScopeIndicatorBorder=" + std::to_string(v.scopeIndicatorBorder) +
               "\r\nScopeIndicatorDuration=" + std::to_string(v.scopeIndicatorDuration) +
               "\r\nScopeIndicatorDetails=" + std::to_string(v.scopeIndicatorDetails ? 1 : 0) +
               "\r\nWheelAcceleration=" + std::to_string(v.wheelAcceleration ? 1 : 0) +
               "\r\nWheelAccelerationLimit=" + std::to_string(v.wheelAccelerationLimit) +
               "\r\nScopeIndicatorX=" + std::to_string(v.scopeIndicatorX) +
               "\r\nScopeIndicatorY=" + std::to_string(v.scopeIndicatorY) +
               "\r\nPointerSpeed=" + std::to_string(v.pointerSpeed) +
               "\r\nOverviewOpacity=" + std::to_string(v.overviewOpacity) +
               "\r\nScopeIndicatorFade=" + std::to_string(v.scopeIndicatorFade) + "\r\n";
    }
} // namespace glide::settings
