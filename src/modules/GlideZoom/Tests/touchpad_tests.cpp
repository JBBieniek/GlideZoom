#include <stdexcept>
#include "../GlideZoom/touchpad_state.h"
#include "../GlideZoom/zoom_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
using namespace glide::touchpad;
std::uint64_t assertions = 0, matrix = 0, transitions = 0;
void Check(bool value, const char* message) {
    ++assertions;
    if (!value) { std::cerr << "FAIL: " << message << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
void Near(double actual, double expected, const char* message) {
    Check(std::isfinite(actual) && std::abs(actual - expected) < 1e-9, message);
}
Frame Contacts(unsigned count, double y = 0.5, std::uintptr_t device = 1, unsigned firstId = 10) {
    Frame frame;
    frame.device = device;
    frame.count = count;
    for (unsigned i = 0; i < count && i < MaxContacts; ++i)
        frame.contacts[i] = {firstId + i, 0.4 + i * 0.05, y, true, true, true};
    return frame;
}
State Ready(std::uintptr_t device = 1) {
    State state;
    state.SetEnabled(true);
    state.Push(Contacts(0, 0.5, device), 1);
    Check(state.Current() == State::Phase::Ready, "all-up arms initial state");
    return state;
}
State Hold() {
    auto state = Ready();
    Check(!state.Push(Contacts(3), 10).begin, "first touchdown does not begin");
    Check(!state.Push(Contacts(0), 50).begin, "first lift does not begin");
    Check(state.Push(Contacts(3), 100).begin && state.Active(), "second touchdown immediately begins HOLD");
    return state;
}

void Scenarios() {
    {
        auto state = Hold();
        const double distance = 220.0 * std::log(1.05) / std::log(2.0) / 1000.0;
        const auto effect = state.Push(Contacts(3, 0.5 - distance), 110);
        Near(glide::DragZoom(1, effect.delta), 1.05, "small physical movement produces precisely 1.05 target");
        Check(effect.delta < 0 && effect.travel > 0 && state.Active(), "upward movement is a continuing hold");
        const auto back = state.Push(Contacts(3), 120);
        Near(effect.delta + back.delta, 0, "opposite movement preserves exact fractional deltas");
        Check(state.Push(Contacts(2), 130).end && !state.Active(), "first finger departure ends hold");
        Check(!state.Push(Contacts(3), 140).begin, "replacing departed finger cannot restart without all-up");
        state.Push(Contacts(0), 150);
        Check(!state.Push(Contacts(3), 160).begin, "post-release first tap is not a triple-tap toggle");
    }
    {
        auto state = Ready();
        state.Push(Contacts(1), 10); state.Push(Contacts(2), 30); state.Push(Contacts(3), 60);
        state.Push(Contacts(2), 80); state.Push(Contacts(1), 90); state.Push(Contacts(0), 100);
        state.Push(Contacts(1), 140); state.Push(Contacts(2), 160);
        Check(state.Push(Contacts(3), 190).begin, "staggered finger touchdown and lift recognized");
        Frame released = Contacts(3);
        for (auto& contact : released.contacts) contact.tip = false;
        const auto end = state.Push(released, 200);
        Check(end.end && !end.cancel && state.Current() == State::Phase::Ready,
              "tip-up contacts count as all-up, release keeps zoom and arms next pair");
    }
    {
        auto state = Ready();
        state.Push(Contacts(1), 10); state.Push(Contacts(0), 20);
        Check(state.Current() == State::Phase::Ready, "ordinary one-finger motion leaves a clean boundary");
        state.Push(Contacts(3), 30); state.Push(Contacts(0), 40);
        Check(state.Push(Contacts(3, 0.5, 1, 100), 50).begin,
              "second tap may use new IDs after complete release");
    }
    {
        auto state = Ready();
        state.Push(Contacts(3), 10); state.Push(Contacts(3, 0.55), 20); state.Push(Contacts(0), 30);
        Check(!state.Push(Contacts(3), 40).begin, "first movement cannot qualify as tap");
    }
    {
        auto state = Ready();
        state.Push(Contacts(3), 10); state.Push(Contacts(0), 50);
        state.Push(Contacts(0), 400);
        Check(!state.Push(Contacts(3), 451).begin, "idle all-up reports do not extend double-tap deadline");
    }
    {
        auto state = Hold();
        const auto move = state.Push(Contacts(3, 0.49), 110, false);
        Check(state.Active() && !move.end, "coherent-read overlap permits existing hold");
        Check(state.SetEnabled(false).cancel && !state.Active(), "independent closed gate cancels hold");
        state.SetEnabled(true);
        Check(!state.Push(Contacts(3), 120).begin, "reenable while fingers held cannot restart");
    }
    {
        auto state = Hold();
        Check(state.Expire(351).cancel, "lost reports cancel active hold after lease");
        Check(!state.Push(Contacts(3), 352).begin, "late held frame cannot restart stale hold");
    }
    {
        auto state = Hold();
        Check(state.Push(Contacts(0, 0.5, 2), 110).cancel, "device change cancels old hold");
        Check(!state.Push(Contacts(3, 0.5, 2), 120).begin, "other device cannot inherit first tap or hold");
    }
    {
        State state;
        state.SetEnabled(true);
        Check(!state.Push(Contacts(3), 1).begin, "startup with held contacts is blocked");
        Check(!state.Push(Contacts(3), 100).begin, "held startup contacts do not create taps");
    }
    {
        auto state = Ready();
        state.Push(Contacts(1), 10);
        Check(!state.Push(Contacts(3), 101).begin && state.Current() == State::Phase::Blocked,
              "slow finger assembly cannot become three-finger tap");
    }
    {
        auto state = Hold();
        state.Configure({400, 120});
        state.Push(Contacts(0), 120); state.Push(Contacts(3), 130); state.Push(Contacts(0), 150);
        Check(state.Push(Contacts(3), 200).begin, "configured hold starts");
        state.Push(Contacts(3), 300);
        Check(state.Expire(320).cancel, "duration limit cancels even with fresh reports");
    }
}

void ValidMatrix() {
    const std::array<std::array<unsigned, 3>, 6> orders{{{{0,1,2}},{{0,2,1}},{{1,0,2}},{{1,2,0}},{{2,0,1}},{{2,1,0}}}};
    for (std::uintptr_t device : {1u, 2u, 99u}) for (unsigned id : {0u, 7u, 0xFFFFFFF0u})
    for (const auto& order : orders) for (std::uint64_t duration : {1u, 100u, 250u})
    for (std::uint64_t gap : {1u, 100u, 400u}) for (double delta : {-0.02, -0.0001, 0.0, 0.0001, 0.02}) {
        ++matrix;
        auto state = Ready(device);
        auto first = Contacts(3, 0.5, device, id);
        auto reordered = first;
        for (unsigned i=0;i<3;++i) reordered.contacts[i] = first.contacts[order[i]];
        state.Push(first, 10);
        state.Push(Contacts(0, 0.5, device), 10 + duration);
        const auto begin = state.Push(reordered, 10 + duration + gap);
        Check(begin.begin && state.Active(), "valid tap pair across timing/device/ID/order matrix");
        for (unsigned i=0;i<3;++i) reordered.contacts[i].y += delta;
        const auto move = state.Push(reordered, 11 + duration + gap);
        Near(move.delta, delta * 1000, "centroid displacement independent of report order");
        Check(state.Invariant(), "valid matrix state invariant");
        const auto end = state.Push(Contacts(0, 0.5, device), 12 + duration + gap);
        Check(end.end && !end.cancel && !state.Active(), "release keeps zoom in valid matrix");
    }
}

void InterruptionMatrix() {
    for (unsigned phase=0; phase<6; ++phase) for (unsigned failure=0; failure<15; ++failure) {
        ++matrix;
        auto state = Ready();
        if (phase >= 1) state.Push(Contacts(1), 10);
        if (phase >= 2) state.Push(Contacts(3), 20);
        if (phase >= 3) state.Push(Contacts(0), 30);
        if (phase >= 4) state.Push(Contacts(1), 40);
        if (phase >= 5) state.Push(Contacts(3), 50);
        const bool wasActive = state.Active();
        Effect effect;
        Frame bad = Contacts(3);
        switch (failure) {
        case 0: effect = state.Cancel(); break; // foreground / explicit reset
        case 1: effect = state.SetEnabled(false); break; // desktop / heartbeat / pause
        case 2: effect = state.Configure({500,30000}); break; // settings / epoch
        case 3: effect = state.Push(Contacts(4), 60); break;
        case 4: effect = state.Push(Contacts(5), 60); break;
        case 5: bad.contacts[1].confidence=false; effect=state.Push(bad,60); break;
        case 6: bad.contacts[1].x=std::numeric_limits<double>::quiet_NaN(); effect=state.Push(bad,60); break;
        case 7: bad.contacts[1].y=std::numeric_limits<double>::infinity(); effect=state.Push(bad,60); break;
        case 8: bad.contacts[1].x=-0.01; effect=state.Push(bad,60); break;
        case 9: bad.contacts[1].id=bad.contacts[0].id; effect=state.Push(bad,60); break;
        case 10: bad.contacts[1].valid=false; effect=state.Push(bad,60); break;
        case 11: bad.device=0; effect=state.Push(bad,60); break;
        case 12: bad.button=true; effect=state.Push(bad,60); break;
        case 13: effect=state.Push(bad,0); break;
        default: effect=state.Expire(6000); break;
        }
        Check(!effect.begin && !state.Active(), "interruption never begins or preserves hold");
        Check(effect.end == wasActive && (!wasActive || effect.cancel), "active interruption emits exactly cancelled end");
        Check(state.Invariant(), "interruption invariant");
        state.SetEnabled(true);
        Check(!state.Push(Contacts(3), 7000).begin, "held contacts after interruption cannot restart");
    }
}

void MixedSequences() {
    // Deterministic model exploration with real state code and invalid native-frame adapters.
    std::uint32_t random = 0x641B20u;
    for (unsigned run=0; run<2000; ++run) {
        auto state = Ready();
        std::uint64_t now=2;
        for (unsigned event=0; event<100; ++event) {
            ++transitions;
            random=random*1664525u+1013904223u;
            now+=(random>>24)&31u;
            const bool wasActive=state.Active();
            Effect effect;
            switch ((random>>8)%10) {
            case 0: effect=state.SetEnabled(false); break;
            case 1: effect=state.SetEnabled(true); break;
            case 2: effect=state.Cancel(); break;
            case 3: effect=state.Expire(now+500); break;
            case 4: effect=state.Push(Contacts(3,0.49,2),now); break;
            default: effect=state.Push(Contacts(random%6),now,(random&128)!=0); break;
            }
            Check(state.Invariant(), "mixed trace preserves state invariant");
            Check(!effect.end || wasActive, "mixed trace cannot invent an End");
            Check(!effect.begin || state.Active(), "Begin establishes active hold");
            Check(!(effect.begin && effect.end), "one frame cannot cancel then begin another hold");
            Check(std::isfinite(effect.delta) && std::isfinite(effect.travel) && effect.travel >= 0,
                  "mixed effects remain finite and bounded");
        }
    }
}
} // namespace

int Run_touchpad() {
    Scenarios(); ValidMatrix(); InterruptionMatrix(); MixedSequences();
    std::cout << "PASS: " << assertions << " touchpad assertions; " << matrix
              << " matrix cases; " << transitions << " mixed transitions\n";
    return 0;
}
