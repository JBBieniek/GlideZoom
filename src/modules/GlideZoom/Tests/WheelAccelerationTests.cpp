// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "CppUnitTest.h"
#include "../GlideZoom/wheel_acceleration.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GlideZoomTests
{
    TEST_CLASS (WheelAccelerationTests)
    {
    public:
        TEST_METHOD (DisabledAndLegacyPacketsKeepOriginalGain)
        {
            glide::wheel::Accelerator accelerator;
            for (const double delta : { -32768.0, -120.0, -15.0, -0.5, 0.0, 0.5, 15.0, 120.0, 32767.0 })
            {
                Assert::AreEqual(delta, accelerator.Apply(delta, 100, 1, false, 4));
                Assert::AreEqual(delta, accelerator.Apply(delta, 0, 1, true, 4));
            }
            for (unsigned event = 0; event < 100; ++event)
                Assert::AreEqual(120.0, accelerator.Apply(120, 100 + event, 1, true, 1));
        }

        TEST_METHOD (PhysicalDetentCadenceRampsAndIsBoundedByPreference)
        {
            for (int limit = 1; limit <= 4; ++limit)
            {
                glide::wheel::Accelerator accelerator;
                for (unsigned detent = 0; detent < 10; ++detent)
                {
                    const double boost = (std::min)(static_cast<int>(detent) + 1, limit);
                    Assert::AreEqual(120.0 * boost, accelerator.Apply(120, 100 + detent * 40, 1, true, limit));
                }
            }
        }

        TEST_METHOD (HighResolutionFragmentsAndCombinedPacketsSpendSameDetentCredit)
        {
            for (const unsigned fragments : { 1u, 2u, 4u, 8u, 15u, 120u })
            {
                glide::wheel::Accelerator fragmented, whole;
                for (unsigned detent = 0; detent < 8; ++detent)
                {
                    const std::uint64_t start = 100 + detent * 40;
                    double actual = 0;
                    for (unsigned fragment = 0; fragment < fragments; ++fragment)
                        actual += fragmented.Apply(120.0 / fragments, start + fragment * 30 / fragments, 1, true, 4);
                    Assert::AreEqual(whole.Apply(120, start, 1, true, 4), actual, 1e-8);
                }
            }
            glide::wheel::Accelerator combined, split;
            const double large = combined.Apply(255, 100, 1, true, 4);
            double fragments = 0;
            for (unsigned index = 0; index < 17; ++index)
                fragments += split.Apply(15, 100, 1, true, 4);
            Assert::AreEqual(405.0, large);
            Assert::AreEqual(large, fragments);
        }

        TEST_METHOD (ResetBoundariesCannotInheritOldBurstGain)
        {
            for (unsigned boundary = 0; boundary < 7; ++boundary)
            {
                glide::wheel::Accelerator accelerator;
                accelerator.Apply(120, 100, 7, true, 4);
                Assert::AreEqual(240.0, accelerator.Apply(120, 140, 7, true, 4));
                double result = 0;
                switch (boundary)
                {
                case 0:
                    result = accelerator.Apply(120, 391, 7, true, 4);
                    break;
                case 1:
                    result = accelerator.Apply(-120, 160, 7, true, 4);
                    break;
                case 2:
                    result = accelerator.Apply(120, 160, 8, true, 4);
                    break;
                case 3:
                    result = accelerator.Apply(120, 139, 7, true, 4);
                    break;
                case 4:
                    result = accelerator.Apply(120, 160, 7, true, 3);
                    break;
                case 5:
                    accelerator.Reset();
                    result = accelerator.Apply(120, 160, 7, true, 4);
                    break;
                case 6:
                    accelerator.Apply(120, 150, 7, false, 4);
                    result = accelerator.Apply(120, 160, 7, true, 4);
                    break;
                }
                Assert::AreEqual(boundary == 1 ? -120.0 : 120.0, result);
            }
            glide::wheel::Accelerator threshold;
            threshold.Apply(120, 100, 1, true, 4);
            Assert::AreEqual(120.0, threshold.Apply(120, 180, 1, true, 4));
        }

        TEST_METHOD (FractionalCreditRespondsImmediatelyAndPreservesDirection)
        {
            glide::wheel::Accelerator accelerator;
            Assert::AreEqual(15.0, accelerator.Apply(15, 100, 1, true, 4));
            Assert::AreEqual(105.0, accelerator.Apply(105, 101, 1, true, 4));
            Assert::AreEqual(30.0, accelerator.Apply(15, 140, 1, true, 4));
            Assert::AreEqual(-15.0, accelerator.Apply(-15, 141, 1, true, 4));
            Assert::AreEqual(0.0, accelerator.Apply((std::numeric_limits<double>::infinity)(), 142, 1, true, 4));
            Assert::AreEqual(120.0, accelerator.Apply(120, 143, 1, true, 4));
        }
    };
}
