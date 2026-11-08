#include <stdexcept>
#include "../GlideZoom/gpu_queue_policy.h"
#include "../GlideZoom/gpu_frame_policy.h"
#include "../GlideZoom/monitor_presenter.h"
#include "../GlideZoom/frame_transaction.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>

namespace
{
    using glide::gpu::PresentationQueue;
    using glide::gpu::QueueDecision;
    using glide::gpu::QueueSignal;
    using glide::monitor::Frame;
    using glide::session::Result;
    using glide::session::Snapshot;
    std::uint64_t assertions = 0, scenarios = 0, integrationRows = 0;

    void Check(bool value, const char* message)
    {
        ++assertions;
        if (!value)
        {
            std::cerr << "FAIL GPU queue policy: " << message << " [scenario=" << scenarios << "]\n";
            throw std::runtime_error("GlideZoom assertion failed; see captured output");
        }
    }
    bool Success(Result result)
    {
        return result == Result::Applied || result == Result::Unchanged;
    }
    Snapshot Mapping(const Frame& frame)
    {
        return { { 1, 0, 0 }, { true, frame.source, frame.destination } };
    }
    void Grant(PresentationQueue& queue, std::uint64_t now)
    {
        Check(queue.Observe(QueueSignal::Ready, now) == QueueDecision::Render &&
                  queue.HasUnsubmittedGrant() && queue.Submitted(now) && !queue.HasUnsubmittedGrant(),
              "one successful wait authorizes exactly one successful Present");
    }

    void InitialAndAcknowledgementDeadlines()
    {
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        const std::array<std::uint64_t, 9> delays{ 0, 1, 99, 100, 101, 500, 1999, 2000, 2001 };
        for (const auto start : { std::uint64_t{ 0 }, std::uint64_t{ 100 }, maximum - 10000 })
        {
            for (const auto delay : delays)
            {
                ++scenarios;
                PresentationQueue queue;
                Check(queue.Cold() && !queue.HasSubmittedFrame() && queue.WaitMilliseconds() == 0,
                      "first queue poll cannot block the owner thread");
                Check(queue.Observe(QueueSignal::Busy, start) == QueueDecision::Pending,
                      "a cold unsignaled queue is pending, not a failed GPU");
                const auto result = queue.Observe(QueueSignal::Ready, start + delay);
                if (delay < 2000)
                {
                    Check(result == QueueDecision::Render && queue.Cold() &&
                              queue.Submitted(start + delay + 5),
                          "first image is submitted after an initial grant");
                    Check(queue.Cold() && queue.HasSubmittedFrame() && queue.WaitMilliseconds() == 0,
                          "successful Present is submission, not proof that the compositor has started");
                    Check(queue.Observe(QueueSignal::Busy, start + delay + 106) == QueueDecision::Pending,
                          "post-Present compositor startup longer than 100 ms is still nonblocking");
                    Grant(queue, start + delay + 2004);
                    Check(!queue.Cold() && queue.WaitMilliseconds() == 100,
                          "the first post-submission grant enables bounded active pacing");
                    Grant(queue, start + delay + 5000);
                }
                else
                {
                    Check(result == QueueDecision::Failed && !queue.Submitted(start + delay),
                          "late initial readiness cannot revive an expired activation");
                    Check(queue.HasUnsubmittedGrant(),
                          "even a rejected late grant was consumed and requires pipeline recreation");
                }
            }
            for (const auto firstDelay : { std::uint64_t{ 0 }, std::uint64_t{ 1999 } })
            {
                for (const auto acknowledgementDelay : delays)
                {
                    ++scenarios;
                    PresentationQueue queue;
                    Check(queue.Observe(QueueSignal::Busy, start) == QueueDecision::Pending,
                          "seed the independent initial-grant deadline");
                    Grant(queue, start + firstDelay);
                    const auto result = queue.Observe(QueueSignal::Ready,
                                                      start + firstDelay + acknowledgementDelay);
                    if (acknowledgementDelay < 2000)
                    {
                        Check(result == QueueDecision::Render && !queue.Cold() &&
                                  queue.Submitted(start + firstDelay + acknowledgementDelay),
                              "first post-Present readiness has a separate full two-second budget");
                    }
                    else
                    {
                        Check(result == QueueDecision::Failed && queue.Cold() &&
                                  queue.HasSubmittedFrame() && queue.HasUnsubmittedGrant(),
                              "late post-Present readiness cannot revive or silently reuse its consumed grant");
                    }
                }
            }
        }
        ++scenarios;
        PresentationQueue queue;
        for (std::uint64_t time = 0; time < 2000; ++time)
            Check(queue.Observe(QueueSignal::Busy, time) == QueueDecision::Pending &&
                      queue.WaitMilliseconds() == 0,
                  "initial retries cannot extend their fixed deadline");
        Check(queue.Observe(QueueSignal::Busy, 2000) == QueueDecision::Failed &&
                  !queue.HasUnsubmittedGrant(),
              "permanent initial congestion expires without inventing a grant");
        queue.Reset();
        Grant(queue, 3000);
        for (std::uint64_t time = 3000; time < 5000; ++time)
            Check(queue.Observe(QueueSignal::Busy, time) == QueueDecision::Pending &&
                      queue.WaitMilliseconds() == 0,
                  "post-Present retries cannot reset the acknowledgement deadline");
        Check(queue.Observe(QueueSignal::Busy, 5000) == QueueDecision::Failed,
              "permanent compositor startup delay has a fixed bound");
        ++scenarios;
        queue.Reset();
        Check(queue.Observe(QueueSignal::Ready, 0) == QueueDecision::Render && queue.Submitted(5000),
              "the post-Present budget starts at submission, not the earlier render grant");
        Check(queue.Observe(QueueSignal::Busy, 6999) == QueueDecision::Pending &&
                  queue.Observe(QueueSignal::Busy, 7000) == QueueDecision::Failed,
              "work before submission neither consumes nor extends the acknowledgement budget");
    }

