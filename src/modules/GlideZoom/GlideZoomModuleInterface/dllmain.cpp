// Copyright (c) Microsoft Corporation
// Licensed under the MIT license. See LICENSE in the project root.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <filesystem>
#include <string>

#include <common/SettingsAPI/settings_objects.h>
#include <common/logger/logger.h>
#include <common/utils/logger_helper.h>
#include <common/utils/process_path.h>
#include <modules/interface/powertoy_module_interface.h>

#include "../GlideZoom/powertoys_settings.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{
    constexpr wchar_t ModuleKey[] = L"GlideZoom";

    bool ReadSettings(PowerToysSettings::PowerToyValues values, glide::settings::Values& output)
    {
        return glide::settings::ParsePowerToys(winrt::to_string(values.get_raw_json().Stringify()), output);
    }

    PowerToysSettings::PowerToyValues SerializeSettings(const glide::settings::Values& values)
    {
        return PowerToysSettings::PowerToyValues::from_json_string(
            winrt::to_hstring(glide::settings::SerializePowerToys(values)), ModuleKey);
    }
}

class GlideZoomModuleInterface final : public PowertoyModuleIface
{
public:
    GlideZoomModuleInterface()
    {
        LoggerHelpers::init_logger(ModuleKey, L"ModuleInterface", "GlideZoomModuleInterface");
        const auto suffix = std::to_wstring(GetCurrentProcessId());
        m_exitEvent = CreateDefaultEvent((L"Local\\PowerToys_GlideZoom_Exit_" + suffix).c_str());
        if (!m_exitEvent)
        {
            Logger::error("Could not create GlideZoom exit event: {}", GetLastError());
        }
        m_settingsEvent = CreateDefaultEvent((L"Local\\PowerToys_GlideZoom_Settings_" + suffix).c_str());
        if (!m_settingsEvent)
        {
            Logger::error("Could not create GlideZoom settings event: {}", GetLastError());
        }
        try
        {
            const auto values = PowerToysSettings::PowerToyValues::load_from_settings_file(ModuleKey);
            if (!ReadSettings(values, m_settings))
            {
                Logger::warn("GlideZoom settings were invalid; using safe defaults");
            }
        }
        catch (...)
        {
            Logger::info("GlideZoom settings were unavailable; using safe defaults");
        }
    }

    ~GlideZoomModuleInterface()
    {
        disable();
        CloseProcessHandle();
        if (m_settingsEvent)
        {
            CloseHandle(m_settingsEvent);
        }
        if (m_exitEvent)
        {
            CloseHandle(m_exitEvent);
        }
    }

    PCWSTR get_name() override { return ModuleKey; }
    const wchar_t* get_key() override { return ModuleKey; }
    bool is_enabled_by_default() const override { return false; }

    powertoys_gpo::gpo_rule_configured_t gpo_policy_enabled_configuration() override
    {
        // Honor the existing policy for all utilities; no new policy is introduced.
        return powertoys_gpo::getConfiguredValue(powertoys_gpo::POLICY_CONFIGURE_ENABLED_GLOBAL_ALL_UTILITIES);
    }

