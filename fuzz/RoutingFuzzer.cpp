// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "../src/input_routing.h"
#include "../src/routing_point.h"
#include "../src/wheel_acceleration.h"
#include "../src/wheel_activation.h"
#include "../src/shortcut_commands.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <limits>

void FuzzPointerSpeed(const std::uint8_t* data, std::size_t size);

namespace
{
    void FuzzPhysicalRoutingPoint(const std::uint8_t* data, std::size_t size)
    {
        using namespace glide::input::routing;
        for (std::size_t at = 0; at + 7 < size && at < 1024; at += 8)
        {
            Snapshot snapshot;
            snapshot.enabled = true;
            snapshot.generation = 1;
            snapshot.epoch = 7;
            snapshot.activeId = 1;
            snapshot.count = 2;
            snapshot.routes[0] = { 1, { -1920, 0, 0, 1080 }, { -1440, 270, -480, 810 }, true };
            snapshot.routes[1] = { 2, { 0, -200, 2560, 1440 }, { 640, 210, 1920, 1030 }, true };
            const auto index = data[at] & 1u;
            const auto route = snapshot.routes[index];
            PhysicalPoint physical{ true, { (route.bounds.left + route.bounds.right) / 2, (route.bounds.top + route.bounds.bottom) / 2 }, route.id, route.bounds };
            glide::Vec2 raw{ (static_cast<int>(data[at + 1]) - 128) * 32.0, (static_cast<int>(data[at + 2]) - 128) * 16.0 };
            bool coherent = true;
            std::uint64_t epoch = 7;
            switch (data[at + 3] % 14)
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
                physical.read = false;
                break;
            case 5:
                physical.monitorId = 99;
                break;
            case 6:
                ++physical.bounds.right;
                break;
            case 7:
                physical.point.x = physical.bounds.right;
                break;
            case 8:
                physical.point.y = (std::numeric_limits<double>::quiet_NaN)();
                break;
            case 9:
                raw.x = (std::numeric_limits<double>::infinity)();
                break;
            case 10:
                raw.x += 0.5;
                break;
            case 11:
                snapshot.routes[1].id = 1;
                break;
            case 12:
                snapshot.count = MaximumRoutes + 1;
                break;
            default:
                break;
            }
            bool expectedNeeds = coherent && snapshot.enabled && snapshot.epoch == epoch && Valid(snapshot) && glide::pointer::ValidPoint(raw);
            if (expectedNeeds)
                for (std::size_t routeIndex = 0; routeIndex < snapshot.count; ++routeIndex)
                    if (glide::pointer::Contains(snapshot.routes[routeIndex].bounds, raw))
                        expectedNeeds = false;
            if (NeedsPhysicalMovePoint(snapshot, coherent, epoch, raw) != expectedNeeds)
                std::abort();
            bool eligiblePhysical = physical.read && physical.monitorId && glide::pointer::ValidPoint(physical.point) && glide::pointer::ValidBounds(physical.bounds) && glide::pointer::Contains(physical.bounds, physical.point);
            bool found = false;
            if (expectedNeeds && eligiblePhysical)
                for (std::size_t routeIndex = 0; routeIndex < snapshot.count; ++routeIndex)
                    found = found || (snapshot.routes[routeIndex].id == physical.monitorId && glide::pointer::Same(snapshot.routes[routeIndex].bounds, physical.bounds));
            const glide::Vec2 untouched{ 123, 456 };
            glide::Vec2 output = untouched;
            const bool accepted = TryPhysicalMovePoint(snapshot, coherent, epoch, raw, physical, output);
            if (accepted != (expectedNeeds && eligiblePhysical && found) ||
                !glide::pointer::Same(output, accepted ? physical.point : untouched))
                std::abort();
            const bool move = (data[at + 4] & 1) != 0, injected = (data[at + 5] & 1) != 0;
            if (CanNormalizeMove(move, injected) != (move && !injected))
                std::abort();
            if (accepted && CanNormalizeMove(move, injected))
            {
                State state;
                state.Observe(snapshot, true, epoch);
                state.ObservePoint({ -960, 540 });
                const auto prior = state.Report(true);
                state.ObservePoint(output);
                const auto after = state.Report(true);
                if (after.targetId != physical.monitorId || after.faulted ||
                    (physical.monitorId == snapshot.activeId && after.requestSerial != prior.requestSerial))
                    std::abort();
            }
        }
    }

    void FuzzWheel(const std::uint8_t* data, std::size_t size)
    {
        glide::wheel::Accelerator accelerator;
        std::uint64_t now = 100, epoch = 1;
        for (std::size_t at = 0; at + 3 < size && at < 2048; at += 4)
        {
            const double delta = static_cast<int>(data[at]) - 128;
            now += data[at + 1];
            const int limit = 1 + data[at + 2] % 4;
            const bool enabled = (data[at + 3] & 1) != 0;
            if (data[at + 3] & 2)
                ++epoch;
            if (data[at + 3] & 4)
                accelerator.Reset();
            auto fragmented = accelerator;
            const double result = accelerator.Apply(delta, now, epoch, enabled, limit);
            double pieces = 0;
            for (unsigned packet = 0; packet < 8; ++packet)
                pieces += fragmented.Apply(delta / 8, now, epoch, enabled, limit);
            if (!std::isfinite(result) || std::abs(result - pieces) > 1e-8 ||
                std::abs(result) < std::abs(delta) || std::abs(result) > std::abs(delta) * limit ||
                (delta != 0 && std::signbit(result) != std::signbit(delta)) ||
                (!enabled && result != delta))
                std::abort();
        }
    }

    void FuzzWheelActivation(const std::uint8_t* data, std::size_t size)
    {
        using namespace glide::wheel;
        for (std::size_t at = 0; at + 7 < size && at < 2048; at += 8)
        {
            Activation admission{ true, 2, 5, 7, 100, 2100, { -1920, -1080, 0, 0 } };
            const unsigned mutation = data[at] % 7;
            if (mutation == 1)
                admission.epoch = 0;
            if (mutation == 2)
                admission.deadline = 99;
            if (mutation == 3)
                admission.deadline = 2101;
            if (mutation == 4)
                admission.bounds.left = (std::numeric_limits<double>::quiet_NaN)();
            const glide::Vec2 point{ mutation == 5 ? 20.0 : -960.0, -540 };
            const double delta = mutation == 6 ? (std::numeric_limits<double>::infinity)() :
                                                 static_cast<int>(data[at + 1]) - 128;
            const std::uint64_t now = 100 + data[at + 2] * 10u;
            ActivationChannel channel;
            if (channel.Publish(admission) != Valid(admission))
                std::abort();
            ActivationIntent intent;
            const bool began = intent.Begin(admission, 1, delta, point, now, true, 3);
            const bool expected = Valid(admission) && glide::pointer::Contains(admission.bounds, point) &&
                                  std::isfinite(delta) && delta != 0 && now <= admission.deadline;
            if (began != expected || intent.Active() != began)
                std::abort();
            if (!began)
                continue;
            double initial = 0;
            Accelerator warm;
            if (!intent.TakeInitial(initial) || initial != warm.Apply(delta, 100, 2, true, 3) || intent.TakeInitial(initial))
                std::abort();
            const std::uint64_t stamp = 100 + data[at + 3];
            const std::uint64_t epoch = 1 + data[at + 4] % 3;
            const double extra = static_cast<int>(data[at + 5]) - 128;
            const bool accepts = intent.Accepts(epoch, stamp, point, now);
            if (intent.Push(extra, stamp, epoch, point, now, true, 3) != accepts)
                std::abort();
            if (data[at + 6] & 1)
            {
                intent.CloseAdmission();
                if (intent.Accepting() || intent.Admission().enabled || intent.Push(120, stamp, 2, point, now, true, 3))
                    std::abort();
            }
            const double pending = intent.TakePending();
            if (!std::isfinite(pending) || std::abs(pending) > MaximumActivationTravel * 4 || intent.TakePending() != 0 ||
                intent.Push(120, 2101, 2, point, 2101, true, 3) ||
                intent.Push(120, stamp, 99, point, now, true, 3) ||
                intent.Push(120, stamp, 2, { 20, -540 }, now, true, 3))
                std::abort();
            if (!intent.Commit(3, now) || !intent.Committed())
                std::abort();
            if (data[at + 7] & 1)
            {
                intent.Reset();
                if (intent.Active() || intent.Committed() || intent.Commit(3, now))
                    std::abort();
            }
        }
    }

    constexpr glide::shortcuts::Command ExpectedShortcut(std::uint32_t key, glide::shortcuts::Modifiers modifiers) noexcept
    {
        using glide::shortcuts::Command;
        // Independent binding oracle: AltGr and Windows-key text combinations
        // never authorize a command, even if Windows supplies synthetic Ctrl.
        if (!modifiers.control || !modifiers.leftAlt || modifiers.rightAlt || modifiers.windows)
            return Command::None;
        switch (key)
        {
        case 'Z':
            return modifiers.shift ? Command::ToggleDesktop : Command::ToggleMonitor;
        case glide::input::vk::Equal:
            return Command::ZoomIn;
        case 'F':
            return modifiers.shift ? Command::FitWindow : Command::None;
        default:
            break;
        }
        if (modifiers.shift)
            return Command::None;
        switch (key)
        {
        case 'I':
            return Command::ToggleIndependent;
        case 'S':
            return Command::Copy;
        case 'V':
            return Command::RevealIndicator;
        case glide::input::vk::Add:
            return Command::ZoomIn;
        case glide::input::vk::Minus:
        case glide::input::vk::Subtract:
            return Command::ZoomOut;
        default:
            return Command::None;
        }
    }

    void FuzzShortcutCommands(const std::uint8_t* data, std::size_t size)
    {
        using namespace glide::shortcuts;
        State state;
        std::array<bool, 256> down{}, repeatRevoked{};
        std::array<Command, 256> owned{};
        std::array<std::uint64_t, 256> acquiredGeneration{};
        std::uint64_t cancelledGeneration = 0;
        unsigned ownedCount = 0;
        const auto cancel = [&]() noexcept {
            state.Cancel();
            ++cancelledGeneration;
        };
        const auto reset = [&]() noexcept {
            state.SessionReset();
            down.fill(false);
            repeatRevoked.fill(false);
            owned.fill(Command::None);
            ownedCount = 0;
            ++cancelledGeneration;
        };
        const auto keyEvent = [&](std::uint32_t key, bool pressed, Modifiers modifiers, bool permitted, bool sinkSucceeds) {
            const bool known = key < down.size();
            if (known)
            {
                if (!permitted)
                    ++cancelledGeneration;
                for (std::size_t index = 0; index < owned.size(); ++index)
                    if (owned[index] != Command::None && ExpectedShortcut(static_cast<std::uint32_t>(index), modifiers) != owned[index])
                        repeatRevoked[index] = true;
            }
            const bool previous = known && down[key];
            const Command precedingOwner = known ? owned[key] : Command::None;
            const bool wasOwned = precedingOwner != Command::None;
            unsigned sinkCalls = 0;
            Command posted = Command::None;
            const auto decision = state.Key(key, pressed, modifiers, permitted, [&](Binding binding, bool fresh) noexcept {
                if (++sinkCalls > 1 || !known || !pressed || !permitted || modifiers.rightAlt || modifiers.windows ||
                    binding.key != key || binding.shift != modifiers.shift || binding.command == Command::None ||
                    binding.command != ExpectedShortcut(key, modifiers))
                    std::abort();
                if (fresh)
                {
                    if (previous || wasOwned)
                        std::abort(); // Availability may be reserved once per fresh DOWN only.
                }
                else if (!wasOwned || !previous || repeatRevoked[key] || acquiredGeneration[key] != cancelledGeneration ||
                         (precedingOwner != Command::ZoomIn && precedingOwner != Command::ZoomOut) || binding.command != precedingOwner)
                    std::abort();
                if (sinkSucceeds)
                    posted = binding.command;
                return sinkSucceeds;
            });
            if (decision.command != posted || ((!permitted || modifiers.rightAlt || modifiers.windows) && decision.command != Command::None))
                std::abort();
            if (!known)
            {
                if (decision.consume || sinkCalls)
                    std::abort();
                return;
            }
            if (wasOwned)
            {
                if (!decision.consume)
                    std::abort(); // Cancel/disable/modifier changes cannot steal an owned UP.
                if (!pressed)
                {
                    owned[key] = Command::None;
                    repeatRevoked[key] = false;
                    --ownedCount;
                }
            }
            else
            {
                if (decision.consume != (posted != Command::None) ||
                    (decision.consume && (!pressed || previous || !permitted)))
                    std::abort();
                if (decision.consume)
                {
                    owned[key] = posted;
                    acquiredGeneration[key] = cancelledGeneration;
                    repeatRevoked[key] = false;
                    ++ownedCount;
                }
            }
            down[key] = pressed;
            if (state.ReservedReleasePending() != (ownedCount != 0))
                std::abort();
        };
        constexpr std::array<std::uint32_t, 10> triggers{
            'Z', 'I', 'S', 'V', 'F', glide::input::vk::Equal, glide::input::vk::Add, glide::input::vk::Minus, glide::input::vk::Subtract, glide::input::vk::RightAlt
        };
        // Every input reaches an eligible fresh probe, then AltGr cancellation
        // and an owned release while disabled, before arbitrary interleavings.
        const auto first = triggers[size ? data[0] % (triggers.size() - 1) : 0];
        keyEvent(first, true, { true, true, false, first == 'F', false }, true, true);
        keyEvent(first, true, { true, true, true, false, false }, true, true);
        keyEvent(first, false, { true, false, true, false, false }, false, true);
        if (state.ReservedReleasePending())
            std::abort();

        // At most 64 operations; each policy scan has exactly 256 key slots.
        for (std::size_t at = 0; at + 7 < size && at < 512; at += 8)
        {
            const unsigned action = data[at] % 7;
            const unsigned flags = data[at + 2];
            const Modifiers modifiers{ (flags & 1) != 0, (flags & 2) != 0, (flags & 4) != 0, (flags & 8) != 0, (flags & 16) != 0 };
            const std::uint32_t key = (data[at + 1] & 1) ? triggers[data[at + 3] % triggers.size()] :
                                                           ((data[at + 1] & 2) ? data[at + 3] : (std::uint32_t{ data[at + 3] } << 8) | data[at + 4]);
            if (action <= 2)
                keyEvent(key, (data[at + 5] & 1) != 0, modifiers, (data[at + 6] & 1) != 0, (data[at + 7] & 1) != 0);
            else if (action == 3)
                cancel();
            else if (action == 4)
                reset();
            else if (action == 5)
            {
                const bool pressed = (data[at + 5] & 1) != 0;
                state.Seed(key, pressed);
                if (key < down.size())
                    down[key] = pressed;
            }
            else
            {
                state.ObserveModifiers(modifiers);
                for (std::size_t index = 0; index < owned.size(); ++index)
                    if (owned[index] != Command::None && ExpectedShortcut(static_cast<std::uint32_t>(index), modifiers) != owned[index])
                        repeatRevoked[index] = true;
            }
            if (state.ReservedReleasePending() != (ownedCount != 0))
                std::abort();
        }
        cancel();
        for (std::uint32_t key = 0; key < owned.size(); ++key)
            if (owned[key] != Command::None)
                keyEvent(key, false, { true, false, true, true, true }, false, false);
        if (state.ReservedReleasePending() || ownedCount)
            std::abort();
    }
}