    void FailureAndGrantLifecycle()
    {
        // No submission, first submitted image, confirmed active compositor.
        for (unsigned phase = 0; phase != 3; ++phase)
        {
            for (const auto signal : { QueueSignal::Busy, QueueSignal::Failed, static_cast<QueueSignal>(99) })
            {
                ++scenarios;
                PresentationQueue queue;
                if (phase >= 1)
                    Grant(queue, 1);
                if (phase == 2)
                    Grant(queue, 2);
                const auto expected = phase < 2 && signal == QueueSignal::Busy ?
                                          QueueDecision::Pending :
                                          QueueDecision::Failed;
                Check(queue.Observe(signal, 3) == expected,
                      "only ordinary startup congestion can retry; invalid waits never receive grace");
                if (expected == QueueDecision::Failed)
                    Check(queue.Observe(QueueSignal::Ready, 4) == QueueDecision::Failed &&
                              !queue.Submitted(4),
                          "failure cannot self-recover or submit another frame");
                queue.Reset();
                Check(queue.Cold() && !queue.HasSubmittedFrame() && !queue.HasUnsubmittedGrant() &&
                          queue.WaitMilliseconds() == 0,
                      "new pipeline resets all queue lifecycle evidence");
                Grant(queue, 5);
            }
        }
        for (bool submitted : { false, true })
        {
            for (const auto second : { QueueSignal::Ready, QueueSignal::Busy, QueueSignal::Failed })
            {
                ++scenarios;
                PresentationQueue queue;
                if (submitted)
                    Grant(queue, 0);
                Check(queue.Observe(QueueSignal::Ready, 1) == QueueDecision::Render,
                      "successful wait owns one readiness grant");
                Check(queue.Observe(second, 2) == QueueDecision::Failed && !queue.Submitted(2) &&
                          queue.HasUnsubmittedGrant(),
                      "duplicate wait before Present invalidates consumed native evidence");
            }
        }
        ++scenarios;
        PresentationQueue queue;
        Check(!queue.Submitted(0) && queue.Observe(QueueSignal::Ready, 0) == QueueDecision::Failed,
              "submission without a readiness grant fails closed");
        queue.Reset();
        Grant(queue, 1);
        Check(!queue.Submitted(2), "a readiness grant cannot authorize two Presents");
        queue.Reset();
        Check(queue.Observe(QueueSignal::Ready, 100) == QueueDecision::Render &&
                  !queue.Submitted(99) && queue.HasUnsubmittedGrant(),
              "clock reversal during native Present fails without losing consumed-grant evidence");
        for (unsigned phase = 0; phase != 3; ++phase)
        {
            ++scenarios;
            PresentationQueue backwards;
            if (phase >= 1)
                Grant(backwards, 900);
            if (phase == 2)
                Grant(backwards, 950);
            if (phase < 2)
                Check(backwards.Observe(QueueSignal::Busy, 1000) == QueueDecision::Pending,
                      "seed startup clock-reversal fixture");
            else
                Grant(backwards, 1000);
            Check(backwards.Observe(QueueSignal::Ready, 999) == QueueDecision::Failed &&
                      backwards.Observe(QueueSignal::Ready, 1001) == QueueDecision::Failed,
                  "clock reversal cannot extend any startup phase or revive a failed active queue");
        }
    }

