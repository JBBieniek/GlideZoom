// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "test_assertions.h"
#include "../src/input_routing.h"
#include "../src/routing_point.h"
#include "../src/pointer_adapter.h"
#include "../src/zoom_session.h"
#include "../src/monitor_geometry.h"
#include "../src/viewport_motion.h"

#include <atomic>
#include <array>
#include <limits>
#include <thread>

using namespace glide_test;

namespace GlideZoomTests
{
    namespace
    {
        glide::input::routing::Snapshot Routes()
        {
            using namespace glide::input::routing;
            Snapshot snapshot;
            snapshot.enabled = true;
            snapshot.generation = 1;
            snapshot.epoch = 7;
            snapshot.activeId = 1;
            snapshot.count = 3;
            snapshot.routes[0] = { 1, { -100, 0, 0, 100 }, { -80, 20, -20, 80 }, true };
            snapshot.routes[1] = { 2, { 0, 0, 100, 100 }, { 20, 20, 80, 80 }, true };
            snapshot.routes[2] = { 3, { 100, 0, 200, 100 }, { 100, 0, 200, 100 }, false };
            return snapshot;
        }

        struct MappingBackend
        {
            glide::session::Snapshot current{};
            unsigned inputWrites = 0, visualWrites = 0;
            bool Read(glide::session::Snapshot& output) const
            {
                output = current;
                return true;
            }
            bool SetInput(glide::session::Input value)
            {
                ++inputWrites;
                current.input = value;
                return true;
            }
            bool SetVisual(glide::session::Visual value)
            {
                ++visualWrites;
                current.visual = value;
                return true;
            }
        };
        struct MappingJournal
        {
            bool armed = false;
            void Publish(const glide::session::RecoveryRecord& record)
            {
                Assert::IsTrue(glide::session::Valid(record));
                armed = true;
            }
            void Clear() { armed = false; }
        };
        glide::session::Snapshot Mapping(const glide::input::routing::Route& route)
        {
            const auto rectangle = [](glide::Rect value) {
                return glide::session::Rect{ static_cast<std::int32_t>(value.left), static_cast<std::int32_t>(value.top), static_cast<std::int32_t>(value.right), static_cast<std::int32_t>(value.bottom) };
            };
            return { { 1, 0, 0 }, { true, rectangle(route.source), rectangle(route.bounds) } };
        }
    }

    class RoutingTests
    {
    public:
        void VerifiedPhysicalOuterEdgeMoveDoesNotManufactureAHandoff()
        {
            using namespace glide::input::routing;
            struct Edge
            {
                glide::Vec2 raw, physical;
                std::size_t route;
            };
            const std::array<Edge, 8> edges{
                Edge{ { -101, 50 }, { -100, 50 }, 0 }, Edge{ { 200, 50 }, { 199, 50 }, 2 }, Edge{ { -50, -1 }, { -50, 0 }, 0 }, Edge{ { -50, 100 }, { -50, 99 }, 0 }, Edge{ { -101, -1 }, { -100, 0 }, 0 }, Edge{ { -101, 100 }, { -100, 99 }, 0 }, Edge{ { 200, -1 }, { 199, 0 }, 2 }, Edge{ { 200, 100 }, { 199, 99 }, 2 }
            };
            for (const auto& edge : edges)
            {
                auto snapshot = Routes();
                const auto& selected = snapshot.routes[edge.route];
                snapshot.activeId = selected.id;
                const glide::Vec2 initial{ (selected.bounds.left + selected.bounds.right) / 2, 50 };
                State original, normalized;
                original.Observe(snapshot, true, 7);
                normalized.Observe(snapshot, true, 7);
                original.ObservePoint(initial);
                normalized.ObservePoint(initial);
                original.ObservePoint(edge.raw);
                Assert::IsTrue(original.Report(true).targetId == 0);
                original.ObservePoint(edge.physical);
                const auto falseReturn = original.Report(true);
                Assert::IsTrue(falseReturn.targetId == selected.id && falseReturn.requestSerial > snapshot.ackSerial);

                MappingBackend native;
                MappingJournal journal;
                glide::session::Session session;
                if (selected.zoomed)
                    Assert::IsTrue(session.Apply(Mapping(selected), native, journal) == glide::session::Result::Applied);
                const auto mapping = native.current;
                const auto writes = native.inputWrites;
                const PhysicalPoint physical{ true, edge.physical, selected.id, selected.bounds };
                glide::Vec2 routePoint = edge.raw;
                Assert::IsTrue(CanNormalizeMove(true, false) && NeedsPhysicalMovePoint(snapshot, true, 7, edge.raw));
                Assert::IsTrue(TryPhysicalMovePoint(snapshot, true, 7, edge.raw, physical, routePoint));
                normalized.ObservePoint(routePoint);
                const auto after = normalized.Report(true);
                Assert::IsTrue(glide::pointer::Same(routePoint, edge.physical) && after.targetId == selected.id && after.requestSerial == snapshot.ackSerial && normalized.ZoomReady());
                Assert::IsTrue(native.current == mapping && native.inputWrites == writes);
                // Canonical routing does not expand the source crop or grant a click.
                Assert::IsTrue(normalized.BlockNewButton() == selected.zoomed);
                Assert::IsTrue(normalized.Button(0, true, false) == selected.zoomed);
                Assert::IsTrue(normalized.Button(0, false, false) == selected.zoomed);
            }
        }

