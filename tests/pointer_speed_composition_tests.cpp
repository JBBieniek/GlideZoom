// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "../src/control_channel.h"
#include "../src/pointer_dispatch.h"
#include "../src/settings_update_policy.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
    using glide::Rect;
    using glide::Transform;
    using glide::Vec2;

    std::uint64_t compositionAssertions = 0;
    std::uint64_t compositionPackets = 0;

    void CheckComposition(bool condition, const char* message)
    {
        ++compositionAssertions;
        if (!condition)
            throw std::runtime_error(message);
    }

    void CompositionNear(double actual, double expected, const char* message)
    {
        CheckComposition(std::isfinite(actual) && std::abs(actual - expected) < 1e-7, message);
    }

    // The fake native boundary supplies an already Windows-processed candidate
    // and a separately observed pre-event physical cursor. This does not model
    // undocumented MagSetInputTransform/WH_MOUSE_LL ordering or native velocity.
    struct CompositionNative
    {
        glide::pointer::Adapter* adapter = nullptr;
        Vec2 cursor{};
        unsigned writes = 0;
        unsigned dragChecks = 0;

        bool Ready(const glide::pointer::View&) const noexcept { return true; }
        bool Read(Vec2& output) const noexcept
        {
            output = cursor;
            return true;
        }
        bool CanPlace(Vec2) const noexcept { return true; }
        bool DragBounds(const glide::pointer::View& view, Rect& output) noexcept
        {
            ++dragChecks;
            output = view.bounds;
            return true;
        }
        bool Warp(Vec2 point) noexcept
        {
            ++writes;
            cursor = point;
            adapter->ObserveWarp(point);
            return true;
        }
    };

    struct CompositionDriver
    {
        glide::input::ControlChannel controls;
        glide::input::ControlReader reader;
        glide::input::State gestures;
        glide::input::PointerChannel pointerControls;
        glide::pointer::FeedbackChannel cursorFeedback;
        glide::input::routing::State routing;
        glide::input::routing::Snapshot routes;
        glide::input::PointerSnapshot policy;
        glide::pointer::Adapter ordinary;
        glide::pointer::Adapter drag;
        CompositionNative native;
        Transform applied;
        glide::settings::Values settings;
        std::uint64_t now = 1;

        CompositionDriver(double zoom, int percent, bool automatic, bool negative = false)
        {
            settings.pointerSpeed = percent;
            settings.pointerScaling = automatic;
            const Rect bounds = negative ? Rect{ -40000, -40000, 0, 0 } : Rect{ 0, 0, 40000, 40000 };
            native.cursor = { (bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2 };
            applied = { zoom, { native.cursor.x * (1 - 1 / zoom), native.cursor.y * (1 - 1 / zoom) } };
            policy = { true, zoom > 1, bounds, zoom, 7, {}, 1, percent / 100.0, automatic };
            routes.enabled = true;
            routes.generation = 1;
            routes.epoch = policy.epoch;
            routes.activeId = 1;
            routes.count = 1;
            const Vec2 sourceFirst = glide::ScreenToSource({ bounds.left, bounds.top }, applied);
            const Vec2 sourceLast = glide::ScreenToSource({ bounds.right, bounds.bottom }, applied);
            routes.routes[0] = { 1, bounds,
                                 { std::floor(sourceFirst.x), std::floor(sourceFirst.y), std::ceil(sourceLast.x), std::ceil(sourceLast.y) },
                                 zoom > 1 };
            Publish();
        }

        void Publish()
        {
            controls.Publish({ glide::input::DefaultControlOptions | 1, policy.epoch, policy.zoom });
            CheckComposition(reader.Synchronize(controls, gestures, [] { return true; }, [](glide::input::Decision) {}),
                             "control publication is coherent");
            CheckComposition(pointerControls.Publish(policy), "pointer policy publication succeeds");
            routes.epoch = reader.Epoch();
            routing.Observe(routes, true, reader.Epoch());
            routing.ObservePoint(native.cursor);
            CheckComposition(routing.ZoomReady(), "the committed monitor map admits pointer movement");
        }

        void Button(bool down)
        {
            routing.ObservePoint(native.cursor);
            const auto decision = gestures.ButtonEvent(glide::input::Button::Left, down, native.cursor, now++);
            CheckComposition(!routing.Button(0, down, decision.consume), "the physical drag pair is forwarded");
            ordinary.Pause();
            drag.EndConstraint();
            CheckComposition(routing.HasForwardedDrag() == down, "button ownership selects the drag adapter");
        }

        void Packet(Vec2 delta)
        {
            ++compositionPackets;
            const Vec2 candidate{ native.cursor.x + delta.x, native.cursor.y + delta.y };
            routing.Observe(routes, true, reader.Epoch());
            routing.ObservePoint(candidate);
            glide::input::PointerSnapshot snapshot;
            const bool coherent = pointerControls.Read(snapshot);
            const bool forwarded = routing.HasForwardedDrag();
            native.adapter = forwarded ? &drag : &ordinary;
            const auto decision = gestures.Move(candidate, now);
            const bool consumed = glide::input::DispatchPointer(candidate, now++, snapshot, reader.Epoch(), coherent,
                                                                (reader.Options() & 1) != 0, true, decision.consume,
                                                                routing, ordinary, drag, native);
            const auto feedback = native.adapter->FeedbackSnapshot();
            CheckComposition(cursorFeedback.Publish(feedback), "committed cursor feedback is published");
            if (!consumed)
                native.cursor = candidate;
            CheckComposition(!ordinary.Faulted() && !drag.Faulted(), "movement never faults either adapter");
            if (forwarded)
                CheckComposition(native.dragChecks != 0, "forwarded movement uses the production clip-aware path");
        }

        Vec2 Displayed()
        {
            glide::pointer::Feedback feedback;
            CheckComposition(cursorFeedback.Read(feedback), "the renderer reads a coherent cursor sample");
            Vec2 precise{};
            const unsigned writes = native.writes;
            CheckComposition(glide::pointer::ResolveCursor(feedback, native.cursor, applied, policy.bounds,
                                                          reader.Epoch(), true, precise, settings.pointerSpeed / 100.0,
                                                          settings.pointerScaling),
                             "fractional cursor resolves for the current applied transform and speed");
            CheckComposition(native.writes == writes, "render feedback does not write or re-scale the native pointer");
            return glide::SourceToScreen(precise, applied);
        }

        void LiveSpeed(int percent, bool automatic)
        {
            const Transform before = applied;
            auto candidate = settings;
            candidate.pointerSpeed = percent;
            candidate.pointerScaling = automatic;
            glide::settings::update::Context context;
            glide::settings::update::ObserveZoom(context, applied.zoom);
            CheckComposition(glide::settings::update::Classify(settings, candidate, context) == glide::settings::update::Action::Live,
                             "a compatible speed edit applies without resetting the live camera");
            CheckComposition(glide::settings::update::InputChanged(settings, candidate), "speed edits cancel old input fractions");
            settings = candidate;
            ++policy.epoch;
            ++routes.generation;
            ordinary.Pause();
            drag.EndConstraint();
            policy.speedMultiplier = percent / 100.0;
            policy.adjustForZoom = automatic;
            Publish();
            CompositionNear(applied.zoom, before.zoom, "live speed edits preserve zoom");
            CheckComposition(glide::pointer::Same(applied.offset, before.offset), "live speed edits preserve camera position");
        }
    };

    void SerializedVisibleGain()
    {
        for (const double zoom : { 1.25, 2.0, 4.0, 8.0, 16.0, 64.0 })
            for (const int percent : { 25, 100 })
                for (const bool forwarded : { false, true })
                    for (const bool negative : { false, true })
                    {
                        CompositionDriver driver(zoom, percent, true, negative);
                        if (forwarded)
                            driver.Button(true);
                        const Vec2 sourceStart = driver.native.cursor;
                        const Vec2 displayStart = glide::SourceToScreen(sourceStart, driver.applied);
                        for (unsigned packet = 1; packet <= 320; ++packet)
                        {
                            driver.Packet({ 1, -1 });
                            const auto screen = driver.Displayed();
                            const double expected = packet * percent / 100.0;
                            CompositionNear(screen.x - displayStart.x, expected, "visible horizontal gain applies percentage once");
                            CompositionNear(screen.y - displayStart.y, -expected, "visible vertical gain applies percentage once");
                            const auto repeated = driver.Displayed();
                            CompositionNear(repeated.x, screen.x, "a render tick without physical input cannot compound gain");
                            CompositionNear(repeated.y, screen.y, "repeated rendering keeps the same cursor point");
                        }
                        const auto source = glide::ScreenToSource(driver.Displayed(), driver.applied);
                        CompositionNear(source.x - sourceStart.x, 320 * percent / 100.0 / zoom,
                                        "source movement divides by zoom before its one display projection");
                        if (forwarded)
                            driver.Button(false);
                    }
    }

    void QuarterSpeedAtFourTimesZoom()
    {
        for (const bool forwarded : { false, true })
        {
            CompositionDriver driver(4, 25, true);
            if (forwarded)
                driver.Button(true);
            const Vec2 initial = driver.native.cursor;
            const auto start = glide::SourceToScreen(initial, driver.applied);
            driver.Packet({ 16, 0 }); // The very first measured packet after startup.
            CompositionNear(driver.native.cursor.x - initial.x, 1, "16 processed pixels become one source pixel at 25 percent and 4x");
            CompositionNear(driver.Displayed().x - start.x, 4, "the source pixel projects to four displayed pixels, not one");
            if (forwarded)
                driver.Button(false);
        }
    }

    void LiveChangesRevokeFractionsWithoutCompounding()
    {
        for (const bool forwarded : { false, true })
        {
            CompositionDriver driver(4, 100, true);
            if (forwarded)
                driver.Button(true);
            driver.Packet({ 1, 0 });
            const auto old = driver.native.adapter->FeedbackSnapshot();
            CompositionNear(driver.Displayed().x - glide::SourceToScreen(driver.native.cursor, driver.applied).x, 1,
                            "the pre-edit packet leaves a real fractional displayed pixel");
            driver.LiveSpeed(25, true);
            Vec2 ignored{ 123, 456 };
            CheckComposition(!glide::pointer::ResolveCursor(old, driver.native.cursor, driver.applied, driver.policy.bounds,
                                                           driver.reader.Epoch(), true, ignored, 0.25, true),
                             "pre-edit feedback cannot survive the live settings epoch");
            const auto start = glide::SourceToScreen(driver.native.cursor, driver.applied);
            for (unsigned packet = 1; packet <= 32; ++packet)
            {
                driver.Packet({ 1, 0 });
                CompositionNear(driver.Displayed().x - start.x, packet * 0.25, "new quarter speed starts immediately without reusing the prior fraction");
            }
            driver.LiveSpeed(25, false);
            const auto unadjustedStart = glide::SourceToScreen(driver.native.cursor, driver.applied);
            for (unsigned packet = 1; packet <= 32; ++packet)
            {
                driver.Packet({ 1, 0 });
                CompositionNear(driver.Displayed().x - unadjustedStart.x, static_cast<double>(packet),
                                "with auto adjustment disabled 25 percent source speed projects to 100 percent at 4x");
            }
            if (forwarded)
                driver.Button(false);
        }
    }

    void UnityZoomRetainsNativeSpeed()
    {
        for (const bool automatic : { false, true })
            for (const bool forwarded : { false, true })
            {
                CompositionDriver driver(1, 25, automatic);
                if (forwarded)
                    driver.Button(true);
                const auto initial = driver.native.cursor;
                driver.Packet({ 16, -8 });
                CompositionNear(driver.native.cursor.x - initial.x, 16, "unmagnified horizontal motion remains native");
                CompositionNear(driver.native.cursor.y - initial.y, -8, "unmagnified vertical motion remains native");
                CheckComposition(driver.native.writes == 0, "unity zoom never writes or applies the speed percentage");
                if (forwarded)
                    driver.Button(false);
            }
    }
}

int main()
{
    compositionAssertions = compositionPackets = 0;
    SerializedVisibleGain();
    QuarterSpeedAtFourTimesZoom();
    LiveChangesRevokeFractionsWithoutCompounding();
    UnityZoomRetainsNativeSpeed();
    std::cout << "PASS: " << compositionAssertions << " pointer speed composition assertions; "
              << compositionPackets << " processed packets; production dispatch/adapter/feedback/projection; "
                 "native MagSetInputTransform and hardware velocity not executed\n";
    return 0;
}
