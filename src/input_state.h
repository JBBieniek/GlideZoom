#pragma once

#include "zoom_core.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace glide::input
{

    // Windows virtual-key values, without depending on windows.h. The adapter
    // supplies left/right modifier values and passes injected events through without
    // calling State. No method here synthesizes or replays input.
    namespace vk
    {
        constexpr std::uint32_t Shift = 0x10, Control = 0x11, Alt = 0x12;
        constexpr std::uint32_t Escape = 0x1B, Space = 0x20;
        constexpr std::uint32_t LeftWin = 0x5B, RightWin = 0x5C;
        constexpr std::uint32_t Add = 0x6B, Subtract = 0x6D;
        constexpr std::uint32_t LeftShift = 0xA0, RightShift = 0xA1;
        constexpr std::uint32_t LeftControl = 0xA2, RightControl = 0xA3;
        constexpr std::uint32_t LeftAlt = 0xA4, RightAlt = 0xA5;
        constexpr std::uint32_t Equal = 0xBB, Minus = 0xBD;
    } // namespace vk

    enum class Gesture
    {
        None,
        Control,
        Space,
        Mouse
    };
    enum class Button
    {
        Left,
        Right,
        Middle,
        X1,
        X2
    };

    struct Config
    {
        bool doubleControl = false;
        bool mouseDoubleClick = false;
        std::uint64_t doubleTapMs = 400;
        std::uint64_t maxGestureMs = 30000;
    };

    // Effects, not persistent state. Apply end before begin; cancel qualifies end.
    // A consumed DOWN owns its repeats and UP until an explicit SessionReset.
    struct Decision
    {
        bool consume = false;
        bool begin = false;
        bool end = false;
        bool cancel = false;
        bool reset = false;
        int step = 0; // Left Win + Left Alt + = / + / - / NumAdd / NumSubtract.
    };

    class State
    {
    public:
        Decision Configure(Config config)
        {
            Decision result = Cancel();
            config.doubleTapMs = std::clamp<std::uint64_t>(config.doubleTapMs, 1, 5000);
            config.maxGestureMs = std::clamp<std::uint64_t>(config.maxGestureMs, 1, 30000);
            config_ = config;
            controlTap_ = DoubleTapHold(config_.doubleTapMs);
            mouseTap_ = DoubleTapHold(config_.doubleTapMs, 8.0);
            return result;
        }

        Decision SetEnabled(bool enabled)
        {
            if (enabled_ == enabled)
                return {};
            Decision result = Cancel();
            enabled_ = enabled;
            return result;
        }

        // A control publication can overlap one bounded hook read. Block fresh
        // activation without cancelling a committed hold or its reserved releases.
        void SetStartsAllowed(bool allowed)
        {
            if (!allowed)
            {
                controlTap_.Reset();
                mouseTap_.Reset();
            }
            startsAllowed_ = allowed;
        }

        Decision Key(std::uint32_t key, bool down, std::uint64_t now)
        {
            // Input events update/cancel the physical chord before the service
            // timer may repeat it. A release arriving at a repeat deadline must
            // never issue one final stale step on its way out.
            Decision result = ExpireOnly(now);
            if (key >= keys_.size())
                return result;
            const bool fresh = down && !keys_[key];
            keys_[key] = down;
            if (stepHeld_ && !ExactStepKeys(stepHeld_))
                ClearStep();

            // Preserve paired releases through pause, focus changes, reconfiguration
            // and timeout. A forwarded DOWN is never converted on a later repeat.
            if (swallowedKeys_[key])
            {
                result.consume = true;
                // Windows typematic repeats are paired with the swallowed DOWN,
                // but zoom repeat cadence belongs to Expire(), not keyboard speed.
                if (!down)
                {
                    swallowedKeys_[key] = false;
                    if (key == vk::Space && gesture_ == Gesture::Space)
                        Stop(result, false);
                }
                return result;
            }

            if (!down && gesture_ == Gesture::Control && key == vk::LeftControl)
            {
                Stop(result, false);
            }
            else if (gesture_ != Gesture::None &&
                     ((fresh && !RequiredKey(key)) || !HeldGestureKeys()))
            {
                Stop(result, true);
            }

            if (!enabled_)
                return result;

            if (fresh && key == vk::Escape && ControlDown() && AltDown())
            {
                Stop(result, true);
                swallowedKeys_[key] = true;
                result.consume = result.reset = true;
                return result;
            }

            if (!startsAllowed_)
                return result;

            if (fresh && StepKey(key) && ExactStepKeys(key) && NoButtons())
            {
                Stop(result, true);
                swallowedKeys_[key] = true;
                stepHeld_ = key;
                stepStartedAt_ = stepObservedAt_ = now;
                stepRepeatSlot_ = 0;
                result.consume = true;
                result.step = StepDirection(key);
                return result;
            }

            if (fresh && key == vk::Space && gesture_ == Gesture::None &&
                ExactKeys(true, true, true) && NoButtons())
            {
                swallowedKeys_[key] = true;
                Start(Gesture::Space, now, result);
                result.consume = true;
                return result;
            }

            if (key == vk::LeftControl && config_.doubleControl)
            {
                if (fresh && gesture_ == Gesture::None && ExactKeys(true, false, false) &&
                    NoButtons() && controlTap_.Down(now, {}))
                {
                    Start(Gesture::Control, now, result);
                }
                else if (!down)
                {
                    controlTap_.Up(now);
                }
            }
            else if (fresh)
            {
                controlTap_.Reset();
            }
            if (fresh)
                mouseTap_.Reset();
            return result; // All Ctrl, Alt, Shift and Win events always reach Windows.
        }

        Decision ButtonEvent(Button button, bool down, Vec2 point, std::uint64_t now)
        {
            Decision result = ExpireOnly(now);
            const std::size_t index = Index(button);
            if (index >= buttons_.size())
                return result;
            const bool fresh = down && !buttons_[index];
            buttons_[index] = down;
            if (!NoButtons())
                ClearStep();

            if (swallowedButtons_[index])
            {
                result.consume = true;
                if (!down)
                {
                    swallowedButtons_[index] = false;
                    mouseTap_.Up(now);
                    if (gesture_ == Gesture::Mouse)
                        Stop(result, false);
                }
                return result;
            }

            if (!fresh)
                return result;
            controlTap_.Reset();
            if (gesture_ != Gesture::None)
                Stop(result, true);

            // This optional gesture reserves the complete modified click pair,
            // including its first tap. It is disabled by default for that reason.
            if (enabled_ && startsAllowed_ && config_.mouseDoubleClick && button == Button::Left &&
                ExactKeys(true, true, false) && NoButtonsExceptLeft() && detail::Finite(point))
            {
                swallowedButtons_[index] = true;
                result.consume = true;
                if (mouseTap_.Down(now, point))
                    Start(Gesture::Mouse, now, result);
            }
            else
            {
                mouseTap_.Reset();
            }
            return result;
        }

        Decision Move(Vec2 point, std::uint64_t now)
        {
            Decision result = ExpireOnly(now);
            mouseTap_.Move(point);
            if (!detail::Finite(point))
                Stop(result, true);
            result.consume = gesture_ != Gesture::None;
            return result;
        }

        Decision Wheel(bool configuredChord, std::uint64_t now)
        {
            Decision result = ExpireOnly(now);
            Stop(result, true);
            result.consume = configuredChord && CanZoom();
            return result;
        }

        // Poll only modifiers: they are never suppressed, so their async state is
        // meaningful. Polling Space/Escape or a reserved mouse button could erase an
        // owed release because Windows did not receive the corresponding DOWN.
        Decision ReconcileModifier(std::uint32_t key, bool down)
        {
            if (!IsModifier(key) || keys_[key] == down)
                return {};
            keys_[key] = down;
            if (stepHeld_ && !ExactStepKeys(stepHeld_))
                ClearStep();
            controlTap_.Reset();
            mouseTap_.Reset();
            Decision result;
            if (gesture_ != Gesture::None && !HeldGestureKeys())
                Stop(result, true);
            return result;
        }

        Decision Expire(std::uint64_t now)
        {
            Decision result = ExpireOnly(now);
            if (stepHeld_)
            {
                const auto elapsed = now - stepStartedAt_;
                if (elapsed >= kStepDelayMs)
                {
                    const auto slot = 1 + (elapsed - kStepDelayMs) / kStepRepeatMs;
                    if (slot > stepRepeatSlot_)
                    {
                        // Record the latest slot even if a coherent control read
                        // is temporarily unavailable. No catch-up burst is queued.
                        stepRepeatSlot_ = slot;
                        if (startsAllowed_)
                            result.step = StepDirection(stepHeld_);
                    }
                }
            }
            return result;
        }

        Decision Cancel(bool clearPhysical = false)
        {
            Decision result;
            Stop(result, true);
            if (clearPhysical)
            {
                keys_.fill(false);
                buttons_.fill(false);
                swallowedKeys_.fill(false);
                swallowedButtons_.fill(false);
            }
            return result;
        }

        // Only an input-desktop/session discontinuity establishes a new boundary:
        // releases on the old desktop will not necessarily return to these hooks.
        // Ordinary focus changes, pause and settings changes must use Cancel().
        Decision SessionReset() { return Cancel(true); }

        Gesture Active() const { return gesture_; }
        Gesture Current() const { return gesture_; }
        bool Down(std::uint32_t key) const { return key < keys_.size() && keys_[key]; }
        bool ButtonDown(Button button) const
        {
            return Index(button) < buttons_.size() && buttons_[Index(button)];
        }
        bool SwallowedKey(std::uint32_t key) const
        {
            return key < swallowedKeys_.size() && swallowedKeys_[key];
        }
        bool SwallowedButton(Button button) const
        {
            return Index(button) < swallowedButtons_.size() && swallowedButtons_[Index(button)];
        }

        // Wheel readiness. A foreground key or mouse drag prevents interception.
        bool CanZoom() const
        {
            if (!enabled_ || !startsAllowed_ || !NoButtons())
                return false;
            for (std::size_t key = 0; key < keys_.size(); ++key)
            {
                if (keys_[key] && !IsModifier(static_cast<std::uint32_t>(key)))
                    return false;
            }
            return true;
        }

        // Executable contract used by the portable event-sequence tests.
        bool Invariant() const
        {
            if (stepHeld_ && (!enabled_ || !StepKey(stepHeld_) ||
                              !swallowedKeys_[stepHeld_] || !ExactStepKeys(stepHeld_) ||
                              !NoButtons() || gesture_ != Gesture::None))
                return false;
            for (std::size_t key = 0; key < keys_.size(); ++key)
            {
                if (swallowedKeys_[key] &&
                    (!keys_[key] || (key != vk::Space && key != vk::Escape && !StepKey(static_cast<std::uint32_t>(key)))))
                    return false;
            }
            for (std::size_t i = 0; i < buttons_.size(); ++i)
            {
                if (swallowedButtons_[i] && (!buttons_[i] || i != Index(Button::Left)))
                    return false;
            }
            if (gesture_ == Gesture::None)
                return true;
            if (!enabled_ || !HeldGestureKeys())
                return false;
            if (gesture_ == Gesture::Control)
                return config_.doubleControl && NoButtons();
            if (gesture_ == Gesture::Space)
                return swallowedKeys_[vk::Space] && NoButtons();
            return gesture_ == Gesture::Mouse && config_.mouseDoubleClick &&
                   swallowedButtons_[Index(Button::Left)] && NoButtonsExceptLeft();
        }

    private:
        static constexpr std::uint64_t kStepDelayMs = 250, kStepRepeatMs = 33;
        void ClearStep() noexcept
        {
            stepHeld_ = 0;
            stepStartedAt_ = stepObservedAt_ = stepRepeatSlot_ = 0;
        }
        Decision ExpireOnly(std::uint64_t now)
        {
            Decision result;
            if (gesture_ != Gesture::None &&
                (now < startedAt_ || now - startedAt_ >= config_.maxGestureMs))
            {
                Stop(result, true);
            }
            if (stepHeld_)
            {
                if (!enabled_ || !ExactStepKeys(stepHeld_) || !NoButtons() ||
                    now < stepObservedAt_ || now < stepStartedAt_ ||
                    now - stepStartedAt_ >= config_.maxGestureMs)
                    ClearStep();
                else
                    stepObservedAt_ = now;
            }
            return result;
        }
        static constexpr bool StepKey(std::uint32_t key)
        {
            return key == vk::Equal || key == vk::Minus || key == vk::Add || key == vk::Subtract;
        }
        static constexpr int StepDirection(std::uint32_t key)
        {
            return key == vk::Equal || key == vk::Add ? 1 : -1;
        }
        bool ExactStepKeys(std::uint32_t trigger) const
        {
            for (std::size_t key = 0; key < keys_.size(); ++key)
            {
                // '+' on the main keyboard is Shift+OEM_PLUS. Shift is only an
                // alias modifier for that trigger; numpad and '-' stay exact.
                if (trigger == vk::Equal && (key == vk::LeftShift || key == vk::RightShift))
                    continue;
                const bool wanted = key == vk::LeftWin || key == vk::LeftAlt || key == trigger;
                if (keys_[key] != wanted)
                    return false;
            }
            return true;
        }
        static constexpr std::size_t Index(Button button) { return static_cast<std::size_t>(button); }
        static constexpr bool IsModifier(std::uint32_t key)
        {
            return key == vk::Shift || key == vk::Control || key == vk::Alt ||
                   key == vk::LeftWin || key == vk::RightWin ||
                   (key >= vk::LeftShift && key <= vk::RightAlt);
        }
        bool ControlDown() const
        {
            return keys_[vk::Control] || keys_[vk::LeftControl] || keys_[vk::RightControl];
        }
        bool AltDown() const
        {
            return keys_[vk::Alt] || keys_[vk::LeftAlt] || keys_[vk::RightAlt];
        }
        bool ExactKeys(bool control, bool alt, bool space) const
        {
            for (std::size_t key = 0; key < keys_.size(); ++key)
            {
                const bool wanted = (control && key == vk::LeftControl) ||
                                    (alt && key == vk::LeftAlt) || (space && key == vk::Space);
                if (keys_[key] != wanted)
                    return false;
            }
            return true;
        }
        bool NoButtons() const
        {
            for (bool held : buttons_)
                if (held)
                    return false;
            return true;
        }
        bool NoButtonsExceptLeft() const
        {
            for (std::size_t i = 1; i < buttons_.size(); ++i)
                if (buttons_[i])
                    return false;
            return true;
        }
        bool RequiredKey(std::uint32_t key) const
        {
            if (gesture_ == Gesture::Control)
                return key == vk::LeftControl;
            return key == vk::LeftControl || key == vk::LeftAlt ||
                   (gesture_ == Gesture::Space && key == vk::Space);
        }
        bool HeldGestureKeys() const
        {
            if (gesture_ == Gesture::Control)
                return ExactKeys(true, false, false);
            if (gesture_ == Gesture::Space)
                return ExactKeys(true, true, true);
            if (gesture_ == Gesture::Mouse)
                return ExactKeys(true, true, false);
            return true;
        }
        void Start(Gesture gesture, std::uint64_t now, Decision& result)
        {
            gesture_ = gesture;
            startedAt_ = now;
            result.begin = true;
        }
        void Stop(Decision& result, bool cancel)
        {
            if (gesture_ != Gesture::None)
            {
                result.end = true;
                result.cancel = cancel;
            }
            gesture_ = Gesture::None;
            ClearStep();
            startedAt_ = 0;
            controlTap_.Reset();
            mouseTap_.Reset();
        }

        Config config_{};
        bool enabled_ = false;
        bool startsAllowed_ = true;
        Gesture gesture_ = Gesture::None;
        std::uint64_t startedAt_ = 0;
        std::uint32_t stepHeld_ = 0;
        std::uint64_t stepStartedAt_ = 0, stepObservedAt_ = 0, stepRepeatSlot_ = 0;
        std::array<bool, 256> keys_{};
        std::array<bool, 256> swallowedKeys_{};
        std::array<bool, 5> buttons_{};
        std::array<bool, 5> swallowedButtons_{};
        DoubleTapHold controlTap_{};
        DoubleTapHold mouseTap_{ 400, 8.0 };
    };

} // namespace glide::input
