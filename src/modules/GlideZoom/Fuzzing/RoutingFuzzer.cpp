// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#include "../GlideZoom/input_routing.h"
#include "../GlideZoom/wheel_acceleration.h"
#include "../GlideZoom/wheel_activation.h"

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
}

void FuzzRouting(const std::uint8_t* data, std::size_t size)
{
    using namespace glide::input::routing;
    size = (std::min)(size, std::size_t{ 16384 });
    FuzzWheel(data, size);
    FuzzWheelActivation(data, size);
    FuzzPointerSpeed(data, size);
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
