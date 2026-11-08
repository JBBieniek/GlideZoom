// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "CppUnitTest.h"
#include "../GlideZoom/frame_stall_policy.h"
#include "../GlideZoom/input_state.h"
#include "../GlideZoom/monitor_geometry.h"
#include "../GlideZoom/runtime_state.h"
#include "../GlideZoom/wheel_activation.h"
#include "../GlideZoom/zoom_animation.h"
#include "../GlideZoom/zoom_session.h"

#include <cmath>
#include <limits>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GlideZoomTests
{
    namespace
    {
        glide::wheel::Activation Admission(std::uint64_t epoch = 3)
        {
            return { true, epoch, 9, 2, 100, 2100, { -400, 200, 0, 400 } };
        }
        constexpr glide::Vec2 Anchor{ -200, 300 };

        struct ActivationBackend
        {
            glide::session::Snapshot current{};
            bool readable = true;
            unsigned writes = 0;
            bool Read(glide::session::Snapshot& output) const
            {
                output = current;
                return readable;
            }
            bool SetInput(glide::session::Input value)
            {
                ++writes;
                current.input = value;
                return true;
            }
            bool SetVisual(glide::session::Visual value)
            {
                ++writes;
                current.visual = value;
                return true;
            }
        };
        struct ActivationJournal
        {
            bool armed = false;
            void Publish(const glide::session::RecoveryRecord& record)
            {
                Assert::IsTrue(glide::session::Valid(record));
                armed = true;
            }
            void Clear() { armed = false; }
        };

        double BurstGain(unsigned fragments, bool cold)
        {
            glide::wheel::ActivationIntent intent;
            glide::wheel::Accelerator warm;
            double total = 0;
            for (unsigned detent = 0; detent < 5; ++detent)
            {
                for (unsigned fragment = 0; fragment < fragments; ++fragment)
                {
                    const auto timestamp = 100 + detent * 40 + fragment * 30 / fragments;
                    const double delta = 120.0 / fragments;
                    if (!cold)
                        total += warm.Apply(delta, timestamp, 3, true, 3);
                    else if (!detent && !fragment)
                        Assert::IsTrue(intent.Begin(Admission(), 2, delta, Anchor, 500, true, 3));
                    else
                        Assert::IsTrue(intent.Push(delta, timestamp, 2, Anchor, 500, true, 3));
                }
            }
            if (cold)
            {
                Assert::IsTrue(intent.TakeInitial(total));
                Assert::IsTrue(intent.Commit(4, 500));
                total += intent.TakePending();
            }
            return total;
        }
    }

    TEST_CLASS (WheelActivationTests)
    {
    public:
        TEST_METHOD (ColdFiveNotchBurstKeepsWarmGainAndOriginalCadence)
        {
            const double warm = BurstGain(1, false);
            const double cold = BurstGain(1, true);
            Assert::AreEqual(1440.0, warm);
            Assert::AreEqual(warm, cold);
            Assert::AreEqual(glide::animation::StepZoom(1, warm, 8, 10), glide::animation::StepZoom(1, cold, 8, 10));
        }

        TEST_METHOD (HighResolutionColdFragmentsKeepWholeDetentGain)
        {
            for (const unsigned fragments : { 1u, 2u, 4u, 8u, 15u, 120u })
            {
                Assert::AreEqual(BurstGain(fragments, false), BurstGain(fragments, true), 1e-8);
                Assert::AreEqual(BurstGain(1, true), BurstGain(fragments, true), 1e-8);
            }
        }

        TEST_METHOD (PendingCameraIntentWaitsForVerifiedMappingAndKeepsCommitEpochContinuity)
        {
            using namespace glide;
            runtime::State lifecycle;
            Assert::IsTrue(lifecycle.PrepareEnable() && lifecycle.Activate());
            const auto previousEpoch = lifecycle.Epoch();
            input::State input;
            input.SetEnabled(true);
            input.Key(input::vk::LeftControl, true, 100);
            input.Key(input::vk::LeftAlt, true, 100);
            Assert::IsTrue(input.Wheel(true, 100).consume);
            lifecycle.Pause();
            input.SetEnabled(false);
            wheel::ActivationIntent intent;
            const auto admission = Admission(lifecycle.Epoch());
            Assert::IsTrue(intent.Begin(admission, previousEpoch, 120, Anchor, 100, true, 3));
            for (const auto timestamp : { 140u, 180u, 220u, 260u })
            {
                Assert::IsFalse(lifecycle.Accept(previousEpoch));
                Assert::IsFalse(input.Wheel(true, timestamp).consume);
                Assert::IsTrue(wheel::Admit(intent.Admission(), admission.epoch, 9, 2, Anchor, timestamp, 500, true, true));
                Assert::IsTrue(intent.Push(120, timestamp, previousEpoch, Anchor, 500, true, 3));
            }
            double initial = 0;
            Assert::IsTrue(intent.TakeInitial(initial));
            const double frozen = animation::StepZoom(1, initial, 8, 10);
            Assert::AreEqual(1.1, frozen, 1e-9);
            monitor::Frame frame;
            Assert::IsTrue(monitor::MakeFrame(admission.bounds, AnchorZoom({}, frozen, Anchor, admission.bounds), frame));
            session::Snapshot mapping{ { 1, 0, 0 }, { true, frame.source, frame.destination } };
            session::Session session;
            ActivationBackend native;
            ActivationJournal journal;
            native.readable = false;
            Assert::IsTrue(session.Apply(mapping, native, journal) == session::Result::ReadFailure);
            Assert::IsFalse(intent.Committed());
            Assert::IsTrue(native.writes == 0 && !journal.armed && session::IsIdentity(native.current));
            native.readable = true;
            Assert::IsTrue(session.Apply(mapping, native, journal) == session::Result::Applied);
            Assert::IsTrue(native.current == mapping && journal.armed);
            Assert::IsTrue(lifecycle.Activate());
            Assert::IsTrue(intent.Commit(lifecycle.Epoch(), 500));
            Assert::IsFalse(intent.Admission().enabled);
            double target = animation::StepZoom(frozen, intent.TakePending(), 8, 10);
            Assert::AreEqual(animation::StepZoom(1, BurstGain(1, false), 8, 10), target, 1e-9);
            Assert::AreEqual(0.0, intent.TakePending());
            // The real post-commit epoch changes, but the same physical burst
            // keeps its logical acceleration epoch and therefore its gain.
            Assert::IsTrue(intent.Push(120, 300, lifecycle.Epoch(), Anchor, 500, true, 3));
            target = animation::StepZoom(target, intent.TakePending(), 8, 10);
            Assert::AreEqual(2.5, target, 1e-9);
            Assert::IsTrue(session.Reset(native, journal) == session::Result::Applied);
            Assert::IsTrue(session::IsIdentity(native.current) && !journal.armed);
        }

        TEST_METHOD (QueuedOldEpochPacketsNeedOriginalMonitorAndBoundedTimestamp)
        {
            glide::wheel::ActivationIntent intent;
            Assert::IsTrue(intent.Begin(Admission(), 2, 120, Anchor, 100, false, 3));
            Assert::IsTrue(intent.Push(120, 140, 2, Anchor, 200, false, 3));
            Assert::IsFalse(intent.Push(120, 160, 99, Anchor, 200, false, 3));
            Assert::IsFalse(intent.Push(120, 160, 2, { 0, 300 }, 200, false, 3));
            Assert::IsFalse(intent.Push(120, 99, 2, Anchor, 200, false, 3));
            Assert::IsFalse(intent.Push(120, 139, 2, Anchor, 200, false, 3));
            Assert::IsFalse(intent.Push(120, 201, 2, Anchor, 200, false, 3));
            Assert::AreEqual(120.0, intent.TakePending());
            Assert::IsTrue(intent.Push(120, 160, 3, Anchor, 200, false, 3));
            Assert::AreEqual(120.0, intent.TakePending());
        }

        TEST_METHOD (QuickModifierReleaseClosesAdmissionWithoutDroppingAcceptedIntent)
        {
            glide::wheel::ActivationIntent intent;
            Assert::IsTrue(intent.Begin(Admission(), 2, 120, Anchor, 100, true, 3));
            Assert::IsTrue(intent.Push(120, 140, 2, Anchor, 200, true, 3));
            intent.CloseAdmission();
            Assert::IsFalse(intent.Accepting() || intent.Admission().enabled);
            Assert::IsTrue(intent.Current(500));
            Assert::IsFalse(intent.Push(120, 180, 2, Anchor, 500, true, 3));
            double first = 0;
            Assert::IsTrue(intent.TakeInitial(first));
            Assert::AreEqual(120.0, first);
            Assert::IsTrue(intent.Commit(4, 500));
            Assert::AreEqual(240.0, intent.TakePending());
            Assert::AreEqual(0.0, intent.TakePending());
        }

        TEST_METHOD (TimeoutResetDepartureAndTravelLimitCannotReplayPendingBurst)
        {
            for (const unsigned boundary : { 0u, 1u, 2u, 3u })
            {
                glide::wheel::ActivationIntent intent;
                Assert::IsTrue(intent.Begin(Admission(), 2, 120, Anchor, 100, true, 3));
                if (boundary == 0)
                    intent.Reset();
                const auto now = boundary == 1 ? 2101u : 200u;
                const glide::Vec2 point = boundary == 2 ? glide::Vec2{ -401, 300 } : Anchor;
                const double delta = boundary == 3 ? glide::wheel::MaximumActivationTravel : 120;
                Assert::IsFalse(intent.Push(delta, 140, 2, point, now, true, 3));
                Assert::AreEqual(0.0, intent.TakePending());
            }
            glide::wheel::ActivationIntent intent;
            Assert::IsTrue(intent.Begin(Admission(), 2, 120, Anchor, 100, true, 3));
            Assert::IsFalse(intent.Commit(4, 200)); // Initial target was not taken.
            double first = 0;
            Assert::IsTrue(intent.TakeInitial(first));
            Assert::IsFalse(intent.TakeInitial(first));
            Assert::IsFalse(intent.Commit(0, 200));
            Assert::IsFalse(intent.Commit(4, 2101));
            Assert::IsFalse(intent.Push((std::numeric_limits<double>::quiet_NaN)(), 140, 2, Anchor, 200, true, 3));
            intent.Reset();
            Assert::IsFalse(intent.Active() || intent.Committed() || intent.TakeInitial(first));
        }

        TEST_METHOD (HookAdmissionChannelBindsChordTargetGenerationAndLifetime)
        {
            using namespace glide::wheel;
            ActivationChannel channel;
            Assert::IsTrue(channel.Publish(Admission()));
            Activation read;
            Assert::IsTrue(channel.Read(read));
            Assert::IsTrue(Admit(read, 3, 9, 2, Anchor, 140, 200, true, true));
            Assert::IsFalse(Admit(read, 2, 9, 2, Anchor, 140, 200, true, true));
            Assert::IsFalse(Admit(read, 3, 8, 2, Anchor, 140, 200, true, true));
            Assert::IsFalse(Admit(read, 3, 9, 1, Anchor, 140, 200, true, true));
            Assert::IsFalse(Admit(read, 3, 9, 2, Anchor, 140, 200, false, true));
            Assert::IsFalse(Admit(read, 3, 9, 2, Anchor, 140, 200, true, false));
            Assert::IsFalse(Admit(read, 3, 9, 2, Anchor, 140, 2101, true, true));
            auto invalid = read;
            invalid.deadline = invalid.startedAt + ActivationMilliseconds + 1;
            Assert::IsFalse(channel.Publish(invalid));
            Assert::IsTrue(channel.Read(read) && read.deadline == 2100);
            auto observation = read;
            observation.observeOnly = true;
            Assert::IsTrue(channel.Publish(observation));
            Assert::IsTrue(channel.Read(read) && read.enabled && read.observeOnly && Valid(read));
            Assert::IsTrue(read.epoch == observation.epoch && read.generation == observation.generation &&
                           read.monitorId == observation.monitorId && read.startedAt == observation.startedAt &&
                           read.deadline == observation.deadline && glide::pointer::Same(read.bounds, observation.bounds));
            Assert::IsFalse(Admit(read, 3, 9, 2, Anchor, 140, 200, true, true));
            Assert::IsTrue(channel.Publish(Admission()));
            Assert::IsTrue(channel.Read(read) && !read.observeOnly && Admit(read, 3, 9, 2, Anchor, 140, 200, true, true));
            Assert::IsTrue(channel.Publish({}));
            Assert::IsTrue(channel.Read(read) && !Admit(read, 3, 9, 2, Anchor, 140, 200, true, true));
        }

        TEST_METHOD (QueuedEmergencyResetSurvivesFirstWheelEpochChange)
        {
            using namespace glide;
            runtime::State lifecycle;
            Assert::IsTrue(lifecycle.PrepareEnable() && lifecycle.Activate());
            const auto queuedEpoch = lifecycle.Epoch();
            input::State hook;
            hook.SetEnabled(true);
            hook.Key(input::vk::LeftControl, true, 100);
            hook.Key(input::vk::LeftAlt, true, 100);
            Assert::IsTrue(hook.Wheel(true, 100).consume);
            const auto reset = hook.Key(input::vk::Escape, true, 160);
            Assert::IsTrue(reset.consume && reset.reset);

            lifecycle.Cancel(); // BeginHandoff cancels the old input epoch.
            wheel::ActivationIntent intent;
            Assert::IsTrue(intent.Begin(Admission(lifecycle.Epoch()), queuedEpoch, 120, Anchor, 200, true, 3));
            Assert::IsTrue(intent.Push(120, 140, queuedEpoch, Anchor, 200, true, 3));
            Assert::IsFalse(timing::AcceptQueuedInput(false, queuedEpoch, lifecycle));
            Assert::IsTrue(timing::AcceptQueuedInput(reset.reset, queuedEpoch, lifecycle));
            session::Session mapping;
            ActivationBackend native;
            if (timing::AcceptQueuedInput(reset.reset, queuedEpoch, lifecycle))
                intent.Reset();
            double initial = 0;
            Assert::IsFalse(intent.Active() || intent.TakeInitial(initial) || intent.Committed());
            Assert::AreEqual(0.0, intent.TakePending());
            Assert::IsTrue(native.writes == 0 && !mapping.OwnsState() && session::IsIdentity(native.current));
        }
    };
}
