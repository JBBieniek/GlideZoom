#include "test_assertions.h"
#include "../src/settings_ui_model.h"

#include <array>
#include <limits>
#include <string>

using glide_test::Assert;

namespace
{
    using namespace glide::settings;
    using namespace glide::settings::ui;
    using Text = std::array<std::wstring, IntegerFields.size()>;
    AdvancedInput Draft(const Values& values, Text& text)
    {
        AdvancedInput input;
        for (std::size_t index = 0; index < IntegerFields.size(); ++index)
        {
            text[index] = std::to_wstring(values.*(IntegerFields[index].member));
            input.integers[index] = text[index];
        }
        input.indicatorMode = values.scopeIndicatorMode;
        input.indicatorStyle = values.scopeIndicatorStyle;
        input.wheelAcceleration = values.wheelAcceleration;
        input.showDetails = values.scopeIndicatorDetails;
        return input;
    }
    void ParseCompleteFields()
    {
        for (const auto& field : IntegerFields)
        {
            for (const int valid : {field.minimum, field.maximum})
            {
                int output = -7;
                Assert::IsTrue(ParseInteger(std::to_wstring(valid), field.minimum, field.maximum, output));
                Assert::AreEqual(valid, output);
            }
            for (const std::wstring invalid : {L"", L"-1", L"+1", L" 1", L"1 ", L"1x", L"1.5", L"999999999999", L"\u0661"})
            {
                int output = -7;
                Assert::IsFalse(ParseInteger(invalid, field.minimum, field.maximum, output));
                Assert::AreEqual(-7, output);
            }
            int output = -7;
            Assert::IsFalse(ParseInteger(std::wstring_view(L"1\0", 2), field.minimum, field.maximum, output));
            Assert::AreEqual(-7, output);
            Assert::IsFalse(ParseInteger(std::to_wstring(field.maximum + 1), field.minimum, field.maximum, output));
            Assert::AreEqual(-7, output);
        }
        int output = -7;
        Assert::IsTrue(ParseInteger(L"2147483647", 0, (std::numeric_limits<int>::max)(), output));
        Assert::AreEqual((std::numeric_limits<int>::max)(), output);
        Assert::IsFalse(ParseInteger(L"2147483648", 0, (std::numeric_limits<int>::max)(), output));
        Assert::AreEqual((std::numeric_limits<int>::max)(), output);
    }
    void ApplyAtomicallyAndPreserveCoreChoices()
    {
        Values current;
        current.modifier = 3;
        current.maxZoom = 16;
        current.sensitivity = 2;
        current.keyboardGesture = current.mouseGesture = current.touchpadGesture = true;
        current.reverse = true;
        current.pointerScaling = false;
        current.pointerSpeed = 137;
        for (const bool maximum : {false, true})
        {
            Text text;
            auto input = Draft(current, text);
            auto expected = current;
            for (std::size_t index = 0; index < IntegerFields.size(); ++index)
            {
                const auto& field = IntegerFields[index];
                expected.*(field.member) = maximum ? field.maximum : field.minimum;
                text[index] = std::to_wstring(expected.*(field.member));
                input.integers[index] = text[index];
            }
            input.indicatorMode = expected.scopeIndicatorMode = 3;
            input.indicatorStyle = expected.scopeIndicatorStyle = 2;
            input.wheelAcceleration = expected.wheelAcceleration = true;
            input.showDetails = expected.scopeIndicatorDetails = false;
            Values output;
            std::size_t invalid = InvalidConfiguration;
            Assert::IsTrue(ApplyAdvanced(current, input, output, invalid));
            Assert::IsTrue(output == expected && invalid == NoInvalidField);
        }
        for (std::size_t index = 0; index < IntegerFields.size(); ++index)
        {
            Text text;
            auto input = Draft(current, text);
            text[index] = L"1junk";
            input.integers[index] = text[index];
            Values output = current;
            std::size_t invalid = NoInvalidField;
            Assert::IsFalse(ApplyAdvanced(current, input, output, invalid));
            Assert::IsTrue(output == current && invalid == index);
        }
        for (const int mode : {-1, 4})
        {
            Text text;
            auto input = Draft(current, text);
            input.indicatorMode = mode;
            Values output = current;
            std::size_t invalid = NoInvalidField;
            Assert::IsFalse(ApplyAdvanced(current, input, output, invalid));
            Assert::IsTrue(output == current && invalid == InvalidMode);
        }
        for (const int style : {-1, 3})
        {
            Text text;
            auto input = Draft(current, text);
            input.indicatorStyle = style;
            Values output = current;
            std::size_t invalid = NoInvalidField;
            Assert::IsFalse(ApplyAdvanced(current, input, output, invalid));
            Assert::IsTrue(output == current && invalid == InvalidStyle);
        }
        Text text;
        auto invalidCurrent = current;
        invalidCurrent.pointerSpeed = 301;
        auto input = Draft(invalidCurrent, text);
        Values output = current;
        std::size_t invalid = NoInvalidField;
        Assert::IsFalse(ApplyAdvanced(invalidCurrent, input, output, invalid));
        Assert::IsTrue(output == current && invalid == InvalidConfiguration);
    }
}

int main()
{
    try
    {
        ParseCompleteFields();
        ApplyAtomicallyAndPreserveCoreChoices();
        std::cout << "PASS: standalone advanced settings parsing, ranges, atomic apply and core preservation\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
