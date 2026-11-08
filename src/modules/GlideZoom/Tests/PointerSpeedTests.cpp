// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "CppUnitTest.h"
#include "../GlideZoom/pointer_adapter.h"
#include "../GlideZoom/pointer_channel.h"

#include <array>
#include <cmath>
#include <limits>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace GlideZoomTests
{
    namespace
    {
        glide::pointer::View SpeedView(double zoom, double speed, bool automatic, glide::Rect bounds = { 0, 0, 40000, 40000 })
        {
            glide::pointer::View view{ true, true, bounds, zoom, 7 };
            view.speedMultiplier = speed;
            view.adjustForZoom = automatic;
            return view;
        }
        void SpeedNear(double actual, double expected)
        {
            Assert::IsTrue(std::isfinite(actual) && std::abs(actual - expected) < 1e-7);
        }
        struct SpeedNative
        {
            glide::pointer::Adapter* adapter = nullptr;
            glide::Vec2 cursor{ 10000, 10000 };
            bool canPlace = true, cancelWrite = false, enforceClip = false;
            glide::Rect clip{ 0, 0, 40000, 40000 };
            unsigned writes = 0, reads = 0, checks = 0;
            bool Ready(const glide::pointer::View&) noexcept
            {
                ++checks;
                return true;
            }
            bool Read(glide::Vec2& output) noexcept
            {
                ++reads;
                output = cursor;
                return true;
            }
            bool CanPlace(glide::Vec2 point) const noexcept
            {
                return canPlace && (!enforceClip || glide::pointer::Contains(clip, point));
            }
            bool Warp(glide::Vec2 point) noexcept
            {
                ++writes;
                cursor = point;
                adapter->ObserveWarp(point);
                if (cancelWrite)
                    adapter->Pause();
                return true;
            }
        };
        struct SpeedDriver
        {
            glide::pointer::Adapter adapter;
            SpeedNative native;
            glide::pointer::View view = SpeedView(4, 1, true);
            std::uint64_t now = 1;
            SpeedDriver() { native.adapter = &adapter; }
            bool Send(glide::Vec2 candidate, bool permitted = true, bool gesture = false)
            {
                const bool consumed = adapter.Move(candidate, false, now++, view, permitted, gesture, native);
                if (!consumed)
                    native.cursor = candidate;
                return consumed;
            }
            void Seed() { Send(native.cursor); }
        };
    }

    TEST_CLASS (PointerSpeedTests)
    {
    public:
        TEST_METHOD (InteriorGainUsesProcessedDisplacementAndIndependentAutomaticChoice)
        {
            for (const double zoom : { 1.25, 2.0, 4.0, 16.0, 64.0 })
                for (const double speed : { 0.25, 1.0, 1.37, 2.0, 3.0 })
                    for (const bool automatic : { false, true })
                    {
                        const auto view = SpeedView(zoom, speed, automatic);
                        glide::pointer::Step step;
                        Assert::IsTrue(glide::pointer::Propose({ 20000, 20000 }, { 20008, 19996 }, {}, view, step));
                        const double gain = speed / (automatic ? zoom : 1);
                        SpeedNear(step.point.x + step.remainder.x, 20000 + 8 * gain);
                        SpeedNear(step.point.y + step.remainder.y, 20000 - 4 * gain);
                    }
        }

        TEST_METHOD (FastGainTraversesTheRealExitBeyondTheOriginalCandidate)
        {
            const auto view = SpeedView(4, 3, false, { 0, 0, 100, 100 });
            glide::pointer::Step step;
            // Five processed pixels spend 10/3 reaching x100. The remaining
            // 5-10/3 retain native speed on the neighboring monitor.
            Assert::IsTrue(glide::pointer::Propose({ 90, 50 }, { 95, 50 }, {}, view, step));
            Assert::IsTrue(step.point.x == 102 && step.point.y == 50 && glide::pointer::Same(step.remainder, {}));
            Assert::IsTrue(glide::pointer::Propose({ 120, 50 }, { 90, 50 }, {}, view, step));
            SpeedNear(step.point.x + step.remainder.x, 70);
            Assert::IsTrue(glide::pointer::Propose({ -10, 50 }, { 210, 50 }, {}, view, step));
            Assert::IsTrue(step.point.x == 277); // 10 native + 100/3 inside + remainder outside.
            Assert::IsTrue(glide::pointer::Propose({ 90, 90 }, { 95, 95 }, {}, view, step));
            Assert::IsTrue(step.point.x == 102 && step.point.y == 102);
            const auto mirrored = SpeedView(4, 3, false, { -100, -100, 0, 0 });
            Assert::IsTrue(glide::pointer::Propose({ -90, -50 }, { -95, -50 }, {}, mirrored, step));
            Assert::IsTrue(step.point.x == -102 && step.point.y == -50);
            SpeedDriver limit;
            limit.view = SpeedView(4, 3, false, { 0, 0, 262144, 1000 });
            limit.native.cursor = { 0, 50 };
            limit.Seed();
            Assert::IsFalse(limit.Send({ 262144, 50 }));
            Assert::IsTrue(glide::pointer::Same(limit.native.cursor, { 262144, 50 }) && limit.native.writes == 0);
            Assert::IsFalse(limit.adapter.Faulted());
        }

        TEST_METHOD (UnityZoomAndOutsideMotionKeepWindowsExactAndDiscardResidual)
        {
            for (const bool automatic : { false, true })
                for (const double speed : { 0.25, 1.0, 3.0 })
                {
                    auto view = SpeedView(1, speed, automatic, { 0, 0, 100, 100 });
                    SpeedDriver driver;
                    driver.view = view;
                    driver.native.cursor = { 50, 50 };
                    Assert::IsFalse(driver.Send({ 58, 47 }));
                    Assert::IsTrue(driver.native.writes == 0 && driver.native.reads == 0 && driver.native.checks == 0);
                    Assert::IsTrue(glide::pointer::Same(driver.native.cursor, { 58, 47 }));
                    view.zoom = 4;
                    glide::pointer::Step step;
                    Assert::IsTrue(glide::pointer::Propose({ 150, 150 }, { 161, 145 }, { 0.25, -0.25 }, view, step));
                    Assert::IsTrue(glide::pointer::Same(step.point, { 161, 145 }) && glide::pointer::Same(step.remainder, {}));
                }
        }

        TEST_METHOD (TinyFractionsAccumulateAtMinimumSpeedWithoutQuantizationLoss)
        {
            for (const double speed : { 0.25, 1.37, 3.0 })
                for (const bool automatic : { false, true })
                {
                    const auto view = SpeedView(64, speed, automatic);
                    glide::Vec2 current{ 20000, 20000 }, remainder{};
                    for (unsigned count = 1; count <= 4096; ++count)
                    {
                        glide::pointer::Step step;
                        Assert::IsTrue(glide::pointer::Propose(current, { current.x + 1, current.y - 1 }, remainder, view, step));
                        current = step.point;
                        remainder = step.remainder;
                        const double total = count * speed / (automatic ? 64 : 1);
                        SpeedNear(current.x + remainder.x, 20000 + total);
                        SpeedNear(current.y + remainder.y, 20000 - total);
                        Assert::IsTrue(std::abs(remainder.x) <= 0.500001 && std::abs(remainder.y) <= 0.500001);
                    }
                }
        }

        TEST_METHOD (SameEpochSpeedOrAutomaticChangesReseedAndRevokeOldCursorFeedback)
        {
            SpeedDriver driver;
            driver.view.offset = { 7500, 7500 };
            driver.Seed();
            Assert::IsTrue(driver.Send({ 10001, 10000 }));
            const auto old = driver.adapter.FeedbackSnapshot();
            Assert::IsTrue(old.valid);
            SpeedNear(old.remainder.x, 0.25);
            driver.view.speedMultiplier = 0.25;
            driver.Send({ 10001, 10000 });
            auto feedback = driver.adapter.FeedbackSnapshot();
            SpeedNear(feedback.remainder.x, 0.0625);
            glide::Vec2 output{ 123, 456 };
            Assert::IsFalse(glide::pointer::ResolveCursor(old, driver.native.cursor, { 4, driver.view.offset }, driver.view.bounds, driver.view.epoch, true, output, 0.25, true));
            Assert::IsTrue(glide::pointer::Same(output, { 123, 456 }));
            Assert::IsTrue(glide::pointer::ResolveCursor(feedback, driver.native.cursor, { 4, driver.view.offset }, driver.view.bounds, driver.view.epoch, true, output, 0.25, true));
            SpeedNear(output.x, 10000.0625);
            driver.view.speedMultiplier = 1.5;
            driver.view.adjustForZoom = false;
            driver.Send({ 10001, 10000 });
            feedback = driver.adapter.FeedbackSnapshot();
            SpeedNear(feedback.point.x + feedback.remainder.x, 10001.5);
            ++driver.view.epoch;
            output = { 123, 456 };
            Assert::IsFalse(glide::pointer::ResolveCursor(feedback, driver.native.cursor, { 4, driver.view.offset }, driver.view.bounds, driver.view.epoch, true, output, 1.5, false));
            Assert::IsTrue(glide::pointer::Same(output, { 123, 456 }));
        }

        TEST_METHOD (NativeTransactionCancellationAndAppOwnedClipRemainAuthoritative)
        {
            SpeedDriver canceled;
            canceled.view = SpeedView(4, 3, false);
            canceled.Seed();
            canceled.native.cancelWrite = true;
            canceled.Send({ 10004, 10000 });
            Assert::IsFalse(canceled.adapter.FeedbackSnapshot().valid);
            Assert::IsFalse(canceled.adapter.InWarp());
            SpeedDriver clipped;
            clipped.view = SpeedView(4, 3, false, { 0, 0, 100, 100 });
            clipped.native.cursor = { 95, 50 };
            clipped.native.clip = { 0, 0, 100, 100 };
            clipped.native.enforceClip = true;
            clipped.Seed();
            Assert::IsFalse(clipped.Send({ 98, 50 }));
            Assert::IsTrue(clipped.native.writes == 0 && glide::pointer::Same(clipped.native.cursor, { 98, 50 }));
            Assert::IsFalse(clipped.adapter.Faulted() || clipped.adapter.FeedbackSnapshot().valid);
        }

        TEST_METHOD (PublicationCarriesBothIndependentChoicesAndInvalidGainIsAtomic)
        {
            glide::input::PointerChannel channel;
            glide::input::PointerSnapshot publication{ true, true, { -1920, -1080, 0, 0 }, 4, 7 };
            publication.speedMultiplier = 2.75;
            publication.adjustForZoom = false;
            Assert::IsTrue(channel.Publish(publication));
            glide::input::PointerSnapshot read;
            Assert::IsTrue(channel.Read(read));
            Assert::IsTrue(read.speedMultiplier == 2.75 && !read.adjustForZoom && read.zoom == 4 && read.epoch == 7);
            auto view = SpeedView(4, 1, true);
            for (const double speed : { 0.0, 0.249, 3.001, (std::numeric_limits<double>::infinity)(), (std::numeric_limits<double>::quiet_NaN)() })
            {
                view.speedMultiplier = speed;
                glide::pointer::Step step{ { 123, 456 }, { 0.25, -0.25 } };
                Assert::IsFalse(glide::pointer::Propose({ 100, 100 }, { 101, 100 }, {}, view, step));
                Assert::IsTrue(glide::pointer::Same(step.point, { 123, 456 }) && glide::pointer::Same(step.remainder, { 0.25, -0.25 }));
            }
        }
    };
}
