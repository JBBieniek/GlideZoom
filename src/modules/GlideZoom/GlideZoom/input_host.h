#pragma once

#include "control_channel.h"
#include "desktop_context.h"
#include "foreground_context.h"
#include "input_state.h"
#include "input_health.h"
#include "input_routing.h"
#include "input_transport.h"
#include "touchpad_hid.h"
#include "touchpad_state.h"
#include "pointer_adapter.h"
#include "pointer_channel.h"
#include "wheel_activation.h"

#include <windows.h>
#include <atomic>
#include <type_traits>

namespace glide::input
{

    enum class WheelModifier
    {
        ControlAlt = 0,
        Control = 1,
        LeftAlt = 2,
        Windows = 3
    };
    enum class EventKind
    {
        Begin,
        End,
        Reset,
        Wheel,
        Delta,
        Step,
        TouchpadBegin,
        TouchpadDelta,
        TouchpadEnd,
        WheelCancel
    };
    enum class TouchpadAvailability
    {
        Disabled,
        Waiting,
        Detected,
        Unsupported
    };

    struct Event
    {
        EventKind kind = EventKind::End;
        Gesture gesture = Gesture::None;
        Vec2 anchor{}; // Native desktop/source coordinates; only meaningful for Begin.
        double amount = 0.0;
        double travel = 0.0; // Absolute travel survives coalescing of opposite deltas.
        std::uint64_t epoch = 0;
        bool cancel = false;
        std::uint64_t timestamp = 0; // Physical hook time, never the UI dequeue time.
    };
    static_assert(std::is_trivially_copyable_v<Event>);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(std::atomic<double>::is_always_lock_free);

    // One producer (the private hook thread), one consumer (the UI thread).
    // Hooks never call UI code, allocate, read files, or call Magnification APIs.
    class Host
    {
    public:
        Host() = default;
        Host(const Host&) = delete;
        Host& operator=(const Host&) = delete;
        ~Host() { Stop(); }

        bool Start(HINSTANCE instance)
        {
            if (thread_)
                return Healthy();
            instance_ = instance;
            fault_.store(false);
            ready_.store(false);
            stopping_.store(false);
            queue_.Reset();
            uiBeat_.store(GetTickCount64());
            startedEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!startedEvent_ || !stopEvent_)
            {
                CloseHandles();
                return false;
            }
            thread_ = CreateThread(nullptr, 0, ThreadEntry, this, 0, nullptr);
            if (!thread_)
            {
                CloseHandles();
                return false;
            }
            if (WaitForSingleObject(startedEvent_, 5000) != WAIT_OBJECT_0 || !ready_.load())
            {
                Stop();
                return false;
            }
            return true;
        }

        void Stop()
        {
            if (thread_)
            {
                stopping_.store(true, std::memory_order_release);
                SetEvent(stopEvent_);
                // Never destroy storage while callbacks can still refer to it.
                // The worker has no synchronous UI dependency; no forced termination.
                WaitForSingleObject(thread_, INFINITE);
            }
            CloseHandles();
            ready_.store(false);
        }

        // UI thread only. A changed epoch invalidates pending gesture commands.
        // Controls publish only on change; zoom is independent frame telemetry.
        void Update(Config config, bool enabled, double zoom, std::uint64_t epoch, WheelModifier wheel = WheelModifier::ControlAlt, bool touchpadEnabled = false)
        {
            const auto tap = std::clamp<std::uint64_t>(config.doubleTapMs, 1, 5000);
            const auto duration = std::clamp<std::uint64_t>(config.maxGestureMs, 1, 30000);
            const std::uint64_t options = (enabled ? 1u : 0u) |
                                          (config.doubleControl ? 2u : 0u) | (config.mouseDoubleClick ? 4u : 0u) |
                                          (touchpadEnabled ? 32u : 0u) |
                                          ((static_cast<std::uint64_t>(wheel) & 3u) << 3) | (tap << 16) | (duration << 32);
            publication_.Publish({ options, epoch, zoom });
        }

        void Beat() { uiBeat_.store(GetTickCount64(), std::memory_order_release); }
        void SetRoutingWake(HWND window, UINT message) noexcept
        {
            routingWake_.store(window);
            routingMessage_.store(message);
        }
        bool UpdateRouting(const routing::Snapshot& snapshot) noexcept { return routingPublication_.Publish(snapshot); }
        bool UpdateActivationWheel(const wheel::Activation& admission) noexcept { return activationPublication_.Publish(admission); }
        bool ReadRoutingFeedback(routing::Feedback& feedback) const noexcept { return routingFeedback_.Read(feedback); }
        void UpdatePointer(const PointerSnapshot& snapshot) noexcept { pointerPublication_.Publish(snapshot); }
        bool PointerFaulted() const noexcept { return pointerFault_.load(std::memory_order_acquire); }
        bool ReadPointerFeedback(pointer::Feedback& output) const noexcept
        {
            if (!Healthy() || stopping_.load(std::memory_order_acquire))
                return false;
            return pointerFeedback_.Read(output);
        }
        HostHealth Health() const noexcept
        {
            const auto beat = hostBeat_.load(std::memory_order_acquire);
            const auto now = GetTickCount64();
            return EvaluateHostHealth(ready_.load(std::memory_order_acquire),
                                      fault_.load(std::memory_order_acquire),
                                      thread_ && WaitForSingleObject(thread_, 0) == WAIT_TIMEOUT,
                                      now,
                                      beat);
        }
        bool Healthy() const noexcept { return Health() == HostHealth::Ready; }
        bool Faulted() const noexcept { return Health() == HostHealth::Failed; }
        TouchpadAvailability TouchpadStatus() const { return touchpadStatus_.load(std::memory_order_acquire); }

