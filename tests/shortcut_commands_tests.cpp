#include "../src/shortcut_commands.h"

#include <array>
#include <iostream>
#include <stdexcept>

namespace
{
    using glide::shortcuts::Command;
    using glide::input::vk::LeftControl;
    using glide::input::vk::RightControl;
    using glide::input::vk::LeftAlt;
    using glide::input::vk::RightAlt;
    using glide::input::vk::LeftShift;
    using glide::input::vk::RightShift;

    unsigned assertions = 0;
    void CheckCommand(bool condition, const char* message)
    {
        ++assertions;
        if (!condition)
        {
            std::cerr << message << '\n';
            throw std::runtime_error(message);
        }
    }

    struct Hook
    {
        glide::input::State gestures;
        glide::shortcuts::State commands;
        bool enabled = true;
        bool available = true;
        unsigned probes = 0, posted = 0, cancellations = 0;
        std::uint64_t now = 1, epoch = 7, postedEpoch = 0;
        Command last = Command::None;

        Hook()
        {
            gestures.Configure({ true, false, 400, 30000 });
            gestures.SetEnabled(true);
        }

        bool Key(std::uint32_t key, bool down)
        {
            const auto cancel = glide::shortcuts::CancelForAltGr(gestures, key, down);
            cancellations += cancel.cancel ? 1u : 0u;
            const auto gesture = gestures.Key(key, down, now++);
            const auto command = commands.Key(key, down, glide::shortcuts::ReadModifiers(gestures),
                enabled && gestures.Active() == glide::input::Gesture::None,
                [this](glide::shortcuts::Binding binding, bool fresh) noexcept {
                    if (fresh)
                        ++probes;
                    if (!available)
                        return false;
                    ++posted;
                    last = binding.command;
                    postedEpoch = epoch;
                    return true;
                });
            CheckCommand(gestures.Invariant(), "gesture key/release ledger remains valid");
            if (key == LeftControl || key == RightControl || key == LeftAlt || key == RightAlt || key == LeftShift || key == RightShift)
                CheckCommand(!gesture.consume && !command.consume && !gestures.SwallowedKey(key),
                             "real hook composition forwards every modifier DOWN, repeat and UP");
            return gesture.consume || command.consume;
        }

        void Epoch()
        {
            ++epoch;
            gestures.Cancel();
            commands.Cancel();
        }
    };

    void PolishTextPairs()
    {
        for (const auto key : { 'Z', 'S', 'C', 'N', 'X', 'O', 'E', 'A', 'L' })
            for (const std::uint32_t shift : { 0u, LeftShift, RightShift })
                for (const bool primed : { false, true })
                {
                    Hook hook;
                    if (primed)
                    {
                        CheckCommand(!hook.Key(LeftControl, true) && !hook.Key(LeftControl, false),
                                     "an earlier genuine Ctrl tap remains forwarded");
                    }
                    if (shift)
                        hook.Key(shift, true);
                    for (unsigned repeat = 0; repeat < 8; ++repeat)
                    {
                        // Windows AltGr sequence, including its synthetic Left Ctrl.
                        CheckCommand(!hook.Key(LeftControl, true), "AltGr synthetic Ctrl DOWN remains forwarded");
                        if (primed && !shift && repeat == 0)
                            CheckCommand(hook.gestures.Active() == glide::input::Gesture::Control,
                                         "the regression really exercises a synthetic Ctrl mistaken for a second tap");
                        CheckCommand(!hook.Key(RightAlt, true), "AltGr DOWN remains forwarded");
                        if (primed && !shift && repeat == 0)
                            CheckCommand(hook.cancellations == 1, "Right Alt cancels that primed Ctrl hold explicitly");
                        CheckCommand(hook.gestures.Active() == glide::input::Gesture::None,
                                     "Right Alt cancels a primed synthetic Ctrl gesture before text or mouse movement");
                        CheckCommand(!hook.Key(static_cast<std::uint32_t>(key), true) &&
                                         !hook.Key(static_cast<std::uint32_t>(key), true) &&
                                         !hook.Key(static_cast<std::uint32_t>(key), false),
                                     "Polish character DOWN, typematic and UP reach their original owner");
                        CheckCommand(!hook.Key(RightAlt, false) && !hook.Key(LeftControl, false),
                                     "both AltGr modifier UP events remain forwarded");
                        CheckCommand(!hook.commands.ReservedReleasePending() && hook.posted == 0 && hook.probes == 0 &&
                                         !hook.gestures.SwallowedKey(static_cast<std::uint32_t>(key)),
                                     "AltGr text never probes a registration, issues a command or leaves an owned release");
                    }
                    if (shift)
                        hook.Key(shift, false);
                    CheckCommand(!hook.Key(LeftControl, true) && hook.gestures.Active() == glide::input::Gesture::None,
                                 "typing AltGr does not prime the next genuine Ctrl gesture");
                    hook.Key(LeftControl, false);
                }
        Hook genuine;
        genuine.Key(LeftControl, true);
        genuine.Key(LeftControl, false);
        CheckCommand(!genuine.Key(LeftControl, true) && genuine.gestures.Active() == glide::input::Gesture::Control,
                     "a genuine double Ctrl hold keeps its existing gesture and forwards Ctrl");
        CheckCommand(!genuine.Key(LeftControl, false) && genuine.gestures.Active() == glide::input::Gesture::None,
                     "a genuine Ctrl gesture also preserves the complete modifier pair");
    }

