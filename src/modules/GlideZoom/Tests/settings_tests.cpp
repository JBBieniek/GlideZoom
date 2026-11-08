#include <stdexcept>
#include "../GlideZoom/settings.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {
using glide::settings::Parse;
using glide::settings::Serialize;
using glide::settings::Valid;
using glide::settings::Values;
unsigned assertions = 0;
unsigned multiplierInputs = 0;
unsigned edgeMultiplierInputs = 0;
unsigned pointerSpeedInputs = 0;

void Check(bool condition, std::string_view message) {
    ++assertions;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        throw std::runtime_error("GlideZoom assertion failed; see captured output");
    }
}

const Values sentinel{3, 4, 0, true, false, true, false, true, false, 37, 7, 1};
const Values fixtureValues{2, 16, 2, true, true, true, true, false, true, 1, 1, 0};
const std::string fixture =
    "[GlideZoom]\n"
    "Version=2\n"
    "WheelModifier=2\n"
    "MaximumZoom=16\n"
    "Sensitivity=2\n"
    "DoubleControl=1\n"
    "ModifiedDoubleClick=1\n"
    "ReverseDirection=1\n"
    "CenterFollow=1\n";
constexpr std::array<std::string_view, 8> fields{
    "Version", "WheelModifier", "MaximumZoom", "Sensitivity", "DoubleControl",
    "ModifiedDoubleClick", "ReverseDirection", "CenterFollow"};

void Reject(std::string_view bytes, std::string_view message) {
    Values output = sentinel;
    Check(!Parse(bytes, output), message);
    Check(output == sentinel, "rejected settings leave the caller's state unchanged");
}

void Accept(std::string_view bytes, const Values& expected, std::string_view message) {
    Values output = sentinel;
    Check(Parse(bytes, output), message);
    Check(output == expected, "accepted settings match all expected fields");
}

std::string WithValue(std::string_view name, std::string_view value, const std::string& bytes = fixture) {
    std::string result = bytes;
    const auto start = result.find(std::string(name) + '=') + name.size() + 1;
    result.replace(start, result.find('\n', start) - start, value.data(), value.size());
    return result;
}

std::string WithoutLine(std::string_view name, const std::string& bytes = fixture) {
    std::string result = bytes;
    const auto start = result.find(std::string(name) + '=');
    result.erase(start, result.find('\n', start) - start + 1);
    return result;
}

void MultiplierText() {
    const auto accept = [](std::wstring_view text, int expected) {
        ++multiplierInputs;
        int output = -717;
        Check(glide::settings::ParseZoomMultiplier(text, output), "complete bounded multiplier UI text is accepted");
        Check(output == expected, "multiplier UI text has the exact requested integer value");
    };
    const auto reject = [](std::wstring_view text) {
        ++multiplierInputs;
        int output = 37;
        Check(!glide::settings::ParseZoomMultiplier(text, output), "malformed multiplier UI text is rejected in full");
        Check(output == 37, "rejected multiplier UI text leaves the candidate unchanged");
    };
    for (int value = 1; value <= 100; ++value) {
        auto text = std::to_wstring(value);
        while (text.size() <= 3) {
            accept(text, value);
            text.insert(text.begin(), L'0');
        }
    }
    for (unsigned value = 0; value <= 999; ++value) {
        auto text = std::to_wstring(value);
        text.insert(0, 3 - text.size(), L'0');
        if (value >= 1 && value <= 100) accept(text, static_cast<int>(value));
        else reject(text);
    }
    for (const std::wstring_view text : {
            L"", L"0", L"00", L"000", L"101", L"999", L"1000", L"0001", L"0100",
            L"+1", L"-1", L" 1", L"1 ", L"\t1", L"1\n", L"1\r", L"2.5", L"1.0",
            L"10x", L"1e2", L"1 2", L"1,2", L"1;2", L"1=2", L"NaN", L"inf",
            L"\u0661", L"\uff11", L"\u00b9", L"\u00a01", L"1\u200b", L"\U0001f600",
            L"18446744073709551616"}) reject(text);
    for (std::size_t position = 0; position <= 2; ++position) {
        std::wstring text = L"10";
        text.insert(position, 1, L'\0');
        reject(std::wstring_view{text.data(), text.size()});
    }
    // The native UI reads into a five-wchar buffer. Four returned characters
    // are deliberately invalid, so a longer pasted value cannot become a
    // valid-looking three-digit prefix before this helper sees it.
    for (int value = 1; value <= 100; ++value) {
        auto overlong = std::to_wstring(value);
        overlong.insert(0, 3 - overlong.size(), L'0');
        overlong += L"000 trailing text";
        reject(overlong);
        reject(std::wstring_view{overlong}.substr(0, 4));
    }
    reject(std::wstring(1000000, L'9'));
    // Exhaust all UTF-16 code units as one-character pasted input. Only the
    // ASCII nonzero digits represent a complete valid single-digit value.
    for (unsigned unit = 0; unit <= 0xffff; ++unit) {
        const wchar_t character = static_cast<wchar_t>(unit);
        const std::wstring_view text{&character, 1};
        if (unit >= '1' && unit <= '9') accept(text, static_cast<int>(unit - '0'));
        else reject(text);
    }
}