        void PhysicalFallbackPreservesRealMonitorEntriesAndRejectsUnknownTopology()
        {
            using namespace glide::input::routing;
            const auto snapshot = Routes();
            const PhysicalPoint previous{ true, { -50, 50 }, 1, snapshot.routes[0].bounds };
            for (const glide::Vec2 raw : { glide::Vec2{ 0, 50 }, glide::Vec2{ 50, 50 }, glide::Vec2{ 100, 50 }, glide::Vec2{ 199, 99 } })
            {
                glide::Vec2 output{ 777, 888 };
                Assert::IsFalse(NeedsPhysicalMovePoint(snapshot, true, 7, raw));
                Assert::IsFalse(TryPhysicalMovePoint(snapshot, true, 7, raw, previous, output));
                Assert::IsTrue(glide::pointer::Same(output, glide::Vec2{ 777, 888 }));
                State hook;
                hook.Observe(snapshot, true, 7);
                hook.ObservePoint({ -50, 50 });
                hook.ObservePoint(raw);
                const auto report = hook.Report(true);
                Assert::IsTrue(report.requestSerial == 1 && report.targetId == (raw.x < 100 ? 2u : 3u));
            }
            const glide::Vec2 raw{ 200, 50 };
            const PhysicalPoint actualOther{ true, { 150, 50 }, 3, snapshot.routes[2].bounds };
            glide::Vec2 output{ 777, 888 };
            Assert::IsTrue(TryPhysicalMovePoint(snapshot, true, 7, raw, actualOther, output));
            State transferred;
            transferred.Observe(snapshot, true, 7);
            transferred.ObservePoint({ -50, 50 });
            transferred.ObservePoint(output);
            Assert::IsTrue(transferred.Report(true).targetId == 3 && transferred.Report(true).requestSerial == 1);

            auto gap = snapshot;
            gap.routes[1].bounds.top = gap.routes[1].source.top = 20;
            const PhysicalPoint stillInGap{ true, { 50, 10 }, 2, gap.routes[1].bounds };
            output = { 777, 888 };
            Assert::IsTrue(NeedsPhysicalMovePoint(gap, true, 7, { 50, 10 }));
            Assert::IsFalse(TryPhysicalMovePoint(gap, true, 7, { 50, 10 }, stillInGap, output));
            Assert::IsTrue(glide::pointer::Same(output, glide::Vec2{ 777, 888 }));
        }

        void PhysicalMoveFallbackRefusesStaleMalformedAndChangedMonitorEvidence()
        {
            using namespace glide::input::routing;
            for (unsigned scenario = 0; scenario < 22; ++scenario)
            {
                auto snapshot = Routes();
                bool coherent = true;
                std::uint64_t epoch = 7;
                glide::Vec2 raw{ -101, 50 }, output{ 777, 888 };
                PhysicalPoint physical{ true, { -100, 50 }, 1, snapshot.routes[0].bounds };
                switch (scenario)
                {
                case 0:
                    coherent = false;
                    break;
                case 1:
                    epoch = 8;
                    break;
                case 2:
                    snapshot.enabled = false;
                    break;
                case 3:
                    snapshot.generation = 0;
                    break;
                case 4:
                    snapshot.count = 0;
                    break;
                case 5:
                    snapshot.routes[1].id = 1;
                    break;
                case 6:
                    snapshot.routes[1].bounds.left = -1;
                    break;
                case 7:
                    raw.x = (std::numeric_limits<double>::quiet_NaN)();
                    break;
                case 8:
                    raw.x = -100.5;
                    break;
                case 9:
                    raw.x = -262145;
                    break;
                case 10:
                    physical.read = false;
                    break;
                case 11:
                    physical.monitorId = 0;
                    break;
                case 12:
                    physical.monitorId = 99;
                    break;
                case 13:
                    physical.point.x = -100.5;
                    break;
                case 14:
                    physical.point.x = (std::numeric_limits<double>::infinity)();
                    break;
                case 15:
                    physical.point.x = 0;
                    break; // Exclusive edge belongs to another monitor.
                case 16:
                    physical.bounds.left = -101;
                    break;
                case 17:
                    physical.bounds.right = -1;
                    break;
                case 18:
                    physical.bounds.top = -1;
                    break;
                case 19:
                    physical.bounds.bottom = 101;
                    break;
                case 20:
                    physical.bounds.left = physical.bounds.right;
                    break;
                case 21:
                    physical.bounds.left = -100.5;
                    break;
                }
                if (scenario < 10)
                    Assert::IsFalse(NeedsPhysicalMovePoint(snapshot, coherent, epoch, raw));
                else
                    Assert::IsTrue(NeedsPhysicalMovePoint(snapshot, coherent, epoch, raw));
                Assert::IsFalse(TryPhysicalMovePoint(snapshot, coherent, epoch, raw, physical, output));
                Assert::IsTrue(glide::pointer::Same(output, glide::Vec2{ 777, 888 }));
            }
        }