    void TrueShortcutsAndRepeats()
    {
        struct Case { std::uint32_t key; bool shift; Command command; bool repeats; };
        const std::array<Case, 11> cases{ {
            { 'Z', false, Command::ToggleMonitor, false },
            { 'Z', true, Command::ToggleDesktop, false },
            { 'I', false, Command::ToggleIndependent, false },
            { 'S', false, Command::Copy, false },
            { 'V', false, Command::RevealIndicator, false },
            { 'F', true, Command::FitWindow, false },
            { glide::input::vk::Equal, false, Command::ZoomIn, true },
            { glide::input::vk::Equal, true, Command::ZoomIn, true },
            { glide::input::vk::Add, false, Command::ZoomIn, true },
            { glide::input::vk::Minus, false, Command::ZoomOut, true },
            { glide::input::vk::Subtract, false, Command::ZoomOut, true }
        } };
        for (const auto& item : cases)
            for (const auto control : { LeftControl, RightControl })
            {
                Hook hook;
                hook.Key(control, true);
                hook.Key(LeftAlt, true);
                if (item.shift)
                    hook.Key(LeftShift, true);
                CheckCommand(hook.Key(item.key, true) && hook.last == item.command && hook.probes == 1 && hook.posted == 1,
                             "true Ctrl+Left Alt shortcut probes once and dispatches its intended command");
                for (unsigned repeat = 0; repeat < 5; ++repeat)
                    CheckCommand(hook.Key(item.key, true), "the owned trigger retains every typematic repeat");
                CheckCommand(hook.probes == 1 && hook.posted == (item.repeats ? 6u : 1u),
                             "only zoom commands repeat, without another availability probe");
                CheckCommand(hook.Key(item.key, false) && !hook.Key(item.key, false), "the owned trigger consumes exactly one matching UP");
                if (item.shift)
                    hook.Key(LeftShift, false);
                hook.Key(LeftAlt, false);
                hook.Key(control, false);
                CheckCommand(!hook.commands.ReservedReleasePending(), "a complete genuine shortcut leaves no release obligation");
            }
    }

