// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "CppUnitTest.h"
#include "../GlideZoom/input_health.h"
#include "../GlideZoom/control_channel.h"

#include <cstdint>
#include <initializer_list>
#include <limits>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GlideZoomTests
{
    TEST_CLASS(InputHealthTests)
    {
    public:
        TEST_METHOD(DelayedWorkerStaysFailClosedWithoutBecomingPermanentFault)
        {
            using namespace glide::input;
            Assert::IsTrue(EvaluateHostHealth(true, false, true, 1250, 1000) == HostHealth::Ready);
            Assert::IsTrue(EvaluateHostHealth(true, false, true, 1251, 1000) == HostHealth::Stale);
            // Model UI resuming first after sleep, then the worker getting CPU.
            Assert::IsTrue(EvaluateHostHealth(true, false, true, 3601000, 1000) == HostHealth::Stale);
            Assert::IsFalse(FreshHeartbeat(3601000, 1000));
            Assert::IsTrue(EvaluateHostHealth(true, false, true, 3601001, 3601001) == HostHealth::Ready);
            // A clock discontinuity is uncertain, never a fresh heartbeat.
            Assert::IsTrue(EvaluateHostHealth(true, false, true, 999, 1000) == HostHealth::Stale);
            Assert::IsFalse(FreshHeartbeat(0, (std::numeric_limits<std::uint64_t>::max)()));
        }

        TEST_METHOD(ActualWorkerFailuresAlwaysWinOverFreshHeartbeat)
        {
            using namespace glide::input;
            for (bool ready : {false, true})
            {
                for (bool faulted : {false, true})
                {
                    for (bool running : {false, true})
                    {
                        const bool failed = !ready || faulted || !running;
                        Assert::IsTrue((EvaluateHostHealth(ready, faulted, running, 1000, 1000) == HostHealth::Failed) == failed);
                        Assert::IsTrue((EvaluateHostHealth(ready, faulted, running, 2000, 1000) == HostHealth::Failed) == failed);
                    }
                }
            }
        }

        TEST_METHOD(StaleUiCancelsHeldInputAndPreservesItsOwedRelease)
        {
            using namespace glide::input;
            State state;
            ControlChannel source;
            ControlReader reader;
            source.Publish({DefaultControlOptions | 1, 1, 2});
            std::uint64_t now = 1000, uiBeat = 1000;
            bool cancelled = false;
            const auto sync = [&] {
                reader.Synchronize(source, state,
                                   [&] { return FreshHeartbeat(now, uiBeat); },
                                   [&](Decision decision) {
                                       if (decision.end) cancelled = decision.cancel;
                                   });
            };
            sync();
            state.Key(vk::LeftControl, true, now);
            state.Key(vk::LeftAlt, true, now);
            const auto begin = state.Key(vk::Space, true, now);
            Assert::IsTrue(begin.begin && begin.consume);

            now = 1251;
            sync();
            Assert::IsTrue(state.Active() == Gesture::None);
            Assert::IsTrue(cancelled);
            Assert::IsTrue(state.SwallowedKey(vk::Space));
            Assert::IsFalse(state.CanZoom());
            Assert::IsFalse(state.Key(vk::Equal, true, now).step != 0);
            Assert::IsTrue(state.Key(vk::Space, false, now).consume);
            Assert::IsFalse(state.SwallowedKey(vk::Space));
            Assert::IsTrue(state.Invariant());

            // Freshness alone cannot replay the cancelled gesture or an old key.
            uiBeat = now;
            sync();
            Assert::IsTrue(state.Active() == Gesture::None);
            Assert::IsFalse(state.Key(vk::Equal, true, now).step != 0);
            Assert::IsTrue(state.Invariant());
        }
    };
}