    struct Backend
    {
        Snapshot state{};
        unsigned reads = 0, inputWrites = 0, visualWrites = 0;
        bool failRead = false;
        std::function<void()> onRead{};
        bool Read(Snapshot& output)
        {
            ++reads;
            if (onRead)
                onRead();
            if (failRead)
                return false;
            output = state;
            return true;
        }
        bool SetInput(glide::session::Input input)
        {
            ++inputWrites;
            state.input = input;
            return true;
        }
        bool SetVisual(glide::session::Visual visual)
        {
            ++visualWrites;
            state.visual = visual;
            return true;
        }
    };
    struct Journal
    {
        unsigned publications = 0;
        bool armed = false;
        void Publish(const glide::session::RecoveryRecord& record)
        {
            Check(glide::session::Valid(record), "queue integration publishes valid recovery ownership");
            ++publications;
            armed = true;
        }
        void Clear() { armed = false; }
    };

    // A discrete fake compositor, not a replay of hand-picked Ready responses.
    // The first grant may exist while hidden, but subsequent work cannot retire
    // until the first submitted image is shown and the simulated wake delay ends.
    struct Compositor
    {
        std::uint64_t initialDelay = 0, wakeDelay = 250;
        std::uint64_t createdAt = 0, shownAt = 0, nextReadyAt = 0, generation = 1;
        bool submitted = false, acknowledged = false, shown = false, visible = false, consumed = false;
        bool failed = false, activeStall = false;
        QueueSignal Wait(std::uint64_t now)
        {
            if (failed)
                return QueueSignal::Failed;
            if (consumed)
                return QueueSignal::Busy;
            if (!submitted)
            {
                if (now < createdAt || now - createdAt < initialDelay)
                    return QueueSignal::Busy;
            }
            else if (!acknowledged)
            {
                if (!visible || !shown || now < shownAt || now - shownAt < wakeDelay)
                    return QueueSignal::Busy;
                acknowledged = true;
            }
            else if (!visible || activeStall || now < nextReadyAt)
                return QueueSignal::Busy;
            consumed = true;
            return QueueSignal::Ready;
        }
        void Submit(std::uint64_t now)
        {
            Check(consumed, "fake compositor receives a Present only after consuming its queue grant");
            consumed = false;
            submitted = true;
            nextReadyAt = now + 17;
        }
        void Show(std::uint64_t now)
        {
            visible = true;
            if (!shown)
            {
                shown = true;
                shownAt = now;
            }
        }
        void Hide() { visible = false; }
        void Recreate(std::uint64_t now)
        {
            ++generation;
            createdAt = now;
            shownAt = nextReadyAt = 0;
            submitted = acknowledged = shown = visible = consumed = failed = activeStall = false;
        }
    };

    struct Renderer
    {
        Backend& backend;
        PresentationQueue queue{};
        Compositor compositor{};
        Frame prepared{}, displayed{};
        QueueSignal signal = QueueSignal::Busy;
        std::uint64_t now = 0;
        unsigned waits = 0, renders = 0, presents = 0, hides = 0, rebuilds = 0, retainChecks = 0;
        bool pending = false, staged = false, visible = false, reusable = true, hasDisplayed = false;
        bool automatic = false;
        std::function<void()> onRetain{};
        bool Draw()
        {
            ++waits;
            const auto result = queue.Observe(automatic ? compositor.Wait(now) : signal, now);
            signal = QueueSignal::Busy;
            pending = result == QueueDecision::Pending;
            if (result == QueueDecision::Failed)
                reusable = false;
            if (result != QueueDecision::Render)
                return false;
            ++renders;
            return true;
        }
        bool Prepare(const Frame& frame)
        {
            pending = staged = false;
            if (!reusable)
            {
                ++rebuilds;
                queue.Reset();
                compositor.Recreate(now);
                reusable = true;
                hasDisplayed = visible = false;
            }
            // A blocked request cannot overwrite the frame still on the screen.
            if (!Draw())
                return false;
            prepared = frame;
            staged = true;
            return true;
        }
        bool Pending() const { return pending; }
        bool Matches(const Frame& frame) const
        {
            return visible && hasDisplayed && !pending && glide::monitor::SameFrame(frame, prepared);
        }
        bool CanRetain(const Snapshot& expected)
        {
            ++retainChecks;
            if (onRetain)
                onRetain();
            return pending && visible && hasDisplayed && !staged &&
                   !queue.HasUnsubmittedGrant() && expected == Mapping(displayed);
        }
        bool Show()
        {
            if (!staged && !Draw())
                return false;
            Check(backend.state == Mapping(prepared),
                  "matching input mapping is installed only for a fully rendered image");
            staged = false;
            ++presents;
            Check(queue.Submitted(now), "each mocked Present consumes exactly one readiness grant");
            if (automatic)
                compositor.Submit(now);
            displayed = prepared;
            hasDisplayed = visible = true;
            if (automatic)
                compositor.Show(now);
            return true;
        }
        void Hide(bool preserveCapture = false)
        {
            ++hides;
            pending = staged = visible = hasDisplayed = false;
            compositor.Hide();
            if (!preserveCapture)
            {
                if (queue.HasUnsubmittedGrant() || (queue.HasSubmittedFrame() && queue.Cold()))
                    reusable = false;
                queue.Reset();
            }
        }
    };

