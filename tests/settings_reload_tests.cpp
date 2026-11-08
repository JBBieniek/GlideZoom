#include "../src/settings_update_policy.h"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace
{
    void CheckReload(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }
}

int main()
{
    using glide::settings::Values;
    using namespace glide::settings::update;
    const Values current;
    Context active;
    ObserveZoom(active, 3.75);
    ObserveZoom(active, 4); // A second, parked monitor is still a live camera.
    CheckReload(Classify(current, current, active) == Action::Unchanged, "duplicate notifications are inert");

    struct IntegerChange { int Values::* member; int value; bool input; };
    const std::array<IntegerChange, 21> changes{{
        { &Values::modifier, 1, true }, { &Values::maxZoom, 16, false },
        { &Values::sensitivity, 2, true }, { &Values::zoomMultiplier, 10, true },
        { &Values::edgePanMultiplier, 3, true }, { &Values::pointerSpeed, 175, true },
        { &Values::wheelAccelerationLimit, 4, true }, { &Values::scopeIndicatorMode, 1, false },
        { &Values::scopeIndicatorStyle, 2, false }, { &Values::scopeIndicatorSize, 35, false },
        { &Values::scopeIndicatorOpacity, 80, false }, { &Values::overviewOpacity, 0, false },
        { &Values::scopeIndicatorBorder, 8, false }, { &Values::scopeIndicatorDuration, 5000, false },
        { &Values::scopeIndicatorX, 0, false }, { &Values::scopeIndicatorY, 50, false },
        { &Values::scopeIndicatorFade, 0, false }, { &Values::pointerSpeed, 25, true },
        { &Values::pointerSpeed, 300, true }, { &Values::overviewOpacity, 100, false },
        { &Values::maxZoom, 4, false },
    }};
    for (const auto& change : changes)
    {
        auto candidate = current;
        candidate.*(change.member) = change.value;
        CheckReload(Classify(current, candidate, active) == Action::Live,
                    "compatible numeric settings must retain all live cameras");
        CheckReload(InputChanged(current, candidate) == change.input,
                    "preview-only edits must not cancel input gestures");
    }
    for (bool Values::* member : { &Values::keyboardGesture, &Values::mouseGesture, &Values::reverse,
                                  &Values::centerFollow, &Values::touchpadGesture, &Values::pointerScaling,
                                  &Values::wheelAcceleration, &Values::scopeIndicatorDetails })
    {
        auto candidate = current;
        candidate.*member = !(candidate.*member);
        CheckReload(Classify(current, candidate, active) == Action::Live,
                    "ordinary option changes do not restore magnification");
        CheckReload(InputChanged(current, candidate) == (member != &Values::scopeIndicatorDetails),
                    "only input-affecting options cancel old gesture math");
    }
    for (const int oldScope : {0, 1, 2})
    {
        auto before = current;
        before.zoomScope = oldScope;
        for (const int scope : {0, 1, 2})
        {
            auto candidate = before;
            candidate.zoomScope = scope;
            CheckReload(Classify(before, candidate, active) == (scope == oldScope ? Action::Unchanged : Action::Restore),
                        "scope transitions alone require the ownership restoration barrier");
        }
    }
    auto ceiling = current;
    ceiling.maxZoom = 2;
    CheckReload(Classify(current, ceiling, active) == Action::Restore,
                "a parked camera above the new ceiling cannot be silently retained");
    Context belowCeiling;
    ObserveZoom(belowCeiling, 2);
    CheckReload(Classify(current, ceiling, belowCeiling) == Action::Live,
                "lowering a ceiling that contains every view preserves those views");
    Context pendingTarget = belowCeiling;
    ObserveZoom(pendingTarget, 2.01);
    CheckReload(Classify(current, ceiling, pendingTarget) == Action::Restore,
                "a precise or requested zoom above the ceiling also requires restoration");

    auto preview = current;
    preview.overviewOpacity = 20;
    auto blocked = active;
    blocked.safeBoundary = false;
    CheckReload(Classify(current, preview, blocked) == Action::Defer,
                "capture, handoff, restoration and suspended phases defer compatible edits");
    CheckReload(Classify(current, ceiling, blocked) == Action::Defer,
                "a pending scope/ceiling reset cannot bypass a frozen transaction");
    CheckReload(Classify(current, current, blocked) == Action::Unchanged,
                "duplicate notifications need no lifecycle transition even during a barrier");
    auto invalid = preview;
    invalid.overviewOpacity = 101;
    CheckReload(Classify(current, invalid, active) == Action::Invalid &&
                    Classify(current, invalid, blocked) == Action::Invalid,
                "invalid candidates are rejected before any lifecycle or camera action");
    auto pendingScope = active;
    pendingScope.scopePending = true;
    CheckReload(Classify(current, current, pendingScope) == Action::Restore,
                "a previously pending configured scope still crosses the restoration barrier");
    for (const double malformed : {0.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
    {
        auto uncertain = active;
        ObserveZoom(uncertain, malformed);
        ObserveZoom(uncertain, 1); // A later good view cannot erase a bad observation.
        CheckReload(Classify(current, preview, uncertain) == Action::Restore,
                    "unverifiable live geometry cannot enter the compatible-update path");
    }
    std::cout << "PASS: live settings, all input/indicator fields, scopes, parked cameras and barriers\n";
    return 0;
}