    void UnavailableDisabledAndCanceledPairs()
    {
        for (const bool disabled : { false, true })
        {
            Hook hook;
            hook.Key(LeftControl, true);
            hook.Key(LeftAlt, true);
            hook.available = disabled;
            hook.enabled = !disabled;
            CheckCommand(!hook.Key('Z', true), "a conflicting or disabled shortcut forwards its fresh DOWN");
            hook.available = hook.enabled = true;
            CheckCommand(!hook.Key('Z', true) && !hook.Key('Z', false) && hook.posted == 0,
                         "later availability cannot seize a previously forwarded trigger or its UP");
            CheckCommand(hook.Key('Z', true) && hook.Key('Z', false), "a fresh eligible pair can be captured after the old pair finishes");
        }
        for (unsigned boundary = 0; boundary < 4; ++boundary)
        {
            Hook hook;
            hook.Key(LeftControl, true);
            hook.Key(LeftAlt, true);
            CheckCommand(hook.Key(glide::input::vk::Equal, true), "zoom trigger starts before a cancellation boundary");
            if (boundary == 0)
                hook.Epoch();
            else if (boundary == 1)
            {
                hook.enabled = false;
                hook.commands.Cancel();
                hook.enabled = true;
            }
            else if (boundary == 2)
            {
                hook.Key(LeftAlt, false);
                hook.Key(LeftAlt, true);
            }
            else
            {
                hook.Key(RightAlt, true);
                hook.Key(RightAlt, false);
            }
            CheckCommand(hook.Key(glide::input::vk::Equal, true) && hook.posted == 1 && hook.probes == 1,
                         "cancellation prevents a held repeat from restarting but retains ownership");
            CheckCommand(hook.Key(glide::input::vk::Equal, false), "cancellation keeps its owed UP consumed");
            CheckCommand(hook.Key(glide::input::vk::Equal, true) && hook.posted == 2,
                         "only a new physical press restarts the shortcut after cancellation");
        }
        Hook held;
        held.commands.Seed('Z', true);
        held.Key(LeftControl, true);
        held.Key(LeftAlt, true);
        CheckCommand(!held.Key('Z', true) && !held.Key('Z', false), "startup cannot seize a trigger that was already held");
        CheckCommand(held.Key('Z', true), "a post-startup fresh press may start");
        held.commands.SessionReset();
        CheckCommand(!held.commands.ReservedReleasePending() && !held.Key('Z', false),
                     "a confirmed desktop boundary clears the old desktop's release ledger");
    }

    void PostedEpochAndModifierExclusion()
    {
        Hook hook;
        hook.Key(LeftControl, true);
        hook.Key(LeftAlt, true);
        hook.Key('Z', true);
        CheckCommand(glide::shortcuts::AcceptPosted(static_cast<std::uint64_t>(hook.last), hook.postedEpoch, hook.epoch, true),
                     "the UI accepts a valid command in its original epoch");
        hook.Epoch();
        CheckCommand(!glide::shortcuts::AcceptPosted(static_cast<std::uint64_t>(hook.last), hook.postedEpoch, hook.epoch, true) &&
                         !glide::shortcuts::AcceptPosted(1, hook.epoch, hook.epoch, false) &&
                         !glide::shortcuts::AcceptPosted(0, hook.epoch, hook.epoch, true) &&
                         !glide::shortcuts::AcceptPosted(99, hook.epoch, hook.epoch, true),
                     "stale, paused and invalid posted commands cannot act");
        for (const auto blocker : { RightAlt, glide::input::vk::LeftWin, glide::input::vk::RightWin })
        {
            Hook blocked;
            blocked.Key(LeftControl, true);
            blocked.Key(LeftAlt, true);
            blocked.Key(blocker, true);
            CheckCommand(!blocked.Key('Z', true) && !blocked.Key('Z', false) && blocked.probes == 0,
                         "Right Alt and Windows modifiers always exclude optional Ctrl+Alt commands");
        }
    }
}

int main()
{
    assertions = 0;
    PolishTextPairs();
    TrueShortcutsAndRepeats();
    UnavailableDisabledAndCanceledPairs();
    PostedEpochAndModifierExclusion();
    std::cout << "PASS: " << assertions << " AltGr/shortcut assertions; Polish lower/upper pairs, control gestures, "
                 "availability, repeats, cancellation, posted epochs; native keyboard layout not executed\n";
    return 0;
}