    Frame Make(double zoom)
    {
        Frame result;
        const glide::Rect bounds{ -1920, -1080, 0, 0 };
        Check(glide::monitor::MakeFrame(bounds, glide::AnchorZoom({}, zoom, { -1000, -600 }, bounds), result),
              "construct negative-coordinate frame fixture");
        return result;
    }

    void InitialPresenterBoundaryMatrix()
    {
        for (const auto delay : { std::uint64_t{ 0 }, std::uint64_t{ 100 }, std::uint64_t{ 101 }, std::uint64_t{ 1999 }, std::uint64_t{ 2000 } })
        {
            for (bool foreign : { false, true })
            {
                ++scenarios;
                ++integrationRows;
                Backend backend;
                Journal journal;
                glide::session::Session owner;
                Renderer renderer{ backend };
                const auto initial = Make(2);
                const auto final = Make(4);
                const auto cold = glide::monitor::Present(initial, renderer, owner, backend, journal);
                Check(cold.pending && cold.result == Result::Unchanged && !cold.localFailure &&
                          backend.reads == 0 && backend.inputWrites == 0 && backend.visualWrites == 0 &&
                          renderer.presents == 0 && renderer.renders == 0 && renderer.hides == 0 &&
                          !renderer.visible && !owner.OwnsState() && !journal.armed,
                      "initial pending touches no input mapping, journal, cursor, or displayed image");
                if (foreign)
                    backend.state.visual = { 2.0f, 40, 80 };
                renderer.now = delay;
                renderer.signal = QueueSignal::Ready;
                const auto result = glide::monitor::Present(final, renderer, owner, backend, journal);
                if (delay >= 2000)
                {
                    Check(result.localFailure && !result.pending && backend.reads == 0 &&
                              backend.inputWrites == 0 && renderer.presents == 0 && !renderer.visible,
                          "initial expiry prevents ownership and display even with a late signal");
                }
                else if (foreign)
                {
                    Check(result.result == Result::Conflict && renderer.renders == 1 &&
                              renderer.presents == 0 && backend.inputWrites == 0 && !owner.OwnsState(),
                          "foreign state during initial startup prevents publication");
                }
                else
                {
                    Check(Success(result.result) && !result.pending && renderer.waits == 2 &&
                              renderer.renders == 1 && renderer.presents == 1 && renderer.visible &&
                              backend.inputWrites == 1 && owner.OwnsState() && journal.armed &&
                              backend.state == Mapping(final) && renderer.queue.Cold(),
                          "initial grant stages the latest geometry without mistaking Present for readiness");
                }
                renderer.Hide();
                Check(Success(owner.Reset(backend, journal)) && !owner.NeedsRecovery() && !journal.armed,
                      "cleanup releases every reached initial ownership state");
                Check(backend.visualWrites == 0, "queue handling never writes the full-desktop visual transform");
            }
        }
    }