        void OnlyUninjectedMovementCanRequestFallbackAndOwedReleasesRemainNative()
        {
            using namespace glide::input::routing;
            const auto snapshot = Routes();
            const PhysicalPoint physical{ true, { -100, 50 }, 1, snapshot.routes[0].bounds };
            for (const bool move : { false, true })
                for (const bool injected : { false, true })
                {
                    glide::Vec2 observed{ -101, 50 };
                    unsigned nativeRequests = 0;
                    if (CanNormalizeMove(move, injected) && NeedsPhysicalMovePoint(snapshot, true, 7, observed))
                    {
                        ++nativeRequests;
                        Assert::IsTrue(TryPhysicalMovePoint(snapshot, true, 7, observed, physical, observed));
                    }
                    Assert::IsTrue(nativeRequests == (move && !injected ? 1u : 0u));
                    Assert::IsTrue(glide::pointer::Same(observed, move && !injected ? physical.point : glide::Vec2{ -101, 50 }));
                }
            State hook;
            hook.Observe(snapshot, true, 7);
            hook.ObservePoint({ -50, 50 });
            Assert::IsFalse(hook.Button(0, true, false));
            glide::Vec2 canonical{ -101, 50 };
            Assert::IsTrue(TryPhysicalMovePoint(snapshot, true, 7, canonical, physical, canonical));
            hook.ObservePoint(canonical);
            Assert::IsTrue(hook.HasForwardedDrag() && hook.Report(true).requestSerial == 0);
            Assert::IsFalse(hook.Button(0, false, false));
            Assert::IsFalse(hook.AnyButtonDown());
            // Actual button/wheel observations remain raw and retain the normal gate.
            hook.ObservePoint({ 50, 50 });
            Assert::IsTrue(hook.BlockNewButton() && hook.BlockWheel(true));
            Assert::IsTrue(hook.Button(0, true, false) && hook.Button(0, false, false));
        }

        void EntryRequestsHandoffAndFirstUnreadyClickKeepsItsPair()
        {
            using namespace glide::input::routing;
            State state;
            auto snapshot = Routes();
            state.Observe(snapshot, true, 7);
            state.ObservePoint({ -50, 50 });
            Assert::IsTrue(state.ZoomReady());
            Assert::IsFalse(state.BlockNewButton());
            state.ObservePoint({ 50, 50 });
            auto request = state.Report(true);
            Assert::IsTrue(request.requestSerial == 1 && request.targetId == 2);
            Assert::IsTrue(state.BlockNewButton());
            Assert::IsTrue(state.Button(0, true, false));

            snapshot.busy = true;
            snapshot.generation = 2;
            state.Observe(snapshot, true, 7);
            request = state.Report(true);
            Assert::IsTrue(request.canCommit && request.observedGeneration == 2 && request.forwardedButtons == 0);
            snapshot.busy = false;
            snapshot.generation = 3;
            snapshot.activeId = 2;
            snapshot.ackSerial = request.requestSerial;
            state.Observe(snapshot, true, 7);
            Assert::IsTrue(state.ZoomReady());
            Assert::IsTrue(state.Button(0, false, false));
            Assert::IsFalse(state.Button(0, false, false));
            Assert::IsFalse(state.Button(0, true, false));
            Assert::IsTrue(state.Report(true).forwardedButtons == 1);
            Assert::IsFalse(state.Button(0, false, false));
        }

        void BusyBarrierWaitsForForwardedPairsAndNeverStealsARepeat()
        {
            using namespace glide::input::routing;
            for (unsigned button = 0; button < 5; ++button)
            {
                State state;
                auto snapshot = Routes();
                state.Observe(snapshot, true, 7);
                state.ObservePoint({ -50, 50 });
                Assert::IsFalse(state.Button(button, true, false));
                snapshot.busy = true;
                snapshot.generation = 2;
                state.Observe(snapshot, true, 7);
                Assert::IsFalse(state.Report(true).canCommit);
                Assert::IsFalse(state.Button(button, true, false));
                Assert::IsFalse(state.Button(button, false, false));
                Assert::IsTrue(state.Report(true).canCommit);
                Assert::IsTrue(state.Button(button, true, false));
                Assert::IsTrue(state.HasBlockedButtons());
                Assert::IsTrue(state.Button(button, false, false));
                Assert::IsFalse(state.HasBlockedButtons());
            }
        }