void EdgeMultiplierText() {
    const auto accept = [](std::wstring_view text, int expected) {
        ++edgeMultiplierInputs;
        int output = -717;
        Check(glide::settings::ParseEdgePanMultiplier(text, output),
              "complete bounded edge multiplier UI text is accepted");
        Check(output == expected, "edge multiplier UI text has the exact requested integer value");
    };
    const auto reject = [](std::wstring_view text) {
        ++edgeMultiplierInputs;
        int output = 7;
        Check(!glide::settings::ParseEdgePanMultiplier(text, output),
              "malformed edge multiplier UI text is rejected in full");
        Check(output == 7, "rejected edge multiplier UI text leaves the candidate unchanged");
    };
    for (int value = 1; value <= 10; ++value) {
        auto text = std::to_wstring(value);
        while (text.size() <= 2) {
            accept(text, value);
            text.insert(text.begin(), L'0');
        }
    }
    for (unsigned value = 0; value <= 99; ++value) {
        auto text = std::to_wstring(value);
        text.insert(0, 2 - text.size(), L'0');
        if (value >= 1 && value <= 10) accept(text, static_cast<int>(value));
        else reject(text);
    }
    for (const std::wstring_view text : {
            L"", L"0", L"00", L"000", L"11", L"99", L"100", L"001", L"010",
            L"+1", L"-1", L" 1", L"1 ", L"\t1", L"1\n", L"1\r", L"2.5", L"1.0",
            L"10x", L"1e1", L"1 2", L"1,2", L"1;2", L"1=2", L"NaN", L"inf",
            L"\u0661", L"\uff11", L"\u00b9", L"\u00a01", L"1\u200b", L"\U0001f600",
            L"18446744073709551616"}) reject(text);
    for (std::size_t position = 0; position <= 2; ++position) {
        std::wstring text = L"10";
        text.insert(position, 1, L'\0');
        reject(std::wstring_view{text.data(), text.size()});
    }
    // A four-wchar native read buffer returns three characters on overflow.
    // Both the complete paste and the returned prefix must remain invalid.
    for (int value = 1; value <= 10; ++value) {
        auto overlong = std::to_wstring(value);
        overlong.insert(0, 2 - overlong.size(), L'0');
        overlong += L"000 trailing text";
        reject(overlong);
        reject(std::wstring_view{overlong}.substr(0, 3));
    }
    reject(std::wstring(1000000, L'9'));
    for (unsigned unit = 0; unit <= 0xffff; ++unit) {
        const wchar_t character = static_cast<wchar_t>(unit);
        const std::wstring_view text{&character, 1};
        if (unit >= '1' && unit <= '9') accept(text, static_cast<int>(unit - '0'));
        else reject(text);
    }
}
void PointerSpeedText() {
    const auto reject = [](std::wstring_view text) {
        ++pointerSpeedInputs;
        int output = 137;
        Check(!glide::settings::ParsePointerSpeed(text, output) && output == 137,
              "malformed pointer speed text rejects the complete value atomically");
    };
    for (int value = 25; value <= 300; ++value) {
        auto text = std::to_wstring(value);
        while (text.size() <= 3) {
            ++pointerSpeedInputs;
            int output = -1;
            Check(glide::settings::ParsePointerSpeed(text, output) && output == value,
                  "every pointer speed percentage and bounded leading-zero spelling is accepted");
            text.insert(text.begin(), L'0');
        }
        reject(text + L"000 trailing text");
        reject(std::wstring_view{text}.substr(0, 4));
    }
    for (const std::wstring_view text : {L"", L"0", L"24", L"301", L"999", L"1000", L"0100", L"+25", L"-25",
            L" 100", L"100 ", L"100\n", L"25.0", L"1e2", L"25%", L"NaN", L"inf", L"\uff11\uff10\uff10"}) reject(text);
    for (std::size_t index = 0; index <= 3; ++index) {
        std::wstring text = L"100";
        text.insert(index, 1, L'\0');
        reject(std::wstring_view{text.data(), text.size()});
    }
    for (unsigned unit = 0; unit <= 0xffff; ++unit) {
        const wchar_t character = static_cast<wchar_t>(unit);
        reject(std::wstring_view{&character, 1});
    }
}
} // namespace