    void DelayedCompositorMatrix()
    {
        const std::array<std::uint64_t, 10> wakeDelays{ 0, 1, 99, 100, 101, 250, 750, 1999, 2000, 2001 };
        const std::array<std::uint64_t, 15> polls{ 0, 1, 16, 99, 100, 101, 200, 249, 250, 749, 750, 1500, 1998, 1999, 2000 };
        for (const auto initialDelay : { std::uint64_t{ 0 }, std::uint64_t{ 100 }, std::uint64_t{ 1999 } })
        {
            for (const auto wakeDelay : wakeDelays)
            {
                for (bool changing : { false, true })
                {
                    ++scenarios;
                    ++integrationRows;
                    Backend backend;
                    Journal journal;
                    glide::session::Session owner;
                    Renderer renderer{ backend };
                    renderer.automatic = true;
                    renderer.compositor.initialDelay = initialDelay;
                    renderer.compositor.wakeDelay = wakeDelay;
                    const auto first = Make(2);
                    if (initialDelay)
                        Check(glide::monitor::Present(first, renderer, owner, backend, journal).pending &&
                                  !renderer.visible && backend.inputWrites == 0,
                              "fake initial grant is derived from the device wake delay");
                    renderer.now = initialDelay;
                    Check(Success(glide::monitor::Present(first, renderer, owner, backend, journal).result) &&
                              renderer.visible && renderer.queue.Cold() && renderer.presents == 1 &&
                              !renderer.compositor.acknowledged,
                          "first hidden Present succeeds before the fake compositor acknowledges visibility");
                    const auto original = backend.state;
                    const auto writes = backend.inputWrites;
                    const auto publications = journal.publications;
                    unsigned step = 0;
                    for (const auto delay : polls)
                    {
                        if (delay >= wakeDelay || delay >= 2000)
                            break;
                        renderer.now = initialDelay + delay;
                        const auto requested = changing ? Make(2.01 + 0.1 * step++) : first;
                        const auto retainChecks = renderer.retainChecks;
                        const auto result = glide::monitor::Present(requested, renderer, owner, backend, journal);
                        Check(result.pending && !result.localFailure && result.result == Result::Unchanged,
                              "a slow first compositor acknowledgement is a recoverable nonblocking wait");
                        Check(renderer.visible && renderer.hasDisplayed && renderer.hides == 0 &&
                                  renderer.presents == 1 && renderer.renders == 1 &&
                                  glide::monitor::SameFrame(renderer.displayed, first) &&
                                  glide::monitor::SameFrame(renderer.prepared, first) && backend.state == original &&
                                  backend.inputWrites == writes && journal.publications == publications &&
                                  owner.OwnsState() && owner.Expected() == original && journal.armed,
                              "pending target changes preserve the exact old displayed frame and journaled input map");
                        Check(renderer.retainChecks >= retainChecks + 2,
                              "retaining an owned view verifies both sides of the shared-state read");
                        Check(renderer.queue.WaitMilliseconds() == 0,
                              "compositor activation never blocks the input owner thread");
                    }
                    renderer.now = initialDelay + std::min<std::uint64_t>(wakeDelay, 2000);
                    const auto latest = changing ? Make(4) : first;
                    const auto result = glide::monitor::Present(latest, renderer, owner, backend, journal);
                    if (wakeDelay < 2000)
                    {
                        Check(Success(result.result) && !result.pending && !result.localFailure &&
                                  renderer.visible && renderer.presents == 2 && renderer.renders == 2 &&
                                  backend.state == Mapping(latest) && owner.Expected() == Mapping(latest) &&
                                  renderer.queue.WaitMilliseconds() == 100 && !renderer.queue.Cold(),
                              "confirmed compositor readiness publishes only the latest requested frame and map");
                        Check(backend.inputWrites == writes + (changing ? 1u : 0u),
                              "deferred requests never replay intermediate input mappings");
                        renderer.now += 17;
                        Check(Success(glide::monitor::Present(latest, renderer, owner, backend, journal).result) &&
                                  renderer.presents == 3 && backend.inputWrites == writes + (changing ? 1u : 0u),
                              "ordinary frame cadence resumes after one bounded startup grace");
                    }
                    else
                    {
                        Check(!result.pending && result.localFailure && !renderer.visible && renderer.hides == 1 &&
                                  renderer.presents == 1 && backend.inputWrites == writes && owner.NeedsRecovery(),
                              "permanent startup stall hides the old frame without publishing the pending map");
                    }
                    renderer.Hide();
                    Check(Success(owner.Reset(backend, journal)) && glide::session::IsIdentity(backend.state) &&
                              !owner.NeedsRecovery() && !journal.armed && backend.visualWrites == 0,
                          "every delayed compositor outcome leaves conditionally restorable input ownership");
                }
            }
        }
    }

