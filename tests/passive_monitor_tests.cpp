// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#include "../src/monitor_visual.h"
#include "../src/monitor_presenter.h"
#include "../src/gpu_queue_policy.h"
#include "../src/window_class_lease.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace
{
    using glide::gpu::QueueDecision;
    using glide::gpu::QueueSignal;
    using glide::monitor::Frame;
    using glide::monitor::VisualRefresh;
    unsigned assertions = 0;

    void Check(bool condition, const char* message)
    {
        ++assertions;
        if (!condition)
            throw std::runtime_error(message);
    }

    Frame Make(glide::Rect bounds, double zoom)
    {
        const glide::Vec2 center{ (bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2 };
        Frame result;
        Check(glide::monitor::MakeFrame(bounds, glide::AnchorZoom({}, zoom, center, bounds), result), "valid monitor frame");
        return result;
    }

    struct Renderer
    {
        Frame prepared{}, submitted{};
        glide::gpu::PresentationQueue queue;
        bool cursorEnabled = true, submittedCursor = false;
        bool visible = false, preparedValid = false, hasSubmitted = false, staged = false, pending = false, stagedReused = false;
        bool captureReady = true, resourcesReady = true, orderClear = true, presentFails = false;
        QueueSignal signal = QueueSignal::Ready;
        std::uint64_t now = 0, sourceVersion = 1, stagedVersion = 0, displayedVersion = 0;
        unsigned hides = 0, shows = 0, presents = 0, orderChecks = 0, readinessChecks = 0, rebuilds = 0, restages = 0;

        void SetCursorEnabled(bool enabled) noexcept { cursorEnabled = enabled; }
        bool Active() const noexcept { return visible; }
        bool Pending() const noexcept { return pending; }
        bool Matches(const Frame& frame) const noexcept { return preparedValid && glide::monitor::SameFrame(frame, prepared); }
        bool Ready() noexcept
        {
            ++readinessChecks;
            const auto decision = queue.Observe(signal, now, !cursorEnabled);
            pending = decision == QueueDecision::Pending;
            if (decision == QueueDecision::Failed)
                Hide();
            return decision == QueueDecision::Render;
        }
        bool Prepare(const Frame& frame) noexcept
        {
            if (!orderClear)
            {
                Hide();
                return false;
            }
            const bool compatible = preparedValid && resourcesReady && captureReady && prepared.destination == frame.destination;
            const auto preparation = glide::gpu::PrepareDecision(staged, queue.HasUnsubmittedGrant(), Matches(frame), compatible);
            if (preparation == glide::gpu::Preparation::Reuse)
            {
                pending = false;
                stagedReused = true;
                return true;
            }
            if (preparation == glide::gpu::Preparation::Restage)
            {
                ++restages;
                prepared = frame;
                stagedVersion = sourceVersion;
                stagedReused = pending = false;
                return true;
            }
            if (preparation == glide::gpu::Preparation::Rebuild)
            {
                Hide();
                ++rebuilds;
                resourcesReady = true; // Model successful recreation of private resources.
            }
            if (!captureReady)
            {
                pending = true;
                return false;
            }
            if (!Ready())
                return false;
            prepared = frame;
            preparedValid = staged = true;
            stagedVersion = sourceVersion;
            return true;
        }
        bool Show() noexcept
        {
            ++shows;
            if (!staged)
            {
                if (!Ready())
                    return false;
                stagedVersion = sourceVersion;
            }
            else if (stagedReused)
                stagedVersion = sourceVersion;
            if (!orderClear || presentFails || !queue.Submitted(now))
            {
                Hide();
                return false;
            }
            ++presents;
            submitted = prepared;
            submittedCursor = cursorEnabled;
            displayedVersion = stagedVersion;
            visible = hasSubmitted = true;
            staged = pending = stagedReused = false;
            return true;
        }
        bool TryGetPresentedFrame(Frame& result) const noexcept
        {
            if (!visible || !hasSubmitted)
                return false;
            result = submitted;
            return true;
        }
        bool CanRetainVisual(const Frame& frame) noexcept
        {
            ++orderChecks;
            if (!orderClear)
            {
                Hide();
                return false;
            }
            return visible && hasSubmitted && !staged &&
                   glide::monitor::SameFrame(frame, submitted) && glide::monitor::SameFrame(prepared, submitted) &&
                   (cursorEnabled || !submittedCursor) && !queue.HasUnsubmittedGrant();
        }
        bool CanRetain(const glide::session::Snapshot& snapshot) noexcept
        {
            const glide::session::Snapshot expected{ { 1, 0, 0 }, { true, submitted.source, submitted.destination } };
            return snapshot == expected && CanRetainVisual(submitted);
        }
        void Hide() noexcept
        {
            ++hides;
            visible = preparedValid = hasSubmitted = staged = pending = stagedReused = false;
            submittedCursor = false;
            queue.Reset();
        }
    };

    struct Mapping
    {
        glide::session::Snapshot state{};
        unsigned inputWrites = 0, visualWrites = 0;
        bool Read(glide::session::Snapshot& result) const noexcept
        {
            result = state;
            return true;
        }
        bool SetVisual(glide::session::Visual value) noexcept
        {
            ++visualWrites;
            state.visual = value;
            return true;
        }
        bool SetInput(glide::session::Input value) noexcept
        {
            ++inputWrites;
            state.input = value;
            return true;
        }
    };
    struct Journal
    {
        bool armed = false;
        void Publish(const glide::session::RecoveryRecord&) noexcept { armed = true; }
        void Clear() noexcept { armed = false; }
    };

    void IndependentLiveRefreshNeverTouchesActiveMapping()
    {
        Mapping mapping;
        Journal journal;
        glide::session::Session inputOwner;
        Renderer active, left, right;
        const auto activeFrame = Make({ 0, 0, 1920, 1080 }, 2);
        const auto leftFrame = Make({ -1920, -1080, 0, 0 }, 3);
        const auto rightFrame = Make({ 1920, 0, 4480, 1440 }, 4);
        Check(glide::monitor::Present(activeFrame, active, inputOwner, mapping, journal).result == glide::session::Result::Applied,
              "establish one active monitor and one shared input journal");
        const auto activeMapping = mapping.state;
        const auto writes = mapping.inputWrites;
        for (std::uint64_t version = 1; version <= 100; ++version)
        {
            left.now = right.now = version * 16;
            left.sourceVersion = version;
            right.sourceVersion = version + 100;
            Check(glide::monitor::RefreshPassive(left, leftFrame) == VisualRefresh::Presented &&
                      glide::monitor::RefreshPassive(right, rightFrame) == VisualRefresh::Presented,
                  "both inactive monitors continue presenting their independently magnified live contents");
            Check(left.displayedVersion == version && right.displayedVersion == version + 100 &&
                      !left.submittedCursor && !right.submittedCursor && active.submittedCursor,
                  "passive images update without copying the active view's cursor ownership");
            Check(mapping.state == activeMapping && mapping.inputWrites == writes && mapping.visualWrites == 0 &&
                      inputOwner.OwnsState() && journal.armed,
                  "passive refresh leaves the single active source/destination mapping and recovery responsibility intact");
        }
    }

    void PendingAndCursorHandoff()
    {
        const auto frame = Make({ -1920, 0, 0, 1080 }, 3);
        Renderer view;
        view.captureReady = false;
        Check(glide::monitor::RefreshPassive(view, frame) == VisualRefresh::Pending && !view.Active() && view.hides == 0,
              "initial capture pending keeps retry resources without displaying an unprepared frame");
        view.captureReady = true;
        view.now = 1;
        Check(glide::monitor::RefreshPassive(view, frame) == VisualRefresh::Presented, "first cursorless image is presented");
        view.now = 2;
        view.signal = QueueSignal::Busy;
        Check(glide::monitor::RefreshPassive(view, frame) == VisualRefresh::Pending && view.Active() &&
                  view.orderChecks == 1 && !view.submittedCursor && view.hides == 0,
              "pending readiness retains a freshly verified exact cursorless old image");
        view.signal = QueueSignal::Ready;
        view.now = 3;
        Check(glide::monitor::RefreshPassive(view, frame) == VisualRefresh::Presented, "pending passive image resumes normally");

        Renderer previouslyActive;
        Check(previouslyActive.Prepare(frame) && previouslyActive.Show() && previouslyActive.submittedCursor,
              "establish an old active image containing a software cursor");
        previouslyActive.signal = QueueSignal::Busy;
        previouslyActive.now = 1;
        Check(glide::monitor::RefreshPassive(previouslyActive, frame) == VisualRefresh::Pending &&
                  !previouslyActive.Active() && previouslyActive.hides == 1,
              "a cursor-bearing old image cannot remain on an inactive monitor while its replacement is pending");
        previouslyActive.signal = QueueSignal::Ready;
        previouslyActive.now = 2;
        Check(glide::monitor::RefreshPassive(previouslyActive, frame) == VisualRefresh::Presented &&
                  previouslyActive.Active() && !previouslyActive.submittedCursor &&
                  glide::monitor::SameFrame(previouslyActive.submitted, frame),
              "the pending handoff restores the same zoom as a live cursorless image, rather than resetting its zoom state");

        Renderer staged;
        Check(staged.Prepare(frame) && staged.queue.HasUnsubmittedGrant(),
              "preparing the former active view consumes one readiness grant");
        const auto waits = staged.readinessChecks;
        Check(glide::monitor::RefreshPassive(staged, frame) == VisualRefresh::Presented &&
                  !staged.submittedCursor && staged.readinessChecks == waits && !staged.queue.HasUnsubmittedGrant(),
              "cursor ownership can change after preparation without consuming another grant or submitting the old cursor");
    }

    void FailuresAreLocalAndDoNotRetainWrongGeometry()
    {
        const auto frame = Make({ 0, 0, 1920, 1080 }, 2);
        const auto changed = Make({ 0, 0, 1920, 1080 }, 3);
        Renderer healthy, failing;
        Check(glide::monitor::RefreshPassive(healthy, frame) == VisualRefresh::Presented &&
                  glide::monitor::RefreshPassive(failing, frame) == VisualRefresh::Presented,
              "establish independent passive views");
        Check(!failing.CanRetainVisual(changed), "an old image cannot certify a different source crop");
        failing.orderClear = false;
        failing.signal = QueueSignal::Busy;
        failing.now = 1;
        Check(glide::monitor::RefreshPassive(failing, frame) == VisualRefresh::Failed && !failing.Active() && healthy.Active(),
              "native ordering failure hides only that renderer and is reported to the root recovery policy");
        failing.orderClear = true;
        failing.presentFails = true;
        failing.signal = QueueSignal::Ready;
        failing.now = 2;
        Check(glide::monitor::RefreshPassive(failing, frame) == VisualRefresh::Failed && !failing.Active() && healthy.Active(),
              "a failed passive submission is never disguised as capture pending");
        Frame invalid = frame;
        invalid.source.right = invalid.source.left;
        Check(glide::monitor::RefreshPassive(healthy, invalid) == VisualRefresh::Failed && !healthy.Active(),
              "invalid passive geometry cannot leave a stale overlay visible");
    }

    void DeferredPreparationReusesGrantOrRebuilds()
    {
        const auto frame = Make({ 0, 0, 1920, 1080 }, 2);
        const auto changed = Make({ 0, 0, 1920, 1080 }, 3);
        Renderer view;
        Check(view.Prepare(frame) && view.queue.HasUnsubmittedGrant(), "prepare consumes exactly one readiness signal");
        const auto waits = view.readinessChecks;
        view.now = 500;
        view.sourceVersion = 2;
        for (unsigned retry = 0; retry != 10; ++retry)
            Check(view.Prepare(frame) && view.queue.HasUnsubmittedGrant() && view.readinessChecks == waits && view.rebuilds == 0,
                  "deferred input-barrier feedback can retry an identical prepared camera without waiting or abandoning its grant");
        view.SetCursorEnabled(false);
        Check(view.Show() && view.displayedVersion == 2 && !view.submittedCursor && !view.queue.HasUnsubmittedGrant(),
              "reused preparation submits fresh pixels and current cursor ownership using its original grant");

        view.now = 501;
        Check(view.Prepare(frame) && view.queue.HasUnsubmittedGrant(), "prepare the next frame before a requested camera change");
        const auto oldWaits = view.readinessChecks;
        Check(view.Prepare(changed) && view.restages == 1 && view.rebuilds == 0 && view.hides == 0 && view.readinessChecks == oldWaits &&
                  view.Active() && view.queue.HasUnsubmittedGrant() && view.Show() &&
                  glide::monitor::SameFrame(view.submitted, changed),
              "a different crop on the same output redraws the unsubmitted buffer while retaining the visible image and its original grant");

        view.now = 502;
        const auto otherOutput = Make({ 1920, 0, 4480, 1440 }, 3);
        Check(view.Prepare(changed), "acquire a staged image before changing its native output");
        const auto outputWaits = view.readinessChecks;
        Check(view.Prepare(otherOutput) && view.rebuilds == 1 && view.hides == 1 && view.readinessChecks == outputWaits + 1 && !view.Active() && view.Show(),
              "changed output requires private resource recreation and one new readiness grant");
        Check(view.Prepare(otherOutput), "acquire a staged image before private resources are invalidated");
        view.resourcesReady = false;
        const auto resourceWaits = view.readinessChecks;
        Check(view.Prepare(otherOutput) && view.rebuilds == 2 && view.hides == 2 && view.readinessChecks == resourceWaits + 1 && !view.Active() && view.Show(),
              "even matching geometry cannot reuse a held grant after private resource incompatibility");

        view.now = 503;
        Check(view.Prepare(otherOutput), "prepare a frame before discovering an un-coverable window");
        view.orderClear = false;
        Check(!view.Prepare(otherOutput) && !view.Active() && !view.queue.HasUnsubmittedGrant(),
              "same-frame reuse never skips the current native order check");
        Check(glide::gpu::PrepareDecision(true, false, true, true) == glide::gpu::Preparation::Rebuild &&
                  glide::gpu::PrepareDecision(false, true, true, true) == glide::gpu::Preparation::Rebuild,
              "inconsistent staged/grant ownership cannot authorize reuse or another wait on the old queue");
    }

    void ChangedPendingCropKeepsTheVisibleImageUntilOneVerifiedSubmission()
    {
        Mapping mapping;
        Journal journal;
        glide::session::Session owner;
        Renderer view;
        const glide::Rect bounds{ -1920, -1080, 0, 0 };
        const auto original = Make(bounds, 2);
        Check(glide::monitor::Present(original, view, owner, mapping, journal).result == glide::session::Result::Applied,
              "establish the visible image and its one journaled input mapping");
        const auto before = mapping.state;
        const auto writes = mapping.inputWrites;
        view.now = 1;
        auto pending = Make(bounds, 3);
        Check(view.Prepare(pending) && view.queue.HasUnsubmittedGrant(), "stage the next crop while fresh routing feedback is delayed");
        const auto waits = view.readinessChecks;
        const auto presents = view.presents;
        for (unsigned retry = 0; retry < 12; ++retry)
        {
            view.now = 2 + retry;
            view.sourceVersion = 100 + retry;
            pending = Make(bounds, 3.1 + retry * 0.05);
            Check(view.Prepare(pending) && view.queue.HasUnsubmittedGrant() && view.readinessChecks == waits && view.presents == presents,
                  "changing edge-entry geometry redraws the same backbuffer without another wait or premature Present");
            Check(view.Active() && view.hides == 0 && view.rebuilds == 0 && glide::monitor::SameFrame(view.submitted, original) && mapping.state == before && mapping.inputWrites == writes,
                  "the previously submitted pixels and exact old input mapping stay visible while the new crop is uncommitted");
            Check(!view.CanRetainVisual(pending), "old submitted metadata never certifies the unsubmitted crop");
        }
        const auto restages = view.restages;
        const auto committed = glide::monitor::Present(pending, view, owner, mapping, journal);
        const glide::session::Snapshot expected{ { 1, 0, 0 }, { true, pending.source, pending.destination } };
        Check(committed.result == glide::session::Result::Applied && !committed.pending && view.Active() && mapping.state == expected && owner.Expected() == expected,
              "the production presenter installs only the final crop's exact input mapping before displaying it");
        Check(view.restages == restages && view.restages == 12 && view.presents == presents + 1 && view.readinessChecks == waits && !view.queue.HasUnsubmittedGrant() && view.hides == 0 && view.rebuilds == 0,
              "the final verified image consumes exactly one existing grant without hidden cold startup or extra waits");
        Check(glide::monitor::SameFrame(view.submitted, pending) && view.displayedVersion == 111 && mapping.inputWrites == writes + 1 && mapping.visualWrites == 0 && journal.armed,
              "latest pixels and submitted metadata advance together while full-desktop magnification and the recovery journal remain correct");
    }

    void SharedClassLifetime()
    {
        struct Backend
        {
            bool registered = false, registerFails = false, unregisterFails = false, unsafeUnregister = false;
            unsigned registers = 0, unregisters = 0, liveWindows = 0;
            bool Register(bool& owned) noexcept
            {
                ++registers;
                if (registerFails)
                    return false;
                owned = !registered;
                registered = true;
                return true;
            }
            bool Unregister() noexcept
            {
                ++unregisters;
                unsafeUnregister = unsafeUnregister || liveWindows != 0;
                if (unregisterFails)
                    return false;
                registered = false;
                return true;
            }
        } backend;
        glide::window::ClassLease lease;
        for (unsigned monitor = 0; monitor != 16; ++monitor)
        {
            Check(lease.Acquire(42, backend), "every renderer acquires the same owner-thread window class");
            ++backend.liveWindows;
        }
        Check(backend.registers == 1 && !lease.Acquire(99, backend), "a different HINSTANCE cannot take over the shared class");
        for (unsigned monitor = 0; monitor != 16; ++monitor)
        {
            --backend.liveWindows;
            lease.Release(backend);
            Check(backend.unregisters == (monitor == 15 ? 1u : 0u),
                  "destroying an early renderer leaves all remaining monitor windows registered");
        }
        backend.unregisterFails = true;
        Check(lease.Acquire(42, backend), "class can be reacquired after all old views close");
        lease.Release(backend);
        Check(backend.registered, "an unsuccessful final unregister retains ownership for later retry");
        backend.unregisterFails = false;
        Check(lease.Acquire(42, backend), "retained class is validated on reacquisition");
        lease.Release(backend);
        Check(!backend.registered, "a later final release retries owned class cleanup");
        backend.registered = true;
        const auto unregisters = backend.unregisters;
        Check(lease.Acquire(42, backend), "compatible externally registered class can be referenced");
        lease.Release(backend);
        Check(backend.registered && backend.unregisters == unregisters, "an unowned class is never unregistered");
        Check(!backend.unsafeUnregister, "the shared native class is never unregistered while another monitor owns a window");
    }
}

int main()
{
    IndependentLiveRefreshNeverTouchesActiveMapping();
    PendingAndCursorHandoff();
    FailuresAreLocalAndDoNotRetainWrongGeometry();
    DeferredPreparationReusesGrantOrRebuilds();
    ChangedPendingCropKeepsTheVisibleImageUntilOneVerifiedSubmission();
    SharedClassLifetime();
    std::cout << "PASS: " << assertions << " passive monitor assertions; production visual/queue/class policies; no Windows compositor executed\n";
    return 0;
}