        void OutsideZoomRemainsNativeAndDisabledModeRetainsOwedUps()
        {
            using namespace glide::input::routing;
            State state;
            auto snapshot = Routes();
            state.Observe(snapshot, true, 7);
            state.ObservePoint({ 150, 50 });
            Assert::IsFalse(state.BlockNewButton());
            Assert::IsFalse(state.BlockWheel(false));
            Assert::IsTrue(state.BlockWheel(true));
            Assert::IsFalse(state.Button(1, true, false));
            Assert::IsFalse(state.Button(1, false, false));
            state.ObservePoint({ 50, 50 });
            Assert::IsTrue(state.Button(2, true, false));
            snapshot.enabled = false;
            state.Observe(snapshot, true, 7);
            Assert::IsFalse(state.BlockNewButton());
            Assert::IsTrue(state.Button(2, false, false));
            Assert::IsFalse(state.Button(2, false, false));
        }

        void OnlyCoherentIdleUnzoomedRoutesCanRequestInitialWheelIntent()
        {
            using namespace glide::input::routing;
            State state;
            auto snapshot = Routes();
            const glide::Vec2 entry{ 150, 50 };
            state.Observe(snapshot, true, 7);
            state.ObservePoint(entry);
            Assert::IsTrue(state.BlockWheel(true) && state.CanRequestWheel(entry));
            Assert::IsFalse(state.BlockNewButton()); // Camera intent grants no click authority.
            Assert::IsFalse(state.CanRequestWheel({ 50, 50 })); // Already zoomed neighbor.
            Assert::IsFalse(state.Button(0, true, false));
            Assert::IsFalse(state.CanRequestWheel(entry));
            Assert::IsFalse(state.Button(0, false, false));
            snapshot.busy = true;
            ++snapshot.generation;
            state.Observe(snapshot, true, 7);
            Assert::IsFalse(state.CanRequestWheel(entry));
            Assert::IsTrue(state.Busy() && state.CanRequestWheel(entry, true));
            Assert::IsFalse(state.CanRequestWheel({ 50, 50 }, true));
            Assert::IsTrue(state.Button(0, true, false));
            Assert::IsFalse(state.CanRequestWheel(entry, true));
            Assert::IsTrue(state.Button(0, false, false));
            snapshot.busy = false;
            state.Observe(snapshot, false, 7);
            Assert::IsFalse(state.CanRequestWheel(entry));
            state.Observe(snapshot, true, 8);
            Assert::IsFalse(state.CanRequestWheel(entry));
            snapshot.enabled = false;
            state.Observe(snapshot, true, 7);
            Assert::IsFalse(state.CanRequestWheel(entry));
        }

        void StaleAcknowledgementsAndIncoherentPublicationCannotAuthorizeZoomedClicks()
        {
            using namespace glide::input::routing;
            State state;
            auto snapshot = Routes();
            state.Observe(snapshot, true, 7);
            state.ObservePoint({ -50, 50 });
            state.ObservePoint({ 50, 50 });
            const auto old = state.Report(true);
            state.ObservePoint({ -50, 50 });
            const auto latest = state.Report(true);
            Assert::IsTrue(latest.requestSerial > old.requestSerial && latest.targetId == 1);
            snapshot.activeId = 2;
            snapshot.ackSerial = old.requestSerial;
            ++snapshot.generation;
            state.Observe(snapshot, true, 7);
            Assert::IsTrue(state.BlockNewButton());
            snapshot.activeId = 1;
            snapshot.ackSerial = latest.requestSerial;
            ++snapshot.generation;
            state.Observe(snapshot, true, 7);
            Assert::IsFalse(state.BlockNewButton());
            state.Observe(snapshot, false, 7);
            Assert::IsTrue(state.BlockNewButton());
            Assert::IsTrue(state.Report(true).observedGeneration == 0);
            state.Observe(snapshot, true, 8);
            Assert::IsTrue(state.BlockNewButton());
            Assert::IsFalse(state.Report(true).canCommit);
        }

        void ForwardedDragKeepsItsMonitorWhileSourceCropMoves()
        {
            using namespace glide::input::routing;
            State state;
            auto snapshot = Routes();
            state.Observe(snapshot, true, 7);
            state.ObservePoint({ -50, 50 });
            Assert::IsFalse(state.Button(0, true, false));
            snapshot.routes[0].source = { -90, 10, -10, 90 };
            ++snapshot.generation;
            state.Observe(snapshot, true, 7);
            Route captured;
            Assert::IsFalse(state.DragConstraint({ -50, 50 }, captured));
            Assert::IsFalse(state.DragConstraint({ -5, 50 }, captured));
            Assert::IsTrue(state.DragConstraint({ 50, 50 }, captured));
            Assert::IsTrue(captured.id == 1 && captured.source.left == -90 && captured.source.right == -10);
            const auto serial = state.Report(true).requestSerial;
            state.ObservePoint({ 50, 50 });
            Assert::IsTrue(state.Report(true).targetId == 1 && state.Report(true).requestSerial == serial);
            Assert::IsTrue(state.BlockNewButton());
            Assert::IsTrue(state.Button(1, true, false));
            Assert::IsTrue(state.Button(1, false, false));
            snapshot.busy = true;
            state.Observe(snapshot, true, 7);
            Assert::IsFalse(state.Report(true).canCommit);
            Assert::IsFalse(state.Button(0, false, false));
            Assert::IsFalse(state.DragConstraint({ 50, 50 }, captured));
            Assert::IsTrue(state.Report(true).canCommit);
        }