    void RetainedViewMustBeVerified()
    {
        for (unsigned fault = 0; fault != 8; ++fault)
        {
            ++scenarios;
            ++integrationRows;
            Backend backend;
            Journal journal;
            glide::session::Session owner;
            Renderer renderer{ backend };
            renderer.automatic = true;
            renderer.compositor.wakeDelay = 750;
            const auto first = Make(2);
            Check(Success(glide::monitor::Present(first, renderer, owner, backend, journal).result),
                  "establish a first displayed image before interference");
            renderer.now = 101;
            const auto writes = backend.inputWrites;
            bool reached = false;
            if (fault == 0)
                backend.state.visual = { 2.0f, 20, 30 };
            if (fault == 1)
                backend.state.input.destination.left -= 1;
            if (fault == 2)
                backend.failRead = true;
            if (fault >= 3 && fault <= 6)
                backend.onRead = [&] {
                    reached = true;
                    if (fault == 3)
                        renderer.visible = false;
                    if (fault == 4)
                        renderer.hasDisplayed = false;
                    if (fault == 5)
                        renderer.displayed = Make(3);
                    if (fault == 6)
                        renderer.pending = false;
                };
            if (fault == 7)
                renderer.compositor.failed = true;
            const auto foreign = backend.state;
            const auto result = glide::monitor::Present(Make(4), renderer, owner, backend, journal);
            const auto expected = fault < 2 ? Result::Conflict : fault == 2 ? Result::ReadFailure :
                                                                              Result::WriteFailure;
            Check(result.result == expected && !result.pending && !renderer.visible &&
                      backend.inputWrites == writes && renderer.presents == 1,
                  "foreign, unreadable, cancelled, or missing images cannot be retained beneath owned input");
            if (fault >= 3 && fault <= 6)
                Check(reached && renderer.retainChecks >= 2,
                      "retained-view validity is rechecked after the native ownership read");
            backend.onRead = {};
            backend.failRead = false;
            Check(Success(owner.Reset(backend, journal)) && !owner.NeedsRecovery() && !journal.armed,
                  "interrupted retained startup releases only recognized state");
            Check(fault < 2 ? backend.state == foreign && backend.inputWrites == writes :
                              glide::session::IsIdentity(backend.state),
                  "foreign transforms remain untouched while owned transforms can be restored");
        }
    }

    void CancellationFencesRetainedWait()
    {
        for (unsigned boundary = 0; boundary != 3; ++boundary)
        {
            ++scenarios;
            ++integrationRows;
            Backend backend;
            Journal journal;
            glide::session::Session owner;
            Renderer native{ backend };
            native.automatic = true;
            native.compositor.wakeDelay = 750;
            const auto first = Make(2);
            Check(Success(glide::monitor::Present(first, native, owner, backend, journal).result),
                  "establish a displayed image before lease cancellation");
            glide::runtime::State lifecycle;
            Check(lifecycle.PrepareEnable() && lifecycle.Activate(), "activate the owner lifecycle");
            glide::frame::Lease lease{ lifecycle };
            glide::frame::Renderer renderer{ native, lease };
            glide::frame::Backend fencedBackend{ backend, lease };
            native.now = 101;
            bool reached = false;
            if (boundary == 1)
                backend.onRead = [&] { reached = true; lifecycle.Pause(); };
            else
                native.onRetain = [&] {
                    if ((boundary == 0 && native.retainChecks == 1) ||
                        (boundary == 2 && native.retainChecks == 2))
                    {
                        reached = true;
                        lifecycle.Pause();
                    }
                };
            const auto writes = backend.inputWrites;
            const auto result = glide::monitor::Present(Make(4), renderer, owner, fencedBackend, journal);
            Check(reached && !result.pending && !Success(result.result) && !native.visible &&
                      native.hides == 1 && backend.inputWrites == writes && native.presents == 1,
                  "reentrant pause cannot return a live retained wait or mutate cancelled input state");
            backend.onRead = {};
            native.onRetain = {};
            Check(Success(owner.Reset(backend, journal)) && glide::session::IsIdentity(backend.state),
                  "caller restores exact ownership after each fenced retention boundary");
        }
    }

