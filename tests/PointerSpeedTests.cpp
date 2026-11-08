// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "test_assertions.h"
#include "../src/pointer_adapter.h"
#include "../src/pointer_channel.h"
#include "../src/pointer_dispatch.h"
#include "../src/control_channel.h"

#include <array>
#include <cmath>
#include <limits>
#include <string_view>

using namespace glide_test;

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
            bool canPlace = true, cancelWrite = false, enforceClip = false, ready = true, warpSuccess = true, exactReadback = true;
            glide::Rect clip{ 0, 0, 40000, 40000 };
            unsigned writes = 0, reads = 0, checks = 0;
            bool Ready(const glide::pointer::View&) noexcept
            {
                ++checks;
                return ready;
            }
            bool DragBounds(const glide::pointer::View& view, glide::Rect& output) noexcept
            {
                output = enforceClip ? clip : view.bounds;
                return ready;
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
                if (!warpSuccess)
                    return false;
                cursor = point;
                adapter->ObserveWarp(point);
                if (!exactReadback)
                    cursor.x += 1;
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

    class PointerSpeedTests
    {
    public:
        void InteriorGainUsesProcessedDisplacementAndIndependentAutomaticChoice()
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

        void FastGainTraversesTheRealExitBeyondTheOriginalCandidate()
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

        void UnityZoomAndOutsideMotionKeepWindowsExactAndDiscardResidual()
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

        void TinyFractionsAccumulateAtMinimumSpeedWithoutQuantizationLoss()
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

        void SameEpochSpeedOrAutomaticChangesReseedAndRevokeOldCursorFeedback()
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

        void NativeTransactionCancellationAndAppOwnedClipRemainAuthoritative()
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

        void PublicationCarriesBothIndependentChoicesAndInvalidGainIsAtomic()
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

        void HookDispatchAppliesSpeedWithoutAButtonAndKeepsTheSameGainDuringAndAfterDrag()
        {
            using namespace glide;
            input::ControlChannel controls;
            input::ControlReader reader;
            input::State gestures;
            controls.Publish({ input::DefaultControlOptions | 1, 7, 4 });
            Assert::IsTrue(reader.Synchronize(controls, gestures, [] { return true; }, [](input::Decision) {}));
            input::routing::Snapshot routes;
            routes.enabled = true;
            routes.generation = 1;
            routes.epoch = reader.Epoch();
            routes.activeId = 1;
            routes.count = 1;
            routes.routes[0] = { 1, { 0, 0, 40000, 40000 }, { 7500, 7500, 17500, 17500 }, true };
            input::routing::State routing;
            routing.Observe(routes, true, reader.Epoch());
            routing.ObservePoint({ 10000, 10000 });
            input::PointerChannel pointerControls;
            input::PointerSnapshot policy{ true, true, routes.routes[0].bounds, 4, 7, {}, 1, 0.25, true };
            Assert::IsTrue(pointerControls.Publish(policy));
            pointer::Adapter ordinary, drag;
            SpeedNative native;
            const auto move = [&](Vec2 candidate, std::uint64_t now) {
                routing.Observe(routes, true, reader.Epoch());
                routing.ObservePoint(candidate);
                input::PointerSnapshot snapshot;
                const bool coherent = pointerControls.Read(snapshot);
                native.adapter = routing.HasForwardedDrag() ? &drag : &ordinary;
                const auto decision = gestures.Move(candidate, now);
                const bool consumed = input::DispatchPointer(candidate, now, snapshot, reader.Epoch(), coherent, (reader.Options() & 1) != 0, true, decision.consume, routing, ordinary, drag, native);
                if (!consumed)
                    native.cursor = candidate;
                return consumed;
            };
            Assert::IsFalse(routing.HasForwardedDrag());
            Assert::IsTrue(move({ 10016, 10000 }, 1));
            SpeedNear(native.cursor.x, 10001);
            Assert::IsTrue(ordinary.FeedbackSnapshot().valid && !drag.FeedbackSnapshot().valid);
            Assert::IsFalse(gestures.ButtonEvent(input::Button::Left, true, native.cursor, 2).consume);
            Assert::IsFalse(routing.Button(0, true, false));
            ordinary.Pause();
            drag.EndConstraint();
            Assert::IsTrue(move({ 10017, 10000 }, 3));
            SpeedNear(native.cursor.x, 10002);
            Assert::IsTrue(drag.FeedbackSnapshot().valid);
            Assert::IsFalse(gestures.ButtonEvent(input::Button::Left, false, native.cursor, 4).consume);
            Assert::IsFalse(routing.Button(0, false, false));
            ordinary.Pause();
            drag.EndConstraint();
            Assert::IsTrue(move({ 10018, 10000 }, 5));
            SpeedNear(native.cursor.x, 10003);
            Assert::IsFalse(routing.HasForwardedDrag());
        }

        void HookDispatchRevokesBothBaselinesWhenSharedControlsAreUnavailable()
        {
            using namespace glide;
            input::routing::Snapshot routes;
            routes.enabled = true;
            routes.generation = 1;
            routes.epoch = 7;
            routes.activeId = 1;
            routes.count = 1;
            routes.routes[0] = { 1, { 0, 0, 40000, 40000 }, { 7500, 7500, 17500, 17500 }, true };
            const input::PointerSnapshot policy{ true, true, routes.routes[0].bounds, 4, 7 };
            for (unsigned unavailable = 0; unavailable < 5; ++unavailable)
            {
                input::routing::State routing;
                routing.Observe(routes, true, 7);
                routing.ObservePoint({ 10000, 10000 });
                pointer::Adapter ordinary, drag;
                SpeedNative native;
                native.adapter = &ordinary;
                Assert::IsTrue(input::DispatchPointer({ 10001, 10000 }, 1, policy, 7, true, true, true, false, routing, ordinary, drag, native));
                Assert::IsFalse(routing.Button(0, true, false));
                native.adapter = &drag;
                Assert::IsTrue(input::DispatchPointer({ 10001, 10000 }, 2, policy, 7, true, true, true, false, routing, ordinary, drag, native));
                Assert::IsTrue(ordinary.FeedbackSnapshot().valid && drag.FeedbackSnapshot().valid);
                auto pending = routes;
                pending.busy = unavailable == 4;
                routing.Observe(pending, true, 7);
                const auto checks = native.checks;
                Assert::IsFalse(input::DispatchPointer({ 10002, 10000 }, 3, policy, unavailable == 0 ? 8 : 7, unavailable != 1, unavailable != 2, unavailable != 3, false, routing, ordinary, drag, native));
                Assert::IsFalse(ordinary.FeedbackSnapshot().valid || drag.FeedbackSnapshot().valid);
                Assert::IsTrue(native.checks == checks && !ordinary.Faulted() && !drag.Faulted());
                Assert::IsFalse(routing.Button(0, false, true)); // The real forwarded UP still reaches its app.
            }
        }

        void OrdinaryNativeFailureIsDistinguishableFromATransientPermissionPause()
        {
            using namespace glide;
            input::routing::State routing;
            routing.Observe({}, true, 7); // Selected/linked mode has no independent barrier.
            const input::PointerSnapshot policy{ true, true, { 0, 0, 40000, 40000 }, 4, 7, {}, 1, 0.25, true };
            pointer::Adapter ordinary, drag;
            SpeedNative native;
            native.adapter = &ordinary;
            native.ready = false;
            Assert::IsFalse(input::DispatchPointer({ 10016, 10000 }, 1, policy, 7, true, true, true, false, routing, ordinary, drag, native));
            Assert::IsTrue(ordinary.FailureReason() == pointer::Failure::None);
            native.ready = true;
            Assert::IsTrue(input::DispatchPointer({ 10016, 10000 }, 2, policy, 7, true, true, true, false, routing, ordinary, drag, native));
            SpeedNear(native.cursor.x, 10001);
            native.warpSuccess = false;
            Assert::IsFalse(input::DispatchPointer({ 10017, 10000 }, 3, policy, 7, true, true, true, false, routing, ordinary, drag, native));
            Assert::IsTrue(ordinary.FailureReason() == pointer::Failure::Warp);
            const auto writes = native.writes;
            native.warpSuccess = true;
            Assert::IsFalse(input::DispatchPointer({ 10017, 10000 }, 4, policy, 7, true, true, true, false, routing, ordinary, drag, native));
            Assert::IsTrue(native.writes == writes && !ordinary.FeedbackSnapshot().valid);
            Assert::IsTrue(std::wstring_view(pointer::FailureText(ordinary.FailureReason())).find(L"cursor write") != std::wstring_view::npos);
            input::routing::Snapshot routes;
            routes.enabled = true;
            routes.generation = 1;
            routes.epoch = 7;
            routes.activeId = 1;
            routes.count = 1;
            routes.routes[0] = { 1, policy.bounds, { 7500, 7500, 17500, 17500 }, true };
            routing.Observe(routes, true, 7);
            routing.ObservePoint(native.cursor);
            Assert::IsFalse(routing.Button(0, true, false));
            native.adapter = &drag;
            Assert::IsTrue(input::DispatchPointer({ 10017, 10000 }, 5, policy, 7, true, true, true, false, routing, ordinary, drag, native));
            SpeedNear(native.cursor.x, 10002);
            Assert::IsTrue(ordinary.Faulted() && drag.FeedbackSnapshot().valid);
        }

        void NativePointerFaultRecoversOnlyFromVerifiedIdleContextWithoutReplayingMotion()
        {
            using namespace glide;
            for (const bool writeRejected : { false, true })
            {
                input::routing::State routing;
                routing.Observe({}, true, 7);
                input::PointerSnapshot policy{ true, true, { 0, 0, 40000, 40000 }, 4, 7, {}, 1, 0.25, true };
                pointer::Adapter ordinary, drag;
                SpeedNative native;
                native.adapter = &ordinary;
                const auto send = [&](Vec2 candidate, std::uint64_t now) {
                    const bool consumed = input::DispatchPointer(candidate, now, policy, policy.epoch, true, true, true, false, routing, ordinary, drag, native);
                    if (!consumed)
                        native.cursor = candidate; // The original physical packet passes to Windows.
                    return consumed;
                };
                native.warpSuccess = !writeRejected;
                native.exactReadback = writeRejected;
                Assert::IsFalse(send({ 10016, 10000 }, 1));
                Assert::IsTrue(ordinary.FailureReason() == (writeRejected ? pointer::Failure::Warp : pointer::Failure::Readback));
                native.warpSuccess = native.exactReadback = true;
                const auto writes = native.writes;
                Assert::IsFalse(input::RecoverPointer(500, policy, 7, true, true, true, true, routing, ordinary, native));
                Assert::IsFalse(input::RecoverPointer(501, policy, 7, true, true, true, true, routing, ordinary, native));
                Assert::IsFalse(input::RecoverPointer(550, policy, 7, true, true, true, true, routing, ordinary, native));
                Assert::IsTrue(input::RecoverPointer(551, policy, 7, true, true, true, true, routing, ordinary, native));
                Assert::IsTrue(native.writes == writes && pointer::Same(native.cursor, { 10016, 10000 }));
                Assert::IsTrue(pointer::Same(ordinary.Remainder(), {}) && !ordinary.FeedbackSnapshot().valid);
                Assert::IsTrue(send({ 10032, 10000 }, 552));
                SpeedNear(native.cursor.x, 10017);
                Assert::IsTrue(ordinary.FeedbackSnapshot().valid);
                native.warpSuccess = false;
                Assert::IsFalse(send({ 10033, 10000 }, 553));
                native.warpSuccess = true;
                Assert::IsFalse(input::RecoverPointer(2000, policy, 7, true, true, true, true, routing, ordinary, native));
                Assert::IsTrue(ordinary.Faulted()); // Never repeatedly fight another owner in this epoch.
                ++policy.epoch;
                Assert::IsFalse(input::RecoverPointer(2000, policy, 8, true, true, true, true, routing, ordinary, native));
                Assert::IsTrue(input::RecoverPointer(2050, policy, 8, true, true, true, true, routing, ordinary, native));
            }
        }

        void PointerRecoveryRejectsHeldInputChangingCursorClipAndIncoherentControls()
        {
            using namespace glide;
            const input::PointerSnapshot policy{ true, true, { 0, 0, 40000, 40000 }, 4, 7, {}, 1, 0.25, true };
            for (unsigned blocked = 0; blocked < 9; ++blocked)
            {
                input::routing::State routing;
                routing.Observe({}, true, 7);
                routing.ObservePoint({ 10000, 10000 });
                pointer::Adapter ordinary, drag;
                SpeedNative native;
                native.adapter = &ordinary;
                native.warpSuccess = false;
                Assert::IsFalse(input::DispatchPointer({ 10016, 10000 }, 1, policy, 7, true, true, true, false, routing, ordinary, drag, native));
                native.warpSuccess = true;
                if (blocked == 0)
                    Assert::IsFalse(routing.Button(0, true, false));
                native.ready = blocked != 1; // Represents native desktop/DPI/full-clip refusal.
                native.canPlace = blocked != 8;
                if (blocked == 2)
                    native.cursor = { 40000, 10000 }; // Outside the active physical monitor.
                const auto writes = native.writes;
                Assert::IsFalse(input::RecoverPointer(600, policy, blocked == 3 ? 8 : 7, blocked != 4, blocked != 5, blocked != 6, blocked != 7, routing, ordinary, native));
                Assert::IsFalse(input::RecoverPointer(700, policy, blocked == 3 ? 8 : 7, blocked != 4, blocked != 5, blocked != 6, blocked != 7, routing, ordinary, native));
                Assert::IsTrue(ordinary.Faulted() && native.writes == writes);
                if (blocked == 0)
                    Assert::IsFalse(routing.Button(0, false, false));
                native.ready = true;
                native.canPlace = true;
                native.cursor = { 10000, 10000 };
                Assert::IsFalse(input::RecoverPointer(800, policy, 7, true, true, true, true, routing, ordinary, native));
                native.cursor.x += 1;
                Assert::IsFalse(input::RecoverPointer(850, policy, 7, true, true, true, true, routing, ordinary, native));
                Assert::IsTrue(input::RecoverPointer(900, policy, 7, true, true, true, true, routing, ordinary, native));
                Assert::IsTrue(native.writes == writes && !ordinary.Faulted());
            }
        }
    };
}