void FuzzRouting(const std::uint8_t* data, std::size_t size)
{
    using namespace glide::input::routing;
    size = (std::min)(size, std::size_t{ 16384 });
    FuzzPhysicalRoutingPoint(data, size);
    FuzzWheel(data, size);
    FuzzWheelActivation(data, size);
    FuzzPointerSpeed(data, size);
    FuzzShortcutCommands(data, size);
    Snapshot snapshot;
    snapshot.enabled = true;
    snapshot.generation = 1;
    snapshot.epoch = 1;
    snapshot.activeId = 1;
    snapshot.count = 2;
    snapshot.routes[0] = { 1, { -1920, 0, 0, 1080 }, { -1440, 270, -480, 810 }, true };
    snapshot.routes[1] = { 2, { 0, 0, 1920, 1080 }, { 480, 270, 1440, 810 }, true };
    State state;
    state.Observe(snapshot, true, 1);
    std::array<bool, 5> physical{}, owned{};
    std::size_t at = 0;
    const auto take = [&]() { return at < size ? data[at++] : std::uint8_t{ 0 }; };
    for (unsigned operation = 0; operation < 1024 && at < size; ++operation)
    {
        const unsigned action = take() % 8;
        if (action == 0)
        {
            const double x = (static_cast<int>(take()) - 128) * 16.0;
            const double y = take() * 5.0;
            state.ObservePoint({ x, y });
        }
        else if (action == 1)
        {
            const unsigned button = take() % 5;
            const bool down = (take() & 1) != 0;
            const bool reserve = down && !physical[button] && state.BlockNewButton();
            const bool expected = owned[button] || reserve;
            const bool consumed = state.Button(button, down, false);
            if (consumed != expected)
                std::abort();
            physical[button] = down;
            owned[button] = down && expected;
        }
        else if (action == 2)
        {
            ++snapshot.generation;
            snapshot.enabled = (take() & 1) != 0;
            snapshot.busy = (take() & 1) != 0;
            snapshot.activeId = (take() & 1) ? 1 : 2;
            snapshot.ackSerial = (take() & 1) ? state.Report(true).requestSerial : 0;
            state.Observe(snapshot, true, snapshot.epoch);
        }
        else if (action == 3)
            state.Observe(snapshot, false, snapshot.epoch);
        else if (action == 4)
        {
            state.SessionReset();
            physical.fill(false);
            owned.fill(false);
        }
        else if (action == 5)
        {
            const unsigned mask = take() & 31u;
            state.SeedButtons(mask);
            for (unsigned button = 0; button < 5; ++button)
            {
                physical[button] = (mask & (1u << button)) != 0;
                owned[button] = false;
            }
        }
        else if (action == 6)
        {
            ++snapshot.epoch;
            state.Observe(snapshot, true, snapshot.epoch - 1);
        }
        else
            state.Fault();
        unsigned expectedOwned = 0, expectedForwarded = 0;
        for (unsigned button = 0; button < 5; ++button)
        {
            if (owned[button])
                expectedOwned |= 1u << button;
            if (physical[button] && !owned[button])
                expectedForwarded |= 1u << button;
        }
        const std::uint64_t now = 100 + operation;
        const auto feedback = state.Report(true, now);
        if (feedback.blockedButtons != expectedOwned || feedback.forwardedButtons != expectedForwarded ||
            (feedback.canCommit && (feedback.forwardedButtons || feedback.faulted || !feedback.observedGeneration)) ||
            CommitReady(feedback, snapshot.generation, snapshot.epoch, now + 251) ||
            CommitReady(feedback, snapshot.generation, snapshot.epoch, now - 1) ||
            CommitReady(feedback, feedback.observedGeneration + 1, feedback.epoch, now))
            std::abort();
    }
}
