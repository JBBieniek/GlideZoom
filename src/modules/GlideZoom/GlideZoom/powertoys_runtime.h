// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#pragma once

#include <windows.h>
#include <roapi.h>
#include <string>
#include <string_view>

namespace glide::powertoys
{
    inline bool ParseProcessId(std::wstring_view text, DWORD& output) noexcept
    {
        if (text.empty() || text.size() > 10)
        {
            return false;
        }
        unsigned long long value = 0;
        for (const wchar_t character : text)
        {
            if (character < L'0' || character > L'9')
            {
                return false;
            }
            value = value * 10 + static_cast<unsigned>(character - L'0');
        }
        if (value == 0 || value > MAXDWORD)
        {
            return false;
        }
        output = static_cast<DWORD>(value);
        return true;
    }

    class Apartment
    {
    public:
        Apartment() noexcept :
            status_(RoInitialize(RO_INIT_MULTITHREADED)) {}
        ~Apartment() noexcept
        {
            if (SUCCEEDED(status_))
            {
                RoUninitialize();
            }
        }
        Apartment(const Apartment&) = delete;
        Apartment& operator=(const Apartment&) = delete;
        bool Ready() const noexcept { return SUCCEEDED(status_); }

    private:
        HRESULT status_;
    };

    // PowerToys creates both auto-reset events before starting the engine.
    // Holding a process handle also detects Runner crashes and avoids PID reuse.
    class Runner
    {
    public:
        Runner() = default;
        Runner(const Runner&) = delete;
        Runner& operator=(const Runner&) = delete;
        ~Runner() noexcept
        {
            if (process_)
                CloseHandle(process_);
            if (exit_)
                CloseHandle(exit_);
            if (settings_)
                CloseHandle(settings_);
        }

        bool Connect(DWORD processId)
        {
            if (process_ || exit_ || settings_)
            {
                return false;
            }
            process_ = OpenProcess(SYNCHRONIZE, FALSE, processId);
            const auto suffix = std::to_wstring(processId);
            exit_ = OpenEventW(SYNCHRONIZE, FALSE, (L"Local\\PowerToys_GlideZoom_Exit_" + suffix).c_str());
            settings_ = OpenEventW(SYNCHRONIZE, FALSE, (L"Local\\PowerToys_GlideZoom_Settings_" + suffix).c_str());
            return process_ && exit_ && settings_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
        }

        bool ExitRequested() const noexcept
        {
            return process_ && (WaitForSingleObject(process_, 0) != WAIT_TIMEOUT ||
                                WaitForSingleObject(exit_, 0) != WAIT_TIMEOUT);
        }

        bool SettingsChanged() const noexcept
        {
            return settings_ && WaitForSingleObject(settings_, 0) == WAIT_OBJECT_0;
        }

    private:
        HANDLE process_ = nullptr;
        HANDLE exit_ = nullptr;
        HANDLE settings_ = nullptr;
    };
}