        void ForwardedWindowDragPansBeyondItsDownCropWithoutAHandoff()
        {
            using namespace glide;
            input::routing::State hook;
            auto publication = Routes();
            const Rect bounds = publication.routes[0].bounds;
            const Vec2 center{ -50, 50 };
            auto camera = AnchorZoom({}, 2, center, bounds);
            monitor::Frame frame;
            Assert::IsTrue(monitor::MakeFrame(bounds, camera, frame));
            camera = frame.transform;
            publication.routes[0].source = monitor::AsRect(frame.source);
            hook.Observe(publication, true, 7);
            hook.ObservePoint(center);
            Assert::IsFalse(hook.Button(0, true, false));
            const auto serial = hook.Report(true).requestSerial;
            const auto downCrop = publication.routes[0].source;
            viewport::Follow follow;
            Assert::IsTrue(follow.Rebase(center, bounds));
            session::Session session;
            MappingBackend native;
            MappingJournal journal;
            Assert::IsTrue(session.Apply(Mapping(publication.routes[0]), native, journal) == session::Result::Applied);
            for (const double x : { -24.0, -15.0, -1.0 })
            {
                const Vec2 accepted{ x, 50 };
                Assert::IsTrue(accepted.x >= downCrop.right);
                input::routing::Route constraint;
                Assert::IsFalse(hook.DragConstraint(accepted, constraint));
                hook.ObservePoint(accepted);
                Assert::IsTrue(follow.Advance(camera, accepted, bounds, true, viewport::FollowMode::Edges, center, camera));
                Assert::IsTrue(monitor::MakeFrame(bounds, camera, frame));
                publication.routes[0].source = monitor::AsRect(frame.source);
                ++publication.generation;
                Assert::IsTrue(session.Apply(Mapping(publication.routes[0]), native, journal) == session::Result::Applied);
                hook.Observe(publication, true, 7);
                Assert::IsTrue(hook.ZoomReady() && hook.Report(true).targetId == 1 &&
                               hook.Report(true).requestSerial == serial && !hook.Faulted());
            }
            Assert::IsTrue(native.inputWrites == 4 && publication.routes[0].source.right > downCrop.right && journal.armed);
            // Even an uncertain outside packet or another gesture consumer may
            // not swallow an UP whose native DOWN already reached the app.
            hook.ObservePoint({ 50, 50 });
            Assert::IsFalse(hook.Button(0, false, true));
            Assert::IsTrue(hook.Report(true).forwardedButtons == 0);
            hook.ObservePoint({ 50, 50 });
            Assert::IsTrue(hook.Report(true).targetId == 2 && hook.Report(true).requestSerial > serial);
        }

        void ApplicationClipAndFailedBoundaryCorrectionKeepPhysicalReleaseUsable()
        {
            using namespace glide;
            struct Native
            {
                Vec2 cursor{ -50, 50 };
                Rect clip{ -70, 20, -30, 80 };
                bool warp = true, clipRace = false;
                unsigned writes = 0;
                bool DragBounds(const pointer::View& view, Rect& output) const noexcept
                {
                    output = { std::max(view.bounds.left, clip.left), std::max(view.bounds.top, clip.top), std::min(view.bounds.right, clip.right), std::min(view.bounds.bottom, clip.bottom) };
                    return pointer::ValidBounds(output);
                }
                bool Read(Vec2& output) const noexcept
                {
                    output = cursor;
                    return true;
                }
                static bool CanPlace(Vec2) noexcept { return true; }
                bool Warp(Vec2 point) noexcept
                {
                    ++writes;
                    if (!warp)
                        return false;
                    if (clipRace)
                        clip.right = -40;
                    cursor = { std::clamp(point.x, clip.left, clip.right - 1), std::clamp(point.y, clip.top, clip.bottom - 1) };
                    return true;
                }
            } native;
            const pointer::View view{ true, true, { -100, 0, 0, 100 }, 2, 7, {}, 1 };
            pointer::DragNative<Native> clipped{ native };
            Assert::IsTrue(clipped.Initialize(view) && clipped.Limit().left == -70 && clipped.Limit().right == -30);
            Assert::IsFalse(clipped.CanPlace({ -20, 50 }));
            pointer::Adapter adapter;
            Assert::IsTrue(adapter.Move({ -40, 50 }, false, 1, view, true, false, clipped));
            Assert::IsTrue(native.cursor.x == -45 && native.writes == 1 && !adapter.Faulted());
            for (const bool failedReadback : { false, true })
            {
                Native failing;
                failing.warp = failedReadback;
                failing.clipRace = failedReadback;
                pointer::DragNative<Native> context{ failing };
                Assert::IsTrue(context.Initialize(view));
                pointer::Adapter dragOnly;
                input::routing::State hook;
                hook.Observe(Routes(), true, 7);
                hook.ObservePoint({ -50, 50 });
                Assert::IsFalse(hook.Button(0, true, false));
                Assert::IsTrue(dragOnly.Constrain({ 50, 50 }, 2, view, context.Limit(), context) == pointer::ConstraintResult::Failed);
                Assert::IsFalse(hook.Button(0, false, true));
                Assert::IsFalse(hook.Faulted());
                dragOnly.EndConstraint();
                Assert::IsFalse(dragOnly.Faulted());
                failing.warp = true;
                failing.clipRace = false;
                failing.clip = { -70, 20, -30, 80 };
                failing.cursor = { -50, 50 };
                Assert::IsTrue(context.Initialize(view));
                Assert::IsTrue(dragOnly.Constrain({ 50, 50 }, 100, view, context.Limit(), context) == pointer::ConstraintResult::Consumed);
                Assert::IsTrue(failing.cursor.x == -31 && !dragOnly.Faulted());
            }
        }

