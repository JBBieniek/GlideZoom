#pragma once

#include "input_state.h"

#include <array>
#include <cstdint>

namespace glide::shortcuts
{
    enum class Command : std::uint32_t
    {
        None,
        ToggleMonitor,
        ToggleDesktop,
        ToggleIndependent,
        ZoomIn,
        ZoomOut,
        Copy,
        RevealIndicator,
        FitWindow
    };

    struct Modifiers
    {
        bool control = false;
        bool leftAlt = false;
        bool rightAlt = false;
        bool shift = false;
        bool windows = false;
    };

    inline Modifiers ReadModifiers(const input::State& state) noexcept
    {
        return { state.Down(input::vk::LeftControl) || state.Down(input::vk::RightControl),
                 state.Down(input::vk::LeftAlt), state.Down(input::vk::RightAlt),
                 state.Down(input::vk::LeftShift) || state.Down(input::vk::RightShift),
                 state.Down(input::vk::LeftWin) || state.Down(input::vk::RightWin) };
    }

    struct Binding
    {
        Command command = Command::None;
        std::uint32_t key = 0;
        bool shift = false;
    };

    inline Binding Match(std::uint32_t key, Modifiers modifiers) noexcept
    {
        // Right Alt is deliberately reserved for AltGr text, including the
        // synthetic Left Ctrl that Windows supplies on international layouts.
        if (!modifiers.control || !modifiers.leftAlt || modifiers.rightAlt || modifiers.windows)
            return {};
        Command command = Command::None;
        if (key == 'Z')
            command = modifiers.shift ? Command::ToggleDesktop : Command::ToggleMonitor;
        else if (key == input::vk::Equal)
            command = Command::ZoomIn; // Shift+OEM_PLUS is the printed '+' alias.
        else if (key == 'F' && modifiers.shift)
            command = Command::FitWindow;
        else if (!modifiers.shift)
        {
            switch (key)
            {
            case 'I': command = Command::ToggleIndependent; break;
            case 'S': command = Command::Copy; break;
            case 'V': command = Command::RevealIndicator; break;
            case input::vk::Add: command = Command::ZoomIn; break;
            case input::vk::Minus:
            case input::vk::Subtract: command = Command::ZoomOut; break;
            default: break;
            }
        }
        return { command, key, modifiers.shift };
    }

    inline constexpr bool AcceptPosted(std::uint64_t command, std::uint64_t postedEpoch, std::uint64_t currentEpoch, bool ready) noexcept
    {
        return ready && postedEpoch == currentEpoch && command > static_cast<std::uint64_t>(Command::None) &&
               command <= static_cast<std::uint64_t>(Command::FitWindow);
    }

    inline input::Decision CancelForAltGr(input::State& state, std::uint32_t key, bool down)
    {
        // A preceding synthetic Left Ctrl may resemble a second control tap.
        // Cancel before the Right Alt event or any following character can act;
        // every modifier remains forwarded, including its original DOWN/UP.
        return key == input::vk::RightAlt && down ? state.Cancel() : input::Decision{};
    }

    struct Decision
    {
        bool consume = false;
        Command command = Command::None;
    };

    class State
    {
    public:
        void Cancel() noexcept
        {
            for (auto& slot : keys_)
                slot.repeat = false;
        }
        void SessionReset() noexcept { keys_ = {}; }
        void ObserveModifiers(Modifiers modifiers) noexcept
        {
            for (std::size_t index = 0; index < keys_.size(); ++index)
                if (keys_[index].owned != Command::None &&
                    Match(static_cast<std::uint32_t>(index), modifiers).command != keys_[index].owned)
                    keys_[index].repeat = false;
        }
        bool ReservedReleasePending() const noexcept
        {
            for (const auto& slot : keys_)
                if (slot.owned != Command::None)
                    return true;
            return false;
        }
        void Seed(std::uint32_t key, bool down) noexcept
        {
            if (key < keys_.size())
                keys_[key].down = down;
        }

        // Sink reserves availability only for a fresh eligible trigger, then
        // posts the command. Repeats never reserve again. A failed fresh sink
        // leaves the complete pair with the existing application/shortcut owner.
        template<class Sink>
        Decision Key(std::uint32_t key, bool down, Modifiers modifiers, bool permitted, Sink sink) noexcept
        {
            if (key >= keys_.size())
                return {};
            ObserveModifiers(modifiers);
            if (!permitted)
                Cancel();
            auto& slot = keys_[key];
            const bool previous = slot.down;
            slot.down = down;
            if (slot.owned != Command::None)
            {
                const Command command = slot.owned;
                const bool repeat = down && previous && slot.repeat && permitted &&
                                    (command == Command::ZoomIn || command == Command::ZoomOut);
                if (!down)
                    slot = {};
                if (repeat && sink(Match(key, modifiers), false))
                    return { true, command };
                return { true, Command::None };
            }
            if (!down || previous || !permitted)
                return {};
            const auto binding = Match(key, modifiers);
            if (binding.command == Command::None || !sink(binding, true))
                return {};
            slot.owned = binding.command;
            slot.repeat = true;
            return { true, binding.command };
        }

    private:
        struct KeyState
        {
            bool down = false;
            bool repeat = false;
            Command owned = Command::None;
        };
        std::array<KeyState, 256> keys_{};
    };
}