    bool get_config(wchar_t* buffer, int* bufferSize) override
    {
        PowerToysSettings::Settings settings(reinterpret_cast<HINSTANCE>(&__ImageBase), ModuleKey);
        settings.set_description(L"Interactive desktop magnification with keyboard, mouse and touchpad controls.");
        settings.add_int_spinner(L"WheelModifier", L"Wheel modifier", m_settings.modifier, 0, 3, 1);
        settings.add_int_spinner(L"MaximumZoom", L"Maximum zoom", m_settings.maxZoom, 2, 16, 2);
        settings.add_int_spinner(L"Sensitivity", L"Gesture sensitivity", m_settings.sensitivity, 0, 2, 1);
        settings.add_int_spinner(L"ZoomMultiplier", L"Key and wheel multiplier", m_settings.zoomMultiplier, 1, 100, 1);
        settings.add_int_spinner(L"EdgePanMultiplier", L"Edge pan multiplier", m_settings.edgePanMultiplier, 1, 10, 1);
        settings.add_int_spinner(L"ZoomScope", L"Zoom target", m_settings.zoomScope, 0, 2, 1);
        settings.add_int_spinner(L"ScopeIndicatorMode", L"View indicator visibility", m_settings.scopeIndicatorMode, 0, 3, 1);
        settings.add_int_spinner(L"ScopeIndicatorStyle", L"View indicator style", m_settings.scopeIndicatorStyle, 0, 2, 1);
        settings.add_int_spinner(L"PointerSpeed", L"Pointer speed (%)", m_settings.pointerSpeed, 25, 300, 1);
        settings.add_int_spinner(L"OverviewOpacity", L"Overview opacity (%)", m_settings.overviewOpacity, 0, 100, 1);
        settings.add_int_spinner(L"ScopeIndicatorSize", L"Overview width percent", m_settings.scopeIndicatorSize, 10, 35, 1);
        settings.add_int_spinner(L"ScopeIndicatorOpacity", L"View indicator opacity percent", m_settings.scopeIndicatorOpacity, 10, 90, 1);
        settings.add_int_spinner(L"ScopeIndicatorBorder", L"View indicator border width", m_settings.scopeIndicatorBorder, 1, 8, 1);
        settings.add_int_spinner(L"ScopeIndicatorDuration", L"View indicator duration milliseconds", m_settings.scopeIndicatorDuration, 500, 5000, 100);
        settings.add_int_spinner(L"WheelAccelerationLimit", L"Maximum fast-scroll boost", m_settings.wheelAccelerationLimit, 1, 4, 1);
        settings.add_int_spinner(L"ScopeIndicatorX", L"Overview horizontal position percent", m_settings.scopeIndicatorX, 0, 100, 1);
        settings.add_int_spinner(L"ScopeIndicatorY", L"Overview vertical position percent", m_settings.scopeIndicatorY, 0, 100, 1);
        settings.add_int_spinner(L"ScopeIndicatorFade", L"View indicator fade milliseconds", m_settings.scopeIndicatorFade, 0, 400, 10);
        settings.add_bool_toggle(L"DoubleControl", L"Double-tap Left Ctrl and hold", m_settings.keyboardGesture);
        settings.add_bool_toggle(L"ModifiedDoubleClick", L"Modified double-click and hold", m_settings.mouseGesture);
        settings.add_bool_toggle(L"ReverseDirection", L"Reverse wheel and drag direction", m_settings.reverse);
        settings.add_bool_toggle(L"CenterFollow", L"Keep pointer centered", m_settings.centerFollow);
        settings.add_bool_toggle(L"ThreeFingerTouchpad", L"Three-finger touchpad gesture", m_settings.touchpadGesture);
        settings.add_bool_toggle(L"ZoomScaledPointer", L"Match pointer speed to zoom", m_settings.pointerScaling);
        settings.add_bool_toggle(L"ScopeIndicatorDetails", L"View indicator coordinates and zoom", m_settings.scopeIndicatorDetails);
        settings.add_bool_toggle(L"WheelAcceleration", L"Accelerate fast wheel scrolling", m_settings.wheelAcceleration);
        return settings.serialize_to_buffer(buffer, bufferSize);
    }

    void set_config(const wchar_t* config) override
    {
        try
        {
            const auto values = PowerToysSettings::PowerToyValues::from_json_string(config, ModuleKey);
            glide::settings::Values candidate;
            if (!ReadSettings(values, candidate))
            {
                Logger::warn("Rejected invalid GlideZoom settings");
                return;
            }
            auto serialized = SerializeSettings(candidate);
            serialized.save_to_settings_file();
            m_settings = candidate;
            if (m_settingsEvent)
            {
                if (!SetEvent(m_settingsEvent))
                {
                    Logger::error("Could not notify GlideZoom of saved settings: {}", GetLastError());
                }
            }
        }
        catch (...)
        {
            Logger::error("Could not save GlideZoom settings");
        }
    }

    void enable() override
    {
        if (m_enabled && IsProcessRunning())
        {
            return;
        }
        m_enabled = false;
        if (IsProcessRunning())
        {
            Logger::warn("The previous GlideZoom process is still restoring the desktop");
            return;
        }
        CloseProcessHandle();
        if (!m_exitEvent || !m_settingsEvent)
        {
            Logger::error("GlideZoom lifecycle events are unavailable; startup was refused");
            return;
        }
        if (!ResetEvent(m_exitEvent) || !ResetEvent(m_settingsEvent))
        {
            Logger::error("Could not reset GlideZoom lifecycle events: {}", GetLastError());
            return;
        }
        const auto executable = std::filesystem::path(get_module_folderpath()) / L"PowerToys.GlideZoom.exe";
        const auto arguments = L"--powertoys " + std::to_wstring(GetCurrentProcessId());
        Logger::info(L"Starting GlideZoom: {} (runner PID {})", executable.native(), GetCurrentProcessId());
        SHELLEXECUTEINFOW info{ sizeof(info) };
        info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        info.lpFile = executable.c_str();
        info.lpParameters = arguments.c_str();
        info.nShow = SW_HIDE;
        if (!ShellExecuteExW(&info) || !info.hProcess)
        {
            Logger::error("Could not start GlideZoom: {}", GetLastError());
            return;
        }
        m_process = info.hProcess;
        m_processId = GetProcessId(m_process);
        m_startedAt = GetTickCount64();
        m_exitRequested.store(false, std::memory_order_release);
        m_exitReported.store(false, std::memory_order_release);
        if (!RegisterWaitForSingleObject(&m_processWait, m_process, ProcessExited, this, INFINITE, WT_EXECUTEONLYONCE))
        {
            // Status queries and handle cleanup still report exits if a wait
            // cannot be registered. Never restart an uncertain desktop owner.
            Logger::error("Could not monitor GlideZoom process {}: {}", m_processId, GetLastError());
        }
        m_enabled = true;
        Logger::info("GlideZoom enabled (PID {})", m_processId);
        Logger::flush();
    }

