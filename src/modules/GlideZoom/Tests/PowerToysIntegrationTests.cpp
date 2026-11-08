// Copyright (c) Microsoft Corporation
// The Microsoft Corporation licenses this file to you under the MIT license.
// See the LICENSE file in the project root for more information.

#include "CppUnitTest.h"
#include "../GlideZoom/powertoys_runtime.h"
#include "../GlideZoom/powertoys_settings.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GlideZoomTests
{
    namespace
    {
        constexpr std::array<const wchar_t*, 14> OptionalDisplaySettings{
            L"ScopeIndicatorMode", L"ScopeIndicatorStyle", L"ScopeIndicatorSize", L"ScopeIndicatorOpacity",
            L"ScopeIndicatorBorder", L"ScopeIndicatorDuration", L"ScopeIndicatorDetails", L"WheelAcceleration",
            L"WheelAccelerationLimit", L"ScopeIndicatorX", L"ScopeIndicatorY", L"ScopeIndicatorFade", L"PointerSpeed", L"OverviewOpacity"};

        struct TestApartment
        {
            HRESULT status = RoInitialize(RO_INIT_MULTITHREADED);

            TestApartment()
            {
                Assert::IsTrue(SUCCEEDED(status) || status == RPC_E_CHANGED_MODE);
            }

            ~TestApartment()
            {
                if (SUCCEEDED(status))
                {
                    RoUninitialize();
                }
            }
        };

        struct Event
        {
            HANDLE handle = nullptr;

            explicit Event(const std::wstring& name) :
                handle(CreateEventW(nullptr, FALSE, FALSE, name.c_str()))
            {
                Assert::IsNotNull(handle);
            }

            ~Event()
            {
                CloseHandle(handle);
            }
        };

        void RejectSettings(std::string_view json)
        {
            glide::settings::Values before;
            before.modifier = 3;
            before.zoomMultiplier = 37;
            before.edgePanMultiplier = 7;
            before.touchpadGesture = true;
            before.zoomScope = 1;
            auto after = before;
            Assert::IsFalse(glide::settings::ParsePowerToys(json, after));
            Assert::IsTrue(before == after, L"Rejected settings must leave the active configuration unchanged");
        }
    }

    TEST_CLASS(PowerToysIntegrationTests)
    {
    public:
        TEST_METHOD(ProcessIdParsingIsStrictAndAtomic)
        {
            for (const auto text : {L"1", L"0001", L"4294967295"})
            {
                DWORD processId = 37;
                Assert::IsTrue(glide::powertoys::ParseProcessId(text, processId));
                Assert::IsTrue(processId != 0);
            }
            for (const auto text : {L"", L"0", L"00000", L"-1", L"+1", L" 1", L"1 ",
                                    L"1\n", L"1x", L"4294967296", L"9999999999", L"00000000001"})
            {
                DWORD processId = 37;
                Assert::IsFalse(glide::powertoys::ParseProcessId(text, processId));
                Assert::AreEqual(DWORD{37}, processId);
            }
            const std::wstring embeddedNull(L"1\0", 2);
            DWORD processId = 37;
            Assert::IsFalse(glide::powertoys::ParseProcessId(embeddedNull, processId));
            Assert::AreEqual(DWORD{37}, processId);
        }

        TEST_METHOD(PowerToysSettingsRoundTrip)
        {
            TestApartment apartment;
            for (int modifier = 0; modifier <= 3; ++modifier)
            {
                for (const int maximum : {2, 4, 8, 16})
                {
                    for (int sensitivity = 0; sensitivity <= 2; ++sensitivity)
                    {
                        for (const int zoom : {1, 37, 100})
                        {
                            for (const int edge : {1, 7, 10})
                            {
                                for (const int scope : {0, 1, 2})
                                {
                                    glide::settings::Values before;
                                    before.modifier = modifier;
                                    before.maxZoom = maximum;
                                    before.sensitivity = sensitivity;
                                    before.zoomMultiplier = zoom;
                                    before.edgePanMultiplier = edge;
                                    before.zoomScope = scope;
                                    before.keyboardGesture = (modifier & 1) != 0;
                                    before.mouseGesture = (modifier & 2) != 0;
                                    before.reverse = sensitivity == 2;
                                    before.centerFollow = maximum == 16;
                                    before.touchpadGesture = zoom == 37;
                                    before.pointerScaling = edge != 7;
                                    glide::settings::Values after;
                                    const auto json = glide::settings::SerializePowerToys(before);
                                    Assert::IsFalse(json.empty());
                                    Assert::IsTrue(glide::settings::ParsePowerToys(json, after));
                                    Assert::IsTrue(before == after);
                                    Assert::IsTrue(glide::settings::ParsePowerToys("\xef\xbb\xbf" + json, after));
                                    Assert::IsTrue(before == after);
                                }
                            }
                        }
                    }
                }
            }
        }

        TEST_METHOD(PowerToysSettingsRejectMalformedAndIncompleteData)
        {
            TestApartment apartment;
            using namespace winrt::Windows::Data::Json;
            const auto valid = glide::settings::SerializePowerToys({});
            for (const auto name : {L"WheelModifier", L"MaximumZoom", L"Sensitivity", L"ZoomMultiplier",
                                    L"EdgePanMultiplier", L"DoubleControl", L"ModifiedDoubleClick",
                                    L"ReverseDirection", L"CenterFollow", L"ThreeFingerTouchpad", L"ZoomScaledPointer"})
            {
                auto root = JsonObject::Parse(winrt::to_hstring(valid));
                root.GetNamedObject(L"properties").Remove(name);
                RejectSettings(winrt::to_string(root.Stringify()));

                root = JsonObject::Parse(winrt::to_hstring(valid));
                root.GetNamedObject(L"properties").GetNamedObject(name).Insert(L"value", JsonValue::CreateStringValue(L"1"));
                RejectSettings(winrt::to_string(root.Stringify()));
            }

            for (const auto name : {L"name", L"version", L"properties"})
            {
                auto root = JsonObject::Parse(winrt::to_hstring(valid));
                root.Remove(name);
                RejectSettings(winrt::to_string(root.Stringify()));
            }
            for (const auto input : {"", "{}", "null", "[]", "{", "{\"name\":\"GlideZoom\"}",
                                     "{\"name\":\"GlideZoom\",\"version\":\"2\",\"properties\":{}}"})
            {
                RejectSettings(input);
            }
            RejectSettings(std::string(16385, ' '));
            RejectSettings(std::string("\0", 1) + valid);
            RejectSettings(glide::settings::Serialize({}));
            RejectSettings(std::string("\xc0\xaf", 2));
        }

        TEST_METHOD(PowerToysSettingsAcceptSupportedVersionSpellings)
        {
            TestApartment apartment;
            using namespace winrt::Windows::Data::Json;
            const auto valid = glide::settings::SerializePowerToys({});
            const auto canonical = JsonObject::Parse(winrt::to_hstring(valid));
            Assert::AreEqual(L"GlideZoom", canonical.GetNamedString(L"name").c_str());
            Assert::AreEqual(L"1.0", canonical.GetNamedString(L"version").c_str());
            Assert::AreEqual(std::uint32_t{26}, canonical.GetNamedObject(L"properties").Size());
            Assert::AreEqual(2.0, canonical.GetNamedObject(L"properties").GetNamedObject(L"ZoomScope").GetNamedNumber(L"value"));
            for (const auto version : {L"1", L"1.0"})
            {
                auto root = JsonObject::Parse(winrt::to_hstring(valid));
                root.Insert(L"version", JsonValue::CreateStringValue(version));
                glide::settings::Values parsed;
                Assert::IsTrue(glide::settings::ParsePowerToys(winrt::to_string(root.Stringify()), parsed));
            }

            for (const auto version : {L"", L"0", L"2", L"01.0", L"1.0.0"})
            {
                auto root = JsonObject::Parse(winrt::to_hstring(valid));
                root.Insert(L"version", JsonValue::CreateStringValue(version));
                RejectSettings(winrt::to_string(root.Stringify()));
            }
        }

        TEST_METHOD(PowerToysSettingsRejectInvalidNumbers)
        {
            TestApartment apartment;
            using namespace winrt::Windows::Data::Json;
            const auto valid = glide::settings::SerializePowerToys({});
            const std::array<std::pair<const wchar_t*, double>, 41> invalid{{
                {L"WheelModifier", -1}, {L"WheelModifier", 4}, {L"MaximumZoom", 3},
                {L"MaximumZoom", 32}, {L"Sensitivity", -1}, {L"Sensitivity", 3},
                {L"ZoomMultiplier", 0}, {L"ZoomMultiplier", 101}, {L"ZoomMultiplier", 1.5},
                {L"EdgePanMultiplier", 0}, {L"EdgePanMultiplier", 11}, {L"ZoomMultiplier", 2147483648.0},
                {L"ZoomScope", -1}, {L"ZoomScope", 3}, {L"ZoomScope", 0.5},
                {L"ScopeIndicatorMode", -1}, {L"ScopeIndicatorMode", 4},
                {L"ScopeIndicatorStyle", -1}, {L"ScopeIndicatorStyle", 3},
                {L"ScopeIndicatorSize", 9}, {L"ScopeIndicatorSize", 36},
                {L"ScopeIndicatorOpacity", 9}, {L"ScopeIndicatorOpacity", 91},
                {L"ScopeIndicatorBorder", 0}, {L"ScopeIndicatorBorder", 9},
                {L"ScopeIndicatorDuration", 499}, {L"ScopeIndicatorDuration", 5001},
                {L"WheelAccelerationLimit", 0}, {L"WheelAccelerationLimit", 5},
                {L"ScopeIndicatorX", -1}, {L"ScopeIndicatorX", 101},
                {L"ScopeIndicatorY", -1}, {L"ScopeIndicatorY", 101},
                {L"ScopeIndicatorFade", -1}, {L"ScopeIndicatorFade", 401},
                {L"PointerSpeed", 24}, {L"PointerSpeed", 301}, {L"PointerSpeed", 100.5},
                {L"OverviewOpacity", -1}, {L"OverviewOpacity", 101}, {L"OverviewOpacity", 55.5}}};
            for (const auto& [name, number] : invalid)
            {
                auto root = JsonObject::Parse(winrt::to_hstring(valid));
                root.GetNamedObject(L"properties").GetNamedObject(name).Insert(L"value", JsonValue::CreateNumberValue(number));
                RejectSettings(winrt::to_string(root.Stringify()));
            }
            glide::settings::Values invalidValues;
            invalidValues.zoomMultiplier = 0;
            Assert::IsTrue(glide::settings::SerializePowerToys(invalidValues).empty());
            invalidValues = {};
            invalidValues.zoomScope = 3;
            Assert::IsTrue(glide::settings::SerializePowerToys(invalidValues).empty());
        }

        TEST_METHOD(PowerToysZoomScopeMigratesLegacySettingsAndRejectsWrongTypes)
        {
            TestApartment apartment;
            using namespace winrt::Windows::Data::Json;
            glide::settings::Values original;
            original.zoomScope = 1;
            original.zoomMultiplier = 37;
            original.edgePanMultiplier = 7;
            original.pointerScaling = false;
            auto root = JsonObject::Parse(winrt::to_hstring(glide::settings::SerializePowerToys(original)));
            root.GetNamedObject(L"properties").Remove(L"ZoomScope");
            for (const auto version : {L"1", L"1.0"})
            {
                root.Insert(L"version", JsonValue::CreateStringValue(version));
                glide::settings::Values migrated;
                migrated.zoomScope = 2;
                Assert::IsTrue(glide::settings::ParsePowerToys(winrt::to_string(root.Stringify()), migrated));
                original.zoomScope = 0;
                Assert::IsTrue(original == migrated, L"Legacy settings preserve other preferences and default to selected monitor");
            }

            for (const auto value : {L"true", L"null", L"\"1\"", L"[]", L"{}"})
            {
                JsonObject scope;
                scope.Insert(L"value", JsonValue::Parse(value));
                root.GetNamedObject(L"properties").Insert(L"ZoomScope", scope);
                RejectSettings(winrt::to_string(root.Stringify()));
            }
            root.GetNamedObject(L"properties").Insert(L"ZoomScope", JsonValue::CreateNullValue());
            RejectSettings(winrt::to_string(root.Stringify()));
            root.GetNamedObject(L"properties").Insert(L"ZoomScope", JsonObject{});
            RejectSettings(winrt::to_string(root.Stringify()));
        }

        TEST_METHOD(IndicatorOptionsAreOptionalForLegacyJsonAndStrictWhenPresent)
        {
            TestApartment apartment;
            using namespace winrt::Windows::Data::Json;
            const glide::settings::Values defaults;
            const auto valid = glide::settings::SerializePowerToys(defaults);
            for (const auto name : OptionalDisplaySettings)
            {
                auto root = JsonObject::Parse(winrt::to_hstring(valid));
                root.GetNamedObject(L"properties").Remove(name);
                glide::settings::Values parsed;
                Assert::IsTrue(glide::settings::ParsePowerToys(winrt::to_string(root.Stringify()), parsed));
                Assert::IsTrue(defaults == parsed, L"An omitted display option keeps its documented default");

                root = JsonObject::Parse(winrt::to_hstring(valid));
                root.GetNamedObject(L"properties").GetNamedObject(name).Insert(L"value", JsonValue::CreateStringValue(L"1"));
                RejectSettings(winrt::to_string(root.Stringify()));
                root.GetNamedObject(L"properties").Insert(name, JsonValue::CreateNullValue());
                RejectSettings(winrt::to_string(root.Stringify()));
            }
            auto legacy = JsonObject::Parse(winrt::to_hstring(valid));
            for (const auto name : OptionalDisplaySettings)
                legacy.GetNamedObject(L"properties").Remove(name);
            legacy.GetNamedObject(L"properties").Remove(L"ZoomScope");
            glide::settings::Values expected = defaults;
            expected.zoomScope = 0;
            glide::settings::Values migrated;
            Assert::IsTrue(glide::settings::ParsePowerToys(winrt::to_string(legacy.Stringify()), migrated));
            Assert::IsTrue(expected == migrated, L"Original eleven-field JSON keeps scope and new option defaults");
            for (const auto name : {L"ScopeIndicatorDetails", L"WheelAcceleration"})
            {
                auto root = JsonObject::Parse(winrt::to_hstring(valid));
                root.GetNamedObject(L"properties").GetNamedObject(name).Insert(L"value", JsonValue::CreateNumberValue(1));
                RejectSettings(winrt::to_string(root.Stringify()));
            }
        }

        TEST_METHOD(IndicatorAndAccelerationOptionsRoundTrip)
        {
            TestApartment apartment;
            for (int mode = 0; mode <= 3; ++mode)
            {
                for (int style = 0; style <= 2; ++style)
                {
                    for (unsigned flags = 0; flags < 4; ++flags)
                    {
                        glide::settings::Values before;
                        before.scopeIndicatorMode = mode;
                        before.scopeIndicatorStyle = style;
                        before.scopeIndicatorSize = (flags & 1) ? 10 : 35;
                        before.scopeIndicatorOpacity = (flags & 2) ? 10 : 90;
                        before.scopeIndicatorBorder = (flags & 1) ? 1 : 8;
                        before.scopeIndicatorDuration = (flags & 2) ? 500 : 5000;
                        before.scopeIndicatorDetails = (flags & 1) != 0;
                        before.wheelAcceleration = (flags & 2) != 0;
                        before.wheelAccelerationLimit = (flags & 1) ? 1 : 4;
                        before.scopeIndicatorX = (flags & 1) ? 0 : 100;
                        before.scopeIndicatorY = (flags & 2) ? 0 : 100;
                        before.scopeIndicatorFade = (flags & 1) ? 0 : 400;
                        before.pointerSpeed = (flags & 1) ? 25 : 300;
                        before.overviewOpacity = (flags & 2) ? 0 : 100;
                        glide::settings::Values after;
                        Assert::IsTrue(glide::settings::ParsePowerToys(glide::settings::SerializePowerToys(before), after));
                        Assert::IsTrue(before == after);
                    }
                }
            }
        }

        TEST_METHOD(PointerAndOverviewPreferencesMigrateAndRemainIndependent)
        {
            TestApartment apartment;
            using namespace winrt::Windows::Data::Json;
            for (const bool automatic : {false, true})
            {
                glide::settings::Values original;
                original.pointerScaling = automatic;
                original.scopeIndicatorOpacity = 85;
                original.overviewOpacity = 10;
                original.pointerSpeed = 250;
                auto root = JsonObject::Parse(winrt::to_hstring(glide::settings::SerializePowerToys(original)));
                root.GetNamedObject(L"properties").Remove(L"PointerSpeed");
                root.GetNamedObject(L"properties").Remove(L"OverviewOpacity");
                glide::settings::Values migrated;
                Assert::IsTrue(glide::settings::ParsePowerToys(winrt::to_string(root.Stringify()), migrated));
                original.pointerSpeed = 100;
                original.overviewOpacity = 85;
                Assert::IsTrue(original == migrated, L"Legacy settings preserve the pointer toggle and custom overview appearance");
                for (const int speed : {25, 100, 300})
                {
                    original.pointerSpeed = speed;
                    original.overviewOpacity = automatic ? 0 : 100;
                    glide::settings::Values roundTrip;
                    Assert::IsTrue(glide::settings::ParsePowerToys(glide::settings::SerializePowerToys(original), roundTrip));
                    Assert::IsTrue(original == roundTrip, L"Speed, automatic adjustment and the two opacity values persist independently");
                }
            }
        }

        TEST_METHOD(RunnerEventsSupportSettingsAndCleanExit)
        {
            const auto processId = GetCurrentProcessId();
            const auto suffix = std::to_wstring(processId);
            Event exit(L"Local\\PowerToys_GlideZoom_Exit_" + suffix);
            Event settings(L"Local\\PowerToys_GlideZoom_Settings_" + suffix);
            glide::powertoys::Runner runner;
            Assert::IsTrue(runner.Connect(processId));
            Assert::IsFalse(runner.Connect(processId), L"An active Runner connection cannot be replaced");
            Assert::IsFalse(runner.ExitRequested());
            Assert::IsFalse(runner.SettingsChanged());
            Assert::IsTrue(SetEvent(settings.handle) != FALSE);
            Assert::IsTrue(runner.SettingsChanged());
            Assert::IsFalse(runner.SettingsChanged(), L"Settings notifications must be consumed once");
            Assert::IsTrue(SetEvent(exit.handle) != FALSE);
            Assert::IsTrue(runner.ExitRequested());
        }

        TEST_METHOD(RunnerRejectsUnavailableParent)
        {
            glide::powertoys::Runner runner;
            Assert::IsFalse(runner.Connect(0));
        }
    };
}
