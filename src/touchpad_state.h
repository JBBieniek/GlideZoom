#pragma once

#include "touchpad_packets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace glide::touchpad
{

    struct Effect
    {
        bool begin = false, end = false, cancel = false;
        double delta = 0, travel = 0;
    };

    struct Config
    {
        std::uint64_t doubleTapMs = 400;
        std::uint64_t maxGestureMs = 30000;
    };

    // Three physical contacts, an all-up boundary, then three contacts held down.
    // No key/button input is synthesized and no mouse-derived event counts as a finger.
    class State
    {
    public:
        enum class Phase
        {
            Blocked,
            Ready,
            First,
            Between,
            Second,
            Holding
        };

        Effect Configure(Config config) noexcept
        {
            config.doubleTapMs = std::clamp<std::uint64_t>(config.doubleTapMs, 1, 5000);
            config.maxGestureMs = std::clamp<std::uint64_t>(config.maxGestureMs, 1, 30000);
            if (config.doubleTapMs == config_.doubleTapMs && config.maxGestureMs == config_.maxGestureMs)
                return {};
            config_ = config;
            return Cancel();
        }
        Effect SetEnabled(bool enabled) noexcept
        {
            if (enabled_ == enabled)
                return {};
            const auto effect = Cancel();
            enabled_ = enabled;
            return effect;
        }
        Effect Cancel() noexcept
        {
            Effect effect;
            effect.end = Active();
            effect.cancel = effect.end;
            phase_ = Phase::Blocked;
            sawThree_ = lifting_ = false;
            return effect;
        }
        Effect Expire(std::uint64_t now) noexcept
        {
            if (!enabled_)
                return {};
            if (seen_ && now < lastFrameAt_)
                return Cancel();
            if (phase_ == Phase::Between && now - releasedAt_ > config_.doubleTapMs)
                return Cancel();
            if (phase_ == Phase::First || phase_ == Phase::Second || phase_ == Phase::Holding)
            {
                if (!seen_ || now - lastFrameAt_ > StaleMs || now < startedAt_ ||
                    (Active() ? now - startedAt_ >= config_.maxGestureMs : now - startedAt_ > TapMs))
                    return Cancel();
            }
            return {};
        }

        Effect Push(const Frame& frame, std::uint64_t now, bool startsAllowed = true) noexcept
        {
            Effect effect = Expire(now);
            if (!frame.device || frame.count > MaxContacts || (seen_ && now < lastFrameAt_))
                return Merge(effect, Cancel());
            std::array<Contact, 3> active{};
            unsigned count = 0;
            for (unsigned i = 0; i < frame.count; ++i)
            {
                const auto& contact = frame.contacts[i];
                if (!ValidContact(contact))
                    return Merge(effect, Cancel());
                for (unsigned j = 0; j < i; ++j)
                {
                    if (frame.contacts[j].id == contact.id)
                        return Merge(effect, Cancel());
                }
                if (contact.tip)
                {
                    if (!contact.confidence || count == 3)
                        return Merge(effect, Cancel());
                    active[count++] = contact;
                }
            }
            // A fixed three-element sorting network makes every accessed index
            // explicit, including under GCC's aggressive Release inlining.
            if (count >= 2 && active[1].id < active[0].id)
                std::swap(active[0], active[1]);
            if (count == 3)
            {
                if (active[2].id < active[1].id)
                    std::swap(active[1], active[2]);
                if (active[1].id < active[0].id)
                    std::swap(active[0], active[1]);
            }
            lastFrameAt_ = now;
            seen_ = true;
            if (frame.device != device_)
            {
                effect = Merge(effect, Cancel());
                device_ = frame.device;
            }
            if (!enabled_ || frame.button || (!startsAllowed && !Active()))
                return Merge(effect, Cancel());
            if (effect.end)
                return effect; // A stale hold cannot restart from the same packet.
            if (phase_ == Phase::Blocked)
            {
                if (!count)
                    phase_ = Phase::Ready;
                return effect;
            }
            if (phase_ == Phase::Ready)
            {
                if (!count)
                    return effect;
                phase_ = Phase::First;
                startedAt_ = now;
                sawThree_ = lifting_ = false;
            }
            else if (phase_ == Phase::Between)
            {
                if (!count)
                    return effect; // Idle frames never extend the second-tap deadline.
                phase_ = Phase::Second;
                startedAt_ = now;
            }

            if (phase_ == Phase::First)
            {
                if (!count)
                {
                    if (sawThree_ && now - startedAt_ <= TapMs)
                    {
                        phase_ = Phase::Between;
                        releasedAt_ = now;
                        return effect;
                    }
                    effect = Merge(effect, Cancel());
                    phase_ = Phase::Ready; // This validated all-up frame is a clean new boundary.
                    return effect;
                }
                if (!sawThree_)
                {
                    if (now - startedAt_ > JoinMs)
                        return Merge(effect, Cancel());
                    if (count == 3)
                    {
                        tracked_ = active;
                        firstCenter_ = Center(active);
                        sawThree_ = true;
                    }
                    return effect;
                }
                if (count < 3)
                    lifting_ = true;
                else if (lifting_)
                    return Merge(effect, Cancel());
                for (unsigned i = 0; i < count; ++i)
                {
                    const Contact* original = Find(active[i].id);
                    if (!original || std::hypot(active[i].x - original->x, active[i].y - original->y) > TapMotion)
                        return Merge(effect, Cancel());
                }
                return effect;
            }

            if (phase_ == Phase::Second)
            {
                if (!count)
                {
                    effect = Merge(effect, Cancel());
                    phase_ = Phase::Ready;
                    return effect;
                }
                if (now - startedAt_ > JoinMs)
                    return Merge(effect, Cancel());
                if (count != 3)
                    return effect;
                const auto center = Center(active);
                if (std::hypot(center[0] - firstCenter_[0], center[1] - firstCenter_[1]) > SecondRadius)
                    return Merge(effect, Cancel());
                tracked_ = active;
                lastCenter_ = center;
                phase_ = Phase::Holding;
                startedAt_ = now;
                effect.begin = true; // No lift, click, dwell or movement threshold is required.
                return effect;
            }

            if (phase_ == Phase::Holding)
            {
                if (count < 3)
                {
                    phase_ = count == 0 ? Phase::Ready : Phase::Blocked;
                    effect.end = true;
                    return effect; // Release keeps zoom, never toggles it.
                }
                for (unsigned i = 0; i < 3; ++i)
                {
                    if (active[i].id != tracked_[i].id)
                        return Merge(effect, Cancel());
                }
                const auto center = Center(active);
                const double dx = center[0] - lastCenter_[0], dy = center[1] - lastCenter_[1];
                if (std::abs(dx) > MaxJump || std::abs(dy) > MaxJump)
                    return Merge(effect, Cancel());
                lastCenter_ = center;
                effect.delta = dy * 1000.0;
                effect.travel = std::hypot(dx, dy) * 1000.0;
            }
            return effect;
        }

        bool Active() const noexcept { return phase_ == Phase::Holding; }
        Phase Current() const noexcept { return phase_; }
        std::uintptr_t Device() const noexcept { return device_; }
        bool Invariant() const noexcept
        {
            return !Active() || (enabled_ && seen_ && device_ && tracked_[0].id != tracked_[1].id &&
                                 tracked_[0].id != tracked_[2].id && tracked_[1].id != tracked_[2].id);
        }

    private:
        static constexpr Effect Merge(Effect first, Effect second) noexcept
        {
            first.end = first.end || second.end;
            first.cancel = first.cancel || second.cancel;
            return first;
        }
        static std::array<double, 2> Center(const std::array<Contact, 3>& contacts) noexcept
        {
            return { (contacts[0].x + contacts[1].x + contacts[2].x) / 3.0,
                     (contacts[0].y + contacts[1].y + contacts[2].y) / 3.0 };
        }
        const Contact* Find(std::uint32_t id) const noexcept
        {
            for (const auto& contact : tracked_)
                if (contact.id == id)
                    return &contact;
            return nullptr;
        }
        static constexpr std::uint64_t JoinMs = 90, TapMs = 250, StaleMs = 250;
        static constexpr double TapMotion = 0.035, SecondRadius = 0.12, MaxJump = 0.25;
        Config config_{};
        Phase phase_ = Phase::Blocked;
        bool enabled_ = false, seen_ = false, sawThree_ = false, lifting_ = false;
        std::uintptr_t device_ = 0;
        std::uint64_t lastFrameAt_ = 0, startedAt_ = 0, releasedAt_ = 0;
        std::array<Contact, 3> tracked_{};
        std::array<double, 2> firstCenter_{}, lastCenter_{};
    };

} // namespace glide::touchpad