    void disable() override
    {
        if (m_process && IsProcessRunning())
        {
            Logger::info("Requesting GlideZoom shutdown (PID {})", m_processId);
            m_exitRequested.store(true, std::memory_order_release);
            if (!SetEvent(m_exitEvent))
            {
                m_exitRequested.store(false, std::memory_order_release);
                Logger::error("Could not request GlideZoom shutdown: {}", GetLastError());
            }
            // Let the engine restore its input mapping and cursor. Its guardian
            // remains responsible for recovery if the engine cannot finish.
            if (WaitForSingleObject(m_process, 5000) == WAIT_TIMEOUT)
            {
                Logger::warn("GlideZoom has not completed desktop restoration yet");
            }
        }
        if (m_enabled)
        {
            Logger::info("GlideZoom disabled");
        }
        m_enabled = false;
        if (!IsProcessRunning())
        {
            CloseProcessHandle();
        }
    }

    bool is_enabled() override { return m_enabled && IsProcessRunning(); }
    void destroy() override { delete this; }

private:
    static void CALLBACK ProcessExited(void* context, BOOLEAN) noexcept
    {
        static_cast<GlideZoomModuleInterface*>(context)->ReportProcessExit();
    }

    void ReportProcessExit() noexcept
    {
        if (!m_process || WaitForSingleObject(m_process, 0) != WAIT_OBJECT_0 ||
            m_exitReported.exchange(true, std::memory_order_acq_rel))
        {
            return;
        }
        try
        {
            DWORD exitCode = 0;
            if (!GetExitCodeProcess(m_process, &exitCode))
            {
                Logger::error("Could not read GlideZoom process {} exit code: {}", m_processId, GetLastError());
                Logger::flush();
                return;
            }
            const auto elapsed = GetTickCount64() - m_startedAt;
            if (m_exitRequested.load(std::memory_order_acquire))
            {
                Logger::info("GlideZoom process {} exited after a shutdown request: code {} (0x{:08X}), runtime {} ms",
                             m_processId, exitCode, exitCode, elapsed);
            }
            else
            {
                Logger::warn("GlideZoom process {} exited without a module shutdown request: code {} (0x{:08X}), runtime {} ms. Automatic restart was not attempted",
                             m_processId, exitCode, exitCode, elapsed);
            }
            Logger::flush();
        }
        catch (...)
        {
            // No exception may escape a Windows thread-pool callback.
        }
    }

    bool IsProcessRunning()
    {
        if (!m_process)
        {
            return false;
        }
        const auto wait = WaitForSingleObject(m_process, 0);
        if (wait == WAIT_OBJECT_0)
        {
            ReportProcessExit();
        }
        else if (wait != WAIT_TIMEOUT)
        {
            Logger::error("Could not query GlideZoom process {} state: {}", m_processId, GetLastError());
            // An unsuccessful observation does not establish that restoration
            // finished. Keep the handle and refuse a second engine instance.
            return true;
        }
        return wait == WAIT_TIMEOUT;
    }

    void CloseProcessHandle()
    {
        if (m_process)
        {
            if (m_processWait)
            {
                // Finish the callback before releasing its handle or context.
                UnregisterWaitEx(m_processWait, INVALID_HANDLE_VALUE);
                m_processWait = nullptr;
            }
            ReportProcessExit();
            CloseHandle(m_process);
            m_process = nullptr;
        }
    }

    bool m_enabled = false;
    HANDLE m_process = nullptr;
    HANDLE m_processWait = nullptr;
    DWORD m_processId = 0;
    ULONGLONG m_startedAt = 0;
    std::atomic<bool> m_exitRequested{false};
    std::atomic<bool> m_exitReported{false};
    HANDLE m_exitEvent = nullptr;
    HANDLE m_settingsEvent = nullptr;
    glide::settings::Values m_settings;
};

extern "C" __declspec(dllexport) PowertoyModuleIface* __cdecl powertoy_create()
{
    return new GlideZoomModuleInterface();
}
