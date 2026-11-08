#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(_M_X64) && !defined(__x86_64__) && !defined(_M_ARM64) && !defined(__aarch64__)
#error GlideZoom requires a native 64-bit build. WOW64 is unsupported by the Magnification API.
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <wtsapi32.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cwchar>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "desktop_context.h"
#include "cursor_visibility.h"
#include "cursor_visibility_win32.h"
#include "frame_clock_win32.h"
#include "frame_stall_policy.h"
#include "frame_transaction.h"
#include "input_host.h"
#include "indicator_visibility.h"
#include "magnification_api.h"
#include "monitor_geometry.h"
#include "gpu_monitor_magnifier.h"
#include "monitor_presenter.h"
#include "settings_store.h"
#include "powertoys_runtime.h"
#include "runtime_state.h"
#include "watchdog.h"
#include "zoom_core.h"
#include "zoom_animation.h"
#include "viewport_motion.h"
#include "wheel_acceleration.h"
#include "wheel_activation.h"
#include "zoom_session.h"
#include "view_mode.h"
#include "tray_state.h"
#include "ui_text_cache.h"
#include "zoom_capture_win32.h"
#include <common/logger/logger.h>
#include <common/utils/logger_helper.h>

namespace
{
    constexpr wchar_t kClass[] = L"GlideZoom.AccessibilityZoom.1";
    constexpr UINT kTray = WM_APP + 1, kError = WM_APP + 2;
    constexpr UINT kReset = WM_APP + 3, kQuit = WM_APP + 4;
    constexpr UINT kRoutingWake = WM_APP + 5;
    constexpr UINT_PTR kTimer = 1;
    static_assert(glide::tray::event::ContextMenu == WM_CONTEXTMENU &&
                  glide::tray::event::LeftUp == WM_LBUTTONUP &&
                  glide::tray::event::LeftDoubleClick == WM_LBUTTONDBLCLK &&
                  glide::tray::event::RightUp == WM_RBUTTONUP &&
                  glide::tray::event::Select == NIN_SELECT &&
                  glide::tray::event::KeySelect == NIN_KEYSELECT);
    constexpr int kToggleHotkey = 1, kResetHotkey = 2, kPlusHotkey = 3, kMinusHotkey = 4;
    constexpr int kDesktopHotkey = 5, kCaptureHotkey = 6;
    constexpr int kShiftPlusHotkey = 7, kNumpadPlusHotkey = 8, kNumpadMinusHotkey = 9;
    constexpr int kIndependentHotkey = 10, kScopePreviewHotkey = 11;
    constexpr int kShow = 101, kToggle = 102, kResetCommand = 103, kPause = 104, kExit = 105;
    constexpr int kDesktopToggle = 106, kCaptureCommand = 107;
    constexpr int kIndependentToggle = 108;
    constexpr int kModifier = 201, kMaximum = 202, kSensitivity = 203;
    constexpr int kKeyboardGesture = 204, kMouseGesture = 205, kReverse = 206, kFollowing = 207;
    constexpr int kSave = 208, kHide = 209;
    constexpr int kTouchpadGesture = 210, kPointerScaling = 211;
    constexpr int kZoomMultiplier = 212, kEdgePanMultiplier = 213;
    constexpr int kOpenSettings = 214;
    constexpr int kZoomScope = 215;
    constexpr int kPointerSpeed = 216;

    bool engineLoggerReady = false;

    void Diagnostic(const wchar_t* message, bool error = false) noexcept
    {
        if (!engineLoggerReady)
            return;
        try
        {
            if (error)
                Logger::error(L"{}", message);
            else
                Logger::info(L"{}", message);
            Logger::flush();
        }
        catch (...)
        {
            OutputDebugStringW(L"GlideZoom: could not write engine diagnostic.\n");
        }
    }

    void ReportError(HWND owner, const wchar_t* text, const wchar_t* title, UINT flags, bool powerToys) noexcept
    {
        Diagnostic(text, true);
        if (powerToys)
        {
            // Startup has no frame timer yet. A modal dialog here would prevent
            // Runner's disable event or process exit from stopping the engine.
            OutputDebugStringW(L"PowerToys GlideZoom: ");
            OutputDebugStringW(text);
            OutputDebugStringW(L"\n");
        }
        else
        {
            MessageBoxW(owner, text, title, flags);
        }
    }