        void VerifiedDragBoundaryDoesNotChangeInteriorMotionOrGlobalSettings()
        {
            using namespace glide;
            struct Native
            {
                Vec2 cursor{ -50, 50 };
                bool ready = true, warp = true, readback = true;
                unsigned writes = 0, reads = 0;
                bool Ready(const pointer::View&) const noexcept { return ready; }
                bool Read(Vec2& output) noexcept
                {
                    ++reads;
                    if (!readback && writes)
                        return false;
                    output = cursor;
                    return true;
                }
                static bool CanPlace(Vec2) noexcept { return true; }
                bool Warp(Vec2 point) noexcept
                {
                    ++writes;
                    if (!warp)
                        return false;
                    cursor = point;
                    return true;
                }
            } native;
            pointer::Adapter adapter;
            const pointer::View view{ true, true, { -100, 0, 0, 100 }, 1, 7, {}, 1 };
            const Rect crop{ -80, 20, -20, 80 };
            Assert::IsTrue(adapter.Constrain({ -45, 55 }, 1, view, crop, native) == pointer::ConstraintResult::Passed);
            Assert::IsTrue(native.writes == 0 && native.reads == 0);
            Assert::IsTrue(adapter.Constrain({ 50, 55 }, 2, view, crop, native) == pointer::ConstraintResult::Consumed);
            Assert::IsTrue(native.cursor.x == -21 && native.cursor.y == 55 && native.writes == 1);
            Assert::IsTrue(adapter.BypassEcho(native.cursor, 3));
            for (unsigned failure = 0; failure < 3; ++failure)
            {
                Native failing;
                failing.ready = failure != 0;
                failing.warp = failure != 1;
                failing.readback = failure != 2;
                pointer::Adapter fresh;
                Assert::IsTrue(fresh.Constrain({ 50, 55 }, 4, view, crop, failing) == pointer::ConstraintResult::Failed);
            }
        }

        void SharedSessionCannotChangeMonitorsUntilBusyEpochAndForwardedDragAreAcknowledged()
        {
            using namespace glide::input::routing;
            State hook;
            auto publication = Routes();
            hook.Observe(publication, true, 7);
            hook.ObservePoint({ -50, 50 });
            glide::session::Session session;
            MappingBackend native;
            MappingJournal journal;
            const auto first = Mapping(publication.routes[0]);
            const auto second = Mapping(publication.routes[1]);
            Assert::IsTrue(session.Apply(first, native, journal) == glide::session::Result::Applied);
            Assert::IsFalse(hook.Button(0, true, false));
            hook.ObservePoint({ 50, 50 });
            Route captured;
            Assert::IsTrue(hook.DragConstraint({ 50, 50 }, captured) && captured.id == 1);
            publication.busy = true;
            publication.generation = 2;
            publication.epoch = 8;
            hook.Observe(publication, true, 7);
            Assert::IsFalse(hook.Report(true).canCommit);
            hook.Observe(publication, true, 8);
            Assert::IsFalse(hook.Report(true).canCommit);
            Assert::IsTrue(native.current == first && native.inputWrites == 1 && session.OwnsState());
            // The physical release is delivered at the verified original crop;
            // only a subsequent unheld entry can authorize the new mapping.
            hook.ObservePoint({ -50, 50 });
            Assert::IsFalse(hook.Button(0, false, false));
            hook.ObservePoint({ 50, 50 });
            Assert::IsFalse(hook.Report(false).canCommit);
            const auto acknowledged = hook.Report(true);
            Assert::IsTrue(acknowledged.canCommit && acknowledged.epoch == 8 && acknowledged.observedGeneration == 2);
            // A cancellation during renderer preparation supersedes even a
            // previously usable acknowledgement before any Session write.
            ++publication.generation;
            ++publication.epoch;
            hook.Observe(publication, true, 8);
            Assert::IsFalse(hook.Report(true).canCommit);
            Assert::IsTrue(acknowledged.observedGeneration != publication.generation && native.current == first);
            hook.Observe(publication, true, 9);
            const auto fresh = hook.Report(true);
            Assert::IsTrue(fresh.canCommit && fresh.observedGeneration == publication.generation && fresh.epoch == 9);
            Assert::IsTrue(session.Apply(second, native, journal) == glide::session::Result::Applied);
            Assert::IsTrue(native.current == second && native.inputWrites == 2 && native.visualWrites == 0 && journal.armed);
            publication.activeId = 2;
            publication.ackSerial = fresh.requestSerial;
            publication.busy = false;
            ++publication.generation;
            hook.Observe(publication, true, 9);
            Assert::IsTrue(hook.ZoomReady());
            Assert::IsFalse(hook.Button(0, true, false));
            Assert::IsFalse(hook.Button(0, false, false));
        }