int Run_settings() {
    MultiplierText();
    EdgeMultiplierText();
    PointerSpeedText();
    const Values defaults;
    Check(Valid(defaults), "defaults are valid");
    Check(!defaults.keyboardGesture && !defaults.mouseGesture && !defaults.touchpadGesture,
          "double-tap hooks require explicit opt-in");
    Check(!defaults.reverse && !defaults.centerFollow, "default direction and follow modes are conventional");
    Check(defaults.pointerScaling, "ordinary pointer speed follows zoom by default");
    Check(defaults.pointerSpeed == 100 && defaults.overviewOpacity == 55,
          "new pointer speed and separate overview opacity preserve the default feel");
    Check(defaults.zoomMultiplier == 1, "default key/wheel multiplier preserves the 0.01x step");
    Check(defaults.edgePanMultiplier == 1, "default edge multiplier preserves previous edge following");
    Check(defaults.zoomScope == 2, "new configurations default to independent monitors");
    Check(defaults.scopeIndicatorMode == 0 && defaults.scopeIndicatorStyle == 0 &&
          defaults.scopeIndicatorSize == 20 && defaults.scopeIndicatorOpacity == 55 &&
          defaults.scopeIndicatorBorder == 2 && defaults.scopeIndicatorDuration == 1500 &&
          defaults.scopeIndicatorDetails && !defaults.wheelAcceleration && defaults.wheelAccelerationLimit == 3 &&
          defaults.scopeIndicatorX == 100 && defaults.scopeIndicatorY == 100 && defaults.scopeIndicatorFade == 120,
          "new indicator and wheel options have conservative defaults");
    Check(defaults.modifier == 0 && defaults.maxZoom == 8 && defaults.sensitivity == 1,
          "default wheel modifier, limit and sensitivity remain stable");
    Accept(fixture, fixtureValues, "independent complete settings fixture");

    // Cross every existing option and boolean combination with each offered
    // multiplier preset. The full integer multiplier range is covered below.
    unsigned combinations = 0;
    for (int modifier = 0; modifier != 4; ++modifier) {
        for (int maximum : {2, 4, 8, 16}) {
            for (int sensitivity = 0; sensitivity != 3; ++sensitivity) {
                for (unsigned flags = 0; flags != 64; ++flags) {
                    for (int multiplier : {1, 2, 5, 10, 20, 50, 100}) {
                        const Values value{modifier, maximum, sensitivity,
                            (flags & 1u) != 0, (flags & 2u) != 0,
                            (flags & 4u) != 0, (flags & 8u) != 0, (flags & 16u) != 0,
                            (flags & 32u) != 0, multiplier};
                        Check(Valid(value), "supported configuration is valid");
                        const auto serialized = Serialize(value);
                        Check(!serialized.empty(), "valid state serializes");
                        Accept(serialized, value, "every preset configuration round trips");
                        Check(std::isfinite(value.WheelFactor()) && value.WheelFactor() > 1.0 &&
                              std::isfinite(value.DragDistance()) && value.DragDistance() > 0.0,
                              "valid settings produce finite positive input scales");
                        ++combinations;
                    }
                }
            }
        }
    }
    Check(combinations == 21504, "all base options crossed with seven multiplier presets were covered");
    auto touchpadValues = fixtureValues;
    touchpadValues.touchpadGesture = true;
    const auto v3 = WithValue("Version", "3") + "ThreeFingerTouchpad=1\n";
    Accept(v3, touchpadValues, "complete v3 enables exact three-finger gesture");
    Reject(WithValue("Version", "3"), "v3 cannot omit the explicit touchpad choice");
    Reject(fixture + "ThreeFingerTouchpad=1\n", "v2 cannot silently accept new gesture ownership");
    Reject(WithoutLine("Version") + "ThreeFingerTouchpad=1\n", "versionless config cannot enable new gesture");
    for (std::string_view value : {"-1", "2", "true", "", "99999999999999999999999"})
        Reject(WithValue("Version", "3") + "ThreeFingerTouchpad=" + std::string(value) + "\n",
               "three-finger setting is a strict complete boolean");
    Reject(v3 + "ThreeFingerTouchpad=0\n", "duplicate touchpad ownership rejected");
    auto pointerValues = touchpadValues;
    pointerValues.pointerScaling = true;
    const auto v4 = WithValue("Version", "4") + "ThreeFingerTouchpad=1\nExtraPointerPrecision=1\n";
    Accept(v4, pointerValues, "complete v4 migrates to normal zoom-scaled movement");
    Accept(WithValue("Version", "4") + "ThreeFingerTouchpad=1\nExtraPointerPrecision=0\n",
           pointerValues, "v4 experimental-off default also migrates to fixed pointer speed");
    Reject(WithValue("Version", "4") + "ThreeFingerTouchpad=1\n", "v4 requires an explicit pointer choice");
    Reject(v3 + "ExtraPointerPrecision=1\n", "legacy v3 cannot silently enable pointer changes");
    Reject(fixture + "ExtraPointerPrecision=1\n", "legacy v2 cannot enable pointer changes");
    Reject(v4 + "ExtraPointerPrecision=0\n", "duplicate pointer ownership rejected");
    for (std::string_view value : {"-1", "2", "true", "", "99999999999999999999999"})
        Reject(WithValue("Version", "4") + "ThreeFingerTouchpad=1\nExtraPointerPrecision=" +
                   std::string(value) + "\n", "pointer precision is a strict complete boolean");

    const auto v5 = WithValue("Version", "5") + "ThreeFingerTouchpad=1\nZoomScaledPointer=1\n";
    Accept(v5, pointerValues, "v5 enables ordinary zoom-normalized pointer speed");
    auto optedOut = pointerValues;
    optedOut.pointerScaling = false;
    Accept(WithValue("Version", "5") + "ThreeFingerTouchpad=1\nZoomScaledPointer=0\n",
           optedOut, "v5 explicit opt-out survives reload");
    Accept(Serialize(optedOut), optedOut, "saved opt-out never migrates back on");
    Reject(WithValue("Version", "5") + "ThreeFingerTouchpad=1\n", "v5 requires complete pointer choice");
    Reject(v5 + "ExtraPointerPrecision=1\n", "v5 cannot mix old and new pointer semantics");
    Reject(v5 + "ZoomScaledPointer=0\n", "duplicate new pointer setting rejected");
    Reject(v4 + "ZoomScaledPointer=1\n", "v4 cannot mix old and new pointer semantics");
    for (std::string_view value : {"-1", "2", "true", "", "99999999999999999999999"})
        Reject(WithValue("Version", "5") + "ThreeFingerTouchpad=1\nZoomScaledPointer=" +
                   std::string(value) + "\n", "zoom-scaled pointer is a strict complete boolean");

    const auto v6Prefix = WithValue("Version", "6") + "ThreeFingerTouchpad=1\nZoomScaledPointer=1\n";
    const auto v6 = v6Prefix + "ZoomMultiplier=1\n";
    Accept(v6, pointerValues, "complete v6 preserves all old settings and the unit multiplier");
    Reject(v6Prefix, "v6 requires an explicit multiplier");
    Reject(WithValue("Version", "6") + "ZoomScaledPointer=1\nZoomMultiplier=1\n",
           "v6 requires the explicit touchpad choice");
    Reject(WithValue("Version", "6") + "ThreeFingerTouchpad=1\nZoomMultiplier=1\n",
           "v6 requires the explicit pointer choice");
    Reject(v6 + "ExtraPointerPrecision=1\n", "v6 cannot revive old pointer semantics");
    Reject(v6 + "ZoomMultiplier=1\n", "duplicate multiplier cannot override a complete candidate");
    for (const auto& legacy : {WithoutLine("Version"), fixture, v3, v4, v5}) {
        Reject(legacy + "ZoomMultiplier=1\n", "older schema cannot silently acquire a multiplier field");
        Values migrated = sentinel;
        Check(Parse(legacy, migrated) && migrated.zoomMultiplier == 1,
              "every older accepted schema migrates to a unit multiplier");
        Accept(Serialize(migrated), migrated, "legacy migration remains stable after a current-schema save");
    }
    for (int multiplier = 1; multiplier <= 100; ++multiplier) {
        auto value = pointerValues;
        value.zoomMultiplier = multiplier;
        Accept(v6Prefix + "ZoomMultiplier=" + std::to_string(multiplier) + "\n", value,
               "every bounded integer multiplier parses exactly");
        Accept(Serialize(value), value, "every bounded integer multiplier survives a save");
        value.pointerScaling = false;
        Accept(Serialize(value), value, "new multiplier never erases an explicit pointer opt-out");
        Check(value.DragDistance() == pointerValues.DragDistance(),
              "key/wheel multiplier does not alter gesture sensitivity");
    }
    auto multiplierOnly = defaults;
    multiplierOnly.zoomMultiplier = 2;
    Check(!(multiplierOnly == defaults), "configuration equality observes a multiplier-only change");
    for (std::string_view value : {"-1", "0", "101", "1000", "", "+1", "1.0", "0x1", "NaN",
                                  "inf", "true", "1x", "1 2", "1=2", "1;comment", "2147483648",
                                  "-2147483649", "18446744073709551616"})
        Reject(v6Prefix + "ZoomMultiplier=" + std::string(value) + "\n",
               "multiplier is a strict bounded complete integer");
    for (std::string_view value : {"-1", "2", "true", "", "99999999999999999999999"}) {
        Reject(WithValue("Version", "6") + "ThreeFingerTouchpad=1\nZoomScaledPointer=" +
                   std::string(value) + "\nZoomMultiplier=10\n",
               "v6 retains strict pointer-boolean validation");
        Reject(WithValue("Version", "6") + "ThreeFingerTouchpad=" + std::string(value) +
                   "\nZoomScaledPointer=1\nZoomMultiplier=10\n",
               "v6 retains strict touchpad-boolean validation");
    }

    const auto v7Prefix = WithValue("Version", "7") +
        "ThreeFingerTouchpad=1\nZoomScaledPointer=1\nZoomMultiplier=1\n";
    const auto v7 = v7Prefix + "EdgePanMultiplier=1\n";
    Accept(v7, pointerValues, "complete v7 preserves all old settings and the unit edge multiplier");
    Reject(v7Prefix, "v7 requires an explicit edge multiplier");
    for (std::string_view field : {"WheelModifier", "MaximumZoom", "Sensitivity", "DoubleControl",
                                  "ModifiedDoubleClick", "ReverseDirection", "CenterFollow",
                                  "ThreeFingerTouchpad", "ZoomScaledPointer", "ZoomMultiplier"}) {
        Reject(WithoutLine(field, v7), "v7 still requires every earlier setting");
        Reject(v7 + std::string(field) + "=1\n", "v7 cannot partially override a duplicate field");
    }
    Reject(v7 + "ExtraPointerPrecision=1\n", "v7 cannot revive obsolete pointer semantics");
    Reject(v7 + "EdgePanMultiplier=10\n", "a duplicate edge multiplier never overrides a candidate");
    Reject(WithoutLine("Version", v7), "new settings cannot masquerade as a versionless file");
    Reject(v7 + "Version=6\n", "a duplicate schema version cannot downgrade new settings");
    for (const auto& legacy : {WithoutLine("Version"), fixture, v3, v4, v5, v6}) {
        Reject(legacy + "EdgePanMultiplier=1\n", "older schema cannot silently acquire edge acceleration");
        Values migrated = sentinel;
        Check(Parse(legacy, migrated) && migrated.edgePanMultiplier == 1,
              "every older accepted schema keeps previous edge behavior");
        Accept(Serialize(migrated), migrated, "edge migration remains stable on subsequent saves");
    }
    // Preserve a customized v6 key/wheel multiplier and explicit pointer opt-out,
    // while adding only the new default. This is the existing-user upgrade path.
    for (int multiplier = 1; multiplier <= 100; ++multiplier) {
        auto expected = optedOut;
        expected.zoomMultiplier = multiplier;
        const auto old = WithValue("Version", "6") +
            "ThreeFingerTouchpad=1\nZoomScaledPointer=0\nZoomMultiplier=" +
            std::to_string(multiplier) + "\n";
        Accept(old, expected, "v6 migration preserves every key/wheel multiplier and pointer opt-out");
    }
    unsigned multiplierPairs = 0;
    for (int edge = 1; edge <= 10; ++edge) {
        auto edgeValue = pointerValues;
        edgeValue.edgePanMultiplier = edge;
        Accept(v7Prefix + "EdgePanMultiplier=" + std::to_string(edge) + "\n", edgeValue,
               "every bounded integer edge multiplier parses exactly");
        for (int multiplier = 1; multiplier <= 100; ++multiplier) {
            auto value = edgeValue;
            value.zoomMultiplier = multiplier;
            Check(Valid(value), "every pair of supported multipliers is valid");
            Accept(Serialize(value), value, "all key/wheel and edge multiplier pairs round trip independently");
            value.pointerScaling = false;
            Accept(Serialize(value), value, "accelerated edges never erase an explicit pointer opt-out");
            Check(value.DragDistance() == pointerValues.DragDistance() &&
                  value.WheelFactor() == pointerValues.WheelFactor(),
                  "edge acceleration never changes gesture sensitivity or wheel scale");
            ++multiplierPairs;
        }
        // Cross each edge gain with all gesture/direction/follow/pointer flags.
        // Modifier, zoom ceiling and sensitivity each traverse their full ranges
        // within every gain; the original 21,504 complete base combinations above
        // remain unchanged as a regression check for the default gain.
        for (unsigned flags = 0; flags < 64; ++flags) {
            const Values value{static_cast<int>(flags % 4),
                std::array<int, 4>{2, 4, 8, 16}[(flags / 4) % 4],
                static_cast<int>(flags % 3),
                (flags & 1u) != 0, (flags & 2u) != 0,
                (flags & 4u) != 0, (flags & 8u) != 0,
                (flags & 16u) != 0, (flags & 32u) != 0,
                1 + static_cast<int>(flags), edge};
            Check(Valid(value), "edge acceleration coexists with every flag combination");
            Accept(Serialize(value), value, "all flags and edge gains survive one complete save");
        }
    }
    Check(multiplierPairs == 1000, "all 100 by 10 multiplier pairs were covered");
    auto edgeOnly = defaults;
    edgeOnly.edgePanMultiplier = 2;
    Check(!(edgeOnly == defaults), "configuration equality observes an edge-only change");
    Check(Serialize(defaults).find("Version=11\r\n") != std::string::npos,
          "new saves declare the required schema version");
    for (std::string_view value : {"-1", "0", "11", "100", "", "+1", "1.0", "0x1", "NaN",
                                  "inf", "true", "1x", "1 2", "1=2", "1;comment", "2147483648",
                                  "-2147483649", "18446744073709551616"})
        Reject(v7Prefix + "EdgePanMultiplier=" + std::string(value) + "\n",
               "edge multiplier is a strict bounded complete integer");

    const auto v8Prefix = WithValue("Version", "8") +
        "ThreeFingerTouchpad=1\nZoomScaledPointer=1\nZoomMultiplier=1\nEdgePanMultiplier=1\n";
    const auto v8 = v8Prefix + "ZoomScope=1\n";
    for (const int scope : {0, 1}) {
        auto expected = pointerValues;
        expected.zoomScope = scope;
        Accept(v8Prefix + "ZoomScope=" + std::to_string(scope) + "\n", expected,
               "v8 persists the explicit selected-monitor or all-monitors scope");
        Accept(Serialize(expected), expected, "both zoom scopes survive a complete settings save");
    }
    Reject(v8Prefix, "v8 requires an explicit zoom scope");
    Reject(v8 + "ZoomScope=0\n", "duplicate zoom scope cannot override the first choice");
    for (std::string_view field : {"WheelModifier", "MaximumZoom", "Sensitivity", "DoubleControl",
                                  "ModifiedDoubleClick", "ReverseDirection", "CenterFollow",
                                  "ThreeFingerTouchpad", "ZoomScaledPointer", "ZoomMultiplier", "EdgePanMultiplier"})
        Reject(WithoutLine(field, v8), "v8 still requires every earlier setting");
    for (const auto& legacy : {WithoutLine("Version"), fixture, v3, v4, v5, v6, v7}) {
        Values migrated = sentinel;
        Check(Parse(legacy, migrated) && migrated.zoomScope == 0,
              "legacy settings default to selected-monitor scope");
        Accept(Serialize(migrated), migrated, "scope migration preserves all existing preferences");
        Reject(legacy + "ZoomScope=1\n", "older schema cannot silently acquire a new scope");
    }
    for (std::string_view scope : {"-1", "2", "99", "", "+1", "1.0", "0x1", "NaN", "true",
                                  "1x", "1 0", "2147483648", "-2147483649"})
        Reject(v8Prefix + "ZoomScope=" + std::string(scope) + "\n",
               "zoom scope is a strict bounded complete integer");
    auto allMonitors = defaults;
    allMonitors.zoomScope = 1;
    Check(!(allMonitors == defaults), "configuration equality observes a scope-only change");

    const auto v9Prefix = WithValue("Version", "9") +
        "ThreeFingerTouchpad=1\nZoomScaledPointer=1\nZoomMultiplier=1\nEdgePanMultiplier=1\n";
    const auto v9 = v9Prefix + "ZoomScope=2\n";
    for (const int scope : {0, 1, 2}) {
        auto expected = pointerValues;
        expected.zoomScope = scope;
        Accept(v9Prefix + "ZoomScope=" + std::to_string(scope) + "\n", expected,
               "v9 distinguishes selected, linked, and independent monitors");
        Accept(Serialize(expected), expected, "all three scopes survive a current-schema save");
    }
    Reject(v9Prefix, "v9 requires an explicit scope");
    Reject(v9 + "ZoomScope=0\n", "v9 rejects duplicate scope fields atomically");
    for (std::string_view field : {"WheelModifier", "MaximumZoom", "Sensitivity", "DoubleControl",
                                  "ModifiedDoubleClick", "ReverseDirection", "CenterFollow",
                                  "ThreeFingerTouchpad", "ZoomScaledPointer", "ZoomMultiplier", "EdgePanMultiplier"})
        Reject(WithoutLine(field, v9), "v9 still requires every earlier setting");
    for (std::string_view scope : {"-1", "3", "99", "", "+2", "2.0", "0x2", "NaN", "true"})
        Reject(v9Prefix + "ZoomScope=" + std::string(scope) + "\n",
               "v9 scope is a strict bounded complete integer");

    auto indicatorSettings = pointerValues;
    indicatorSettings.zoomScope = 2;
    indicatorSettings.scopeIndicatorMode = 1;
    indicatorSettings.scopeIndicatorStyle = 1;
    indicatorSettings.scopeIndicatorSize = 35;
    indicatorSettings.scopeIndicatorOpacity = 90;
    indicatorSettings.scopeIndicatorBorder = 8;
    indicatorSettings.scopeIndicatorDuration = 5000;
    indicatorSettings.scopeIndicatorDetails = false;
    indicatorSettings.wheelAcceleration = true;
    indicatorSettings.wheelAccelerationLimit = 4;
    indicatorSettings.scopeIndicatorX = 0;
    indicatorSettings.scopeIndicatorY = 50;
    indicatorSettings.scopeIndicatorFade = 0;
    std::string v11;
    for (const char character : Serialize(indicatorSettings))
        if (character != '\r') v11 += character;
    const auto v10 = WithValue("Version", "10", WithoutLine("OverviewOpacity", WithoutLine("PointerSpeed", v11)));
    auto legacyIndicatorSettings = indicatorSettings;
    legacyIndicatorSettings.overviewOpacity = legacyIndicatorSettings.scopeIndicatorOpacity;
    Accept(v10, legacyIndicatorSettings, "v10 inherits its old custom overview opacity and normal pointer speed");
    Accept(v11, indicatorSettings, "v11 persists separate pointer speed, preview opacity and earlier preferences");
    for (int style = 0; style <= 2; ++style) {
        auto expected = indicatorSettings;
        expected.scopeIndicatorStyle = style;
        Accept(WithValue("ScopeIndicatorStyle", std::to_string(style), v11), expected,
               "all indicator styles preserve visibility, preview position and unrelated preferences");
        Accept(Serialize(expected), expected, "saved combined and legacy indicator styles round trip");
    }
    for (std::string_view field : {"ScopeIndicatorMode", "ScopeIndicatorStyle", "ScopeIndicatorSize",
                                  "ScopeIndicatorOpacity", "ScopeIndicatorBorder", "ScopeIndicatorDuration",
                                  "ScopeIndicatorDetails", "WheelAcceleration", "WheelAccelerationLimit",
                                  "ScopeIndicatorX", "ScopeIndicatorY", "ScopeIndicatorFade", "PointerSpeed", "OverviewOpacity"}) {
        Reject(WithoutLine(field, v11), "v11 requires every option in a complete save");
        Reject(v11 + std::string(field) + "=0\n", "v11 does not accept duplicate option overrides");
        Reject(v9 + std::string(field) + "=0\n", "earlier INI schemas cannot partially acquire new options");
    }
    for (const auto& legacy : {fixture, v3, v4, v5, v6, v7, v8, v9}) {
        Values migrated;
        Check(Parse(legacy, migrated) && migrated.scopeIndicatorMode == 0 &&
              migrated.scopeIndicatorStyle == 0 && migrated.scopeIndicatorSize == 20 &&
              migrated.scopeIndicatorOpacity == 55 && migrated.scopeIndicatorBorder == 2 &&
              migrated.scopeIndicatorDuration == 1500 && migrated.scopeIndicatorDetails &&
              !migrated.wheelAcceleration && migrated.wheelAccelerationLimit == 3 &&
              migrated.scopeIndicatorX == 100 && migrated.scopeIndicatorY == 100 && migrated.scopeIndicatorFade == 120,
              "legacy INI files keep conservative indicator and acceleration defaults");
        Accept(Serialize(migrated), migrated, "saving migrated options preserves all previous preferences");
    }
    for (const auto& legacy : {fixture, v3, v4, v5, v6, v7, v8, v9, v10}) {
        Values migrated;
        Check(Parse(legacy, migrated) && migrated.pointerSpeed == 100 && migrated.overviewOpacity == migrated.scopeIndicatorOpacity,
              "legacy schemas retain normal pointer speed and their previous overview opacity");
    }
    Reject(v10 + "PointerSpeed=100\n", "v10 cannot partially acquire the v11 pointer setting");
    Reject(v10 + "OverviewOpacity=55\n", "v10 cannot partially acquire the v11 overview setting");
    struct OptionRange { std::string_view name; int Values::*member; int minimum; int maximum; };
    const OptionRange indicatorRanges[]{
        {"ScopeIndicatorMode", &Values::scopeIndicatorMode, 0, 3},
        {"ScopeIndicatorStyle", &Values::scopeIndicatorStyle, 0, 2},
        {"ScopeIndicatorSize", &Values::scopeIndicatorSize, 10, 35},
        {"ScopeIndicatorOpacity", &Values::scopeIndicatorOpacity, 10, 90},
        {"ScopeIndicatorBorder", &Values::scopeIndicatorBorder, 1, 8},
        {"ScopeIndicatorDuration", &Values::scopeIndicatorDuration, 500, 5000},
        {"WheelAccelerationLimit", &Values::wheelAccelerationLimit, 1, 4},
        {"ScopeIndicatorX", &Values::scopeIndicatorX, 0, 100},
        {"ScopeIndicatorY", &Values::scopeIndicatorY, 0, 100},
        {"ScopeIndicatorFade", &Values::scopeIndicatorFade, 0, 400},
        {"PointerSpeed", &Values::pointerSpeed, 25, 300},
        {"OverviewOpacity", &Values::overviewOpacity, 0, 100}};
    for (const auto& range : indicatorRanges) {
        for (const int bound : {range.minimum, range.maximum}) {
            auto expected = indicatorSettings;
            expected.*(range.member) = bound;
            Accept(WithValue(range.name, std::to_string(bound), v11), expected,
                   "indicator numeric bounds are valid and preserve unrelated options");
        }
        for (const int bad : {range.minimum - 1, range.maximum + 1,
                              std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
            Reject(WithValue(range.name, std::to_string(bad), v11), "numeric settings ranges reject invalid input atomically");
            auto invalid = indicatorSettings;
            invalid.*(range.member) = bad;
            Check(!Valid(invalid) && Serialize(invalid).empty(), "invalid indicator options cannot be saved");
        }
    }
    for (std::string_view field : {"ScopeIndicatorDetails", "WheelAcceleration"})
        for (std::string_view value : {"-1", "2", "true", "", "1.0"})
            Reject(WithValue(field, value, v11), "indicator and acceleration flags are strict booleans");

    // Serializer rejects invalid state rather than persisting an inconsistent
    // partial configuration that the next launch would interpret differently.
    for (int bad : {-1, 4, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        Values value = defaults;
        value.modifier = bad;
        Check(!Valid(value) && Serialize(value).empty(), "invalid modifiers cannot be saved");
    }
    for (int bad : {-1, 3, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        Values value = defaults;
        value.sensitivity = bad;
        Check(!Valid(value) && Serialize(value).empty(), "invalid sensitivities cannot be saved");
    }
    for (int bad : {-1, 0, 1, 3, 5, 32, std::numeric_limits<int>::max()}) {
        Values value = defaults;
        value.maxZoom = bad;
        Check(!Valid(value) && Serialize(value).empty(), "unsupported zoom limits cannot be saved");
    }
    for (int bad : {-1, 0, 101, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        Values value = defaults;
        value.zoomMultiplier = bad;
        Check(!Valid(value) && Serialize(value).empty(), "out-of-range multipliers cannot be saved");
    }
    for (int bad : {-1, 0, 11, 100, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        Values value = defaults;
        value.edgePanMultiplier = bad;
        Check(!Valid(value) && Serialize(value).empty(), "out-of-range edge multipliers cannot be saved");
    }
    for (int bad : {-1, 3, 100, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        Values value = defaults;
        value.zoomScope = bad;
        Check(!Valid(value) && Serialize(value).empty(), "unsupported zoom scopes cannot be saved");
    }

    Accept(WithoutLine("Version"), fixtureValues, "complete versionless v0.1 files remain readable");
    Accept("\xef\xbb\xbf" + fixture, fixtureValues, "UTF-8 BOM is accepted at the beginning");
    Accept("; prior comment\n# another comment\n\n" + fixture + "\t# trailing comment\n",
           fixtureValues, "whole-line comments and blank lines are harmless");
    std::string windowsLines;
    for (char c : fixture) { if (c == '\n') windowsLines += '\r'; windowsLines += c; }
    Accept(windowsLines, fixtureValues, "CRLF files parse correctly");
    Accept(fixture.substr(0, fixture.size() - 1), fixtureValues, "a final newline is optional");
    Accept(" \t[GlideZoom] \r\n Version = 2 \r\n WheelModifier = 2 \r\n"
           " MaximumZoom = 16 \r\n Sensitivity = 2 \r\n DoubleControl = 1 \r\n"
           " ModifiedDoubleClick = 1 \r\n ReverseDirection = 1 \r\n CenterFollow = 1 \r\n",
           fixtureValues, "whitespace around section, keys and values is accepted");
    Accept("[GlideZoom]\nCenterFollow=1\nReverseDirection=1\nModifiedDoubleClick=1\n"
           "DoubleControl=1\nSensitivity=2\nMaximumZoom=16\nWheelModifier=2\nVersion=2\n",
           fixtureValues, "field order has no effect on the complete candidate");

    for (std::string_view bad : {"", "\n\t\r", "# only comments\n", "[GlideZoom]\n"})
        Reject(bad, "empty and incomplete files are rejected");
    for (auto field : fields) {
        if (field != "Version") Reject(WithoutLine(field), "every required field must be present");
        Reject(fixture + std::string(field) + "=1\n", "duplicate fields are rejected");
        for (std::string_view bad : {"", "+1", "1.0", "0x1", "NaN", "inf", "true", "1x",
                                    "1 2", "1=2", "1;comment", "2147483648", "-2147483649",
                                    "18446744073709551616"}) {
            Reject(WithValue(field, bad), "malformed and overflowing integers are rejected");
        }
    }
    for (std::string_view bad : {"-1", "4", "99"})
        Reject(WithValue("WheelModifier", bad), "out-of-range modifiers are rejected");
    for (std::string_view bad : {"-1", "0", "1", "3", "5", "32"})
        Reject(WithValue("MaximumZoom", bad), "unsupported zoom limits are rejected");
    for (std::string_view bad : {"-1", "3", "99"})
        Reject(WithValue("Sensitivity", bad), "out-of-range sensitivities are rejected");
    for (auto field : {"DoubleControl", "ModifiedDoubleClick", "ReverseDirection", "CenterFollow"}) {
        for (std::string_view bad : {"-1", "2", "99"})
            Reject(WithValue(field, bad), "booleans must be zero or one");
    }
    for (std::string_view bad : {"-1", "0", "1", "3", "99"})
        Reject(WithValue("Version", bad), "unsupported explicit schema versions are rejected");

    Reject("WheelModifier=2\n" + fixture, "fields cannot precede the section");
    Reject(fixture + "UnknownOption=1\n", "unknown fields do not silently change meaning");
    Reject(fixture + "[GlideZoom]\n", "duplicate sections are rejected");
    Reject(fixture + "[Other]\nValue=1\n", "unrecognized sections are rejected");
    Reject(fixture + "unstructured trailing data\n", "trailing garbage invalidates the whole file");
    Reject(fixture + "version=2\n", "differently cased field names are not silently accepted");
    Reject("[glidezoom]\n" + fixture.substr(fixture.find('\n') + 1), "section name is exact");
    Reject("\xef\xbb\xbf\xef\xbb\xbf" + fixture, "repeated BOMs are rejected");
    Reject(fixture + "\xef\xbb\xbf", "BOM bytes inside the file are rejected");
    Reject(WithValue("WheelModifier", "\xc2\xa0" "2"), "non-ASCII whitespace cannot obscure a value");

    // Every truncation that removes part of the final required value must fail.
    // Removing only the optional final newline remains a complete, valid file.
    for (const auto& bytes : {fixture, v3, v4, v5, v6, v7, v8, v9, v10, v11}) {
        for (std::size_t length = 0; length + 1 < bytes.size(); ++length)
            Reject(std::string_view(bytes).substr(0, length), "truncated configuration never partially commits");
        for (std::size_t position = 0; position <= bytes.size(); ++position) {
            std::string corrupted = bytes;
            corrupted.insert(position, 1, '\0');
            Reject(corrupted, "NUL at any byte position rejects the entire file");
        }
    }
    std::string maximum = fixture + '#' + std::string(16384 - fixture.size() - 1, 'x');
    Accept(maximum, fixtureValues, "a complete file at the size limit remains readable");
    Reject(maximum + 'x', "oversized files are rejected before parsing");

    // Deterministic corruption sweep: each one-byte substitution either yields a
    // wholly valid bounded state or preserves the previous state. This exercises
    // input bytes the menu and serializer would never generate.
    for (const auto& bytes : {fixture, v3, v4, v5, v6, v7, v8, v9, v10, v11}) {
        for (std::size_t position = 0; position < bytes.size(); ++position) {
            for (unsigned replacement = 0; replacement < 256; ++replacement) {
                std::string corrupted = bytes;
                corrupted[position] = static_cast<char>(replacement);
                Values output = sentinel;
                if (Parse(corrupted, output)) {
                    Check(Valid(output), "accepted corrupted input still satisfies every range invariant");
                    Values roundTrip = sentinel;
                    Check(Parse(Serialize(output), roundTrip) && roundTrip == output,
                          "accepted alternate spelling produces one stable complete state");
                } else {
                    Check(output == sentinel, "rejected byte corruption cannot change live settings");
                }
            }
        }
    }

    std::cout << "PASS: " << assertions << " settings assertions; " << combinations
              << " preset configurations; all 100 integer multipliers; " << multiplierInputs
              << " complete multiplier UI inputs; " << multiplierPairs << " zoom/edge multiplier pairs; "
              << edgeMultiplierInputs << " edge multiplier UI inputs; " << pointerSpeedInputs
              << " pointer-speed UI inputs; ten-schema byte-corruption sweep\n";
    return 0;
}
