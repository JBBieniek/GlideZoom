#pragma once

#include <cstdint>

namespace glide::tray
{

    inline constexpr std::uint16_t IconId = 1;
    namespace event
    {
        inline constexpr std::uint16_t ContextMenu = 0x007b;
        inline constexpr std::uint16_t LeftUp = 0x0202;
        inline constexpr std::uint16_t LeftDoubleClick = 0x0203;
        inline constexpr std::uint16_t RightUp = 0x0205;
        inline constexpr std::uint16_t Select = 0x0400;
        inline constexpr std::uint16_t KeySelect = 0x0401;
    }

    enum class Action
    {
        None,
        Restore,
        Menu
    };
    enum class Visibility
    {
        None,
        Show,
        Hide
    };

    // UI-thread presentation policy only. It never changes zoom, input ownership,
    // settings, or the runtime epoch. Native visibility is not inferred from a
    // remembered flag: ShowWindow/IsWindowVisible remain the native boundary.
    class State
    {
    public:
        bool Registered() const noexcept { return registered_; }
        bool Version4() const noexcept { return version4_; }
        bool Stopping() const noexcept { return stopping_; }
        bool MenuOpen() const noexcept { return menuOpen_; }
        bool Invariant() const noexcept { return !version4_ || registered_; }

        void Installed(bool version4) noexcept
        {
            if (stopping_)
                return;
            registered_ = true;
            version4_ = version4;
        }
        void Lost() noexcept
        {
            registered_ = version4_ = false;
            // A native popup may still be unwinding during Explorer restart.
            // Do not allow another popup until its owner calls EndMenu.
        }
        void Stop() noexcept { stopping_ = true; }

        Visibility RequestVisibility(bool hide) const noexcept
        {
            if (stopping_)
                return Visibility::None;
            return hide && registered_ ? Visibility::Hide : Visibility::Show;
        }
        bool BeginMenu() noexcept
        {
            if (stopping_ || !registered_ || menuOpen_)
                return false;
            menuOpen_ = true;
            return true;
        }
        void EndMenu() noexcept { menuOpen_ = false; }

        Action Callback(std::uintptr_t wParam, std::uintptr_t lParam) const noexcept
        {
            if (stopping_ || !registered_)
                return Action::None;
            const auto id = version4_ ? ((lParam >> 16) & 0xffffu) : wParam;
            const auto message = version4_ ? (lParam & 0xffffu) : lParam;
            if (id != IconId)
                return Action::None;
            if (message == event::LeftUp || message == event::LeftDoubleClick ||
                message == event::Select || message == event::KeySelect)
                return Action::Restore;
            // v4 sends WM_CONTEXTMENU for the menu. Accepting its raw RightUp as
            // well can show the same popup twice. Older shells use RightUp.
            if (message == (version4_ ? event::ContextMenu : event::RightUp))
                return Action::Menu;
            return Action::None;
        }

    private:
        bool registered_ = false, version4_ = false, stopping_ = false, menuOpen_ = false;
    };

    // The native adapter supplies Add, Refresh and Version4. A failed Add may mean
    // a duplicate TaskbarCreated arrived while our HWND/ID icon still exists;
    // Refresh verifies and updates that exact icon before recovering its callbacks.
    // A failed version negotiation
    // retains a usable legacy registration and its matching callback decoder.
    // Repeat registration requests do not add duplicate icons. Explorer loss must
    // first be reported through Lost, and every new Add negotiates again.
    template<class Shell>
    bool Register(State& state, Shell& shell) noexcept
    {
        if (state.Stopping())
            return false;
        if (state.Registered())
            return true;
        if (!shell.Add() && !shell.Refresh())
            return false;
        state.Installed(shell.Version4());
        return true;
    }

} // namespace glide::tray
