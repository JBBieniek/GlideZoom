#include <stdexcept>
#include "../GlideZoom/pointer_channel.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
unsigned checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
glide::input::PointerSnapshot Frame(std::uint64_t generation) {
    const double n = static_cast<double>(generation);
    return {(generation & 1) != 0, (generation & 2) != 0,
            {-n, n, n + 100, n + 200}, 1.0 + n / 1000000.0, generation,
            {n / 3.0, -n / 7.0}, 1.0 + static_cast<double>(generation % 10)};
}
bool Matches(const glide::input::PointerSnapshot& value) {
    if (!value.epoch) return !value.requested && !value.active && value.zoom == 1 &&
        value.bounds.left == 0 && value.bounds.top == 0 &&
        value.bounds.right == 0 && value.bounds.bottom == 0 &&
        value.offset.x == 0 && value.offset.y == 0 && value.edgePanMultiplier == 1;
    const auto expected = Frame(value.epoch);
    return value.requested == expected.requested && value.active == expected.active &&
        value.bounds.left == expected.bounds.left && value.bounds.top == expected.bounds.top &&
        value.bounds.right == expected.bounds.right && value.bounds.bottom == expected.bounds.bottom &&
        value.zoom == expected.zoom && value.offset.x == expected.offset.x &&
        value.offset.y == expected.offset.y && value.edgePanMultiplier == expected.edgePanMultiplier;
}

bool Equal(const glide::input::PointerSnapshot& a, const glide::input::PointerSnapshot& b) {
    return a.requested == b.requested && a.active == b.active && a.epoch == b.epoch &&
           a.zoom == b.zoom && a.bounds.left == b.bounds.left &&
           a.bounds.top == b.bounds.top && a.bounds.right == b.bounds.right &&
           a.bounds.bottom == b.bounds.bottom && a.offset.x == b.offset.x &&
           a.offset.y == b.offset.y && a.edgePanMultiplier == b.edgePanMultiplier;
}

void EveryField() {
    using glide::input::PointerSnapshot;
    glide::input::PointerChannel channel;
    PointerSnapshot snapshot;
    const PointerSnapshot baseline{true, true, {-1920, -1080, 0, 0}, 2.5, 42,
                                    {-600.0, -300.0}, 3};
    Check(channel.Publish(baseline), "initial complete policy publication succeeds");
    for (unsigned field = 0; field < 11; ++field) {
        auto changed = baseline;
        switch (field) {
        case 0: changed.requested = false; break;
        case 1: changed.active = false; break;
        case 2: changed.bounds.left -= 100; break;
        case 3: changed.bounds.top -= 100; break;
        case 4: changed.bounds.right += 100; break;
        case 5: changed.bounds.bottom += 100; break;
        case 6: changed.zoom += 0.01; break;
        case 7: ++changed.epoch; break;
        case 8: changed.offset.x += 0.25; break;
        case 9: changed.offset.y -= 0.125; break;
        case 10: changed.edgePanMultiplier += 1; break;
        }
        Check(channel.Publish(changed), "changing any one field remains a successful publication");
        Check(channel.Read(snapshot) && Equal(snapshot, changed), "each field change reaches the atomic reader");
        for (unsigned repeat = 0; repeat < 32; ++repeat) {
            Check(channel.Publish(changed), "already-current duplicate publication reports success");
            Check(channel.Read(snapshot) && Equal(snapshot, changed), "duplicate publication preserves all current fields");
        }
        Check(channel.Publish(baseline), "returning to the previous policy is not mistaken for a duplicate");
        Check(channel.Read(snapshot) && Equal(snapshot, baseline), "previous policy can become current again");
    }
}

unsigned UnchangedConcurrency() {
    glide::input::PointerChannel channel;
    const glide::input::PointerSnapshot expected{true, true, {-1920, 0, 0, 1080}, 4, 91,
                                                {-1200, 100}, 10};
    // The initial real publication precedes both threads. Every subsequent
    // writer request is a duplicate of this complete stable policy.
    Check(channel.Publish(expected), "seed unchanged-concurrency policy before starting threads");
    std::atomic<bool> begin{false}, finished{false}, rejected{false};
    std::thread writer([&] {
        while (!begin.load()) std::this_thread::yield();
        for (unsigned publication = 0; publication < 500000; ++publication)
            if (!channel.Publish(expected)) rejected.store(true);
        finished.store(true);
    });
    unsigned attempts = 0, failed = 0;
    bool invalid = false;
    glide::input::PointerSnapshot sample;
    begin.store(true);
    do {
        ++attempts;
        if (!channel.Read(sample)) ++failed;
        else if (!Equal(sample, expected)) invalid = true;
    } while (attempts < 500000 || !finished.load());
    writer.join();
    Check(!rejected.load(), "unchanged writer retains Publish success contract");
    Check(failed == 0, "identical presentation updates create zero failed bounded hook reads");
    Check(!invalid, "unchanged concurrent reads preserve the entire stable policy");
    Check(channel.Read(sample) && Equal(sample, expected), "unchanged final policy remains readable after join");
    return attempts;
}
}

int Run_pointer_channel() {
    EveryField();
    const auto unchangedAttempts = UnchangedConcurrency();
    glide::input::PointerChannel channel;
    glide::input::PointerSnapshot snapshot;
    Check(channel.Read(snapshot) && Matches(snapshot), "startup does not enable pointer changes");
    for (std::uint64_t generation = 1; generation <= 10000; ++generation) {
        Check(channel.Publish(Frame(generation)), "single-writer publication succeeds");
        Check(channel.Read(snapshot) && snapshot.epoch == generation && Matches(snapshot),
              "all scope/zoom/bounds/opt-in/edge geometry fields belong to one publication");
    }
    std::atomic<bool> begin{false}, finished{false}, invalid{false};
    std::atomic<unsigned> accepted{0}, attempts{0};
    std::thread writer([&] {
        while (!begin.load()) std::this_thread::yield();
        for (std::uint64_t generation = 10001; generation <= 250000; ++generation)
            if (!channel.Publish(Frame(generation))) invalid.store(true);
        finished.store(true);
    });
    begin.store(true);
    do {
        const auto previous = snapshot;
        ++attempts;
        if (channel.Read(snapshot)) {
            ++accepted;
            if (!Matches(snapshot)) invalid.store(true);
        } else {
            // Failed publication reads must not leak a half-updated candidate.
            if (!Equal(snapshot, previous)) invalid.store(true);
        }
    } while (!finished.load());
    writer.join();
    Check(!invalid.load(), "mixed UI/hook scheduling cannot combine different publications");
    Check(channel.Read(snapshot) && snapshot.epoch == 250000 && Matches(snapshot),
          "final publication is visible after join");
    Check(attempts.load() > 0, "concurrent read schedule was exercised");
    std::cout << "pointer_channel: " << checks << " assertions; 250000 publications; "
              << attempts.load() << " concurrent read attempts, " << accepted.load()
              << " coherent samples; 11 independently changed fields; 500000 duplicate publications, "
              << unchangedAttempts << " stable read attempts, 0 failed reads; passed\n";
    return 0;
}