        void OneGlobalMappingRetainsTwoIndependentCamerasAndIdentityOnlyReleasesTheActiveMapping()
        {
            using namespace glide::input::routing;
            auto publication = Routes();
            // These immutable submitted cameras stand in for the two renderer
            // instances: Session owns the one global map, neither camera.
            const auto parkedCamera = Mapping(publication.routes[0]);
            const auto activeCamera = Mapping(publication.routes[1]);
            glide::session::Session session;
            MappingBackend native;
            MappingJournal journal;
            Assert::IsTrue(session.Apply(parkedCamera, native, journal) == glide::session::Result::Applied);
            State hook;
            hook.Observe(publication, true, 7);
            hook.ObservePoint({ 50, 50 });
            publication.busy = true;
            ++publication.generation;
            hook.Observe(publication, true, 7);
            auto feedback = hook.Report(true);
            Assert::IsTrue(feedback.canCommit);
            Assert::IsTrue(session.Apply(activeCamera, native, journal) == glide::session::Result::Applied);
            Assert::IsTrue(native.current == activeCamera && session.Expected() == activeCamera);
            Assert::IsTrue(Mapping(publication.routes[0]) == parkedCamera && Mapping(publication.routes[1]) == activeCamera);

            // The selected monitor reaches 1x while the first monitor remains
            // visibly zoomed. Restoring global input has no camera side effect.
            Assert::IsTrue(session.Reset(native, journal) == glide::session::Result::Applied);
            Assert::IsTrue(glide::session::IsIdentity(native.current) && !session.NeedsRecovery() && !journal.armed);
            publication.routes[1].zoomed = false;
            publication.routes[1].source = publication.routes[1].bounds;
            publication.activeId = 2;
            publication.ackSerial = feedback.requestSerial;
            publication.busy = false;
            ++publication.generation;
            hook.Observe(publication, true, 7);
            Assert::IsTrue(hook.ZoomReady());
            Assert::IsFalse(hook.Button(0, true, false));
            Assert::IsFalse(hook.Button(0, false, false));
            Assert::IsTrue(publication.routes[0].zoomed && Mapping(publication.routes[0]) == parkedCamera);
            hook.ObservePoint({ -50, 50 });
            Assert::IsTrue(hook.BlockNewButton());
            publication.busy = true;
            ++publication.generation;
            hook.Observe(publication, true, 7);
            feedback = hook.Report(true);
            Assert::IsTrue(feedback.canCommit);
            Assert::IsTrue(session.Apply(parkedCamera, native, journal) == glide::session::Result::Applied);
            publication.activeId = 1;
            publication.ackSerial = feedback.requestSerial;
            publication.busy = false;
            ++publication.generation;
            hook.Observe(publication, true, 7);
            Assert::IsTrue(hook.ZoomReady() && native.current == parkedCamera && native.visualWrites == 0);
        }

        void CommitRequiresCurrentGenerationEpochAndRecentHookAcknowledgement()
        {
            using namespace glide::input::routing;
            State hook;
            auto publication = Routes();
            publication.busy = true;
            hook.Observe(publication, true, 7);
            hook.ObservePoint({ -50, 50 });
            const auto feedback = hook.Report(true, 1000);
            FeedbackChannel channel;
            Assert::IsTrue(channel.Publish(feedback));
            Feedback read;
            Assert::IsTrue(channel.Read(read) && read.reportedAt == 1000);
            Assert::IsTrue(CommitReady(read, 1, 7, 1000));
            Assert::IsTrue(CommitReady(read, 1, 7, 1250));
            Assert::IsFalse(CommitReady(read, 1, 7, 1251));
            Assert::IsFalse(CommitReady(read, 1, 7, 999));
            Assert::IsFalse(CommitReady(read, 2, 7, 1000));
            Assert::IsFalse(CommitReady(read, 1, 8, 1000));
            read.reportedAt = 0;
            Assert::IsFalse(CommitReady(read, 1, 7, 1000));
            read = feedback;
            read.canCommit = false;
            Assert::IsFalse(CommitReady(read, 1, 7, 1000));
            read = feedback;
            read.faulted = true;
            Assert::IsFalse(CommitReady(read, 1, 7, 1000));
        }