    std::wstring ErrorText(DWORD code)
    {
        wchar_t* message = nullptr;
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                           FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr,
                       code,
                       0,
                       reinterpret_cast<wchar_t*>(&message),
                       0,
                       nullptr);
        std::wstring result;
        try
        {
            result = message ? message : L"Unknown Windows error";
        }
        catch (...)
        {
            if (message)
                LocalFree(message);
            throw;
        }
        if (message)
            LocalFree(message);
        return result + L" (" + std::to_wstring(code) + L")";
    }

    bool HasUiAccess() noexcept
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;
        DWORD uiAccess = 0, bytes = 0;
        const BOOL ok = GetTokenInformation(token, TokenUIAccess, &uiAccess, sizeof(uiAccess), &bytes);
        CloseHandle(token);
        return ok && uiAccess;
    }

    class App;
    App* app = nullptr;

    class App
    {
    public:
        explicit App(DWORD runnerPid = 0) :
            runnerPid_(runnerPid), settingsStore_(runnerPid != 0) {}
        ~App() noexcept { Cleanup(); }

        int Run(HINSTANCE instance, bool quiet)
        {
            instance_ = instance;
            if (runnerPid_ && !runner_.Connect(runnerPid_))
                return Fail(L"The PowerToys runner or its control events are unavailable.");
            const auto loaded = settingsStore_.Load(settings_);
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = WindowProc;
            wc.hInstance = instance;
            wc.lpszClassName = kClass;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
            if (!wc.hIcon)
                wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            wc.hIconSm = wc.hIcon;
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            if (!RegisterClassExW(&wc))
                return Fail(L"Could not register the application window.");
            hwnd_ = CreateWindowExW(0, kClass, runnerPid_ ? L"PowerToys GlideZoom" : L"GlideZoom 0.6.2 - Accessibility Zoom", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 720, 700, nullptr, nullptr, instance, nullptr);
            if (!hwnd_)
                return Fail(L"Could not create the application window.");
            CreateControls();
            taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
            if (!mag_.Load() || !mag_.Initialize())
                return Fail(L"Windows could not initialize screen magnification.");
            magInitialized_ = true;
            if (!monitor_->Initialize(instance_, mag_))
                return Fail(L"GPU monitor zoom could not initialize. Windows 10 version 2004 or newer and Windows Graphics Capture are required.");
            glide::session::Snapshot baseline{};
            if (!mag_.Read(baseline))
                return Fail(L"Windows could not read the desktop magnification state.");
            if (!glide::session::Valid(baseline) || !glide::session::IsIdentity(baseline))
            {
                ReportError(hwnd_, L"Another magnifier is active. Close it before starting GlideZoom. "
                                   L"GlideZoom has left its transforms unchanged.",
                            L"GlideZoom",
                            MB_OK | MB_ICONINFORMATION,
                            runnerPid_ != 0);
                return 1;
            }
            // Merely starting the app never writes a global magnification transform.
            if (!RefreshDesktop())
                return Fail(L"Windows did not provide usable display geometry.");
            if (!guardian_.Start())
                return Fail(L"The recovery process could not start.");
            guardianStarted_ = true;
            if (!input_.Start(instance_))
                return Fail(L"Could not start the dedicated input thread.");
            input_.SetRoutingWake(hwnd_, kRoutingWake);
            parkedViews_.reserve(displays_.size());
            monitor_->SetCursorPositionResolver(ResolveCursorPosition, this);
            notificationsRegistered_ = WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION) != FALSE;
            if (!notificationsRegistered_)
                return Fail(L"Could not subscribe to session changes.");
            if (!AddTray())
                return Fail(L"Could not create tray controls.");
            if (!frameClock_.Start())
                return Fail(L"Could not start the animation clock.");
            // A native menu/dialog has its own message loop. This fallback services
            // the same deadline policy there and keeps recovery heartbeats alive.
            if (!SetTimer(hwnd_, kTimer, 10, nullptr))
                return Fail(L"Could not start the modal-loop timer.");
            lastTick_ = GetTickCount64();
            EnableControls();
            if (loaded == glide::settings::LoadResult::Invalid)
                QueueError(runnerPid_ ?
                               L"GlideZoom settings were invalid. Safe defaults are in use. "
                               L"Update the options in PowerToys Settings to save a valid configuration." :
                               L"The settings file was incomplete or invalid. Safe defaults are in use. "
                               L"The original file has been left unchanged; Save settings replaces it with a complete configuration.");
            else if (loaded == glide::settings::LoadResult::Unavailable)
                QueueError(runnerPid_ ?
                               L"GlideZoom settings could not be read. Safe defaults are in use. "
                               L"Review the GlideZoom options in PowerToys Settings and check access to your PowerToys settings folder." :
                               L"Settings could not be read safely. Safe defaults are in use. "
                               L"Saving is disabled for this run; check the profile folder and restart.");
            if (!quiet)
                Show();
            bool loopOkay = true, quit = false;
            while (!quit && !quitReceived_ && !cleaned_)
            {
                if (!ServiceFrame())
                {
                    loopOkay = false;
                    break;
                }
                if (cleaned_ || quitReceived_)
                    break;
                if (!frameClock_.Arm())
                {
                    loopOkay = false;
                    break;
                }
                const HANDLE timer = frameClock_.Handle();
                const DWORD wake = MsgWaitForMultipleObjectsEx(1, &timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (wake != WAIT_OBJECT_0 && wake != WAIT_OBJECT_0 + 1)
                {
                    loopOkay = false;
                    break;
                }
                // A stream of posted input must not starve the next frame. Native
                // messages are still dispatched on this UI thread, in queue order.
                MSG message{};
                for (unsigned count = 0; count < 64 && !cleaned_ && !quitReceived_; ++count)
                {
                    if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
                        break;
                    if (message.message == WM_QUIT)
                    {
                        quitCode_ = static_cast<int>(message.wParam);
                        quit = true;
                        break;
                    }
                    if (!IsDialogMessageW(hwnd_, &message))
                    {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                    if ((count & 7u) == 7u && !ServiceFrame())
                    {
                        loopOkay = false;
                        quit = true;
                        break;
                    }
                }
            }
            Cleanup();
            if (!loopOkay)
                return Fail(L"The Windows animation/message loop stopped unexpectedly.");
            return failed_ ? 1 : quitCode_;
        }

    private:
        using Phase = glide::runtime::Phase;
        HINSTANCE instance_{};
        DWORD runnerPid_ = 0;
        glide::powertoys::Runner runner_;
        bool runnerStopping_ = false, settingsReloadPending_ = false, settingsReloadResume_ = false;
        bool scopeReloadPending_ = true;
        HWND hwnd_{}, status_{}, modifier_{}, maximum_{}, sensitivity_{}, zoomMultiplier_{}, edgePanMultiplier_{}, zoomScope_{}, pointerSpeed_{};
        HWND keyboardGesture_{}, mouseGesture_{}, touchpadGesture_{}, touchpadStatus_{}, reverse_{}, following_{}, pointerScaling_{};
        HFONT font_{}, titleFont_{};
        UINT taskbarCreated_{};
        bool magInitialized_ = false, cleaned_ = false, guardianStarted_ = false;
        bool notificationsRegistered_ = false, failed_ = false, captureBusy_ = false, captureRequested_ = false;
        bool queuedCopy_ = false;
        std::uint64_t copyRoutingGeneration_ = 0, copyRoutingEpoch_ = 0;
        bool captureUserCancelled_ = false, deferredQuit_ = false, deferredReset_ = false;
        bool frameInProgress_ = false, nativeOperation_ = false, messageDuringOperation_ = false;
        bool drainingDeferred_ = false;
        bool quitReceived_ = false;
        int quitCode_ = 0;
        glide::tray::State tray_;
        glide::ui::TextCache statusText_, touchpadText_, trayTip_;
        std::array<bool, 12> hotkeys_{};
        glide::runtime::State lifecycle_;
        glide::settings::Values settings_;
        glide::settings::Store settingsStore_;
        MagnificationApi mag_;
        glide::cursor::Lease cursor_;
        glide::timing::NativeClock frameClock_;
        std::unique_ptr<GpuMonitorMagnifier> monitor_ = std::make_unique<GpuMonitorMagnifier>();
        glide::view::State view_;
        glide::session::Session session_;
        glide::watchdog::Guardian guardian_;
        glide::input::Host input_;
        glide::Rect desktop_{};
        glide::Rect monitorBounds_{};
        HMONITOR selectedMonitor_ = nullptr;
        glide::session::Result lastRestore_ = glide::session::Result::Unchanged;
        glide::Transform transform_{}, precise_{};
        double target_ = 1, rawTarget_ = 1, savedZoom_ = 2;
        glide::viewport::Anchor zoomAnchor_;
        glide::viewport::Follow followingState_;
        glide::indicator::State indicator_;
        glide::wheel::Accelerator wheelAcceleration_;
        glide::wheel::ActivationIntent wheelIntent_;
        struct Display
        {
            HMONITOR id = nullptr;
            glide::Rect bounds{};
        };
        struct ParkedView
        {
            Display display;
            glide::Transform applied{}, precise{};
            double target = 1, rawTarget = 1, savedZoom = 2;
            glide::viewport::Anchor anchor;
            glide::viewport::Follow following;
            glide::indicator::State indicator;
            std::unique_ptr<GpuMonitorMagnifier> renderer;
        };
        std::array<Display, 16> displays_{};
        std::size_t displayCount_ = 0;
        std::vector<ParkedView> parkedViews_;
        bool routingBusy_ = false, viewLoadedForHandoff_ = false, routingPublishFailed_ = false, routingWasEnabled_ = false;
        HMONITOR handoffTarget_ = nullptr;
        std::uint64_t routingGeneration_ = 0, routingAck_ = 0, handoffSerial_ = 0;
        std::uint64_t handoffPreviousEpoch_ = 0;
        ULONGLONG lastTick_ = 0, lastUi_ = 0, lastCopy_ = 0;
        std::wstring pendingError_;
        struct DragState
        {
            bool active = false;
            bool touchpad = false;
            double startZoom = 1, pixels = 0, travel = 0;
        } drag_;

        struct NativeOperation
        {
            bool& flag;
            bool previous;
            explicit NativeOperation(bool& value) noexcept :
                flag(value), previous(value) { flag = true; }
            ~NativeOperation() noexcept { flag = previous; }
        };

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) noexcept
        {
            if (!app)
                return DefWindowProcW(hwnd, message, wParam, lParam);
            const bool previous = app->messageDuringOperation_;
            app->messageDuringOperation_ = previous || app->captureRequested_ ||
                                           app->frameInProgress_ || app->nativeOperation_;
            try
            {
                const LRESULT result = app->WindowMessage(hwnd, message, wParam, lParam);
                app->messageDuringOperation_ = previous;
                app->DrainDeferred();
                return result;
            }
            catch (...)
            {
                // C++ exceptions must not unwind through a Windows callback.
                app->messageDuringOperation_ = previous;
                app->UnexpectedFault();
                app->DrainDeferred();
                return 0;
            }
        }

        int Fail(const wchar_t* text)
        {
            const DWORD error = GetLastError();
            ReportError(hwnd_, (std::wstring(text) + L"\n\n" + ErrorText(error)).c_str(), L"GlideZoom", MB_OK | MB_ICONERROR, runnerPid_ != 0);
            return 1;
        }

        void QueueError(const wchar_t* text)
        {
            if (!pendingError_.empty())
                return;
            Diagnostic(text, true);
            pendingError_ = text;
            PostMessageW(hwnd_, kError, 0, 0);
        }

        bool PointerAdjustmentRequested() const noexcept { return settings_.pointerScaling || settings_.pointerSpeed != 100; }

        static bool ResolveCursorPosition(void* context, glide::Vec2 physical, glide::Transform applied, glide::Rect bounds, glide::Vec2& precise) noexcept
        {
            const auto& owner = *static_cast<const App*>(context);
            const bool enabled = owner.lifecycle_.Ready() && owner.MonitorMode() &&
                                 !owner.routingBusy_ &&
                                 owner.PointerAdjustmentRequested() && !owner.settings_.centerFollow &&
                                 !owner.drag_.active && !owner.captureBusy_ && !owner.captureRequested_;
            glide::pointer::Feedback feedback;
            return enabled && owner.input_.ReadPointerFeedback(feedback) &&
                   glide::pointer::ResolveCursor(feedback, physical, applied, bounds, owner.lifecycle_.Epoch(), true, precise, owner.settings_.pointerSpeed / 100.0, owner.settings_.pointerScaling);
        }

        void PublishPointer() noexcept
        {
            const double edgeGain = settings_.pointerScaling && !settings_.centerFollow ? settings_.edgePanMultiplier : 1;
            // Edge acceleration uses the last successfully presented geometry.
            // Gain 1 does not need the moving offset, retaining the stable
            // duplicate-publication path of the working pointer controller.
            input_.UpdatePointer({ PointerAdjustmentRequested(),
                                   lifecycle_.Ready() && !routingBusy_ && !captureBusy_ && !captureRequested_ && glide::animation::Magnified(transform_.zoom),
                                   ZoomBounds(),
                                   transform_.zoom,
                                   lifecycle_.Epoch(),
                                   edgeGain > 1 ? transform_.offset : glide::Vec2{},
                                   edgeGain,
                                   settings_.pointerSpeed / 100.0,
                                   settings_.pointerScaling });
            if (!PublishRouting())
                routingPublishFailed_ = true;
        }

        void ConfigureInput() noexcept
        {
            const glide::input::Config config{ settings_.keyboardGesture, settings_.mouseGesture, GetDoubleClickTime(), 30000 };
            input_.Update(config, lifecycle_.Ready() && !routingBusy_ && !captureBusy_ && !captureRequested_, transform_.zoom, lifecycle_.Epoch(), static_cast<glide::input::WheelModifier>(settings_.modifier), settings_.touchpadGesture);
            PublishPointer();
        }

        void CancelInput(bool preserveWheelIntent = false) noexcept
        {
            lifecycle_.Cancel();
            wheelAcceleration_.Reset();
            if (!preserveWheelIntent)
                wheelIntent_.Reset();
            if (!input_.UpdateActivationWheel({}))
                routingPublishFailed_ = true;
            drag_ = {};
            followingState_.Reset();
            ConfigureInput();
            // A cancellation advances the hook epoch. A frozen busy barrier
            // must be republished in that epoch before it can be acknowledged.
            if (routingBusy_ && !PublishRouting(true))
                routingPublishFailed_ = true;
        }

        void ReleaseHotkeys() noexcept
        {
            for (int id = kToggleHotkey; id <= kScopePreviewHotkey; ++id)
            {
                if (hotkeys_[id])
                    UnregisterHotKey(hwnd_, id);
                hotkeys_[id] = false;
            }
        }

        bool EnableControls()
        {
            if (!lifecycle_.PrepareEnable())
                return false;
            ReleaseHotkeys();
            CancelInput();
            if (!guardian_.Alive() || input_.Faulted())
            {
                QueueError(L"Input or recovery is unavailable. Restart GlideZoom before using zoom.");
                return false;
            }
            if (!input_.Healthy())
            {
                lifecycle_.Suspend(true);
                ConfigureInput();
                return true;
            }
            if (!glide::OnInputDesktop())
            {
                lifecycle_.Suspend(true);
                ConfigureInput();
                return true;
            }
            if (lifecycle_.NeedsRestore() && !RestoreOwned())
                return false;
            if (!RefreshDesktop())
            {
                QueueError(L"Display geometry is unavailable. GlideZoom remains paused.");
                return false;
            }
            glide::session::Snapshot current{};
            if (!mag_.Read(current) || !glide::session::Valid(current) || !glide::session::IsIdentity(current))
            {
                QueueError(L"Another magnifier is active, or its state could not be verified. "
                           L"GlideZoom remains paused. Close the other magnifier, then resume from the tray.");
                return false;
            }
            if (!ApplyPreferredScope())
            {
                lifecycle_.Suspend(true);
                ConfigureInput();
                return true;
            }
            if (!hotkeys_[kResetHotkey])
            {
                hotkeys_[kResetHotkey] = RegisterShortcut(kResetHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_ESCAPE, L"Ctrl+Alt+Esc");
                if (!hotkeys_[kResetHotkey])
                {
                    QueueError(L"Ctrl+Alt+Esc is already in use. GlideZoom remains paused because "
                               L"its emergency reset shortcut could not be reserved.");
                    return false;
                }
                hotkeys_[kToggleHotkey] = RegisterShortcut(kToggleHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Z', L"Ctrl+Alt+Z (selected monitor)");
                hotkeys_[kPlusHotkey] = RegisterShortcut(kPlusHotkey, MOD_CONTROL | MOD_ALT, VK_OEM_PLUS, L"Ctrl+Alt+=");
                hotkeys_[kMinusHotkey] = RegisterShortcut(kMinusHotkey, MOD_CONTROL | MOD_ALT, VK_OEM_MINUS, L"Ctrl+Alt+-");
                hotkeys_[kDesktopHotkey] = RegisterShortcut(kDesktopHotkey, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'Z', L"Ctrl+Alt+Shift+Z (all monitors)");
                hotkeys_[kCaptureHotkey] = RegisterShortcut(kCaptureHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'S', L"Ctrl+Alt+S");
                hotkeys_[kShiftPlusHotkey] = RegisterShortcut(kShiftPlusHotkey, MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_OEM_PLUS, L"Ctrl+Alt+Shift+= (plus)");
                hotkeys_[kNumpadPlusHotkey] = RegisterShortcut(kNumpadPlusHotkey, MOD_CONTROL | MOD_ALT, VK_ADD, L"Ctrl+Alt+Num+");
                hotkeys_[kNumpadMinusHotkey] = RegisterShortcut(kNumpadMinusHotkey, MOD_CONTROL | MOD_ALT, VK_SUBTRACT, L"Ctrl+Alt+Num-");
                hotkeys_[kIndependentHotkey] = RegisterShortcut(kIndependentHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'I', L"Ctrl+Alt+I (independent monitors)");
                hotkeys_[kScopePreviewHotkey] = RegisterShortcut(kScopePreviewHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'V', L"Ctrl+Alt+V (scope indicator)");
                if (!hotkeys_[kToggleHotkey] || !hotkeys_[kPlusHotkey] || !hotkeys_[kMinusHotkey] ||
                    !hotkeys_[kDesktopHotkey] || !hotkeys_[kCaptureHotkey] || !hotkeys_[kShiftPlusHotkey] ||
                    !hotkeys_[kNumpadPlusHotkey] || !hotkeys_[kNumpadMinusHotkey] || !hotkeys_[kIndependentHotkey] || !hotkeys_[kScopePreviewHotkey])
                    QueueError(L"An optional zoom or copy-view shortcut is already in use and was left with its current owner. "
                               L"The wheel, drag gesture, tray controls and emergency reset remain available.");
            }
            if (!lifecycle_.Activate())
                return false;
            CancelInput();
            Diagnostic(L"Input controls ready; keyboard hooks and emergency reset are available.");
            return true;
        }

        bool RegisterShortcut(int id, UINT modifiers, UINT key, const wchar_t* label) noexcept
        {
            const bool registered = RegisterHotKey(hwnd_, id, modifiers, key) != FALSE;
            const DWORD error = registered ? ERROR_SUCCESS : GetLastError();
            if (engineLoggerReady)
            {
                try
                {
                    if (registered)
                        Logger::info(L"Shortcut registered: {}", label);
                    else
                        Logger::warn(L"Shortcut unavailable: {} (virtual key {}, modifiers {}, Win32 error {})", label, key, modifiers, error);
                    Logger::flush();
                }
                catch (...)
                {
                }
            }
            return registered;
        }

        bool ApplyPreferredScope() noexcept
        {
            if (!scopeReloadPending_)
                return true;
            const auto requested = settings_.zoomScope == 1 ? glide::view::Scope::Desktop :
                                   settings_.zoomScope == 2 ? glide::view::Scope::Independent :
                                                              glide::view::Scope::Monitor;
            if (!view_.CommitSwitch(requested, lastRestore_, glide::OnInputDesktop(), lifecycle_.NeedsRestore() || session_.NeedsRecovery(), lifecycle_.Terminal()))
                return false;
            selectedMonitor_ = nullptr;
            monitorBounds_ = {};
            scopeReloadPending_ = false;
            Diagnostic(settings_.zoomScope == 1 ? L"Configured zoom target: all monitors together." :
                       settings_.zoomScope == 2 ? L"Configured zoom target: independent monitors." :
                                                  L"Configured zoom target: selected monitor.");
            return true;
        }

        bool RefreshDesktop() noexcept
        {
            const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (width <= 0 || height <= 0 || width > 262144 || height > 262144 ||
                left < -262144 || top < -262144 || left > 262144 - width || top > 262144 - height)
                return false;
            desktop_ = { static_cast<double>(left), static_cast<double>(top), static_cast<double>(left) + width, static_cast<double>(top) + height };
            displayCount_ = 0;
            struct Enumeration
            {
                App& owner;
                bool valid = true;
            } enumeration{ *this };
            const auto enumerate = [](HMONITOR id, HDC, LPRECT, LPARAM data) noexcept -> BOOL {
                auto& scan = *reinterpret_cast<Enumeration*>(data);
                MONITORINFO info{ sizeof(info) };
                if (scan.owner.displayCount_ == scan.owner.displays_.size() || !GetMonitorInfoW(id, &info))
                {
                    scan.valid = false;
                    return FALSE;
                }
                const glide::Rect bounds{ static_cast<double>(info.rcMonitor.left), static_cast<double>(info.rcMonitor.top), static_cast<double>(info.rcMonitor.right), static_cast<double>(info.rcMonitor.bottom) };
                if (!glide::monitor::ValidBounds(bounds))
                {
                    scan.valid = false;
                    return FALSE;
                }
                scan.owner.displays_[scan.owner.displayCount_++] = { id, bounds };
                return TRUE;
            };
            if (!EnumDisplayMonitors(nullptr, nullptr, enumerate, reinterpret_cast<LPARAM>(&enumeration)) || !enumeration.valid || !displayCount_)
                return false;
            selectedMonitor_ = nullptr;
            monitorBounds_ = {};
            if (frameClock_.Handle())
                UpdateFrameRate(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY));
            return true;
        }

        void UpdateFrameRate(HMONITOR monitor) noexcept
        {
            if (!frameClock_.Handle())
                return;
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            double rate = 60;
            if (monitor && GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&info)) &&
                EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
                mode.dmDisplayFrequency > 1)
                rate = mode.dmDisplayFrequency;
            if (!frameClock_.SetRate(rate))
                UnexpectedFault();
        }

        bool MonitorMode() const noexcept { return view_.Current() != glide::view::Scope::Desktop; }
        bool IndependentMode() const noexcept { return view_.Current() == glide::view::Scope::Independent; }
        glide::Rect ZoomBounds() const noexcept { return MonitorMode() ? monitorBounds_ : desktop_; }

        void UpdateIndicator(GpuMonitorMagnifier& renderer, glide::indicator::State& state, glide::Transform view) noexcept
        {
            GpuMonitorMagnifier::ScopeIndicator indicator;
            indicator.alpha = state.Advance(GetTickCount64(), view, settings_.scopeIndicatorMode, settings_.scopeIndicatorDuration, settings_.scopeIndicatorFade, glide::animation::Magnified(view.zoom) && settings_.scopeIndicatorMode != 0);
            indicator.style = settings_.scopeIndicatorStyle;
            indicator.sizePercent = settings_.scopeIndicatorSize;
            indicator.opacity = settings_.scopeIndicatorOpacity;
            indicator.overviewOpacity = settings_.overviewOpacity;
            indicator.border = settings_.scopeIndicatorBorder;
            indicator.details = settings_.scopeIndicatorDetails;
            indicator.xPercent = settings_.scopeIndicatorX;
            indicator.yPercent = settings_.scopeIndicatorY;
            renderer.SetScopeIndicator(indicator);
        }

        void RevealScopeIndicator() noexcept
        {
            if (!lifecycle_.Ready() || !MonitorMode() || settings_.scopeIndicatorMode == 0)
                return;
            const auto now = GetTickCount64();
            indicator_.Reveal(now, settings_.scopeIndicatorDuration);
            for (auto& item : parkedViews_)
                item.indicator.Reveal(now, settings_.scopeIndicatorDuration);
        }

        bool PublishRouting(bool force = false) noexcept
        {
            const bool captureBarrier = queuedCopy_ || captureBusy_ || captureRequested_;
            if (routingBusy_ && !force && lifecycle_.Ready() && !captureBarrier && !deferredReset_)
                return true;
            const bool visible = monitor_->Active() ||
                                 std::any_of(parkedViews_.begin(), parkedViews_.end(), [](const auto& item) { return item.renderer->Active(); });
            // Gesture availability and click protection have different lifetimes.
            // Keep the hook's busy barrier until every private view is hidden
            // and the shared input/cursor recovery has actually completed.
            const bool enabled = IndependentMode() && (lifecycle_.Ready() || visible || session_.NeedsRecovery() || cursor_.NeedsRecovery());
            if (!enabled && !routingWasEnabled_ && !force)
                return true;
            if (routingGeneration_ == UINT64_MAX)
                return false;
            glide::input::routing::Snapshot publication{};
            publication.enabled = enabled;
            publication.busy = routingBusy_ || !lifecycle_.Ready() || captureBarrier || deferredReset_;
            publication.generation = ++routingGeneration_;
            publication.epoch = lifecycle_.Epoch();
            publication.activeId = reinterpret_cast<std::uintptr_t>(selectedMonitor_);
            publication.ackSerial = routingAck_;
            publication.count = displayCount_;
            for (std::size_t index = 0; index < displayCount_; ++index)
            {
                const auto& display = displays_[index];
                auto& route = publication.routes[index];
                route.id = reinterpret_cast<std::uintptr_t>(display.id);
                route.bounds = route.source = display.bounds;
                glide::monitor::Frame submitted{};
                if (display.id == selectedMonitor_)
                {
                    route.zoomed = glide::animation::Magnified(target_) || monitor_->Active();
                    if (monitor_->TryGetPresentedFrame(submitted))
                        route.source = glide::monitor::AsRect(submitted.source);
                }
                else
                {
                    const auto found = std::find_if(parkedViews_.begin(), parkedViews_.end(), [&](const auto& item) { return item.display.id == display.id; });
                    if (found != parkedViews_.end())
                    {
                        route.zoomed = glide::animation::Magnified(found->target);
                        if (found->renderer->TryGetPresentedFrame(submitted))
                            route.source = glide::monitor::AsRect(submitted.source);
                    }
                }
            }
            const bool published = input_.UpdateRouting(publication);
            if (published)
                routingWasEnabled_ = enabled;
            return published;
        }

        void CloseWheelIntent(bool discard) noexcept
        {
            if (discard)
                wheelIntent_.Reset();
            else
                wheelIntent_.CloseAdmission();
            wheelAcceleration_.Reset();
            if (!input_.UpdateActivationWheel(discard ? glide::wheel::Activation{} : wheelIntent_.Observation()))
                routingPublishFailed_ = true;
        }

        bool WheelIntentCurrent() noexcept
        {
            if (!wheelIntent_.Active())
                return false;
            POINT physical{};
            if (!wheelIntent_.Current(GetTickCount64()) || !IndependentMode() || !lifecycle_.Ready() ||
                !GetPhysicalCursorPos(&physical) ||
                !glide::pointer::Contains(wheelIntent_.Bounds(), { static_cast<double>(physical.x), static_cast<double>(physical.y) }))
            {
                CloseWheelIntent(true);
                return false;
            }
            return true;
        }

        void WheelIntentEvent(const glide::input::Event& event)
        {
            if (!WheelIntentCurrent())
                return;
            if (event.kind == glide::input::EventKind::WheelCancel)
            {
                if (wheelIntent_.MatchesEpoch(event.epoch) && event.timestamp >= wheelIntent_.StartedAt())
                    CloseWheelIntent(event.cancel);
                return;
            }
            if (!wheelIntent_.Push(event.amount, event.timestamp, event.epoch, event.anchor, GetTickCount64(), settings_.wheelAcceleration, settings_.wheelAccelerationLimit))
            {
                if (wheelIntent_.Accepting())
                    CloseWheelIntent(false); // Retain already accepted, bounded camera travel.
                return;
            }
            if (wheelIntent_.Committed())
            {
                const double amount = wheelIntent_.TakePending();
                SetTarget(glide::animation::StepZoom(rawTarget_, settings_.reverse ? -amount : amount, settings_.maxZoom, settings_.zoomMultiplier), false);
            }
        }

        void CollectActivationWheel()
        {
            if (!wheelIntent_.Active() || wheelIntent_.Committed())
                return;
            glide::input::Event queued{};
            for (unsigned count = 0; count < 256 && input_.Poll(queued); ++count)
            {
                if (queued.kind == glide::input::EventKind::Reset)
                {
                    Reset();
                    return;
                }
                if (queued.kind == glide::input::EventKind::Wheel || queued.kind == glide::input::EventKind::WheelCancel)
                    WheelIntentEvent(queued);
                if (!wheelIntent_.Active())
                    return;
            }
        }

        bool StartActivationWheel(const glide::input::Event& event)
        {
            if (!IndependentMode() || !lifecycle_.Ready() || queuedCopy_ || captureRequested_ || captureBusy_ ||
                deferredReset_ || deferredQuit_)
                return false;
            const auto now = GetTickCount64();
            if (!event.timestamp || now < event.timestamp || now - event.timestamp > 250 ||
                !std::isfinite(event.amount) || event.amount == 0 || !glide::pointer::ValidPoint(event.anchor))
                return false;
            const auto display = std::find_if(displays_.begin(), displays_.begin() + displayCount_, [&](const auto& item) {
                return glide::pointer::Contains(item.bounds, event.anchor);
            });
            if (display == displays_.begin() + displayCount_)
                return false;
            const bool same = display->id == selectedMonitor_;
            const bool cold = same && !glide::animation::Magnified(transform_.zoom) && !glide::animation::Magnified(target_);
            if (same && !cold && !routingBusy_)
                return false;
            if (cold && (settings_.reverse ? -event.amount : event.amount) < 0)
                return false; // An outward step at 1x needs no capture startup.
            if (routingBusy_)
            {
                if (display->id != handoffTarget_ ||
                    (event.epoch != handoffPreviousEpoch_ && event.epoch != lifecycle_.Epoch()))
                    return false;
            }
            else
            {
                if (event.epoch != lifecycle_.Epoch() || !BeginHandoff(display->id, routingAck_, same))
                    return false;
            }
            const glide::wheel::Activation admission{ true, lifecycle_.Epoch(), routingGeneration_, reinterpret_cast<std::uintptr_t>(display->id), event.timestamp, glide::wheel::ActivationDeadline(event.timestamp), display->bounds };
            if (!wheelIntent_.Begin(admission, event.epoch, event.amount, event.anchor, now, settings_.wheelAcceleration, settings_.wheelAccelerationLimit))
                return false;
            if (!input_.UpdateActivationWheel(wheelIntent_.Admission()))
                routingPublishFailed_ = true;
            return true;
        }

        void HandleWheel(const glide::input::Event& event)
        {
            if (wheelIntent_.Active())
            {
                if (WheelIntentCurrent() && (wheelIntent_.Accepting() || !wheelIntent_.Committed()))
                {
                    WheelIntentEvent(event);
                    return;
                }
                CloseWheelIntent(true);
            }
            if (StartActivationWheel(event))
                return;
            if (event.epoch != lifecycle_.Epoch() || routingBusy_ || !EnsurePointerView())
                return;
            const double amount = wheelAcceleration_.Apply(event.amount, event.timestamp, event.epoch, settings_.wheelAcceleration, settings_.wheelAccelerationLimit);
            SetTarget(glide::animation::StepZoom(rawTarget_, settings_.reverse ? -amount : amount, settings_.maxZoom, settings_.zoomMultiplier), false);
        }

        bool BeginHandoff(HMONITOR target, std::uint64_t serial, bool alreadyLoaded = false) noexcept
        {
            if (!target || !IndependentMode() || !lifecycle_.Ready())
                return false;
            handoffTarget_ = target;
            handoffSerial_ = serial;
            viewLoadedForHandoff_ = alreadyLoaded;
            routingBusy_ = true;
            handoffPreviousEpoch_ = lifecycle_.Epoch();
            CancelInput();
            return PublishRouting(true);
        }

        bool LoadIndependentView(HMONITOR target)
        {
            const auto display = std::find_if(displays_.begin(), displays_.begin() + displayCount_, [&](const auto& item) { return item.id == target; });
            if (display == displays_.begin() + displayCount_)
                return false;
            // Allocate before moving either renderer; recovery must always
            // retain a valid active owner even if memory allocation fails.
            if (parkedViews_.capacity() < displayCount_)
                parkedViews_.reserve(displayCount_);
            glide::cursor::NativeBackend cursorBackend{ mag_ };
            if (!cursor_.Release(cursorBackend, guardian_))
                return false;
            monitor_->SetSystemCursorHiddenByUs(false);
            monitor_->SetCursorEnabled(false);
            ParkedView incoming;
            const auto saved = std::find_if(parkedViews_.begin(), parkedViews_.end(), [&](const auto& item) { return item.display.id == target; });
            if (saved != parkedViews_.end())
            {
                incoming = std::move(*saved);
                parkedViews_.erase(saved);
            }
            else
            {
                incoming.display = *display;
                if (!selectedMonitor_)
                    incoming.renderer = std::move(monitor_);
                else
                {
                    incoming.renderer = std::make_unique<GpuMonitorMagnifier>();
                    if (!incoming.renderer->Initialize(instance_, mag_))
                        return false;
                    incoming.renderer->SetCursorPositionResolver(ResolveCursorPosition, this);
                }
            }
            if (selectedMonitor_)
            {
                ParkedView outgoing;
                outgoing.display = { selectedMonitor_, monitorBounds_ };
                outgoing.applied = transform_;
                outgoing.precise = precise_;
                outgoing.target = target_;
                outgoing.rawTarget = rawTarget_;
                outgoing.savedZoom = savedZoom_;
                outgoing.anchor = zoomAnchor_;
                outgoing.following = followingState_;
                outgoing.indicator = indicator_;
                outgoing.renderer = std::move(monitor_);
                parkedViews_.push_back(std::move(outgoing));
            }
            monitor_ = std::move(incoming.renderer);
            selectedMonitor_ = incoming.display.id;
            monitorBounds_ = incoming.display.bounds;
            transform_ = incoming.applied;
            precise_ = incoming.precise;
            target_ = incoming.target;
            rawTarget_ = incoming.rawTarget;
            savedZoom_ = incoming.savedZoom;
            zoomAnchor_ = incoming.anchor;
            followingState_ = incoming.following;
            indicator_ = incoming.indicator;
            drag_ = {};
            monitor_->SetCursorEnabled(true);
            monitor_->SetSystemCursorHiddenByUs(false);
            UpdateFrameRate(selectedMonitor_);
            return true;
        }

        bool CompleteHandoff()
        {
            if (!routingBusy_)
                return true;
            NativeOperation operation{ nativeOperation_ };
            CollectActivationWheel();
            if (!routingBusy_ || !lifecycle_.Ready() || deferredReset_ || deferredQuit_)
                return false;
            glide::input::routing::Feedback feedback{};
            if (!input_.ReadRoutingFeedback(feedback))
                return true;
            if (feedback.faulted)
                return false;
            if (!glide::input::routing::CommitReady(feedback, routingGeneration_, lifecycle_.Epoch(), GetTickCount64()))
                return true;
            if (feedback.requestSerial > handoffSerial_ && feedback.targetId)
            {
                handoffTarget_ = reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(feedback.targetId));
                handoffSerial_ = feedback.requestSerial;
                viewLoadedForHandoff_ = handoffTarget_ == selectedMonitor_;
            }
            if (!viewLoadedForHandoff_)
            {
                if (!LoadIndependentView(handoffTarget_))
                    return false;
                viewLoadedForHandoff_ = true;
            }
            if (!lifecycle_.Ready() || deferredReset_ || deferredQuit_)
                return false;
            if (WheelIntentCurrent())
            {
                if (wheelIntent_.MonitorId() != reinterpret_cast<std::uintptr_t>(selectedMonitor_))
                    CloseWheelIntent(true);
                else
                {
                    double initial = 0;
                    if (wheelIntent_.TakeInitial(initial))
                    {
                        if (!zoomAnchor_.Set(precise_, wheelIntent_.Anchor(), monitorBounds_))
                            return false;
                        rawTarget_ = glide::animation::StepZoom(rawTarget_, settings_.reverse ? -initial : initial, settings_.maxZoom, settings_.zoomMultiplier);
                        target_ = glide::animation::Quantize(rawTarget_, settings_.maxZoom);
                        if (rawTarget_ == 1 && (settings_.reverse ? -initial : initial) < 0)
                            CloseWheelIntent(false);
                    }
                    if (!glide::animation::Magnified(target_))
                    {
                        // High-resolution packets may need several reports to
                        // earn the first visible 0.01x step. Keep their detent
                        // credit until then; do not repeatedly reopen startup.
                        const double extra = wheelIntent_.TakePending();
                        rawTarget_ = glide::animation::StepZoom(rawTarget_, settings_.reverse ? -extra : extra, settings_.maxZoom, settings_.zoomMultiplier);
                        target_ = glide::animation::Quantize(rawTarget_, settings_.maxZoom);
                        if (rawTarget_ == 1 && (settings_.reverse ? -extra : extra) < 0)
                            CloseWheelIntent(false);
                        if (!glide::animation::Magnified(target_) && wheelIntent_.Accepting())
                            return true;
                    }
                }
            }
            glide::frame::Lease lease{ lifecycle_ };
            glide::frame::Backend<MagnificationApi> backend{ mag_, lease };
            if (!guardian_.Alive() || !input_.Healthy())
                return false;
            glide::input::routing::Feedback afterLoad{};
            if (!input_.ReadRoutingFeedback(afterLoad))
                return true;
            if (afterLoad.faulted)
                return false;
            if (!glide::input::routing::CommitReady(afterLoad, routingGeneration_, lifecycle_.Epoch(), GetTickCount64()) ||
                afterLoad.requestSerial != feedback.requestSerial || afterLoad.targetId != feedback.targetId)
                return true;
            if (glide::animation::Magnified(target_))
            {
                auto requested = precise_;
                if (requested.zoom != target_ && !zoomAnchor_.At(target_, monitorBounds_, requested))
                    return false;
                glide::monitor::Frame frame{};
                if (!glide::monitor::MakeFrame(monitorBounds_, requested, frame))
                    return false;
                UpdateIndicator(*monitor_, indicator_, frame.transform);
                if (!lease.Call([&]() { return monitor_->Prepare(frame); }))
                    return monitor_->Pending() && lease.Valid();
                // Device/capture startup may have taken a scheduling interval.
                // Require the live worker's current barrier again before writing
                // Windows' single global input map.
                if (!guardian_.Alive() || !input_.Healthy())
                    return false;
                glide::input::routing::Feedback current{};
                if (!input_.ReadRoutingFeedback(current))
                    return true;
                if (current.faulted)
                    return false;
                if (!glide::input::routing::CommitReady(current, routingGeneration_, lifecycle_.Epoch(), GetTickCount64()) ||
                    current.requestSerial != feedback.requestSerial ||
                    current.targetId != feedback.targetId)
                    return true;
                const glide::session::Snapshot desired{ { 1, 0, 0 }, { true, frame.source, frame.destination } };
                const auto applied = session_.Apply(desired, backend, guardian_);
                if ((applied != glide::session::Result::Applied && applied != glide::session::Result::Unchanged) ||
                    !lease.Valid() || !lease.Call([&]() { return monitor_->Show(); }) ||
                    !lease.Call([&]() { return monitor_->Matches(frame); }))
                    return false;
                precise_ = requested;
                transform_ = frame.transform;
            }
            else
            {
                if (!guardian_.Alive() || !input_.Healthy())
                    return false;
                const auto reset = session_.Reset(backend, guardian_);
                if ((reset != glide::session::Result::Applied && reset != glide::session::Result::Unchanged) || !lease.Valid())
                    return false;
                monitor_->Hide();
                if (monitor_->Active())
                    return false;
                transform_ = precise_ = {};
                indicator_.Reset();
            }
            routingAck_ = handoffSerial_;
            routingBusy_ = viewLoadedForHandoff_ = false;
            handoffTarget_ = nullptr;
            const bool preserveWheel = WheelIntentCurrent() &&
                                       wheelIntent_.MonitorId() == reinterpret_cast<std::uintptr_t>(selectedMonitor_);
            CancelInput(preserveWheel);
            if (preserveWheel && wheelIntent_.Commit(lifecycle_.Epoch(), GetTickCount64()))
            {
                if (!input_.UpdateActivationWheel(wheelIntent_.Observation()))
                    routingPublishFailed_ = true;
                const double extra = wheelIntent_.TakePending();
                if (extra != 0)
                    SetTarget(glide::animation::StepZoom(rawTarget_, settings_.reverse ? -extra : extra, settings_.maxZoom, settings_.zoomMultiplier), false);
            }
            return true;
        }

        bool ServiceIndependentViews()
        {
            if (!IndependentMode() || !lifecycle_.Ready() || captureBusy_ || captureRequested_)
                return true;
            glide::input::routing::Feedback feedback{};
            if (input_.ReadRoutingFeedback(feedback))
            {
                if (feedback.faulted)
                    return false;
                const auto target = reinterpret_cast<HMONITOR>(static_cast<std::uintptr_t>(feedback.targetId));
                if (!routingBusy_ && target && (target != selectedMonitor_ || feedback.requestSerial > routingAck_))
                    if (!BeginHandoff(target, feedback.requestSerial, target == selectedMonitor_))
                        return false;
            }
            if (routingBusy_ && !CompleteHandoff())
                return false;
            for (auto& item : parkedViews_)
            {
                NativeOperation operation{ nativeOperation_ };
                if (!glide::animation::Magnified(item.target))
                {
                    item.renderer->Hide();
                    item.applied = item.precise = {};
                    continue;
                }
                auto requested = item.precise;
                if (requested.zoom != item.target && !item.anchor.At(item.target, item.display.bounds, requested))
                    return false;
                glide::monitor::Frame frame{};
                if (!glide::monitor::MakeFrame(item.display.bounds, requested, frame))
                    return false;
                UpdateIndicator(*item.renderer, item.indicator, frame.transform);
                const auto refreshed = item.renderer->RefreshPassive(frame);
                if (refreshed == GpuMonitorMagnifier::VisualRefresh::Failed || !lifecycle_.Ready())
                    return false;
                if (refreshed == GpuMonitorMagnifier::VisualRefresh::Presented)
                {
                    item.applied = frame.transform;
                    item.precise = requested;
                }
                input_.Beat();
                guardian_.Beat();
            }
            return true;
        }

        bool MonitorStillValid() const noexcept
        {
            if (!selectedMonitor_)
                return false;
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            return GetMonitorInfoW(selectedMonitor_, &info) &&
                   monitorBounds_.left == info.rcMonitor.left && monitorBounds_.top == info.rcMonitor.top &&
                   monitorBounds_.right == info.rcMonitor.right && monitorBounds_.bottom == info.rcMonitor.bottom;
        }

        bool SelectMonitor(glide::Vec2 source, bool allowRetarget)
        {
            if (!MonitorMode())
                return true;
            if (!glide::detail::Finite(source) || source.x < -262144 || source.x > 262144 ||
                source.y < -262144 || source.y > 262144)
                return false;
            const POINT point{ static_cast<LONG>(std::lround(source.x)), static_cast<LONG>(std::lround(source.y)) };
            const HMONITOR selected = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (!selected || !GetMonitorInfoW(selected, &info))
                return false;
            const glide::Rect bounds{ static_cast<double>(info.rcMonitor.left), static_cast<double>(info.rcMonitor.top), static_cast<double>(info.rcMonitor.right), static_cast<double>(info.rcMonitor.bottom) };
            if (!glide::monitor::ValidBounds(bounds))
                return false;
            if (selected == selectedMonitor_ && MonitorStillValid())
                return !routingBusy_;
            if (IndependentMode())
            {
                if (!allowRetarget || drag_.active || routingBusy_)
                    return false;
                if (!BeginHandoff(selected, routingAck_))
                    return false;
                return CompleteHandoff() && !routingBusy_;
            }
            // A held gesture is bound to its original monitor. A fresh wheel/key
            // command may transfer the view, only after restoring the old session.
            if (session_.NeedsRecovery() || glide::animation::Magnified(transform_.zoom) || monitor_->Active())
            {
                if (!allowRetarget || drag_.active)
                    return false;
                if (!Reset() || !lifecycle_.Ready() || session_.NeedsRecovery() ||
                    (lastRestore_ != glide::session::Result::Applied && lastRestore_ != glide::session::Result::Unchanged))
                    return false;
            }
            selectedMonitor_ = selected;
            monitorBounds_ = bounds;
            UpdateFrameRate(selected);
            return true;
        }

        bool CursorSource(glide::Vec2& source) const noexcept
        {
            POINT point{};
            if (!GetPhysicalCursorPos(&point))
                return false;
            source = { static_cast<double>(point.x), static_cast<double>(point.y) };
            return true;
        }

        bool Anchor(glide::Vec2 source, bool allowRetarget = true)
        {
            if (!SelectMonitor(source, allowRetarget))
                return false;
            if (!MonitorMode())
            {
                const POINT point{ static_cast<LONG>(std::lround(source.x)), static_cast<LONG>(std::lround(source.y)) };
                UpdateFrameRate(MonitorFromPoint(point, MONITOR_DEFAULTTOPRIMARY));
            }
            // Keep the precise anchor through a wheel/key burst. A rendered
            // integer crop is an output, never the next controller input.
            return zoomAnchor_.Set(precise_, source, ZoomBounds());
        }

        bool EnsurePointerView()
        {
            if (queuedCopy_)
                return false;
            if (!IndependentMode())
                return lifecycle_.Ready();
            glide::Vec2 source{};
            if (!CursorSource(source))
                return false;
            return SelectMonitor(source, true);
        }

        void StepZoom(double amount)
        {
            if (!EnsurePointerView())
                return;
            SetTarget(glide::animation::StepZoom(target_, amount, settings_.maxZoom, settings_.zoomMultiplier));
        }

        void SetTarget(double value, bool cancelInput = true)
        {
            if (!lifecycle_.Ready() || routingBusy_ || queuedCopy_)
                return;
            if (cancelInput)
                CancelInput();
            glide::Vec2 source{};
            if (!CursorSource(source))
            {
                Suspend();
                return;
            }
            if (routingPublishFailed_)
            {
                UnexpectedFault(L"The independent-monitor input state could not be published coherently.");
                return;
            }
            // Zoom-out remains available after panning, capture, or leaving the
            // selected monitor. Re-anchor the current view, without retargeting it.
            if (value <= 1 && transform_.zoom > 1)
            {
                const auto bounds = ZoomBounds();
                if (!glide::monitor::ValidBounds(bounds))
                {
                    Suspend();
                    return;
                }
                if (!glide::monitor::Contains(bounds, source))
                {
                    source = glide::ScreenToSource({ (bounds.left + bounds.right) / 2,
                                                     (bounds.top + bounds.bottom) / 2 },
                                                   precise_);
                    source.x = std::clamp(source.x, bounds.left, bounds.right - 1);
                    source.y = std::clamp(source.y, bounds.top, bounds.bottom - 1);
                }
                if (!zoomAnchor_.Set(precise_, source, bounds))
                {
                    Suspend();
                    return;
                }
                target_ = rawTarget_ = 1;
                if (IndependentMode() && !BeginHandoff(selectedMonitor_, routingAck_, true))
                    UnexpectedFault(L"The independent-monitor input gate could not be prepared.");
                return;
            }
            if (!Anchor(source))
            {
                if (!routingBusy_)
                    Suspend();
                return;
            }
            rawTarget_ = glide::ClampZoom(value, settings_.maxZoom);
            target_ = glide::animation::Quantize(rawTarget_, settings_.maxZoom);
            if (IndependentMode() && !glide::animation::Magnified(transform_.zoom) && glide::animation::Magnified(target_))
                if (!BeginHandoff(selectedMonitor_, routingAck_, true))
                    UnexpectedFault(L"The independent-monitor input gate could not be prepared.");
        }

        void Toggle(bool cancelInput = true)
        {
            if (!lifecycle_.Ready() || !EnsurePointerView())
                return;
            if (glide::animation::Magnified(target_) || glide::animation::Magnified(transform_.zoom))
            {
                savedZoom_ = std::max(target_, transform_.zoom);
                SetTarget(1, cancelInput);
            }
            else
                SetTarget(std::min(savedZoom_, static_cast<double>(settings_.maxZoom)), cancelInput);
        }

        void ToggleScope(glide::view::Scope requested)
        {
            if (!lifecycle_.Ready())
                return;
            if (requested == glide::view::Scope::Monitor && settings_.zoomScope == 2)
                requested = glide::view::Scope::Independent;
            if (view_.Current() != requested)
            {
                if (!Reset())
                    return;
                const bool terminal = lifecycle_.Current() == Phase::Faulted || lifecycle_.Current() == Phase::Stopping;
                if (!view_.CommitSwitch(requested, lastRestore_, glide::OnInputDesktop(), lifecycle_.NeedsRestore() || session_.NeedsRecovery(), terminal) ||
                    !lifecycle_.Ready())
                    return;
                selectedMonitor_ = nullptr;
                monitorBounds_ = {};
            }
            Toggle();
            UpdateStatus();
        }

        void BeginDrag(glide::Vec2 source, bool touchpad = false)
        {
            if (!lifecycle_.Ready() || drag_.active)
                return;
            if (!Anchor(source, false))
            {
                if (!routingBusy_)
                    CancelInput();
                return;
            }
            drag_ = {};
            drag_.active = true;
            drag_.touchpad = touchpad;
            drag_.startZoom = precise_.zoom;
            target_ = rawTarget_ = precise_.zoom;
            followingState_.Reset();
        }

        void DragPixels(double delta, double travel) noexcept
        {
            if (!drag_.active || !std::isfinite(delta) || !std::isfinite(travel) || travel < 0)
                return;
            drag_.pixels = std::clamp(drag_.pixels + delta, -1000000.0, 1000000.0);
            drag_.travel = std::min(1000000.0, drag_.travel + std::max(std::abs(delta), travel));
            const double signedPixels = settings_.reverse ? -drag_.pixels : drag_.pixels;
            rawTarget_ = glide::DragZoom(drag_.startZoom, signedPixels, settings_.DragDistance(), settings_.maxZoom);
            target_ = glide::animation::QuantizeHeld(rawTarget_, target_, settings_.maxZoom);
        }

        void EndDrag(bool cancel)
        {
            if (!drag_.active)
                return;
            const bool tapOnly = !drag_.touchpad && drag_.travel < 4;
            drag_.active = false;
            // Start later wheel/key input at the selected step, without inheriting
            // a held gesture's sub-step hysteresis remainder.
            rawTarget_ = target_;
            if (!cancel && tapOnly)
                Toggle(false);
        }

        // Restoration is conditional on the session's observed ownership. If the
        // desktop is temporarily unavailable, retain the journal for return/recovery.
        bool RestoreOwned() noexcept
        {
            NativeOperation operation{ nativeOperation_ };
            deferredReset_ = false;
            target_ = rawTarget_ = 1;
            glide::cursor::NativeBackend cursorBackend{ mag_ };
            const bool cursorRestored = cursor_.Release(cursorBackend, guardian_);
            monitor_->SetSystemCursorHiddenByUs(cursor_.Hidden());
            // Screenshot reconstruction briefly releases input/visibility but
            // needs the same complete GPU frame for synchronous restoration.
            // Ordinary reset/1x closes capture, so idle zoom does not capture.
            monitor_->Hide(captureBusy_ && captureRequested_);
            for (auto& item : parkedViews_)
                item.renderer->Hide(captureBusy_ && captureRequested_);
            const bool parkedStillVisible = std::any_of(parkedViews_.begin(), parkedViews_.end(), [](const auto& item) { return item.renderer->Active(); });
            if (!(captureBusy_ && captureRequested_) && !parkedStillVisible)
            {
                parkedViews_.clear();
                indicator_.Reset();
            }
            routingBusy_ = viewLoadedForHandoff_ = false;
            handoffTarget_ = nullptr;
            lastRestore_ = glide::session::Result::Unchanged;
            if (session_.NeedsRecovery() && magInitialized_)
            {
                if (!glide::OnInputDesktop())
                {
                    lastRestore_ = glide::session::Result::ReadFailure;
                    lifecycle_.RequestRestore();
                    // This can also interrupt the same-hold 1x path, which has
                    // not called Reset. Its queued deltas must not revive zoom
                    // after a deferred restoration completes on the next tick.
                    CancelInput();
                    return false;
                }
                const auto result = session_.Reset(mag_, guardian_);
                lastRestore_ = result;
                if (result == glide::session::Result::Conflict)
                {
                    lifecycle_.RestoreConflict();
                    ReleaseHotkeys();
                    CancelInput();
                }
                else if (result != glide::session::Result::Applied && result != glide::session::Result::Unchanged)
                {
                    lifecycle_.Fault();
                    failed_ = true;
                    ReleaseHotkeys();
                    CancelInput();
                    if (!cleaned_)
                        PostMessageW(hwnd_, kQuit, 0, 0);
                    return false;
                }
            }
            if (!cursorRestored || cursor_.NeedsRecovery())
            {
                lifecycle_.RequestRestore();
                CancelInput();
                if (!glide::OnInputDesktop())
                    return false;
                lifecycle_.Fault();
                failed_ = true;
                ReleaseHotkeys();
                if (!cleaned_)
                    PostMessageW(hwnd_, kQuit, 0, 0);
                return false;
            }
            if (monitor_->Active() || parkedStillVisible)
            {
                // Global input/cursor recovery above remains independent of a
                // failed private-window hide. Do not report a reset as complete
                // while a stale overlay survives; process exit removes its HWND.
                lifecycle_.Fault();
                failed_ = true;
                ReleaseHotkeys();
                CancelInput();
                if (!cleaned_ && hwnd_)
                    PostMessageW(hwnd_, kQuit, 0, 0);
                return false;
            }
            if (deferredReset_)
                lifecycle_.RequestRestore();
            else
                lifecycle_.RestoreComplete();
            transform_ = precise_ = {};
            followingState_.Reset();
            if (!drag_.active)
            {
                selectedMonitor_ = nullptr;
                monitorBounds_ = {};
                zoomAnchor_.Reset();
            }
            ConfigureInput();
            return true;
        }

        bool ReleaseIndependentZoom() noexcept
        {
            NativeOperation operation{ nativeOperation_ };
            glide::frame::Lease lease{ lifecycle_ };
            glide::frame::Backend<MagnificationApi> backend{ mag_, lease };
            glide::cursor::NativeBackend cursorBackend{ mag_ };
            if (!cursor_.Release(cursorBackend, guardian_))
                return false;
            monitor_->SetSystemCursorHiddenByUs(false);
            monitor_->Hide();
            const auto reset = session_.Reset(backend, guardian_);
            if (!lease.Valid() || monitor_->Active() ||
                (reset != glide::session::Result::Applied && reset != glide::session::Result::Unchanged))
                return false;
            target_ = rawTarget_ = 1;
            transform_ = precise_ = {};
            indicator_.Reset();
            followingState_.Reset();
            if (!drag_.active)
                zoomAnchor_.Reset();
            ConfigureInput();
            return true;
        }

        bool ResetNow() noexcept
        {
            lifecycle_.RequestRestore();
            CancelInput();
            return RestoreOwned();
        }

        bool Reset() noexcept
        {
            if (captureRequested_ || nativeOperation_ || messageDuringOperation_)
            {
                // A sent message can reenter from a native capture or magnifier
                // call. Invalidate immediately, but never reenter Session::Reset
                // while an outer Session operation may still be on the stack.
                target_ = rawTarget_ = 1;
                deferredReset_ = true;
                lifecycle_.RequestRestore();
                CancelInput();
                return false;
            }
            return ResetNow();
        }

        void DrainDeferred() noexcept
        {
            if (cleaned_ || drainingDeferred_ || captureRequested_ || frameInProgress_ ||
                nativeOperation_ || messageDuringOperation_)
                return;
            NativeOperation draining{ drainingDeferred_ };
            // Consume one request at a safe boundary. If a native recovery call
            // receives another reset, leave it latched for the next boundary; do
            // not recursively enter Session or spin through hostile callbacks.
            if (deferredReset_)
            {
                deferredReset_ = false;
                ResetNow();
            }
            if (deferredQuit_ || lifecycle_.Terminal())
            {
                if (IsWindow(hwnd_))
                    PostMessageW(hwnd_, kQuit, 0, 0);
                else
                {
                    Cleanup();
                    PostQuitMessage(0);
                }
            }
            else if (!pendingError_.empty())
                PostMessageW(hwnd_, kError, 0, 0);
        }

        void Pause() noexcept
        {
            if (lifecycle_.Current() == Phase::Faulted || lifecycle_.Current() == Phase::Stopping)
                return;
            lifecycle_.Pause();
            ReleaseHotkeys();
            Reset();
        }

        void Suspend(bool resumeWhenReady = false) noexcept
        {
            if (lifecycle_.Current() == Phase::Faulted || lifecycle_.Current() == Phase::Stopping)
                return;
            if (lifecycle_.Current() != Phase::Suspended)
                Diagnostic(L"Controls suspended; pending input and zoom were canceled until the desktop and input thread are ready.");
            lifecycle_.Suspend(resumeWhenReady);
            ReleaseHotkeys();
            Reset();
        }

        void UnexpectedFault(const wchar_t* reason = L"An unexpected engine fault requested safe shutdown.") noexcept
        {
            if (!failed_)
                Diagnostic(reason, true);
            failed_ = true;
            lifecycle_.Fault();
            ReleaseHotkeys();
            Reset();
            lifecycle_.Fault();
            ConfigureInput();
            if (hwnd_)
                PostMessageW(hwnd_, kQuit, 0, 0);
        }

        void SessionFailure(glide::session::Result result, const wchar_t* detail = nullptr)
        {
            if (!glide::OnInputDesktop())
            {
                Suspend();
                return;
            }
            // Capture the reason before deferred/native recovery can reset it.
            std::wstring message;
            if (result == glide::session::Result::Conflict)
                message = L"Another application changed desktop magnification. GlideZoom released control "
                          L"and paused without resetting that application's state.";
            else
            {
                message = L"GlideZoom could not present or verify zoom and paused its controls. "
                          L"Recovery of its recognized display/input state has been requested. "
                          L"Check the display, then resume from the tray.";
                if (detail && *detail)
                    message += L"\n\n" + std::wstring(detail);
            }
            lifecycle_.Pause();
            ReleaseHotkeys();
            Reset();
            // Reset deliberately defers inside a native frame. A deferred reset
            // is not a reason to suppress the only explanation for lost controls.
            // QueueError/DrainDeferred display this after the outer call unwinds.
            QueueError(message.c_str());
        }

        bool UpdateCursor() noexcept
        {
            NativeOperation operation{ nativeOperation_ };
            const auto epoch = lifecycle_.Epoch();
            glide::Vec2 source{};
            const bool replacement = lifecycle_.Ready() && MonitorMode() &&
                                     !routingBusy_ &&
                                     glide::animation::Magnified(transform_.zoom) && monitor_->Active() &&
                                     CursorSource(source) && monitor_->ReplacementVisibleAt(source) && lifecycle_.Accept(epoch);
            struct CursorBackend
            {
                App& owner;
                const std::uint64_t epoch;
                glide::cursor::NativeBackend native;
                bool coverageLost = false;
                bool OnInputDesktop() const noexcept { return native.OnInputDesktop(); }
                bool CursorShowing(bool& showing) const noexcept
                {
                    return owner.lifecycle_.Accept(epoch) && native.CursorShowing(showing) &&
                           owner.lifecycle_.Accept(epoch);
                }
                bool ShowSystemCursor(bool show) noexcept
                {
                    // Restoring the cursor remains permitted after cancellation.
                    if (!show && owner.lifecycle_.Accept(epoch) && !owner.monitor_->ReplacementVisibleNow())
                    {
                        coverageLost = true;
                        return false;
                    }
                    if ((!show && !owner.lifecycle_.Accept(epoch)) || !native.ShowSystemCursor(show))
                        return false;
                    if (!show && !owner.monitor_->ReplacementVisibleNow())
                    {
                        // A normal monitor crossing can race the hide. Lease
                        // immediately restores the global cursor in this case.
                        coverageLost = true;
                        return false;
                    }
                    return show || owner.lifecycle_.Accept(epoch);
                }
            } backend{ *this, epoch, glide::cursor::NativeBackend{ mag_ } };
            const bool result = cursor_.Update(replacement, backend, guardian_);
            if (!lifecycle_.Accept(epoch))
            {
                const bool released = cursor_.Release(backend, guardian_);
                monitor_->SetSystemCursorHiddenByUs(cursor_.Hidden());
                return released;
            }
            monitor_->SetSystemCursorHiddenByUs(cursor_.Hidden());
            // Losing replacement coverage is ordinary pointer movement, not an
            // engine failure, provided the hide was completely recovered.
            return result || (backend.coverageLost && !cursor_.NeedsRecovery() && !cursor_.Hidden());
        }

        void CursorFailure() noexcept
        {
            if (!glide::OnInputDesktop())
                Suspend();
            else
                UnexpectedFault();
        }

        bool Apply(glide::Transform next)
        {
            glide::frame::Lease lease{ lifecycle_ };
            if (!lease.Valid())
                return false;
            if (!guardian_.Alive())
            {
                UnexpectedFault();
                return false;
            }
            if (!glide::OnInputDesktop())
            {
                Suspend();
                return false;
            }
            if (!std::isfinite(next.zoom) || next.zoom < 1 || next.zoom > settings_.maxZoom ||
                !glide::detail::Finite(next.offset) || std::abs(next.offset.x) > 262144 ||
                std::abs(next.offset.y) > 262144)
            {
                UnexpectedFault();
                return false;
            }
            // Reaching 1x releases the transforms, but a still-held gesture remains
            // active so reversing the same movement can zoom in again.
            if (glide::animation::RestoreIdentity(target_, next.zoom))
            {
                if (IndependentMode())
                {
                    if (!ReleaseIndependentZoom())
                        SessionFailure(glide::session::Result::WriteFailure, L"The current monitor could not release its zoom safely.");
                    return lifecycle_.Ready();
                }
                return drag_.active ? RestoreOwned() : Reset();
            }
            // An identity step must not acquire an overlay/input mapping.
            if (!glide::animation::Magnified(next.zoom))
                return true;
            NativeOperation operation{ nativeOperation_ };
            glide::frame::Backend<MagnificationApi> backend{ mag_, lease };
            if (MonitorMode())
            {
                glide::monitor::Frame frame{};
                if (!MonitorStillValid() || !glide::monitor::MakeFrame(monitorBounds_, next, frame))
                {
                    Suspend();
                    return false;
                }
                monitor_->SetSystemCursorHiddenByUs(cursor_.Hidden());
                UpdateIndicator(*monitor_, indicator_, frame.transform);
                glide::frame::Renderer<GpuMonitorMagnifier> renderer{ *monitor_, lease };
                const auto presented = glide::monitor::Present(frame, renderer, session_, backend, guardian_);
                if (!lease.Valid())
                    return false; // Deferred reset owns the next write.
                // Capture/compositor startup is asynchronous. Keep the applied
                // transform and pointer publication on the displayed frame. Still
                // check cursor coverage: moving to another monitor while waiting
                // must release our hidden-system-cursor lease immediately.
                if (presented.pending)
                {
                    if (!UpdateCursor())
                    {
                        CursorFailure();
                        return false;
                    }
                    return true;
                }
                if (presented.result != glide::session::Result::Applied && presented.result != glide::session::Result::Unchanged)
                {
                    std::wstring detail;
                    if (presented.localFailure)
                    {
                        wchar_t code[48]{};
                        swprintf_s(code, L"\nGraphics result: 0x%08lX", static_cast<unsigned long>(monitor_->LastResult()));
                        detail = std::wstring(monitor_->FailureReason()) + code;
                    }
                    SessionFailure(presented.result, detail.c_str());
                    return false;
                }
                const bool zoomChanged = transform_.zoom != frame.transform.zoom;
                transform_ = frame.transform;
                if (zoomChanged)
                    ConfigureInput();
                else
                    PublishPointer();
                if (!UpdateCursor())
                {
                    CursorFailure();
                    return false;
                }
                return true;
            }
            const int x = static_cast<int>(std::lround(next.offset.x));
            const int y = static_cast<int>(std::lround(next.offset.y));
            const float zoom = static_cast<float>(next.zoom);
            const glide::session::Snapshot desired{
                { zoom, x, y }, { true, { static_cast<LONG>(std::lround(x + desktop_.left / zoom)), static_cast<LONG>(std::lround(y + desktop_.top / zoom)), static_cast<LONG>(std::lround(x + desktop_.right / zoom)), static_cast<LONG>(std::lround(y + desktop_.bottom / zoom)) }, { static_cast<LONG>(desktop_.left), static_cast<LONG>(desktop_.top), static_cast<LONG>(desktop_.right), static_cast<LONG>(desktop_.bottom) } }
            };
            const auto result = session_.Apply(desired, backend, guardian_);
            if (!lease.Valid())
                return false;
            if (result != glide::session::Result::Applied && result != glide::session::Result::Unchanged)
            {
                SessionFailure(result);
                return false;
            }
            const bool zoomChanged = transform_.zoom != zoom;
            transform_ = { zoom, { static_cast<double>(x), static_cast<double>(y) } };
            if (zoomChanged)
                ConfigureInput();
            else
                PublishPointer();
            if (!UpdateCursor())
            {
                CursorFailure();
                return false;
            }
            return true;
        }

        struct CaptureTransaction
        {
            App& owner;
            explicit CaptureTransaction(App& application) noexcept :
                owner(application) {}
            glide::capture::Plan plan{};
            glide::capture::Lease lease;
            glide::capture::NativeImage image;
            glide::Transform saved{};
            glide::view::Scope scope = glide::view::Scope::Monitor;
            HMONITOR selected = nullptr;
            glide::Rect bounds{};

            bool Ready() const noexcept
            {
                return !owner.captureBusy_ && owner.lifecycle_.Ready() &&
                       owner.guardian_.Alive() && owner.input_.Healthy() && glide::OnInputDesktop();
            }
            bool EnvironmentMatches() const noexcept
            {
                glide::capture::Topology current;
                glide::capture::PixelRect cachedDesktop;
                return lease.Matches(owner.lifecycle_.Epoch()) && glide::OnInputDesktop() &&
                       owner.guardian_.Alive() && owner.input_.Healthy() &&
                       !owner.lifecycle_.Terminal() && owner.view_.Current() == scope &&
                       glide::capture::ReadTopology(current) &&
                       glide::capture::ToPixelRect(owner.desktop_, cachedDesktop) &&
                       glide::capture::DesktopBounds(current) == cachedDesktop &&
                       glide::capture::SameTopology(plan.topology, current) &&
                       lease.Matches(owner.lifecycle_.Epoch()) && owner.lifecycle_.Ready();
            }
            bool Prepare() noexcept
            {
                lease.Acquire(owner.lifecycle_.Epoch());
                saved = owner.transform_; // Applied float/integer state, never an animation target.
                scope = owner.view_.Current();
                selected = owner.selectedMonitor_;
                bounds = owner.monitorBounds_;
                glide::capture::Topology topology;
                if (!glide::capture::ReadTopology(topology))
                    return false;
                glide::capture::PixelRect cachedDesktop;
                if (!glide::capture::ToPixelRect(owner.desktop_, cachedDesktop) ||
                    !(glide::capture::DesktopBounds(topology) == cachedDesktop))
                {
                    owner.Suspend(); // Refresh cached geometry after this request unwinds.
                    return false;
                }
                if (scope != glide::view::Scope::Desktop)
                {
                    if (glide::animation::Magnified(saved.zoom))
                    {
                        if (!owner.MonitorStillValid())
                            return false;
                    }
                    else
                    {
                        POINT point{};
                        MONITORINFO info{};
                        info.cbSize = sizeof(info);
                        if (!GetPhysicalCursorPos(&point))
                            return false;
                        selected = MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
                        if (!selected || !GetMonitorInfoW(selected, &info))
                            return false;
                        bounds = { static_cast<double>(info.rcMonitor.left), static_cast<double>(info.rcMonitor.top), static_cast<double>(info.rcMonitor.right), static_cast<double>(info.rcMonitor.bottom) };
                    }
                }
                return glide::capture::MakePlan(scope != glide::view::Scope::Desktop ?
                                                    glide::capture::Scope::Monitor :
                                                    glide::capture::Scope::Desktop,
                                                bounds,
                                                saved,
                                                topology,
                                                plan) &&
                       image.Prepare(plan) && EnvironmentMatches() &&
                       owner.lifecycle_.Ready() && owner.transform_.zoom == saved.zoom &&
                       owner.transform_.offset.x == saved.offset.x && owner.transform_.offset.y == saved.offset.y;
            }
            void Quiesce() noexcept
            {
                owner.captureBusy_ = true;
                if (!lease.AdvanceOne(owner.lifecycle_.Epoch()))
                    return;
                owner.CancelInput();
                if (!lease.Matches(owner.lifecycle_.Epoch()))
                    return;
                owner.target_ = owner.rawTarget_ = saved.zoom;
                owner.precise_ = saved;
                Pulse(this);
            }
            bool EnterIdentity() noexcept
            {
                // Reset retains its recovery journal on a partial write and gives
                // up ownership on an observed foreign transform. No blanket reset.
                if (!lease.AdvanceOne(owner.lifecycle_.Epoch()) || !owner.ResetNow())
                    return false;
                if (IdentityMatches())
                    return true;
                owner.Pause();
                return false;
            }
            bool IdentityMatches() const noexcept
            {
                glide::session::Snapshot current;
                return EnvironmentMatches() && !owner.session_.NeedsRecovery() &&
                       !owner.monitor_->Active() &&
                       std::none_of(owner.parkedViews_.begin(), owner.parkedViews_.end(), [](const auto& item) { return item.renderer->Active(); }) &&
                       lease.Observe(owner.lifecycle_, [&]() noexcept {
                           return owner.mag_.Read(current) && glide::session::Valid(current) &&
                                  glide::session::IsIdentity(current);
                       });
            }
            static bool Pulse(void* context) noexcept
            {
                auto& transaction = *static_cast<CaptureTransaction*>(context);
                auto& application = transaction.owner;
                application.input_.Beat();
                application.guardian_.Beat();
                return application.captureBusy_ && transaction.lease.Matches(application.lifecycle_.Epoch()) && !application.lifecycle_.Terminal() &&
                       application.guardian_.Alive() && application.input_.Healthy() && glide::OnInputDesktop();
            }
            bool Capture() noexcept { return image.Capture({ Pulse, this }); }
            bool RestoreView() noexcept
            {
                if (!IdentityMatches() || !owner.lifecycle_.Ready())
                {
                    owner.Pause();
                    return false;
                }
                try
                {
                    if (glide::animation::Magnified(saved.zoom))
                    {
                        owner.selectedMonitor_ = selected;
                        owner.monitorBounds_ = bounds;
                        owner.target_ = owner.rawTarget_ = saved.zoom;
                        owner.precise_ = saved;
                        if (!owner.Apply(saved))
                            return false;
                    }
                    if (!lease.Matches(owner.lifecycle_.Epoch()) || !owner.lifecycle_.Ready())
                        return false;
                    // A screenshot ends the gesture/animation at the captured
                    // applied view. Stale deltas must not restart it on completion.
                    owner.target_ = owner.rawTarget_ = owner.transform_.zoom;
                    owner.precise_ = owner.transform_;
                    return true;
                }
                catch (...)
                {
                    owner.UnexpectedFault();
                    return false;
                }
            }
            bool Render() noexcept { return image.Render({ Pulse, this }); }
            bool RestoredMatches() noexcept
            {
                if (!EnvironmentMatches() || !owner.lifecycle_.Ready() ||
                    owner.transform_.zoom != saved.zoom ||
                    owner.transform_.offset.x != saved.offset.x || owner.transform_.offset.y != saved.offset.y)
                {
                    owner.Pause();
                    return false;
                }
                if (owner.MonitorMode() && glide::animation::Magnified(saved.zoom) && !owner.MonitorStillValid())
                {
                    owner.Suspend();
                    return false;
                }
                try
                {
                    if (owner.session_.OwnsState())
                    {
                        auto result = glide::session::Result::Unchanged;
                        const bool verified = lease.Observe(owner.lifecycle_, [&] {
                            result = owner.session_.Check(owner.mag_, owner.guardian_);
                            return result == glide::session::Result::Unchanged;
                        });
                        if (result != glide::session::Result::Unchanged)
                            owner.SessionFailure(result);
                        return verified;
                    }
                    return IdentityMatches();
                }
                catch (...)
                {
                    owner.UnexpectedFault();
                    return false;
                }
            }
            glide::capture::ClipboardResult Publish() noexcept { return image.Publish(owner.hwnd_); }
            void Finish() noexcept
            {
                // Cancel while disabled, then publish one new live generation.
                owner.CancelInput();
                owner.lastTick_ = GetTickCount64();
                owner.input_.Beat();
                owner.guardian_.Beat();
                if (!owner.guardian_.Alive() || owner.input_.Faulted())
                    owner.UnexpectedFault(L"Input or recovery failed while finishing a screenshot.");
                else if (!glide::OnInputDesktop() || !owner.input_.Healthy())
                    owner.Suspend(!owner.captureUserCancelled_);
                owner.captureBusy_ = false;
                owner.ConfigureInput();
            }
        };

        void CopyZoomedView(bool queuedAuthorized = false)
        {
            if (captureRequested_ || (!queuedAuthorized && frameInProgress_) || nativeOperation_ || messageDuringOperation_ ||
                deferredReset_ || deferredQuit_)
                return;
            if (!queuedAuthorized && IndependentMode())
            {
                if (queuedCopy_ || routingBusy_ || !lifecycle_.Ready())
                    return;
                // No mapping/window change is allowed until the hook confirms
                // this busy publication and all previously forwarded drags end.
                queuedCopy_ = true;
                captureUserCancelled_ = false;
                CancelInput();
                if (!PublishRouting(true))
                {
                    UnexpectedFault(L"The screenshot input barrier could not be published.");
                    return;
                }
                copyRoutingGeneration_ = routingGeneration_;
                copyRoutingEpoch_ = lifecycle_.Epoch();
                return;
            }
            if (queuedAuthorized && !queuedCopy_)
                return;
            // Guard even allocation/preparation: User32 can dispatch sent messages
            // inside native calls despite this method not pumping a message loop.
            captureRequested_ = true;
            queuedCopy_ = false;
            captureUserCancelled_ = deferredQuit_ = deferredReset_ = false;
            CancelInput();
            struct RequestGuard
            {
                App& owner;
                ~RequestGuard() noexcept
                {
                    owner.CancelInput();
                    // No capture/Session/native-image method remains on the stack.
                    // Drain an emergency reset/pause/exit only at this boundary.
                    if ((owner.deferredReset_ || owner.lifecycle_.NeedsRestore()) && !owner.cleaned_)
                    {
                        owner.lifecycle_.RequestRestore();
                        owner.RestoreOwned();
                    }
                    owner.lastTick_ = GetTickCount64();
                    owner.input_.Beat();
                    owner.guardian_.Beat();
                    owner.captureRequested_ = false;
                    if (!owner.monitor_->Active())
                        owner.monitor_->Hide();
                    if (!owner.frameClock_.Rebase())
                        owner.UnexpectedFault();
                    owner.ConfigureInput();
                    owner.DrainDeferred();
                }
            } requestGuard{ *this };
            CaptureTransaction transaction{ *this };
            const auto result = glide::capture::Run(transaction);
            using Result = glide::capture::Result;
            if (cleaned_ || lifecycle_.Terminal())
                return;
            if (result == Result::ClipboardCloseFailed)
            {
                // Two CloseClipboard attempts failed. Do not keep running or show
                // a modal dialog while we may hold a shared OS clipboard lock.
                // Orderly shutdown resets our recognized zoom and ends the owner
                // thread/process, allowing Windows to release its resources.
                OutputDebugStringW(L"GlideZoom: clipboard release could not be verified; "
                                   L"stopping. The image may or may not have transferred.\n");
                UnexpectedFault();
                return;
            }
            if (captureUserCancelled_)
                return;
            if (result == Result::Copied)
            {
                lastCopy_ = GetTickCount64();
                UpdateStatus();
                return;
            }
            const wchar_t* message = L"The zoomed view could not be copied. No new image was placed on the clipboard.";
            switch (result)
            {
            case Result::NotReady:
                message = L"Resume GlideZoom before copying the view. No screenshot was taken.";
                break;
            case Result::PrepareFailed:
                message = L"The screenshot could not be prepared. The display state may have changed, the layout may exceed "
                          L"the capture size limit, or Windows may not have enough memory. No image was published.";
                break;
            case Result::IdentityFailed:
            case Result::EnvironmentChanged:
            case Result::RestoreFailed:
                message = L"The desktop, display layout or magnification changed while copying. The screenshot was abandoned. "
                          L"GlideZoom restores only state it can still verify as its own. Check the current view before resuming.";
                break;
            case Result::ClipboardBusy:
                message = L"Another application is using the clipboard. The existing clipboard was left unchanged. Try Copy zoomed view again.";
                break;
            case Result::ClipboardSetFailed:
                message = L"Windows cleared the clipboard but could not accept the prepared image. The prior clipboard contents "
                          L"could not be preserved at this final step. Zoom was restored.";
                break;
            default:
                break;
            }
            QueueError(message);
            UpdateStatus();
        }

        bool ServiceFrame() noexcept
        {
            if (cleaned_ || quitReceived_ || frameInProgress_ || captureRequested_ || nativeOperation_ || messageDuringOperation_)
                return true;
            if (runnerStopping_)
                return true;
            if (runner_.ExitRequested())
            {
                runnerStopping_ = true;
                if (!PostMessageW(hwnd_, kQuit, 0, 0))
                {
                    UnexpectedFault();
                    return false;
                }
                return true;
            }
            if (!settingsReloadPending_ && runner_.SettingsChanged())
            {
                settingsReloadResume_ = lifecycle_.Current() == Phase::Ready ||
                                        (lifecycle_.Current() == Phase::Suspended && lifecycle_.ResumeRequested());
                settingsReloadPending_ = true;
            }
            if (settingsReloadPending_)
            {
                try
                {
                    settingsReloadPending_ = !ReloadSettings();
                }
                catch (...)
                {
                    UnexpectedFault();
                    return false;
                }
            }
            double dt = 0;
            const auto due = frameClock_.Take(dt);
            if (due == glide::timing::NativeClock::Poll::Failed)
            {
                UnexpectedFault();
                return false;
            }
            if (due == glide::timing::NativeClock::Poll::Idle)
                return true;
            {
                NativeOperation frame{ frameInProgress_ };
                try
                {
                    Tick();
                }
                catch (...)
                {
                    UnexpectedFault();
                }
            }
            DrainDeferred();
            return true;
        }

        void Tick()
        {
            if (captureRequested_)
                return; // Preparation/capture owns a frozen view, including sent-message reentry.
            if (routingPublishFailed_)
            {
                UnexpectedFault(L"The independent-monitor input state could not be published coherently.");
                return;
            }
            input_.Beat();
            guardian_.Beat();
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG elapsed = now - lastTick_;
            lastTick_ = now;
            if (lifecycle_.Current() == Phase::Stopping)
                return;
            if (lifecycle_.Current() == Phase::Faulted)
            {
                if (!failed_)
                    UnexpectedFault();
                return;
            }
            if (!guardian_.Alive() || input_.Faulted())
            {
                UnexpectedFault(L"The input hook thread or recovery guardian stopped unexpectedly.");
                return;
            }
            if (!glide::OnInputDesktop())
            {
                Suspend();
                return;
            }
            if (!input_.Healthy())
            {
                // A scheduling/resume delay is not proof that the hook thread
                // died. Release the current view and wait for a fresh heartbeat;
                // queued gestures remain canceled and are never replayed.
                Suspend();
                return;
            }
            // Retry deferred restoration even when the requested UI mode is Paused.
            // A brief inaccessible-desktop interval may end before this next tick.
            if (lifecycle_.NeedsRestore() && !RestoreOwned())
                return;
            if (lifecycle_.Current() == Phase::Suspended && !RefreshDesktop())
                return;
            if (lifecycle_.Current() == Phase::Suspended && lifecycle_.ReturnFromSuspend())
                EnableControls();
            if (elapsed > 250)
            {
                const glide::timing::StallContext context{
                    lifecycle_.Ready(), MonitorMode(), monitor_->Pending(), session_.OwnsState(), monitor_->Active(), cursor_.NeedsRecovery(), deferredReset_ || lifecycle_.NeedsRestore()
                };
                if ((IndependentMode() && lifecycle_.Ready()) || glide::timing::DecideStall(context) ==
                                                                     glide::timing::StallAction::PreserveInitialTargetAndDiscardInput)
                {
                    // Initial GPU setup can exceed one input heartbeat. Retain
                    // only the already requested target while capture warms up;
                    // discard old queued motion/steps and held-gesture state.
                    // A queued emergency reset still passes the check below.
                    if (routingBusy_ && wheelIntent_.Active() && !wheelIntent_.Committed() && wheelIntent_.Current(now))
                    {
                        // Startup already canceled every held gesture. Retain
                        // only wheel travel physically accepted for this exact
                        // monitor before the stall, then close its admission.
                        CollectActivationWheel();
                        CloseWheelIntent(false);
                    }
                    else
                        CancelInput();
                }
                else
                    Reset();
            }
            if (MonitorMode() && selectedMonitor_ && !MonitorStillValid())
            {
                Suspend();
                return;
            }
            if (queuedCopy_)
            {
                if (!IndependentMode() || !lifecycle_.Ready() || deferredReset_ || deferredQuit_ ||
                    copyRoutingEpoch_ != lifecycle_.Epoch())
                {
                    queuedCopy_ = false;
                    ConfigureInput();
                    return;
                }
                // Emergency reset remains serviceable while capture is waiting
                // for a drag release; stale zoom deltas are deliberately dropped.
                glide::input::Event queued{};
                for (unsigned processed = 0; processed < 256 && input_.Poll(queued); ++processed)
                {
                    if (queued.kind == glide::input::EventKind::Reset)
                    {
                        queuedCopy_ = false;
                        captureUserCancelled_ = true;
                        Reset();
                        return;
                    }
                }
                glide::input::routing::Feedback barrier{};
                if (input_.ReadRoutingFeedback(barrier))
                {
                    if (barrier.faulted)
                    {
                        UnexpectedFault(L"The screenshot input barrier reported a hook failure.");
                        return;
                    }
                    if (glide::input::routing::CommitReady(barrier, copyRoutingGeneration_, copyRoutingEpoch_, GetTickCount64()) &&
                        input_.Healthy() && guardian_.Alive() && glide::OnInputDesktop())
                        CopyZoomedView(true);
                }
                return;
            }
            if (lifecycle_.Ready())
            {
                glide::frame::Lease lease{ lifecycle_ };
                glide::frame::Backend<MagnificationApi> backend{ mag_, lease };
                NativeOperation operation{ nativeOperation_ };
                if (session_.OwnsState())
                {
                    const auto result = session_.Check(backend, guardian_);
                    if (!lease.Valid())
                        return;
                    if (result != glide::session::Result::Unchanged)
                        SessionFailure(result);
                }
                else
                {
                    glide::session::Snapshot current{};
                    const bool read = backend.Read(current);
                    if (!lease.Valid())
                        return;
                    if (!read)
                        SessionFailure(glide::session::Result::ReadFailure);
                    else if (!glide::session::Valid(current) || !glide::session::IsIdentity(current))
                        SessionFailure(glide::session::Result::Conflict);
                }
            }
            // Release the global cursor before capture/presentation can wait.
            // Other monitors must keep their ordinary cursor during GPU stalls.
            if (!UpdateCursor())
            {
                CursorFailure();
                return;
            }
            glide::input::Event event{};
            for (unsigned processed = 0; processed < 256 && input_.Poll(event); ++processed)
            {
                // Wheel intent validates its explicit monitor, original time
                // and bounded activation epochs before any camera change. This
                // retains packets queued immediately before the busy barrier.
                if (event.kind == glide::input::EventKind::Wheel)
                {
                    HandleWheel(event);
                    continue;
                }
                if (event.kind == glide::input::EventKind::WheelCancel)
                {
                    WheelIntentEvent(event);
                    continue;
                }
                if (!glide::timing::AcceptQueuedInput(event.kind == glide::input::EventKind::Reset,
                                                      event.epoch,
                                                      lifecycle_))
                    continue;
                if (routingBusy_ && event.kind != glide::input::EventKind::Reset)
                    continue;
                switch (event.kind)
                {
                case glide::input::EventKind::Begin:
                    BeginDrag(event.anchor);
                    break;
                case glide::input::EventKind::TouchpadBegin:
                    BeginDrag(event.anchor, true);
                    break;
                case glide::input::EventKind::End:
                    EndDrag(event.cancel);
                    break;
                case glide::input::EventKind::TouchpadEnd:
                    EndDrag(event.cancel);
                    break;
                case glide::input::EventKind::Reset:
                    Reset();
                    break;
                case glide::input::EventKind::Step:
                    CloseWheelIntent(true);
                    if (EnsurePointerView())
                        SetTarget(glide::animation::StepZoom(target_, event.amount, settings_.maxZoom, settings_.zoomMultiplier), false);
                    break;
                case glide::input::EventKind::Delta:
                    DragPixels(event.amount, event.travel);
                    break;
                case glide::input::EventKind::TouchpadDelta:
                    DragPixels(event.amount, event.travel);
                    break;
                default:
                    break;
                }
            }
            if (!ServiceIndependentViews())
            {
                if (lifecycle_.Ready())
                    SessionFailure(glide::session::Result::WriteFailure, L"A monitor view or its input handoff could not be verified.");
                return;
            }
            if (routingBusy_ || !lifecycle_.Ready())
                return;
            bool cursorUpdated = false;
            if (lifecycle_.Ready() && (glide::animation::Magnified(transform_.zoom) ||
                                       glide::animation::Magnified(target_) ||
                                       session_.NeedsRecovery() || monitor_->Active()))
            {
                const auto bounds = ZoomBounds();
                const bool zoomChanging = precise_.zoom != target_;
                // Present the current selected step once. There is no app-owned
                // interpolation, queued transition, or settling tail.
                const double zoom = target_;
                auto next = precise_;
                next.zoom = zoom;
                if (zoomChanging && !zoomAnchor_.At(zoom, bounds, next))
                {
                    Suspend();
                    return;
                }
                glide::Vec2 source{};
                if (!CursorSource(source))
                {
                    Suspend();
                    return;
                }
                glide::Vec2 center{ (bounds.left + bounds.right) / 2.0,
                                    (bounds.top + bounds.bottom) / 2.0 };
                if (!MonitorMode() && settings_.centerFollow && !drag_.active)
                {
                    const auto screen = glide::SourceToScreen(source, next);
                    POINT point{ static_cast<LONG>(std::lround(std::clamp(screen.x, desktop_.left, desktop_.right - 1))),
                                 static_cast<LONG>(std::lround(std::clamp(screen.y, desktop_.top, desktop_.bottom - 1))) };
                    MONITORINFO info{};
                    info.cbSize = sizeof(info);
                    if (!GetMonitorInfoW(MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST), &info))
                    {
                        Suspend();
                        return;
                    }
                    center = { (static_cast<double>(info.rcMonitor.left) + info.rcMonitor.right) / 2.0,
                               (static_cast<double>(info.rcMonitor.top) + info.rcMonitor.bottom) / 2.0 };
                }
                // Always observe source coordinates, including held zoom changes.
                // Disabled frames rebase; ending a hold cannot replay old movement.
                // A timer tick or changing projection is never a pointer delta.
                const auto mode = settings_.centerFollow ? glide::viewport::FollowMode::Centered : glide::viewport::FollowMode::Edges;
                if (!followingState_.Advance(next, source, bounds, !drag_.active, mode, center, next))
                {
                    Suspend();
                    return;
                }
                precise_ = next;
                if (MonitorMode() || glide::animation::RestoreIdentity(target_, next.zoom) ||
                    glide::animation::NativeScaleChanged(next.zoom, transform_.zoom) ||
                    std::lround(next.offset.x) != std::lround(transform_.offset.x) ||
                    std::lround(next.offset.y) != std::lround(transform_.offset.y))
                {
                    if (!Apply(next))
                        return;
                    cursorUpdated = true; // Apply/restoration already checked the cursor lease.
                }
            }
            if (!cursorUpdated && !UpdateCursor())
            {
                CursorFailure();
                return;
            }
            if (now - lastUi_ > 250)
            {
                UpdateStatus();
                lastUi_ = now;
            }
        }

        HWND Control(const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int width, int height)
        {
            HWND control = CreateWindowExW(type == std::wstring(L"COMBOBOX") ? WS_EX_CLIENTEDGE : 0,
                                           type,
                                           text,
                                           WS_CHILD | WS_VISIBLE | style,
                                           x,
                                           y,
                                           width,
                                           height,
                                           hwnd_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                           instance_,
                                           nullptr);
            if (!control)
                throw std::runtime_error("Could not create a settings control");
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            return control;
        }

        void CreateControls()
        {
            statusText_.Invalidate();
            touchpadText_.Invalidate();
            const UINT dpi = GetDpiForWindow(hwnd_);
            const double scale = dpi / 96.0;
            auto px = [&](int x) { return static_cast<int>(std::lround(x * scale)); };
            font_ = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            titleFont_ = CreateFontW(-MulDiv(22, dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            auto create = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
                return Control(type, text, style, id, px(x), px(y), px(w), px(h));
            };
            HWND title = create(L"STATIC", L"GlideZoom", 0, 0, 28, 16, 580, 43);
            SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont_), TRUE);
            create(L"STATIC", L"Keep each monitor's zoom, use one view, or link all monitors.", 0, 0, 30, 65, 600, 30);
            status_ = create(L"STATIC", L"Starting  |  1.00x", 0, 0, 30, 102, 600, 25);
            create(L"STATIC",
                   L"Left Win + Left Alt + = / -       Tap or hold (also + and numpad)\n"
                   L"Chosen modifier + wheel          Uses zoom multiplier below\n"
                   L"Ctrl + Alt + Space (hold)         Move up / down to zoom\n"
                   L"Ctrl + Alt + Z / + Shift + Z      Current monitor / all together\n"
                   L"Ctrl + Alt + I / V                 Independent monitors / view indicator\n"
                   L"Ctrl + Alt + Esc / S              Reset all / copy view (brief 1x flash)",
                   0,
                   0,
                   30,
                   132,
                   610,
                   106);
            create(L"STATIC", L"Wheel modifier", 0, 0, 30, 244, 170, 23);
            modifier_ = create(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, kModifier, 230, 240, 370, 190);
            for (const auto* label : { L"Left Ctrl + Left Alt (default)", L"Ctrl (macOS-style)", L"Left Alt", L"Windows key" })
                SendMessageW(modifier_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(modifier_, CB_SETCURSEL, settings_.modifier, 0);
            create(L"STATIC", L"Maximum zoom", 0, 0, 30, 286, 170, 23);
            maximum_ = create(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, kMaximum, 230, 282, 170, 160);
            int selected = 0, i = 0;
            for (int value : { 2, 4, 8, 16 })
            {
                const auto label = std::to_wstring(value) + L"x";
                SendMessageW(maximum_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
                if (settings_.maxZoom == value)
                    selected = i;
                ++i;
            }
            SendMessageW(maximum_, CB_SETCURSEL, selected, 0);
            create(L"STATIC", L"Gesture sensitivity", 0, 0, 30, 328, 170, 23);
            sensitivity_ = create(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, kSensitivity, 230, 324, 170, 160);
            for (const auto* label : { L"Gentle", L"Balanced", L"Fast" })
                SendMessageW(sensitivity_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(sensitivity_, CB_SETCURSEL, settings_.sensitivity, 0);
            create(L"STATIC", L"Default zoom target", 0, 0, 410, 286, 200, 23);
            zoomScope_ = create(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, kZoomScope, 410, 324, 200, 160);
            for (const auto* label : { L"Selected monitor (one view)", L"All monitors together", L"Independent monitors" })
                SendMessageW(zoomScope_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(zoomScope_, CB_SETCURSEL, settings_.zoomScope, 0);
            create(L"STATIC", L"Key / wheel multiplier", 0, 0, 30, 370, 165, 23);
            const auto multiplierText = std::to_wstring(settings_.zoomMultiplier);
            zoomMultiplier_ = create(L"EDIT", multiplierText.c_str(), WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, kZoomMultiplier, 198, 366, 54, 27);
            // Keep one overflow character visible: otherwise pasting 1000 could
            // silently become the valid 100 before whole-field validation.
            SendMessageW(zoomMultiplier_, EM_LIMITTEXT, 4, 0);
            create(L"STATIC", L"1-100", 0, 0, 260, 370, 60, 23);
            create(L"STATIC", L"Edge pan multiplier", 0, 0, 335, 370, 160, 23);
            const auto edgeText = std::to_wstring(settings_.edgePanMultiplier);
            edgePanMultiplier_ = create(L"EDIT", edgeText.c_str(), WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, kEdgePanMultiplier, 500, 366, 54, 27);
            // The parser rejects the third digit instead of accepting a truncated
            // valid prefix (for example, 100 must not silently become 10).
            SendMessageW(edgePanMultiplier_, EM_LIMITTEXT, 3, 0);
            create(L"STATIC", L"1-10", 0, 0, 562, 370, 60, 23);
            create(L"STATIC", L"Zoom step: 0.01x times multiplier. Edge pan: 1 = original, 3 = faster.", 0, 0, 30, 394, 600, 17);
            keyboardGesture_ = create(L"BUTTON", L"Double-tap Left Ctrl and hold (optional)", BS_AUTOCHECKBOX | WS_TABSTOP, kKeyboardGesture, 30, 412, 590, 26);
            mouseGesture_ = create(L"BUTTON", L"Ctrl+Alt + left double-click and hold (reserves matching clicks)", BS_AUTOCHECKBOX | WS_TABSTOP, kMouseGesture, 30, 440, 590, 26);
            touchpadGesture_ = create(L"BUTTON", L"Three-finger touchpad double-tap and hold (optional)", BS_AUTOCHECKBOX | WS_TABSTOP, kTouchpadGesture, 30, 468, 590, 26);
            reverse_ = create(L"BUTTON", L"Reverse wheel and drag direction", BS_AUTOCHECKBOX | WS_TABSTOP, kReverse, 30, 496, 590, 26);
            following_ = create(L"BUTTON", L"Keep pointer centered (disables edge pan multiplier)", BS_AUTOCHECKBOX | WS_TABSTOP, kFollowing, 30, 524, 590, 26);
            pointerScaling_ = create(L"BUTTON", L"Adjust pointer speed with zoom (required for edge pan multiplier)", BS_AUTOCHECKBOX | WS_TABSTOP, kPointerScaling, 30, 552, 600, 26);
            create(L"STATIC", L"Pointer speed (%)", 0, 0, 30, 584, 160, 23);
            pointerSpeed_ = create(L"EDIT", std::to_wstring(settings_.pointerSpeed).c_str(), WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, kPointerSpeed, 198, 580, 54, 27);
            SendMessageW(pointerSpeed_, EM_LIMITTEXT, 4, 0);
            create(L"STATIC", L"25-300; 100 = normal. Applies only while zoomed.", 0, 0, 260, 584, 365, 23);
            SendMessageW(keyboardGesture_, BM_SETCHECK, settings_.keyboardGesture ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(mouseGesture_, BM_SETCHECK, settings_.mouseGesture ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(touchpadGesture_, BM_SETCHECK, settings_.touchpadGesture ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(reverse_, BM_SETCHECK, settings_.reverse ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(following_, BM_SETCHECK, settings_.centerFollow ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(pointerScaling_, BM_SETCHECK, settings_.pointerScaling ? BST_CHECKED : BST_UNCHECKED, 0);
            touchpadStatus_ = create(L"STATIC", L"Three-finger gesture: disabled", 0, 0, 30, 610, 600, 23);
            create(L"STATIC", runnerPid_ ? L"Change these options in PowerToys Settings.\n"
                                           L"Touchpad: set Windows three-finger Taps and Swipes to Nothing." :
                                           L"Touchpad: tap, lift, tap-and-hold with three fingers; move up/down.\n"
                                           L"0.01x steps. Set Windows three-finger Taps and Swipes to Nothing.",
                   0,
                   0,
                   30,
                   636,
                   610,
                   36);
            create(L"BUTTON", runnerPid_ ? L"Open settings" : L"Save settings", BS_DEFPUSHBUTTON | WS_TABSTOP, runnerPid_ ? kOpenSettings : kSave, 30, 676, 125, 32);
            create(L"BUTTON", L"Reset zoom", BS_PUSHBUTTON | WS_TABSTOP, kResetCommand, 165, 676, 110, 32);
            create(L"BUTTON", L"Copy zoomed view", BS_PUSHBUTTON | WS_TABSTOP, kCaptureCommand, 285, 676, 155, 32);
            create(L"BUTTON", L"Minimize to tray", BS_PUSHBUTTON | WS_TABSTOP, kHide, 450, 676, 150, 32);
            if (runnerPid_)
            {
                // PowerToys Settings owns edits. The engine displays its current
                // configuration without becoming a second unsynchronized editor.
                for (const HWND control : { modifier_, maximum_, sensitivity_, zoomScope_, zoomMultiplier_, edgePanMultiplier_, keyboardGesture_, mouseGesture_, touchpadGesture_, reverse_, following_, pointerScaling_, pointerSpeed_ })
                {
                    EnableWindow(control, FALSE);
                }
            }
            RECT area{ 0, 0, px(650), px(724) };
            AdjustWindowRectExForDpi(&area, static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE)), FALSE, 0, dpi);
            SetWindowPos(hwnd_, nullptr, 0, 0, area.right - area.left, area.bottom - area.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        void SaveControls()
        {
            if (runnerPid_)
                return;
            if (lifecycle_.Current() != Phase::Ready && lifecycle_.Current() != Phase::Paused)
                return;
            auto candidate = settings_;
            const auto modifier = SendMessageW(modifier_, CB_GETCURSEL, 0, 0);
            const auto maximum = SendMessageW(maximum_, CB_GETCURSEL, 0, 0);
            const auto sensitivity = SendMessageW(sensitivity_, CB_GETCURSEL, 0, 0);
            const auto zoomScope = SendMessageW(zoomScope_, CB_GETCURSEL, 0, 0);
            if (modifier < 0 || modifier > 3 || maximum < 0 || maximum > 3 || sensitivity < 0 || sensitivity > 2 || zoomScope < 0 || zoomScope > 2)
                return;
            wchar_t multiplierText[5]{};
            const int characters = GetWindowTextW(zoomMultiplier_, multiplierText, 5);
            int multiplier = settings_.zoomMultiplier;
            if (characters <= 0 || !glide::settings::ParseZoomMultiplier(
                                       { multiplierText, static_cast<std::size_t>(characters) }, multiplier))
            {
                QueueError(L"Enter a whole zoom multiplier from 1 to 100. For example, 1 changes zoom by "
                           L"0.01x per key/wheel step; 10 changes it by 0.10x. Gesture sensitivity is separate.");
                SetFocus(zoomMultiplier_);
                return;
            }
            wchar_t edgeText[4]{};
            const int edgeCharacters = GetWindowTextW(edgePanMultiplier_, edgeText, 4);
            int edgeMultiplier = settings_.edgePanMultiplier;
            if (edgeCharacters <= 0 || !glide::settings::ParseEdgePanMultiplier(
                                           { edgeText, static_cast<std::size_t>(edgeCharacters) }, edgeMultiplier))
            {
                QueueError(L"Enter a whole edge pan multiplier from 1 to 10. 1 preserves the original speed; "
                           L"3 moves the view faster at the five-pixel edge. Enable Adjust pointer speed with zoom "
                           L"and leave Keep pointer centered unchecked.");
                SetFocus(edgePanMultiplier_);
                return;
            }
            wchar_t pointerText[5]{};
            const int pointerCharacters = GetWindowTextW(pointerSpeed_, pointerText, 5);
            int pointerSpeed = settings_.pointerSpeed;
            if (pointerCharacters <= 0 || !glide::settings::ParsePointerSpeed(
                                              { pointerText, static_cast<std::size_t>(pointerCharacters) }, pointerSpeed))
            {
                QueueError(L"Enter a whole pointer speed from 25 to 300 percent. 100 preserves normal speed; "
                           L"automatic zoom adjustment can be enabled separately.");
                SetFocus(pointerSpeed_);
                return;
            }
            candidate.modifier = static_cast<int>(modifier);
            candidate.maxZoom = std::array<int, 4>{ 2, 4, 8, 16 }[static_cast<std::size_t>(maximum)];
            candidate.sensitivity = static_cast<int>(sensitivity);
            candidate.zoomScope = static_cast<int>(zoomScope);
            candidate.zoomMultiplier = multiplier;
            candidate.edgePanMultiplier = edgeMultiplier;
            candidate.pointerSpeed = pointerSpeed;
            candidate.keyboardGesture = SendMessageW(keyboardGesture_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            candidate.mouseGesture = SendMessageW(mouseGesture_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            candidate.touchpadGesture = SendMessageW(touchpadGesture_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            candidate.reverse = SendMessageW(reverse_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            candidate.centerFollow = SendMessageW(following_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            candidate.pointerScaling = SendMessageW(pointerScaling_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            const bool resume = lifecycle_.Current() == Phase::Ready;
            lifecycle_.Pause();
            ReleaseHotkeys();
            if (!Reset())
                return;
            // No new input is reserved and no zoom is owned during synchronous disk IO.
            const auto result = settingsStore_.Save(candidate);
            if (result == glide::settings::SaveResult::Saved)
            {
                scopeReloadPending_ = scopeReloadPending_ || settings_.zoomScope != candidate.zoomScope;
                settings_ = candidate;
            }
            if (resume)
                EnableControls();
            else
            {
                ApplyPreferredScope();
                ConfigureInput();
            }
            UpdateStatus();
            if (result == glide::settings::SaveResult::ChangedOutside)
                MessageBoxW(hwnd_, L"The settings file changed outside GlideZoom. Your changes were not applied or saved. "
                                   L"Restart GlideZoom to load that file before editing again.",
                            L"GlideZoom",
                            MB_OK | MB_ICONWARNING);
            else if (result != glide::settings::SaveResult::Saved)
                MessageBoxW(hwnd_, L"The complete settings update could not be saved. The previous active configuration "
                                   L"is still in use. Check your profile folder and try again.",
                            L"GlideZoom",
                            MB_OK | MB_ICONWARNING);
        }

        void OpenPowerToysSettings()
        {
            if (!runnerPid_)
                return;
            std::wstring executable(32768, L'\0');
            const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
            if (!length || length >= executable.size())
            {
                QueueError(L"PowerToys Settings could not be located. Open it from the PowerToys tray icon.");
                return;
            }
            executable.resize(length);
            const auto runner = std::filesystem::path(executable).parent_path() / L"PowerToys.exe";
            SHELLEXECUTEINFOW info{};
            info.cbSize = sizeof(info);
            info.fMask = SEE_MASK_FLAG_NO_UI;
            info.hwnd = hwnd_;
            info.lpFile = runner.c_str();
            info.lpParameters = L"--open-settings=GlideZoom";
            info.nShow = SW_SHOWNORMAL;
            NativeOperation launch{ nativeOperation_ };
            if (!ShellExecuteExW(&info))
            {
                QueueError(L"PowerToys Settings could not be opened. Open it from the PowerToys tray icon.");
            }
        }

        bool ReloadSettings()
        {
            lifecycle_.Pause();
            ReleaseHotkeys();
            // Release every owned transform before reading a replacement configuration.
            if (!Reset())
                return false;
            glide::settings::Values candidate;
            const auto loaded = settingsStore_.Load(candidate);
            if (loaded == glide::settings::LoadResult::Loaded)
            {
                scopeReloadPending_ = scopeReloadPending_ || settings_.zoomScope != candidate.zoomScope;
                settings_ = candidate;
                SendMessageW(modifier_, CB_SETCURSEL, settings_.modifier, 0);
                const auto maxima = std::array<int, 4>{ 2, 4, 8, 16 };
                const auto maximum = std::find(maxima.begin(), maxima.end(), settings_.maxZoom) - maxima.begin();
                SendMessageW(maximum_, CB_SETCURSEL, static_cast<WPARAM>(maximum), 0);
                SendMessageW(sensitivity_, CB_SETCURSEL, settings_.sensitivity, 0);
                SendMessageW(zoomScope_, CB_SETCURSEL, settings_.zoomScope, 0);
                SetWindowTextW(zoomMultiplier_, std::to_wstring(settings_.zoomMultiplier).c_str());
                SetWindowTextW(edgePanMultiplier_, std::to_wstring(settings_.edgePanMultiplier).c_str());
                SetWindowTextW(pointerSpeed_, std::to_wstring(settings_.pointerSpeed).c_str());
                SendMessageW(keyboardGesture_, BM_SETCHECK, settings_.keyboardGesture ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(mouseGesture_, BM_SETCHECK, settings_.mouseGesture ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(touchpadGesture_, BM_SETCHECK, settings_.touchpadGesture ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(reverse_, BM_SETCHECK, settings_.reverse ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(following_, BM_SETCHECK, settings_.centerFollow ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(pointerScaling_, BM_SETCHECK, settings_.pointerScaling ? BST_CHECKED : BST_UNCHECKED, 0);
            }
            else
            {
                QueueError(L"PowerToys settings could not be read as a complete valid configuration. The previous configuration remains active.");
            }
            if (settingsReloadResume_)
                EnableControls();
            else
            {
                if (!ApplyPreferredScope())
                    return false;
                ConfigureInput();
            }
            UpdateStatus();
            return true;
        }

        NOTIFYICONDATAW TrayIcon() const noexcept
        {
            NOTIFYICONDATAW icon{};
            icon.cbSize = sizeof(icon);
            icon.hWnd = hwnd_;
            icon.uID = glide::tray::IconId;
            return icon;
        }

        bool AddTray() noexcept
        {
            trayTip_.Invalidate();
            auto icon = TrayIcon();
            icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
            icon.uCallbackMessage = kTray;
            icon.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
            if (!icon.hIcon)
                icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            wcscpy_s(icon.szTip, L"GlideZoom - left-click to open, right-click for controls");
            struct Shell
            {
                NOTIFYICONDATAW& icon;
                bool Add() noexcept { return Shell_NotifyIconW(NIM_ADD, &icon) != FALSE; }
                bool Refresh() noexcept { return Shell_NotifyIconW(NIM_MODIFY, &icon) != FALSE; }
                bool Version4() noexcept
                {
                    icon.uVersion = NOTIFYICON_VERSION_4;
                    return Shell_NotifyIconW(NIM_SETVERSION, &icon) != FALSE;
                }
            } shell{ icon };
            return glide::tray::Register(tray_, shell);
        }

        bool EnsureTray() noexcept
        {
            if (tray_.Stopping())
                return false;
            // This explicit presence probe may replace the tooltip outside the
            // normal status path. Do not retain a cache of its previous contents.
            trayTip_.Invalidate();
            if (tray_.Registered())
            {
                auto icon = TrayIcon();
                icon.uFlags = NIF_TIP | NIF_SHOWTIP;
                wcscpy_s(icon.szTip, L"GlideZoom - left-click to open, right-click for controls");
                if (Shell_NotifyIconW(NIM_MODIFY, &icon))
                    return true;
                tray_.Lost();
            }
            return AddTray();
        }

        void TrayUnavailable()
        {
            Show();
            QueueError(L"Windows could not restore GlideZoom's tray icon. The window remains available. "
                       L"Try Minimize again after Explorer recovers; closing the window exits if the tray is still unavailable.");
        }

        void HideToTray(bool closeIfUnavailable = false)
        {
            if (tray_.Stopping())
                return;
            const bool available = EnsureTray();
            if (!available && closeIfUnavailable)
            {
                DestroyWindow(hwnd_);
                return;
            }
            if (tray_.RequestVisibility(true) == glide::tray::Visibility::Hide)
            {
                ShowWindow(hwnd_, SW_HIDE);
            }
            else
                TrayUnavailable();
        }

        void UpdateStatus()
        {
            if (!hwnd_ || cleaned_)
                return;
            const wchar_t* phase = lifecycle_.Current() == Phase::Faulted   ? L"Stopping after error" :
                                   lifecycle_.Current() == Phase::Stopping  ? L"Stopping" :
                                   lifecycle_.NeedsRestore()                ? L"Waiting to restore zoom" :
                                   lifecycle_.Current() == Phase::Suspended ? L"Suspended" :
                                   !lifecycle_.Ready()                      ? L"Paused" :
                                   drag_.active                             ? L"Drag to zoom" :
                                                                              L"Ready";
            if (lifecycle_.Ready() && lastCopy_ && GetTickCount64() - lastCopy_ < 4000)
                phase = L"View copied to clipboard";
            wchar_t text[180]{};
            const wchar_t* scope = IndependentMode() ? L"Independent monitors" : MonitorMode() ? L"Selected monitor (one view)" :
                                                                                                 L"All monitors together";
            swprintf_s(text, L"%ls  |  %.2fx  |  %ls%ls", phase, transform_.zoom, scope, PointerAdjustmentRequested() && input_.PointerFaulted() ? L"  |  Pointer adjustment OFF: restart GlideZoom" : L"");
            statusText_.Update(reinterpret_cast<std::uintptr_t>(status_), text, [&]() { return SetWindowTextW(status_, text) != FALSE; });
            const auto touchpad = input_.TouchpadStatus();
            const wchar_t* touchpadText = !settings_.touchpadGesture                                  ? L"Three-finger gesture: disabled" :
                                          touchpad == glide::input::TouchpadAvailability::Detected    ? L"Three-finger gesture: contact reports detected" :
                                          touchpad == glide::input::TouchpadAvailability::Unsupported ? L"Three-finger gesture: no supported raw contact format" :
                                                                                                        L"Three-finger gesture: waiting for compatible contact reports";
            touchpadText_.Update(reinterpret_cast<std::uintptr_t>(touchpadStatus_), touchpadText, [&]() { return SetWindowTextW(touchpadStatus_, touchpadText) != FALSE; });
            if (tray_.Registered())
            {
                auto icon = TrayIcon();
                icon.uFlags = NIF_TIP | NIF_SHOWTIP;
                // Zoom feedback belongs in the app's status label. Keeping the
                // active tray tooltip stable avoids poking Explorer during zoom.
                swprintf_s(icon.szTip, L"GlideZoom - %ls", lifecycle_.Ready() ? L"Ctrl+Alt+Esc to reset" : phase);
                bool shellFailed = false;
                trayTip_.Update(reinterpret_cast<std::uintptr_t>(hwnd_), icon.szTip, [&]() {
                    const bool success = Shell_NotifyIconW(NIM_MODIFY, &icon) != FALSE;
                    shellFailed = !success;
                    return success;
                });
                // A reentrant cache invalidation is not evidence of a missing
                // icon. Re-register only after an actual failed Shell operation.
                if (shellFailed)
                {
                    tray_.Lost();
                    trayTip_.Invalidate();
                    if (!AddTray())
                        TrayUnavailable();
                }
            }
        }

        void Show()
        {
            if (tray_.RequestVisibility(false) != glide::tray::Visibility::Show)
                return;
            // SW_RESTORE also displays a hidden normal window. Showing an iconic
            // window first can re-enter SIZE_MINIMIZED and hide it again.
            ShowWindow(hwnd_, SW_RESTORE);
            SetForegroundWindow(hwnd_);
        }

        void TrayMenu()
        {
            if (!tray_.BeginMenu())
                return;
            HMENU menu = CreatePopupMenu();
            if (!menu)
            {
                tray_.EndMenu();
                return;
            }
            AppendMenuW(menu, MF_STRING, kShow, L"Open GlideZoom");
            SetMenuDefaultItem(menu, kShow, FALSE);
            AppendMenuW(menu, MF_STRING | (IsWindowVisible(hwnd_) ? 0 : MF_GRAYED), kHide, L"Minimize to tray");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kToggle, L"Toggle zoom on current monitor\tCtrl+Alt+Z");
            AppendMenuW(menu, MF_STRING, kDesktopToggle, L"All-monitor zoom\tCtrl+Alt+Shift+Z");
            AppendMenuW(menu, MF_STRING, kIndependentToggle, L"Independent-monitor zoom\tCtrl+Alt+I");
            AppendMenuW(menu, MF_STRING, kResetCommand, L"Reset all zoom to 1x\tCtrl+Alt+Esc");
            AppendMenuW(menu, MF_STRING | (lifecycle_.Ready() ? 0 : MF_GRAYED), kCaptureCommand, L"Copy zoomed view\tCtrl+Alt+S");
            AppendMenuW(menu, MF_STRING | (lifecycle_.Current() == Phase::Ready ? 0 : MF_CHECKED), kPause, L"Pause input controls");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kExit, L"Exit GlideZoom");
            POINT point{};
            RECT bounds{};
            NOTIFYICONIDENTIFIER identity{};
            identity.cbSize = sizeof(identity);
            identity.hWnd = hwnd_;
            identity.uID = glide::tray::IconId;
            // Anchor to the tray icon, including keyboard activation and negative
            // monitor origins. WM_CONTEXTMENU's v4 wParam is not defined as a point.
            if (SUCCEEDED(Shell_NotifyIconGetRect(&identity, &bounds)))
            {
                point = { bounds.left, bounds.top };
            }
            else if (!GetCursorPos(&point))
            {
                DestroyMenu(menu);
                tray_.EndMenu();
                return;
            }
            SetForegroundWindow(hwnd_);
            const UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, hwnd_, nullptr);
            DestroyMenu(menu);
            tray_.EndMenu();
            if (tray_.Stopping())
                return;
            if (command)
                PostMessageW(hwnd_, WM_COMMAND, command, 0);
            else
            {
                auto icon = TrayIcon();
                Shell_NotifyIconW(NIM_SETFOCUS, &icon);
            }
            PostMessageW(hwnd_, WM_NULL, 0, 0);
        }

        LRESULT WindowMessage(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
        {
            if (cleaned_)
            {
                // Shutdown may dispatch sent messages from native destruction or
                // MagUninitialize. No callback may restart a magnification call.
                if (message == WM_DESTROY)
                {
                    PostQuitMessage(0);
                    return 0;
                }
                return DefWindowProcW(hwnd, message, wParam, lParam);
            }
            if (messageDuringOperation_)
            {
                if (message == kTray || message == kError)
                    return 0; // No nested popup/modal loop.
                const bool command = message == WM_COMMAND;
                const int id = command ? LOWORD(wParam) : 0;
                if (message == kQuit || (command && id == kExit) || message == WM_DESTROY ||
                    (message == WM_ENDSESSION && wParam))
                {
                    deferredQuit_ = captureUserCancelled_ = true;
                    lifecycle_.Stop();
                    ReleaseHotkeys();
                    Reset();
                    return 0;
                }
                if (message == kReset || (message == WM_HOTKEY && wParam == kResetHotkey) ||
                    (command && id == kResetCommand))
                {
                    captureUserCancelled_ = true;
                    Reset();
                    return 0;
                }
                if (message == WM_HOTKEY && wParam != kResetHotkey)
                    return 0;
                if (command)
                {
                    if (id == kPause)
                    {
                        captureUserCancelled_ = true;
                        Pause();
                    }
                    return 0; // Never start zoom, resume, save, or another capture.
                }
                if (message == WM_CLOSE || message == WM_SYSCOMMAND)
                    return 0;
            }
            if (taskbarCreated_ && message == taskbarCreated_)
            {
                tray_.Lost();
                trayTip_.Invalidate();
                if (!tray_.Stopping() && !AddTray())
                    TrayUnavailable();
                return 0;
            }
            switch (message)
            {
            case kRoutingWake:
                if (!messageDuringOperation_)
                    ServiceFrame();
                return 0;
            case WM_TIMER:
                if (wParam == kTimer)
                    ServiceFrame();
                return 0;
            case WM_HOTKEY:
                if (wParam < hotkeys_.size() && hotkeys_[wParam])
                {
                    if (wParam == kResetHotkey)
                        Reset();
                    else if (lifecycle_.Ready())
                    {
                        if (wParam == kToggleHotkey)
                            ToggleScope(glide::view::Scope::Monitor);
                        else if (wParam == kDesktopHotkey)
                            ToggleScope(glide::view::Scope::Desktop);
                        else if (wParam == kIndependentHotkey)
                            ToggleScope(glide::view::Scope::Independent);
                        else if (wParam == kPlusHotkey || wParam == kShiftPlusHotkey || wParam == kNumpadPlusHotkey)
                            StepZoom(120);
                        else if (wParam == kMinusHotkey || wParam == kNumpadMinusHotkey)
                            StepZoom(-120);
                        else if (wParam == kCaptureHotkey)
                            CopyZoomedView();
                        else if (wParam == kScopePreviewHotkey)
                            RevealScopeIndicator();
                    }
                }
                return 0;
            case kReset:
                Reset();
                return 0;
            case kQuit:
                DestroyWindow(hwnd);
                return 0;
            case kError:
            {
                std::wstring error;
                pendingError_.swap(error);
                if (!error.empty())
                    MessageBoxW(hwnd_, error.c_str(), L"GlideZoom", MB_OK | MB_ICONWARNING);
                return 0;
            }
            case kTray:
                switch (tray_.Callback(static_cast<std::uintptr_t>(wParam), static_cast<std::uintptr_t>(lParam)))
                {
                case glide::tray::Action::Restore:
                    Show();
                    break;
                case glide::tray::Action::Menu:
                    TrayMenu();
                    break;
                case glide::tray::Action::None:
                    break;
                }
                return 0;
            case WM_COMMAND:
                switch (LOWORD(wParam))
                {
                case kOpenSettings:
                    OpenPowerToysSettings();
                    break;
                case kSave:
                    SaveControls();
                    break;
                case kShow:
                    Show();
                    break;
                case kToggle:
                    ToggleScope(glide::view::Scope::Monitor);
                    break;
                case kDesktopToggle:
                    ToggleScope(glide::view::Scope::Desktop);
                    break;
                case kIndependentToggle:
                    ToggleScope(glide::view::Scope::Independent);
                    break;
                case kResetCommand:
                    Reset();
                    break;
                case kCaptureCommand:
                    CopyZoomedView();
                    break;
                case kPause:
                    if (lifecycle_.Current() == Phase::Ready)
                        Pause();
                    else if (lifecycle_.Current() == Phase::Paused)
                        EnableControls();
                    UpdateStatus();
                    break;
                case kExit:
                    DestroyWindow(hwnd);
                    break;
                case kHide:
                    HideToTray();
                    break;
                }
                return 0;
            case WM_SYSCOMMAND:
                if ((wParam & 0xfff0u) == SC_MINIMIZE)
                {
                    HideToTray();
                    return 0;
                }
                break;
            case WM_SIZE:
                if (wParam == SIZE_MINIMIZED)
                {
                    HideToTray();
                    return 0;
                }
                break;
            case WM_CLOSE:
                HideToTray(true);
                return 0;
            case WM_DISPLAYCHANGE:
                // Latch the refresh requirement in Suspended even when restoration
                // is deferred. Tick retries it before controls can resume.
                Suspend();
                return 0;
            case WM_DPICHANGED:
            {
                Suspend();
                const auto* suggested = reinterpret_cast<const RECT*>(lParam);
                if (suggested)
                    SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                while (HWND child = GetWindow(hwnd, GW_CHILD))
                    DestroyWindow(child);
                if (font_)
                    DeleteObject(font_);
                if (titleFont_)
                    DeleteObject(titleFont_);
                font_ = titleFont_ = nullptr;
                CreateControls();
                return 0;
            }
            case WM_WTSSESSION_CHANGE:
                if (wParam == WTS_SESSION_LOCK || wParam == WTS_CONSOLE_DISCONNECT ||
                    wParam == WTS_REMOTE_DISCONNECT || wParam == WTS_SESSION_UNLOCK ||
                    wParam == WTS_CONSOLE_CONNECT || wParam == WTS_REMOTE_CONNECT)
                    Suspend();
                return 0;
            case WM_POWERBROADCAST:
                if (wParam == PBT_APMSUSPEND || wParam == PBT_APMRESUMEAUTOMATIC)
                    Suspend();
                return TRUE;
            case WM_QUERYENDSESSION:
                Suspend();
                return TRUE;
            case WM_ENDSESSION:
                if (wParam)
                    Cleanup();
                return 0;
            case WM_DESTROY:
                Cleanup();
                PostQuitMessage(0);
                return 0;
            }
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        void Cleanup() noexcept
        {
            if (cleaned_)
                return;
            if (captureRequested_ || frameInProgress_ || nativeOperation_ || messageDuringOperation_)
            {
                deferredQuit_ = true;
                lifecycle_.Stop();
                ReleaseHotkeys();
                Reset();
                return;
            }
            cleaned_ = true;
            tray_.Stop();
            if (tray_.MenuOpen())
                EndMenu();
            lifecycle_.Stop();
            ReleaseHotkeys();
            Reset();
            NativeOperation shutdown{ nativeOperation_ };
            lifecycle_.Stop();
            ConfigureInput();
            input_.Stop();
            if (guardianStarted_ && !session_.NeedsRecovery() && !cursor_.NeedsRecovery())
                guardian_.MarkClean();
            if (hwnd_)
            {
                KillTimer(hwnd_, kTimer);
                if (notificationsRegistered_)
                    WTSUnRegisterSessionNotification(hwnd_);
                if (tray_.Registered())
                {
                    auto icon = TrayIcon();
                    Shell_NotifyIconW(NIM_DELETE, &icon);
                    tray_.Lost();
                }
            }
            frameClock_.Stop();
            for (auto& item : parkedViews_)
                item.renderer->Destroy();
            parkedViews_.clear();
            monitor_->Destroy();
            if (magInitialized_)
                mag_.Uninitialize();
            magInitialized_ = false;
            if (font_)
                DeleteObject(font_);
            if (titleFont_)
                DeleteObject(titleFont_);
            font_ = titleFont_ = nullptr;
        }
    };
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments)
        return 1;
    const bool powerToysRequested = count > 1 && std::wcscmp(arguments[1], L"--powertoys") == 0;
    HANDLE singleton = nullptr;
    int result = 1;
    try
    {
        if (powerToysRequested)
        {
            try
            {
                glide::powertoys::Apartment loggingApartment;
                if (!loggingApartment.Ready())
                    throw std::runtime_error("Could not initialize logging apartment");
                LoggerHelpers::init_logger(L"GlideZoom", L"Engine", "GlideZoomEngine");
                engineLoggerReady = true;
                Diagnostic(L"GlideZoom engine starting.");
            }
            catch (...)
            {
                OutputDebugStringW(L"GlideZoom: could not initialize its diagnostic log.\n");
            }
        }
        if (count == 4 && std::wcscmp(arguments[1], L"--watch") == 0)
        {
            DWORD pid = 0;
            result = glide::powertoys::ParseProcessId(arguments[2], pid) ? glide::watchdog::Run(pid, arguments[3]) : 2;
        }
        else
        {
            HWND existing = FindWindowW(kClass, nullptr);
            DWORD runnerPid = 0;
            const bool powerToys = count == 3 && std::wcscmp(arguments[1], L"--powertoys") == 0 &&
                                   glide::powertoys::ParseProcessId(arguments[2], runnerPid);
            const bool quit = count == 2 && std::wcscmp(arguments[1], L"--quit") == 0;
            const bool reset = count == 2 && std::wcscmp(arguments[1], L"--reset") == 0;
            const bool quiet = powerToys || (count == 2 && std::wcscmp(arguments[1], L"--quiet") == 0);
            if (quit || reset)
            {
                if (existing)
                    PostMessageW(existing, quit ? kQuit : kReset, 0, 0);
                result = 0;
            }
            else if (count > 1 && !quiet)
            {
                ReportError(nullptr, L"Supported arguments: --quiet, --reset, --quit, --powertoys <runner PID>.", L"GlideZoom", MB_OK, powerToysRequested);
                result = 2;
            }
            else if (existing)
            {
                if (powerToys)
                {
                    ReportError(nullptr, L"GlideZoom is already running. Exit that instance before enabling GlideZoom in PowerToys.", L"PowerToys GlideZoom", MB_OK | MB_ICONINFORMATION, true);
                    result = 1;
                }
                else
                {
                    ShowWindow(existing, SW_RESTORE);
                    SetForegroundWindow(existing);
                    result = 0;
                }
            }
            else
            {
                singleton = CreateMutexW(nullptr, FALSE, glide::watchdog::kInstanceMutexName);
                if (!singleton || GetLastError() == ERROR_ALREADY_EXISTS)
                {
                    Diagnostic(L"Startup deferred: another engine or its recovery guardian owns the instance mutex.");
                    result = 0; // An older process or its recovery guardian still exists.
                }
                else if (!HasUiAccess())
                {
                    ReportError(nullptr,
                                L"GlideZoom needs Windows UIAccess so clicks match the magnified desktop.\n\n"
                                L"Use a signed PowerToys build installed in a trusted Windows location such as Program Files.\n\n"
                                L"Running the unsigned file as administrator does not replace installation.",
                                L"GlideZoom - installation required",
                                MB_OK | MB_ICONINFORMATION,
                                powerToysRequested);
                }
                else
                {
                    glide::powertoys::Apartment apartment;
                    if (!apartment.Ready())
                        throw std::runtime_error("Could not initialize the Windows Runtime");
                    App application(runnerPid);
                    app = &application;
                    result = application.Run(instance, quiet);
                    app = nullptr;
                }
            }
        }
    }
    catch (...)
    {
        app = nullptr;
        ReportError(nullptr, L"GlideZoom stopped after an unexpected error. Its recovery process will "
                             L"attempt to restore only a recognized GlideZoom transform. Restart from its installed folder in Explorer.",
                    L"GlideZoom",
                    MB_OK | MB_ICONERROR,
                    powerToysRequested);
    }
    if (singleton)
        CloseHandle(singleton);
    LocalFree(arguments);
    if (engineLoggerReady)
    {
        try
        {
            Logger::info("GlideZoom engine stopped with exit code {}", result);
            Logger::flush();
        }
        catch (...)
        {
        }
    }
    return result;
}