        bool Poll(Event& event) { return queue_.Pop(event); }

    private:
        // Each hook-thread entry publishes its final committed state, including
        // early returns and cancellation. Nested setter callbacks cannot expose a
        // provisional position: the outer Move publishes after verified readback.
        class FeedbackScope
        {
        public:
            explicit FeedbackScope(Host& owner) noexcept :
                owner_(owner) {}
            ~FeedbackScope() noexcept
            {
                owner_.PublishPointerFeedback();
                owner_.PublishRoutingFeedback();
            }

        private:
            Host& owner_;
        };
        void PublishPointerFeedback() noexcept
        {
            if (pointer_.InWarp() || dragPointer_.InWarp())
                return;
            pointerFault_.store(pointer_.Faulted(), std::memory_order_release);
            const bool eligible = pointerControlsCoherent_ && InputGate(GetTickCount64()) &&
                                  state_.Active() == Gesture::None && !touchpadState_.Active();
            pointerFeedback_.Publish(eligible ? (routing_.HasForwardedDrag() ? dragPointer_.FeedbackSnapshot() : pointer_.FeedbackSnapshot()) : pointer::Feedback{});
        }
        void PublishRoutingFeedback() noexcept
        {
            const auto now = GetTickCount64();
            const auto feedback = routing_.Report(state_.Active() == Gesture::None && !touchpadState_.Active() &&
                                                      InputGate(now),
                                                  now);
            if (!routingFeedback_.Publish(feedback))
                fault_.store(true, std::memory_order_release);
            if (feedback.requestSerial != lastRoutingFeedback_.requestSerial ||
                feedback.observedGeneration != lastRoutingFeedback_.observedGeneration ||
                feedback.canCommit != lastRoutingFeedback_.canCommit || feedback.faulted != lastRoutingFeedback_.faulted)
            {
                if (const HWND window = routingWake_.load())
                    PostMessageW(window, routingMessage_.load(), 0, 0);
            }
            lastRoutingFeedback_ = feedback;
        }
        enum class Motion
        {
            None,
            Pending,
            Raw,
            Hook
        };
        static constexpr wchar_t ClassName[] = L"GlideZoom.InputHost";
        inline static thread_local Host* current_ = nullptr;