        void PauseOrQueuedCaptureKeepsVisibleRoutesGatedUntilIdentityIsVerified()
        {
            using namespace glide::input::routing;
            State hook;
            auto publication = Routes();
            hook.Observe(publication, true, 7);
            hook.ObservePoint({ -50, 50 });
            Assert::IsFalse(hook.Button(0, true, false));
            glide::session::Session session;
            MappingBackend native;
            MappingJournal journal;
            const auto original = Mapping(publication.routes[0]);
            Assert::IsTrue(session.Apply(original, native, journal) == glide::session::Result::Applied);
            // Publishing busy must retain the known visible routes while the
            // old journaled mapping is being released, even with starts paused.
            publication.busy = true;
            publication.epoch = 8;
            ++publication.generation;
            hook.Observe(publication, true, 8);
            Assert::IsFalse(CommitReady(hook.Report(true, 1000), 2, 8, 1000));
            Assert::IsTrue(hook.Button(1, true, false));
            Assert::IsFalse(hook.Button(0, false, false));
            const auto barrier = hook.Report(true, 1010);
            Assert::IsTrue(CommitReady(barrier, 2, 8, 1010));
            Assert::IsTrue(native.current == original && native.inputWrites == 1);
            Assert::IsTrue(session.Reset(native, journal) == glide::session::Result::Applied);
            Assert::IsTrue(glide::session::IsIdentity(native.current) && !journal.armed);
            // The UI may disable routing only after the windows are hidden and
            // native identity is observed. Its previously blocked UP is owed.
            publication.enabled = false;
            ++publication.generation;
            hook.Observe(publication, true, 8);
            Assert::IsTrue(hook.Button(1, false, false));
            Assert::IsFalse(hook.Button(1, true, false));
            Assert::IsFalse(hook.Button(1, false, false));
        }

        void PublicationRejectsInvalidGeometryAndPreservesCoherentSnapshots()
        {
            using namespace glide::input::routing;
            Channel channel;
            auto snapshot = Routes();
            Assert::IsTrue(channel.Publish(snapshot));
            auto invalid = snapshot;
            invalid.routes[1].id = 1;
            Assert::IsFalse(channel.Publish(invalid));
            invalid = snapshot;
            invalid.routes[1].bounds.left = -1;
            Assert::IsFalse(channel.Publish(invalid));
            Snapshot read;
            Assert::IsTrue(channel.Read(read) && read.activeId == snapshot.activeId && read.routes[1].id == 2);
            std::atomic<bool> finished{ false }, failed{ false };
            std::thread writer([&] {
                for (std::uint64_t index = 2; index < 10000; ++index)
                {
                    snapshot.generation = index;
                    snapshot.ackSerial = index * 2;
                    if (!channel.Publish(snapshot))
                        failed.store(true);
                }
                finished.store(true);
            });
            while (!finished.load())
                if (channel.Read(read) && read.generation > 1 &&
                    (!Valid(read) || read.ackSerial != read.generation * 2))
                    failed.store(true);
            writer.join();
            Assert::IsFalse(failed.load());
        }
    };
}


int main()
{
    try
    {
        GlideZoomTests::RoutingTests suite;
        suite.VerifiedPhysicalOuterEdgeMoveDoesNotManufactureAHandoff();
        suite.PhysicalFallbackPreservesRealMonitorEntriesAndRejectsUnknownTopology();
        suite.PhysicalMoveFallbackRefusesStaleMalformedAndChangedMonitorEvidence();
        suite.OnlyUninjectedMovementCanRequestFallbackAndOwedReleasesRemainNative();
        suite.EntryRequestsHandoffAndFirstUnreadyClickKeepsItsPair();
        suite.BusyBarrierWaitsForForwardedPairsAndNeverStealsARepeat();
        suite.OutsideZoomRemainsNativeAndDisabledModeRetainsOwedUps();
        suite.OnlyCoherentIdleUnzoomedRoutesCanRequestInitialWheelIntent();
        suite.StaleAcknowledgementsAndIncoherentPublicationCannotAuthorizeZoomedClicks();
        suite.ForwardedDragKeepsItsMonitorWhileSourceCropMoves();
        suite.ForwardedWindowDragPansBeyondItsDownCropWithoutAHandoff();
        suite.ApplicationClipAndFailedBoundaryCorrectionKeepPhysicalReleaseUsable();
        suite.VerifiedDragBoundaryDoesNotChangeInteriorMotionOrGlobalSettings();
        suite.SharedSessionCannotChangeMonitorsUntilBusyEpochAndForwardedDragAreAcknowledged();
        suite.OneGlobalMappingRetainsTwoIndependentCamerasAndIdentityOnlyReleasesTheActiveMapping();
        suite.CommitRequiresCurrentGenerationEpochAndRecentHookAcknowledgement();
        suite.PauseOrQueuedCaptureKeepsVisibleRoutesGatedUntilIdentityIsVerified();
        suite.PublicationRejectsInvalidGeometryAndPreservesCoherentSnapshots();
        std::cout << "PASS: RoutingTests (18 methods)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