int main()
{
    try
    {
        GlideZoomTests::PointerSpeedTests suite;
        suite.InteriorGainUsesProcessedDisplacementAndIndependentAutomaticChoice();
        suite.FastGainTraversesTheRealExitBeyondTheOriginalCandidate();
        suite.UnityZoomAndOutsideMotionKeepWindowsExactAndDiscardResidual();
        suite.TinyFractionsAccumulateAtMinimumSpeedWithoutQuantizationLoss();
        suite.SameEpochSpeedOrAutomaticChangesReseedAndRevokeOldCursorFeedback();
        suite.NativeTransactionCancellationAndAppOwnedClipRemainAuthoritative();
        suite.PublicationCarriesBothIndependentChoicesAndInvalidGainIsAtomic();
        suite.HookDispatchAppliesSpeedWithoutAButtonAndKeepsTheSameGainDuringAndAfterDrag();
        suite.HookDispatchRevokesBothBaselinesWhenSharedControlsAreUnavailable();
        suite.OrdinaryNativeFailureIsDistinguishableFromATransientPermissionPause();
        suite.NativePointerFaultRecoversOnlyFromVerifiedIdleContextWithoutReplayingMotion();
        suite.PointerRecoveryRejectsHeldInputChangingCursorClipAndIncoherentControls();
        std::cout << "PASS: PointerSpeedTests (12 methods)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}