    void ActivePacingAndCaptureRetention()
    {
        for (bool sameFrame : { false, true })
        {
            ++scenarios;
            ++integrationRows;
            Backend backend;
            Journal journal;
            glide::session::Session owner;
            Renderer renderer{ backend };
            renderer.automatic = true;
            renderer.compositor.wakeDelay = 0;
            const auto first = Make(2);
            Check(Success(glide::monitor::Present(first, renderer, owner, backend, journal).result) &&
                      Success(glide::monitor::Present(first, renderer, owner, backend, journal).result) &&
                      !renderer.queue.Cold(),
                  "establish a genuinely acknowledged active compositor");
            const auto writes = backend.inputWrites;
            renderer.compositor.activeStall = true;
            renderer.now = 100;
            const auto failed = glide::monitor::Present(sameFrame ? first : Make(4), renderer, owner, backend, journal);
            Check(failed.localFailure && !failed.pending && !renderer.visible &&
                      backend.inputWrites == writes && renderer.presents == 2 && owner.NeedsRecovery(),
                  "a confirmed active queue keeps the short failure policy instead of repeated startup grace");
            Check(Success(owner.Reset(backend, journal)) && glide::session::IsIdentity(backend.state),
                  "caller restores the old map after an established queue stops responding");
        }
        for (bool warm : { false, true })
        {
            ++scenarios;
            ++integrationRows;
            Backend backend;
            Journal journal;
            glide::session::Session owner;
            Renderer renderer{ backend };
            const auto frame = Make(2);
            renderer.signal = QueueSignal::Ready;
            Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result),
                  "establish screenshot capture fixture");
            if (warm)
            {
                renderer.signal = QueueSignal::Ready;
                renderer.now = 17;
                Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result),
                      "acknowledge the screenshot fixture before hiding it");
            }
            renderer.Hide(true);
            Check(renderer.queue.HasSubmittedFrame() && renderer.queue.Cold() == !warm &&
                      !renderer.CanRetain(owner.Expected()),
                  "preserved capture keeps pacing state but an invisible window cannot retain mapped input");
            Check(Success(owner.Reset(backend, journal)), "screenshot releases the temporary input mapping");
            renderer.now = warm ? 6000 : 100;
            renderer.signal = QueueSignal::Ready;
            Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result) &&
                      renderer.visible && !renderer.queue.Cold(),
                  "retained capture can resume only with a fresh valid queue grant");
            renderer.Hide();
            const auto lost = glide::monitor::Present(frame, renderer, owner, backend, journal);
            Check(lost.localFailure && !lost.pending && !renderer.visible && owner.NeedsRecovery(),
                  "a lost private image cannot retain a previously owned mapping while waiting");
            Check(Success(owner.Reset(backend, journal)), "cleanup a lost retained capture");
        }
    }

    void CaptureAndQueueHaveSeparateBoundedStarts()
    {
        ++scenarios;
        glide::gpu::FrameGate capture;
        PresentationQueue queue;
        const auto generation = capture.Begin(1920, 1080, 0);
        Check(capture.State(1900) == glide::gpu::FrameState::Pending &&
                  capture.Accept(generation, 1920, 1080),
              "first capture can arrive near its own deadline");
        Check(queue.Observe(QueueSignal::Busy, 1900) == QueueDecision::Pending,
              "queue initial deadline starts when capture permits its first readiness poll");
        Grant(queue, 3899);
        Check(capture.State(5898) == glide::gpu::FrameState::Ready &&
                  queue.Observe(QueueSignal::Busy, 5898) == QueueDecision::Pending,
              "capture, initial queue grant, and first acknowledgement have independent fixed bounds");
        Check(queue.Observe(QueueSignal::Ready, 5899) == QueueDecision::Failed && queue.HasUnsubmittedGrant(),
              "a late compositor acknowledgement cannot override its own expired deadline");
    }

    void CancellationAtEveryAcquisitionBoundary()
    {
        // Before wait; pending initial; granted; mapped; first submitted; second
        // grant staged; confirmed active; retained post-submit pending.
        for (unsigned phase = 0; phase != 8; ++phase)
        {
            ++scenarios;
            ++integrationRows;
            Backend backend;
            Journal journal;
            glide::session::Session owner;
            Renderer renderer{ backend };
            const auto frame = Make(4);
            if (phase == 1)
            {
                Check(glide::monitor::Present(frame, renderer, owner, backend, journal).pending,
                      "cancel before the initial readiness grant");
            }
            else if (phase == 2 || phase == 3)
            {
                renderer.signal = QueueSignal::Ready;
                Check(renderer.Prepare(frame), "cancel a rendered first image before Present");
                if (phase == 3)
                    Check(Success(owner.Apply(Mapping(frame), backend, journal)),
                          "cancel after acquiring the rendered image's input mapping");
            }
            else if (phase >= 4)
            {
                renderer.signal = QueueSignal::Ready;
                Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result),
                      "present the first image before cancellation");
                renderer.now = 17;
                if (phase == 5)
                {
                    renderer.signal = QueueSignal::Ready;
                    Check(renderer.Prepare(frame), "cancel a consumed acknowledgement before its Present");
                }
                else if (phase == 6)
                {
                    renderer.signal = QueueSignal::Ready;
                    Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result),
                          "cancel a fully acknowledged and completed active frame");
                }
                else if (phase == 7)
                {
                    Check(glide::monitor::Present(Make(5), renderer, owner, backend, journal).pending,
                          "cancel while retaining the first unacknowledged displayed frame");
                }
            }
            renderer.Hide();
            const bool recreate = phase == 2 || phase == 3 || phase == 4 || phase == 5 || phase == 7;
            Check(renderer.reusable == !recreate && !renderer.visible && !renderer.staged &&
                      !renderer.queue.HasUnsubmittedGrant() && !renderer.queue.HasSubmittedFrame(),
                  "cancelled grants and unacknowledged submissions cannot reuse a hidden stalled swap chain");
            Check(Success(owner.Reset(backend, journal)) && glide::session::IsIdentity(backend.state) &&
                      !owner.NeedsRecovery() && !journal.armed,
                  "each acquisition phase can conditionally release its exact ownership");
            const auto oldWrites = backend.inputWrites;
            const auto oldGeneration = renderer.compositor.generation;
            renderer.now = 50;
            renderer.signal = QueueSignal::Busy;
            Check(glide::monitor::Present(frame, renderer, owner, backend, journal).pending &&
                      backend.inputWrites == oldWrites && !renderer.visible &&
                      renderer.rebuilds == (recreate ? 1u : 0u) &&
                      renderer.compositor.generation == oldGeneration + (recreate ? 1u : 0u),
                  "restart establishes a fresh resource generation when old work cannot safely resume");
            renderer.now = 100;
            renderer.signal = QueueSignal::Ready;
            Check(Success(glide::monitor::Present(frame, renderer, owner, backend, journal).result) &&
                      renderer.visible && backend.inputWrites == oldWrites + 1,
                  "fresh readiness completes reactivation after every cancellation phase");
        }
    }
} // namespace