        static DWORD WINAPI ThreadEntry(void* value) noexcept
        {
            auto* host = static_cast<Host*>(value);
            current_ = host;
            try
            {
                host->Run();
            }
            catch (...)
            {
                host->LatchFault();
                host->Cleanup();
                SetEvent(host->startedEvent_);
            }
            current_ = nullptr;
            return 0;
        }
        static LRESULT CALLBACK KeyboardProc(int code, WPARAM message, LPARAM data) noexcept
        {
            if (code == HC_ACTION && current_)
            {
                const auto& input = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
                const State before = current_->state_;
                try
                {
                    if (current_->Keyboard(message, input))
                        return 1;
                }
                catch (...)
                {
                    current_->state_ = before;
                    current_->LatchFault();
                    const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                    const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
                    // Reprocess in disabled state: a new DOWN passes, while an
                    // already reserved UP is consumed once and clears its ledger.
                    if (!(input.flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) && (down || up) &&
                        current_->state_.Key(input.vkCode, down, GetTickCount64()).consume)
                        return 1;
                }
            }
            return CallNextHookEx(nullptr, code, message, data);
        }
        static LRESULT CALLBACK MouseProc(int code, WPARAM message, LPARAM data) noexcept
        {
            if (code == HC_ACTION && current_)
            {
                const auto& input = *reinterpret_cast<const MSLLHOOKSTRUCT*>(data);
                const State before = current_->state_;
                try
                {
                    if (current_->Mouse(message, input))
                        return 1;
                }
                catch (...)
                {
                    current_->state_ = before;
                    current_->LatchFault();
                    Button button;
                    bool down;
                    if (!(input.flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) &&
                        DecodeButton(message, input, button, down) &&
                        current_->state_.ButtonEvent(button, down, {}, GetTickCount64()).consume)
                        return 1;
                }
            }
            return CallNextHookEx(nullptr, code, message, data);
        }
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) noexcept
        {
            if (message == WM_INPUT && current_)
            {
                try
                {
                    current_->Raw(reinterpret_cast<HRAWINPUT>(lParam), static_cast<DWORD>(GetMessageTime()));
                }
                catch (...)
                {
                    current_->LatchFault();
                }
            }
            else if (message == WM_INPUT_DEVICE_CHANGE && current_)
            {
                FeedbackScope feedback(*current_);
                current_->CancelTouchpad();
                current_->touchpadHid_.Removed(reinterpret_cast<HANDLE>(lParam));
                if ((current_->controls_.Options() & 32) != 0)
                    current_->touchpadStatus_.store(TouchpadAvailability::Waiting, std::memory_order_release);
            }
            // DefWindowProc performs the required foreground WM_INPUT cleanup.
            return DefWindowProcW(window, message, wParam, lParam);
        }

        void Run()
        {
            state_ = State{};
            routing_ = routing::State{};
            activation_ = {};
            activationClosed_ = activationCancelled_ = activationClosePending_ = false;
            activationCloseAt_ = 0;
            activationIdentity_ = activationMonitor_ = 0;
            desktop_ = false;
            seeded_ = false;
            confirmedAway_ = false;
            foreground_ = ForegroundContext{};
            controls_ = ControlReader{};
            touchpadState_ = touchpad::State{};
            touchpadAssembler_.Reset();
            touchpadStatus_.store(TouchpadAvailability::Disabled, std::memory_order_release);
            WNDCLASSW type{};
            type.lpfnWndProc = WindowProc;
            type.hInstance = instance_;
            type.lpszClassName = ClassName;
            classRegistered_ = RegisterClassW(&type) != 0;
            if (classRegistered_ || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            {
                window_ = CreateWindowExW(0, ClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance_, nullptr);
            }
            if (window_)
            {
                keyboard_ = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, instance_, 0);
                mouse_ = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, instance_, 0);
                RAWINPUTDEVICE device{ 0x01, 0x02, RIDEV_INPUTSINK, window_ };
                rawRegistered_ = RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
                RAWINPUTDEVICE touchpadDevice{ 0x0D, 0x05, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window_ };
                touchpadRegistered_ = RegisterRawInputDevices(&touchpadDevice, 1, sizeof(touchpadDevice)) != FALSE;
            }
            const bool started = window_ && keyboard_ && mouse_;
            if (started)
            {
                Service(GetTickCount64());
            }
            ready_.store(started, std::memory_order_release);
            SetEvent(startedEvent_);
            bool draining = false;
            std::uint64_t stopAt = 0;
            while (started)
            {
                const DWORD wake = MsgWaitForMultipleObjects(1, &stopEvent_, FALSE, 8, QS_ALLINPUT);
                if (wake == WAIT_FAILED)
                {
                    LatchFault();
                    break;
                }
                if (wake == WAIT_OBJECT_0 && !draining)
                {
                    draining = true;
                    stopAt = GetTickCount64();
                    ResetEvent(stopEvent_);
                    Apply(state_.SetEnabled(false), stopAt);
                    CancelTouchpad();
                }
                MSG message{};
                for (unsigned count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count)
                {
                    if (message.message == WM_QUIT)
                    {
                        draining = true;
                        stopAt = 0;
                        break;
                    }
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                const auto now = GetTickCount64();
                Service(now);
                if (draining && (!ReservedReleasePending() || now - stopAt >= 500))
                    break;
            }
            Cleanup();
        }

        void Cleanup() noexcept
        {
            pointer_.Pause();
            PublishPointerFeedback();
            state_.SetEnabled(false);
            touchpadState_.SetEnabled(false);
            touchpadAssembler_.Reset();
            touchpadHid_.Reset();
            ClearMotion();
            if (keyboard_)
                UnhookWindowsHookEx(keyboard_);
            if (mouse_)
                UnhookWindowsHookEx(mouse_);
            keyboard_ = mouse_ = nullptr;
            if (rawRegistered_)
            {
                RAWINPUTDEVICE device{ 0x01, 0x02, RIDEV_REMOVE, nullptr };
                RegisterRawInputDevices(&device, 1, sizeof(device));
            }
            rawRegistered_ = false;
            if (touchpadRegistered_)
            {
                RAWINPUTDEVICE device{ 0x0D, 0x05, RIDEV_REMOVE, nullptr };
                RegisterRawInputDevices(&device, 1, sizeof(device));
            }
            touchpadRegistered_ = false;
            touchpadStatus_.store(TouchpadAvailability::Disabled, std::memory_order_release);
            if (window_)
                DestroyWindow(window_);
            window_ = nullptr;
            if (classRegistered_)
                UnregisterClassW(ClassName, instance_);
            classRegistered_ = false;
            ready_.store(false, std::memory_order_release);
        }

        void Service(std::uint64_t now)
        {
            FeedbackScope feedback(*this);
            hostBeat_.store(now, std::memory_order_release);
            const auto context = glide::QueryInputDesktop();
            const bool desktop = context == glide::InputDesktop::Current;
            if (!desktop)
            {
                pointer_.Pause();
                Apply(state_.SetEnabled(false), now);
                // An unavailable query is not proof that input moved to another
                // desktop: retain owed releases unless a different name was read.
                if (context == glide::InputDesktop::Other && !confirmedAway_)
                {
                    Apply(state_.SessionReset(), now);
                    routing_.SessionReset();
                    confirmedAway_ = true;
                    seeded_ = false;
                }
            }
            else if (!seeded_)
            {
                Apply(state_.SetEnabled(false), now);
                if (confirmedAway_)
                    Apply(state_.SessionReset(), now);
                SeedHeldKeys();
                POINT seedPoint{};
                if (GetPhysicalCursorPos(&seedPoint))
                    routing_.ObservePoint({ static_cast<double>(seedPoint.x), static_cast<double>(seedPoint.y) });
                unsigned physicalButtons = 0;
                for (unsigned button = 0; button < 5; ++button)
                    if (state_.ButtonDown(static_cast<Button>(button)))
                        physicalButtons |= 1u << button;
                routing_.SeedButtons(physicalButtons);
                seeded_ = true;
                confirmedAway_ = false;
            }
            desktop_ = desktop;
            Synchronize(now);
            if (desktop_)
            {
                for (std::uint32_t key : { vk::LeftShift, vk::RightShift, vk::LeftControl, vk::RightControl, vk::LeftAlt, vk::RightAlt, vk::LeftWin, vk::RightWin })
                {
                    Apply(state_.ReconcileModifier(key, (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0), now);
                }
            }
            // Service held =/- zoom after current controls, desktop and physical
            // modifiers have been reconciled. State emits at most one due repeat;
            // OS key-repeat messages do not control or multiply this cadence.
            Apply(state_.Expire(now), now);
            ApplyTouchpad(touchpadState_.Expire(now));
            if (touchpadAssembler_.Expire(now))
                CancelTouchpad();
            if (motion_ == Motion::Pending && firstHookMotion_ && now - firstHookMotion_ >= 40)
            {
                motion_ = Motion::Hook;
                Accumulate(pendingHook_, pendingTravel_);
                pendingHook_ = pendingTravel_ = 0;
            }
            FlushDelta();
        }

        void Synchronize(std::uint64_t now, const Vec2* physical = nullptr)
        {
            // Global controls keep their owned hold through ordinary app activation.
            // A focus transition only discards the pointer baseline, never the
            // keyboard/touchpad release ledger or the first tap of a double tap.
            foreground_.Observe(reinterpret_cast<std::uintptr_t>(GetForegroundWindow()), pointer_);
            const auto oldEpoch = controls_.Epoch(), oldOptions = controls_.Options();
            const bool coherent = controls_.Synchronize(publication_, state_, [this, now] { return InputGate(now); }, [this, now](Decision decision) { Apply(decision, now); });
            controlsCoherent_ = coherent;
            routing::Snapshot routeSnapshot;
            const bool routeCoherent = routingPublication_.Read(routeSnapshot);
            routing_.Observe(routeSnapshot, routeCoherent, controls_.Epoch());
            POINT cursor{};
            bool havePoint = physical != nullptr;
            Vec2 observed = physical ? *physical : Vec2{};
            if (physical)
                routing_.ObservePoint(*physical);
            else if (GetPhysicalCursorPos(&cursor))
            {
                havePoint = true;
                observed = { static_cast<double>(cursor.x), static_cast<double>(cursor.y) };
                routing_.ObservePoint({ static_cast<double>(cursor.x), static_cast<double>(cursor.y) });
            }
            ObserveActivation(now, observed, havePoint);
            const bool routeReady = routing_.ZoomReady();
            pointerControlsCoherent_ = coherent && routeReady;
            state_.SetStartsAllowed(coherent && routeReady && InputGate(now));
            Apply(state_.SetEnabled((controls_.Options() & 1) != 0 && routeReady && InputGate(now)), now);
            if (routing_.Faulted())
                LatchFault();
            if (oldEpoch != controls_.Epoch() || oldOptions != controls_.Options())
            {
                pointer_.Pause();
                ApplyTouchpad(touchpadState_.Cancel(), oldEpoch);
                touchpadAssembler_.Reset();
            }
            const auto options = controls_.Options();
            ApplyTouchpad(touchpadState_.Configure({ (options >> 16) & 0xFFFFu, (options >> 32) & 0xFFFFu }));
            const bool selected = (options & 32) != 0;
            const bool enabled = routeReady && selected && touchpadRegistered_ && (options & 1) != 0 &&
                                 InputGate(now) && !OtherInputHeld();
            ApplyTouchpad(touchpadState_.SetEnabled(enabled));
            touchpadStartsAllowed_ = coherent && enabled;
            if (!selected)
                touchpadStatus_.store(TouchpadAvailability::Disabled, std::memory_order_release);
            else if (!touchpadRegistered_)
                touchpadStatus_.store(TouchpadAvailability::Unsupported, std::memory_order_release);
            else if (touchpadStatus_.load(std::memory_order_relaxed) == TouchpadAvailability::Disabled)
                touchpadStatus_.store(TouchpadAvailability::Waiting, std::memory_order_release);
            const double appliedZoom = publication_.Zoom();
            if (appliedZoom != zoom_)
                pointer_.InvalidateFeedback();
            zoom_ = appliedZoom;
        }

        bool InputGate(std::uint64_t now) const
        {
            const auto beat = uiBeat_.load(std::memory_order_acquire);
            return desktop_ && FreshHeartbeat(now, beat) &&
                   !fault_.load(std::memory_order_acquire) && !stopping_.load(std::memory_order_acquire);
        }
        bool OtherInputHeld() const
        {
            if (state_.Active() != Gesture::None)
                return true;
            for (std::uint32_t key = 8; key < 256; ++key)
                if (state_.Down(key))
                    return true;
            for (unsigned button = 0; button < 5; ++button)
                if (state_.ButtonDown(static_cast<Button>(button)))
                    return true;
            return false;
        }

        void SeedHeldKeys()
        {
            // Called only while disabled at startup or a real desktop boundary.
            for (std::uint32_t key = 8; key < 256; ++key)
            {
                if (key == vk::Shift || key == vk::Control || key == vk::Alt)
                    continue;
                if ((GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0)
                    state_.Key(key, true, 0);
            }
            const int buttons[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
            for (unsigned i = 0; i < 5; ++i)
            {
                if ((GetAsyncKeyState(buttons[i]) & 0x8000) != 0)
                {
                    state_.ButtonEvent(static_cast<Button>(i), true, {}, 0);
                }
            }
        }

        bool Keyboard(WPARAM message, const KBDLLHOOKSTRUCT& data)
        {
            FeedbackScope feedback(*this);
            if (data.flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED))
                return false;
            const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
            if (!down && !up)
                return false;
            const auto now = GetTickCount64();
            Synchronize(now);
            CancelTouchpad(); // A deliberate keyboard action ends the touchpad command.
            const Decision decision = state_.Key(data.vkCode, down, now);
            Apply(decision, now);
            if (!WheelChord() || (down && data.vkCode != vk::LeftControl && data.vkCode != vk::RightControl &&
                                  data.vkCode != vk::LeftAlt && data.vkCode != vk::RightAlt &&
                                  data.vkCode != vk::LeftWin && data.vkCode != vk::RightWin))
                CloseActivation(now, false);
            return decision.consume;
        }

        bool Mouse(WPARAM message, const MSLLHOOKSTRUCT& data)
        {
            FeedbackScope feedback(*this);
            const auto now = GetTickCount64();
            const Vec2 point{ static_cast<double>(data.pt.x), static_cast<double>(data.pt.y) };
            if (message == WM_MOUSEMOVE && pointer_.InWarp())
            {
                pointer_.ObserveWarp(point);
                return false;
            }
            if (message == WM_MOUSEMOVE && dragPointer_.InWarp())
            {
                dragPointer_.ObserveWarp(point);
                return false;
            }
            if (message == WM_MOUSEMOVE && dragPointer_.BypassEcho(point, now))
            {
                routing::Route boundary;
                if (!routing_.DragConstraint(point, boundary))
                    return false;
            }
            if (message == WM_MOUSEMOVE && !routing_.HasForwardedDrag() && pointer_.BypassEcho(point, now))
            {
                pointerFault_.store(pointer_.Faulted(), std::memory_order_release);
                return false;
            }
            if (data.flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED))
            {
                if (message == WM_MOUSEMOVE)
                    pointer_.Pause();
                return false;
            }
            Synchronize(now, &point);
            Decision decision;
            routing::Route dragRoute;
            if (message == WM_MOUSEMOVE && routing_.DragConstraint(point, dragRoute))
            {
                // Same-monitor movement may leave the old crop so Follow can
                // pan it. Only a different monitor needs confinement, and a
                // racing clip/setter must never consume the app's owed UP.
                pointer_.Pause();
                const pointer::View dragView{ true, true, dragRoute.bounds, 1, controls_.Epoch(), {}, 1 };
                pointer::DragNative<pointer::NativeOps> native{ pointerNative_ };
                if (native.Initialize(dragView))
                    dragPointer_.Constrain(point, now, dragView, native.Limit(), native);
                // Suppress this original cross-monitor MOVE even if a native
                // correction was unavailable. Yield to app-owned clipping;
                // keyboard controls and the physical release stay usable.
                return true;
            }
            if (message == WM_MOUSEMOVE)
            {
                if (touchpadState_.Active())
                {
                    pointer_.Pause();
                    return true;
                } // Freeze its anchor.
                decision = state_.Move(point, now);
                Apply(decision, now);
                if (decision.consume)
                    pointer_.Pause();
                if (decision.consume && motion_ != Motion::Raw)
                {
                    POINT cursor{};
                    if (!GetPhysicalCursorPos(&cursor))
                    {
                        Apply(state_.Cancel(), now);
                        return false;
                    }
                    const double delta = std::clamp((point.y - cursor.y) * zoom_,
                                                    -240.0,
                                                    240.0);
                    const double dx = std::clamp((point.x - cursor.x) * zoom_, -240.0, 240.0);
                    const double travel = std::hypot(dx, delta);
                    if (motion_ == Motion::Hook)
                        Accumulate(delta, travel);
                    else if (motion_ == Motion::Pending)
                    {
                        pendingHook_ = std::clamp(pendingHook_ + delta, -100000.0, 100000.0);
                        pendingTravel_ = std::min(100000.0, pendingTravel_ + travel);
                        if (travel != 0 && !firstHookMotion_)
                            firstHookMotion_ = now;
                    }
                }
                if (decision.consume)
                    return true;
                PointerSnapshot snapshot;
                if (!pointerControlsCoherent_ || !pointerPublication_.Read(snapshot) ||
                    snapshot.epoch != controls_.Epoch() || (controls_.Options() & 1) == 0 || !InputGate(now))
                {
                    pointer_.Pause();
                    return false;
                }
                const pointer::View view{ snapshot.requested, snapshot.active, snapshot.bounds, snapshot.zoom, snapshot.epoch, snapshot.offset, routing_.HasForwardedDrag() ? 1 : snapshot.edgePanMultiplier, snapshot.speedMultiplier, snapshot.adjustForZoom };
                bool consume = false;
                if (routing_.HasForwardedDrag())
                {
                    pointer::DragNative<pointer::NativeOps> native{ pointerNative_ };
                    if (native.Initialize(view))
                        consume = dragPointer_.Move(point, false, now, view, true, state_.Active() != Gesture::None, native);
                    else
                        dragPointer_.Pause();
                }
                else
                    consume = pointer_.Move(point, false, now, view, true, state_.Active() != Gesture::None, pointerNative_);
                pointerFault_.store(pointer_.Faulted(), std::memory_order_release);
                return consume;
            }
            if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL)
            {
                CancelTouchpad();
                const bool chord = message == WM_MOUSEWHEEL && WheelChord();
                if (chord && routing_.BlockWheel(true) && QueueActivationWheel(point, now, static_cast<SHORT>(HIWORD(data.mouseData))))
                    return true;
                if (routing_.BlockWheel(message == WM_MOUSEWHEEL && WheelChord()))
                {
                    Apply(state_.Cancel(), now);
                    return true;
                }
                decision = state_.Wheel(message == WM_MOUSEWHEEL && WheelChord(), now);
                Apply(decision, now);
                if (decision.consume)
                {
                    Event event;
                    event.kind = EventKind::Wheel;
                    event.amount = static_cast<SHORT>(HIWORD(data.mouseData));
                    event.timestamp = now;
                    event.anchor = point;
                    Queue(event);
                }
                return decision.consume;
            }
            Button button;
            bool down;
            if (!DecodeButton(message, data, button, down))
                return false;
            if (down)
                CloseActivation(now, true);
            CancelTouchpad();
            decision = state_.ButtonEvent(button, down, point, now);
            Apply(decision, now);
            const bool wasForwarded = routing_.HasForwardedDrag();
            const bool consume = routing_.Button(static_cast<unsigned>(button), down, decision.consume);
            if (wasForwarded != routing_.HasForwardedDrag())
            {
                pointer_.Pause();
                dragPointer_.EndConstraint();
            }
            return consume;
        }

        static bool DecodeButton(WPARAM message, const MSLLHOOKSTRUCT& data, Button& button, bool& down) noexcept
        {
            switch (message)
            {
            case WM_LBUTTONDOWN:
                button = Button::Left;
                down = true;
                break;
            case WM_LBUTTONUP:
                button = Button::Left;
                down = false;
                break;
            case WM_RBUTTONDOWN:
                button = Button::Right;
                down = true;
                break;
            case WM_RBUTTONUP:
                button = Button::Right;
                down = false;
                break;
            case WM_MBUTTONDOWN:
                button = Button::Middle;
                down = true;
                break;
            case WM_MBUTTONUP:
                button = Button::Middle;
                down = false;
                break;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
                button = HIWORD(data.mouseData) == XBUTTON1 ? Button::X1 : Button::X2;
                down = message == WM_XBUTTONDOWN;
                break;
            default:
                return false;
            }
            return true;
        }

        bool WheelChord() const
        {
            const bool control = state_.Down(vk::LeftControl) || state_.Down(vk::RightControl);
            const bool alt = state_.Down(vk::LeftAlt) || state_.Down(vk::RightAlt);
            const bool win = state_.Down(vk::LeftWin) || state_.Down(vk::RightWin);
            if (state_.Down(vk::LeftShift) || state_.Down(vk::RightShift))
                return false;
            switch (static_cast<WheelModifier>((controls_.Options() >> 3) & 3))
            {
            case WheelModifier::ControlAlt:
                return state_.Down(vk::LeftControl) && state_.Down(vk::LeftAlt) &&
                       !state_.Down(vk::RightControl) && !state_.Down(vk::RightAlt) && !win;
            case WheelModifier::Control:
                return control && !alt && !win;
            case WheelModifier::LeftAlt:
                return state_.Down(vk::LeftAlt) &&
                       !state_.Down(vk::RightAlt) && !control && !win;
            case WheelModifier::Windows:
                return win && !control && !alt;
            }
            return false;
        }

        void CloseActivation(std::uint64_t now, bool cancel) noexcept
        {
            if (!activation_.enabled || (activationClosed_ && !activationClosePending_ && (!cancel || activationCancelled_)))
                return;
            if (!activationClosed_ || (cancel && !activationCancelled_))
                activationCloseAt_ = now;
            activationClosed_ = true;
            activationCancelled_ = activationCancelled_ || cancel;
            Event event;
            event.kind = EventKind::WheelCancel;
            event.cancel = activationCancelled_;
            event.timestamp = activationCloseAt_; // Retries retain the physical cancellation time.
            activationClosePending_ = !QueueOptional(event, activation_.epoch);
        }

        void ObserveActivation(std::uint64_t now, Vec2 point, bool havePoint) noexcept
        {
            wheel::Activation candidate;
            if (!activationPublication_.Read(candidate))
                return;
            if (!candidate.enabled)
            {
                if (activationClosePending_)
                    CloseActivation(now, activationCancelled_);
                if (!activationClosePending_)
                    activation_ = {}; // UI withdrawal alone is not a physical cancellation.
                return;
            }
            if (candidate.startedAt != activationIdentity_ || candidate.monitorId != activationMonitor_)
            {
                activationIdentity_ = candidate.startedAt;
                activationMonitor_ = candidate.monitorId;
                activationClosed_ = activationCancelled_ = activationClosePending_ = false;
            }
            activation_ = candidate;
            if (!havePoint || !InputGate(now) || !pointer::Contains(candidate.bounds, point))
                CloseActivation(now, true);
            else if (!WheelChord())
                CloseActivation(now, false);
            else if (activationClosePending_)
                CloseActivation(now, activationCancelled_);
        }

        bool QueueActivationWheel(Vec2 point, std::uint64_t now, double amount) noexcept
        {
            const auto feedback = routing_.Report(false, now);
            const bool idle = controlsCoherent_ && state_.Active() == Gesture::None && !touchpadState_.Active() &&
                              !feedback.forwardedButtons && !feedback.blockedButtons && InputGate(now);
            const bool explicitAdmission = !activationClosed_ && wheel::Admit(activation_, controls_.Epoch(), feedback.observedGeneration, feedback.targetId, point, now, now, WheelChord(), idle);
            // Before the explicit admission channel catches up, a known 1x
            // route can accept camera intent under the coherent busy barrier.
            // No click or mapping authority is granted. A closed observation
            // for this SAME target must never reopen admission.
            const bool closedTarget = activation_.enabled && activation_.monitorId == feedback.targetId &&
                                      (activationClosed_ || activation_.observeOnly);
            const bool firstUnzoomed = idle && !closedTarget && ((controls_.Options() & 1) || routing_.Busy()) &&
                                       routing_.CanRequestWheel(point, true);
            if (!explicitAdmission && !firstUnzoomed)
                return false;
            Event event;
            event.kind = EventKind::Wheel;
            event.amount = amount;
            event.anchor = point;
            event.timestamp = now;
            QueueOptional(event, controls_.Epoch());
            return true;
        }

        void Raw(HRAWINPUT handle, DWORD messageTime)
        {
            FeedbackScope feedback(*this);
            const auto now = GetTickCount64();
            Synchronize(now);
            RAWINPUTHEADER header{};
            UINT headerBytes = sizeof(header);
            if (GetRawInputData(handle, RID_HEADER, &header, &headerBytes, sizeof(header)) != sizeof(header) ||
                headerBytes != sizeof(header))
            {
                CancelTouchpad();
                return;
            }
            if (header.dwType == RIM_TYPEHID)
            {
                if (!(controls_.Options() & 32))
                    return;
                const LONG age = static_cast<LONG>(static_cast<DWORD>(now) - messageTime);
                if (age < 0 || age > 250)
                {
                    CancelTouchpad();
                    return;
                }
                const auto decoded = touchpadHid_.Read(handle, [this, now](const touchpad::Report& report) {
                    const auto result = touchpadAssembler_.Push(report, now);
                    if (result.invalid)
                    {
                        CancelTouchpad();
                        return;
                    }
                    if (result.complete)
                    {
                        touchpadStatus_.store(TouchpadAvailability::Detected, std::memory_order_release);
                        ApplyTouchpad(touchpadState_.Push(result.frame, now, touchpadStartsAllowed_));
                    }
                });
                if (decoded == touchpad::DecodeResult::Invalid || decoded == touchpad::DecodeResult::Unsupported)
                {
                    CancelTouchpad();
                    if (decoded == touchpad::DecodeResult::Unsupported &&
                        touchpadStatus_.load(std::memory_order_relaxed) != TouchpadAvailability::Detected)
                        touchpadStatus_.store(TouchpadAvailability::Unsupported, std::memory_order_release);
                }
                return;
            }
            if (touchpadState_.Active())
                return; // Never combine HID and raw-mouse movement.
            if (state_.Active() == Gesture::None || motion_ == Motion::Hook ||
                static_cast<LONG>(messageTime - static_cast<DWORD>(gestureAt_)) < 0)
                return;
            RAWINPUT input{};
            UINT bytes = sizeof(input);
            const UINT received = GetRawInputData(handle, RID_INPUT, &input, &bytes, sizeof(RAWINPUTHEADER));
            if (received == static_cast<UINT>(-1) || received > sizeof(input) || bytes != received ||
                received < offsetof(RAWINPUT, data) + sizeof(RAWMOUSE) ||
                input.header.dwSize != received || input.header.dwType != RIM_TYPEMOUSE ||
                (input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                return;
            if (!input.data.mouse.lLastX && !input.data.mouse.lLastY)
                return;
            if (motion_ == Motion::Pending)
            {
                motion_ = Motion::Raw;
                pendingHook_ = pendingTravel_ = 0;
            }
            if (motion_ == Motion::Raw)
            {
                const double delta = std::clamp(static_cast<double>(input.data.mouse.lLastY), -240.0, 240.0);
                const double dx = std::clamp(static_cast<double>(input.data.mouse.lLastX), -240.0, 240.0);
                Accumulate(delta, std::hypot(dx, delta));
            }
        }

        void Apply(Decision decision, std::uint64_t now)
        {
            if (decision.end)
            {
                if (!decision.cancel)
                {
                    if (motion_ == Motion::Pending)
                        Accumulate(pendingHook_, pendingTravel_);
                    FlushDelta();
                }
                ClearMotion();
                Event event;
                event.kind = EventKind::End;
                event.cancel = decision.cancel;
                Queue(event);
            }
            if (decision.begin)
            {
                // A held gesture freezes the anchor even when no WM_MOUSEMOVE
                // occurs. Discard pre-hold fractional render feedback immediately.
                pointer_.Pause();
                ClearMotion();
                POINT cursor{};
                if (!GetPhysicalCursorPos(&cursor))
                {
                    Apply(state_.Cancel(), now);
                    return;
                }
                gestureAt_ = now;
                motion_ = rawRegistered_ ? Motion::Pending : Motion::Hook;
                Event event;
                event.kind = EventKind::Begin;
                event.gesture = state_.Current();
                event.anchor = { static_cast<double>(cursor.x), static_cast<double>(cursor.y) };
                Queue(event);
            }
            if (decision.reset)
            {
                Event event;
                event.kind = EventKind::Reset;
                Queue(event);
            }
            if (decision.step)
            {
                FlushDelta();
                Event event;
                event.kind = EventKind::Step;
                event.amount = 120.0 * decision.step;
                Queue(event);
            }
        }

        void ApplyTouchpad(touchpad::Effect effect)
        {
            ApplyTouchpad(effect, controls_.Epoch());
        }
        void ApplyTouchpad(touchpad::Effect effect, std::uint64_t epoch)
        {
            if (effect.end)
            {
                Event event;
                event.kind = EventKind::TouchpadEnd;
                event.cancel = effect.cancel;
                QueueAt(event, epoch);
            }
            if (effect.begin)
            {
                pointer_.Pause();
                POINT cursor{};
                if (!GetPhysicalCursorPos(&cursor))
                {
                    touchpadState_.Cancel();
                    touchpadAssembler_.Reset();
                    return;
                }
                Event event;
                event.kind = EventKind::TouchpadBegin;
                event.anchor = { static_cast<double>(cursor.x), static_cast<double>(cursor.y) };
                QueueAt(event, epoch);
            }
            if (effect.travel != 0)
            {
                Event event;
                event.kind = EventKind::TouchpadDelta;
                event.amount = effect.delta;
                event.travel = effect.travel;
                QueueAt(event, epoch);
            }
        }
        void CancelTouchpad()
        {
            ApplyTouchpad(touchpadState_.Cancel());
            touchpadAssembler_.Reset();
        }

        void Accumulate(double delta, double travel)
        {
            delta_ = std::clamp(delta_ + delta, -100000.0, 100000.0);
            travel_ = std::min(100000.0, travel_ + travel);
        }
        void FlushDelta()
        {
            if (travel_ == 0)
                return;
            Event event;
            event.kind = EventKind::Delta;
            event.amount = delta_;
            event.travel = travel_;
            delta_ = travel_ = 0;
            Queue(event);
        }
        void ClearMotion()
        {
            motion_ = Motion::None;
            delta_ = travel_ = pendingHook_ = pendingTravel_ = 0;
            firstHookMotion_ = gestureAt_ = 0;
        }
        void LatchFault() noexcept
        {
            pointer_.Pause();
            fault_.store(true, std::memory_order_release);
            PublishPointerFeedback();
            state_.SetEnabled(false); // Retain every reserved release; no recursive queue writes.
            touchpadState_.SetEnabled(false);
            touchpadAssembler_.Reset();
            ClearMotion();
        }
        bool Queue(Event event)
        {
            return QueueAt(event, controls_.Epoch());
        }
        bool QueueOptional(Event event, std::uint64_t epoch) noexcept
        {
            // Camera-only startup packets own no physical key/button release.
            // Bounded pressure may drop intent; it must not kill the hook.
            event.epoch = epoch;
            return !fault_.load(std::memory_order_relaxed) && queue_.Push(event);
        }
        bool QueueAt(Event event, std::uint64_t epoch)
        {
            if (fault_.load(std::memory_order_relaxed))
                return false;
            event.epoch = epoch;
            if (!queue_.Push(event))
            {
                LatchFault();
                return false;
            }
            return true;
        }
        bool ReservedReleasePending() const
        {
            return state_.SwallowedKey(vk::Space) || state_.SwallowedKey(vk::Escape) ||
                   state_.SwallowedKey(vk::Equal) || state_.SwallowedKey(vk::Minus) ||
                   state_.SwallowedKey(vk::Add) || state_.SwallowedKey(vk::Subtract) ||
                   state_.SwallowedButton(Button::Left) || routing_.HasBlockedButtons();
        }
        void CloseHandles()
        {
            if (thread_)
                CloseHandle(thread_);
            if (startedEvent_)
                CloseHandle(startedEvent_);
            if (stopEvent_)
                CloseHandle(stopEvent_);
            thread_ = startedEvent_ = stopEvent_ = nullptr;
        }

        HINSTANCE instance_ = nullptr;
        HANDLE thread_ = nullptr, startedEvent_ = nullptr, stopEvent_ = nullptr;
        HWND window_ = nullptr;
        HHOOK keyboard_ = nullptr, mouse_ = nullptr;
        std::atomic<bool> ready_{ false }, fault_{ false }, stopping_{ false };
        std::atomic<bool> pointerFault_{ false };
        std::atomic<std::uint64_t> uiBeat_{ 0 }, hostBeat_{ 0 };
        std::atomic<TouchpadAvailability> touchpadStatus_{ TouchpadAvailability::Disabled };
        ControlChannel publication_{};
        PointerChannel pointerPublication_{};
        pointer::FeedbackChannel pointerFeedback_{};
        routing::Channel routingPublication_{};
        routing::FeedbackChannel routingFeedback_{};
        wheel::ActivationChannel activationPublication_{};
        std::atomic<HWND> routingWake_{ nullptr };
        std::atomic<UINT> routingMessage_{ 0 };
        SpscQueue<Event, 256> queue_{};

        // Everything below is owned exclusively by the hook thread.
        State state_{};
        routing::State routing_{};
        routing::Feedback lastRoutingFeedback_{};
        wheel::Activation activation_{};
        std::uint64_t activationIdentity_ = 0, activationMonitor_ = 0;
        bool activationClosed_ = false, activationCancelled_ = false, activationClosePending_ = false;
        std::uint64_t activationCloseAt_ = 0;
        ForegroundContext foreground_{};
        pointer::Adapter pointer_{};
        pointer::Adapter dragPointer_{};
        pointer::NativeOps pointerNative_{};
        bool pointerControlsCoherent_ = false, controlsCoherent_ = false;
        double zoom_ = 1;
        ControlReader controls_{};
        bool desktop_ = false, seeded_ = false, confirmedAway_ = false;
        bool rawRegistered_ = false, classRegistered_ = false;
        bool touchpadRegistered_ = false, touchpadStartsAllowed_ = false;
        touchpad::HidReader touchpadHid_{};
        touchpad::Assembler touchpadAssembler_{};
        touchpad::State touchpadState_{};
        Motion motion_ = Motion::None;
        double delta_ = 0, travel_ = 0, pendingHook_ = 0, pendingTravel_ = 0;
        std::uint64_t firstHookMotion_ = 0, gestureAt_ = 0;
    };

} // namespace glide::input
