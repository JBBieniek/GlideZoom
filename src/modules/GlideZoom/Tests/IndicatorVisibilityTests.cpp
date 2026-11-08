// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#include "CppUnitTest.h"
#include "../GlideZoom/indicator_visibility.h"

#include <cmath>
#include <limits>

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace GlideZoomTests
{
    namespace
    {
        void AlphaNear(double actual, double expected)
        {
            Assert::IsTrue(std::isfinite(actual) && actual >= 0 && actual <= 1 && std::abs(actual - expected) < 0.000001);
        }
        constexpr glide::Transform camera{ 2, { 100, -200 } };
    }

    TEST_CLASS (IndicatorVisibilityTests)
    {
    public:
        TEST_METHOD (MotionModeUsesCameraChangesAndFirstMagnifiedFrame)
        {
            glide::indicator::State state;
            AlphaNear(state.Advance(0, camera, 1, 500, 0, true), 1);
            AlphaNear(state.Advance(499, camera, 1, 500, 0, true), 1);
            AlphaNear(state.Advance(500, camera, 1, 500, 0, true), 0);
            auto changed = camera;
            changed.offset.x += 0.125;
            AlphaNear(state.Advance(600, changed, 1, 500, 0, true), 1);
            AlphaNear(state.Advance(1099, changed, 1, 500, 0, true), 1);
            AlphaNear(state.Advance(1100, changed, 1, 500, 0, true), 0);
            changed.zoom = 3;
            AlphaNear(state.Advance(1200, changed, 1, 500, 0, true), 1);
            AlphaNear(state.Advance(1201, changed, 1, 500, 0, false), 0);
            AlphaNear(state.Advance(1202, changed, 1, 500, 0, true), 1);
        }

        TEST_METHOD (OffAndShortcutModesOnlyHonorExplicitReveal)
        {
            for (const int mode : { 0, 2 })
            {
                glide::indicator::State state;
                AlphaNear(state.Advance(0, camera, mode, 500, 0, true), 0);
                auto moved = camera;
                moved.offset.y += 20;
                AlphaNear(state.Advance(10, moved, mode, 500, 0, true), 0);
                state.Reveal(20, 100);
                AlphaNear(state.Advance(20, moved, mode, 500, 0, true), 1);
                AlphaNear(state.Advance(119, moved, mode, 500, 0, true), 1);
                AlphaNear(state.Advance(120, moved, mode, 500, 0, true), 0);
            }
        }

        TEST_METHOD (FadesContinueWithoutCameraMotionAndDelayedTicksDoNotExtendHolds)
        {
            glide::indicator::State state;
            AlphaNear(state.Advance(0, camera, 1, 500, 120, true), 0);
            AlphaNear(state.Advance(60, camera, 1, 500, 120, true), 0.5);
            AlphaNear(state.Advance(120, camera, 1, 500, 120, true), 1);
            AlphaNear(state.Advance(500, camera, 1, 500, 120, true), 1);
            AlphaNear(state.Advance(560, camera, 1, 500, 120, true), 0.5);
            AlphaNear(state.Advance(620, camera, 1, 500, 120, true), 0);
            state.Reset();
            state.Advance(0, camera, 1, 500, 120, true);
            AlphaNear(state.Advance(2000, camera, 1, 500, 120, true), 0);
        }

        TEST_METHOD (ReversingAFadeStartsAtItsCurrentAlpha)
        {
            glide::indicator::State state;
            state.Advance(0, camera, 1, 500, 120, true);
            AlphaNear(state.Advance(560, camera, 1, 500, 120, true), 0.5);
            auto changed = camera;
            changed.offset.x += 1;
            AlphaNear(state.Advance(560, changed, 1, 500, 120, true), 0.5);
            AlphaNear(state.Advance(620, changed, 1, 500, 120, true), 0.75);
            AlphaNear(state.Advance(680, changed, 1, 500, 120, true), 1);
            AlphaNear(state.Advance(680, changed, 0, 500, 120, true), 1);
            AlphaNear(state.Advance(740, changed, 0, 500, 120, true), 0.5);
        }

        TEST_METHOD (AHoldMayExpireBeforeFadeInCompletes)
        {
            glide::indicator::State state;
            AlphaNear(state.Advance(0, camera, 1, 60, 120, true), 0);
            AlphaNear(state.Advance(30, camera, 1, 60, 120, true), 0.15625);
            AlphaNear(state.Advance(60, camera, 1, 60, 120, true), 0.5);
            AlphaNear(state.Advance(120, camera, 1, 60, 120, true), 0.25);
            AlphaNear(state.Advance(180, camera, 1, 60, 120, true), 0);
        }

        TEST_METHOD (NewMotionAfterADelayedExpiryDoesNotReviveAnUnfadedImage)
        {
            glide::indicator::State state;
            state.Advance(0, camera, 1, 500, 120, true);
            auto changed = camera;
            changed.zoom = 4;
            AlphaNear(state.Advance(560, changed, 1, 500, 120, true), 0.5);
            AlphaNear(state.Advance(620, changed, 1, 500, 120, true), 0.75);
        }

        TEST_METHOD (AlwaysModeRemainsVisibleWhileMagnified)
        {
            glide::indicator::State state;
            AlphaNear(state.Advance(0, camera, 3, 1, 100, true), 0);
            AlphaNear(state.Advance(100, camera, 3, 1, 100, true), 1);
            AlphaNear(state.Advance(600000, camera, 3, 1, 100, true), 1);
            AlphaNear(state.Advance(600000, camera, 3, 1, 100, false), 1);
            AlphaNear(state.Advance(600100, camera, 3, 1, 100, false), 0);
        }

        TEST_METHOD (MalformedAndBackwardTimeResetSafely)
        {
            glide::indicator::State state;
            AlphaNear(state.Advance(100, camera, 3, 100, -1, true), 1);
            AlphaNear(state.Advance(99, camera, 3, 100, 100, true), 0);
            auto invalid = camera;
            invalid.offset.x = std::numeric_limits<double>::infinity();
            AlphaNear(state.Advance(100, invalid, 3, 100, 100, true), 0);
            state.Reveal(100, 500);
            state.Reveal(99, 500);
            AlphaNear(state.Advance(100, camera, 0, 100, 0, true), 0);
            AlphaNear(state.Advance(101, camera, 999, -1, 999, true), 0);
        }

        TEST_METHOD (DeadlineSaturatesAndRepeatedRevealExtendsOnlyItsHold)
        {
            glide::indicator::State state;
            state.Reveal(0, 100);
            AlphaNear(state.Advance(0, camera, 2, 100, 0, true), 1);
            state.Reveal(50, 100);
            AlphaNear(state.Advance(149, camera, 2, 100, 0, true), 1);
            AlphaNear(state.Advance(150, camera, 2, 100, 0, true), 0);
            state.Reset();
            constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
            state.Reveal(maximum - 5, 100);
            AlphaNear(state.Advance(maximum - 4, camera, 2, 100, 0, true), 1);
            AlphaNear(state.Advance(maximum, camera, 2, 100, 0, true), 0);
        }
    };
}