void PassivePollingHasItsOwnNonblockingDeadline()
{
    for (const bool late : { false, true })
    {
        PresentationQueue queue;
        Check(queue.Observe(QueueSignal::Ready, 0, true) == QueueDecision::Render && queue.Submitted(0),
              "passive first frame consumes one nonblocking readiness grant");
        Check(queue.Observe(QueueSignal::Ready, 1, true) == QueueDecision::Render && queue.Submitted(1) && !queue.Cold(),
              "passive compositor acknowledgement completes startup normally");
        Check(queue.WaitMilliseconds(true) == 0 && queue.WaitMilliseconds() == 100,
              "parked monitors never add active-view 100 ms waits to the UI loop");
        Check(queue.Observe(QueueSignal::Busy, 10, true) == QueueDecision::Pending &&
                  queue.Observe(QueueSignal::Busy, 109, true) == QueueDecision::Pending,
              "zero-timeout passive polls retain their old cursorless frame within one fixed active deadline");
        const auto decision = queue.Observe(QueueSignal::Ready, late ? 110 : 109, true);
        Check(decision == (late ? QueueDecision::Failed : QueueDecision::Render),
              "late readiness cannot revive an expired passive interval");
        Check(queue.HasUnsubmittedGrant(), "even an expired passive readiness signal requires its pipeline to be rebuilt");
        if (!late)
        {
            Check(queue.Submitted(109) && queue.Observe(QueueSignal::Busy, 110, true) == QueueDecision::Pending &&
                      queue.Observe(QueueSignal::Ready, 209, true) == QueueDecision::Render && queue.Submitted(209),
                  "a successful passive submission starts a fresh bounded readiness interval");
        }
    }
    std::array<PresentationQueue, 16> monitors;
    for (auto& queue : monitors)
    {
        Check(queue.Observe(QueueSignal::Ready, 0, true) == QueueDecision::Render && queue.Submitted(0) &&
                  queue.Observe(QueueSignal::Ready, 1, true) == QueueDecision::Render && queue.Submitted(1),
              "each independent monitor owns a separate presentation queue");
    }
    for (std::uint64_t now = 10; now < 110; ++now)
    {
        Check(monitors[0].Observe(QueueSignal::Busy, now, true) == QueueDecision::Pending,
              "one busy passive monitor remains pending without blocking other monitors");
        for (std::size_t monitor = 1; monitor < monitors.size(); ++monitor)
        {
            auto& queue = monitors[monitor];
            Check(queue.WaitMilliseconds(true) == 0 &&
                      queue.Observe(QueueSignal::Ready, now, true) == QueueDecision::Render && queue.Submitted(now),
                  "a neighboring monitor's pending readiness does not stop live submissions elsewhere");
        }
    }
    Check(monitors[0].Observe(QueueSignal::Busy, 110, true) == QueueDecision::Failed,
          "continual passive congestion expires without refreshing its deadline on every poll");
}

int Run_gpu_queue_policy()
{
    InitialAndAcknowledgementDeadlines();
    FailureAndGrantLifecycle();
    InitialPresenterBoundaryMatrix();
    DelayedCompositorMatrix();
    RetainedViewMustBeVerified();
    CancellationFencesRetainedWait();
    ActivePacingAndCaptureRetention();
    CaptureAndQueueHaveSeparateBoundedStarts();
    CancellationAtEveryAcquisitionBoundary();
    PassivePollingHasItsOwnNonblockingDeadline();
    std::cout << "PASS: " << assertions << " GPU queue policy assertions; " << scenarios
              << " scenarios; " << integrationRows
              << " production presenter/session integration rows; simulated compositor; native GPU wait/Present not executed\n";
    return 0;
}
