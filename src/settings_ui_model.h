#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "settings.h"

namespace glide::settings::ui
{
    struct IntegerField
    {
        const wchar_t* label;
        int minimum;
        int maximum;
        int Values::* member;
    };

    inline constexpr std::array<IntegerField, 11> IntegerFields{{
        { L"Zoom gain (1-100)", 1, 100, &Values::zoomMultiplier },
        { L"Edge pan gain (1-10)", 1, 10, &Values::edgePanMultiplier },
        { L"Maximum wheel boost (1-4)", 1, 4, &Values::wheelAccelerationLimit },
        { L"Overview width (10-35%)", 10, 35, &Values::scopeIndicatorSize },
        { L"Screen outline opacity (10-90%)", 10, 90, &Values::scopeIndicatorOpacity },
        { L"Overview opacity (0-100%)", 0, 100, &Values::overviewOpacity },
        { L"Frame thickness (1-8 px)", 1, 8, &Values::scopeIndicatorBorder },
        { L"Visible duration (500-5000 ms)", 500, 5000, &Values::scopeIndicatorDuration },
        { L"Overview horizontal position (0-100%)", 0, 100, &Values::scopeIndicatorX },
        { L"Overview vertical position (0-100%)", 0, 100, &Values::scopeIndicatorY },
        { L"Fade duration (0-400 ms)", 0, 400, &Values::scopeIndicatorFade },
    }};

    inline constexpr std::size_t InvalidMode = IntegerFields.size();
    inline constexpr std::size_t InvalidStyle = InvalidMode + 1;
    inline constexpr std::size_t InvalidConfiguration = InvalidStyle + 1;
    inline constexpr std::size_t NoInvalidField = InvalidConfiguration + 1;

    // Bound the complete text before arithmetic. A pasted suffix, sign, fraction,
    // whitespace, embedded NUL or overlong prefix cannot become a valid value.
    inline bool ParseInteger(std::wstring_view text, int minimum, int maximum, int& output) noexcept
    {
        if (minimum < 0 || maximum < minimum || text.empty())
            return false;
        std::size_t digits = 1;
        for (int remaining = maximum; remaining >= 10; remaining /= 10)
            ++digits;
        if (text.size() > digits)
            return false;
        int value = 0;
        for (const wchar_t character : text)
        {
            if (character < L'0' || character > L'9')
                return false;
            const int digit = static_cast<int>(character - L'0');
            if (digit > maximum || value > (maximum - digit) / 10)
                return false;
            value = value * 10 + digit;
        }
        if (value < minimum || value > maximum)
            return false;
        output = value;
        return true;
    }

    struct AdvancedInput
    {
        std::array<std::wstring_view, IntegerFields.size()> integers{};
        int indicatorMode = 0;
        int indicatorStyle = 0;
        bool wheelAcceleration = false;
        bool showDetails = true;
    };

    inline bool ApplyAdvanced(const Values& current, const AdvancedInput& input,
                              Values& output, std::size_t& invalidField) noexcept
    {
        Values candidate = current;
        for (std::size_t index = 0; index < IntegerFields.size(); ++index)
        {
            const auto& field = IntegerFields[index];
            if (!ParseInteger(input.integers[index], field.minimum, field.maximum, candidate.*(field.member)))
            {
                invalidField = index;
                return false;
            }
        }
        if (input.indicatorMode < 0 || input.indicatorMode > 3)
        {
            invalidField = InvalidMode;
            return false;
        }
        if (input.indicatorStyle < 0 || input.indicatorStyle > 2)
        {
            invalidField = InvalidStyle;
            return false;
        }
        candidate.scopeIndicatorMode = input.indicatorMode;
        candidate.scopeIndicatorStyle = input.indicatorStyle;
        candidate.wheelAcceleration = input.wheelAcceleration;
        candidate.scopeIndicatorDetails = input.showDetails;
        if (!Valid(candidate))
        {
            invalidField = InvalidConfiguration;
            return false;
        }
        output = candidate;
        invalidField = NoInvalidField;
        return true;
    }
}
