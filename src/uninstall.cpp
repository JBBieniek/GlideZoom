#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#include <array>
#include <string>
#include "installer_security.h"

namespace {
int Fail(const wchar_t* message) noexcept {
    MessageBoxW(nullptr, message, L"GlideZoom uninstaller", MB_OK | MB_ICONERROR);
    return 1;
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    if (arguments && *arguments) return Fail(L"Run the GlideZoom uninstaller without additional arguments.");
    try {
        PWSTR known = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &known)) || !known)
            return Fail(L"The protected Program Files folder is unavailable.");
        std::wstring programFiles;
        try { programFiles = known; } catch (...) { CoTaskMemFree(known); throw; }
        CoTaskMemFree(known);
        const std::wstring installed = programFiles + L"\\GlideZoom";
        const std::wstring expected = installed + L"\\GlideZoom-Uninstall.exe";
        std::array<wchar_t, 32768> buffer{};
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length || length >= buffer.size() ||
            CompareStringOrdinal(buffer.data(), static_cast<int>(length), expected.c_str(), -1, TRUE) != CSTR_EQUAL)
            return Fail(L"Run the installed uninstaller from Windows Settings > Apps > Installed apps, or from the GlideZoom folder in Program Files.");
        length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (!length || length >= buffer.size()) return Fail(L"The Windows system folder is unavailable.");
        const std::wstring system(buffer.data(), length);
        length = GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (!length || length >= buffer.size()) return Fail(L"The Windows folder is unavailable.");
        const std::wstring windows(buffer.data(), length);
        const std::wstring powershell = system + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
        const std::wstring script = installed + L"\\Uninstall.ps1";
        glide::installer::SecurePaths paths;
        if (!paths.Lock(expected, false) || !paths.Lock(script, false) ||
            !paths.Lock(installed + L"\\install-manifest.json", false) || !paths.Lock(powershell, false))
            return Fail(L"The installed uninstaller paths or their permissions could not be verified. Keep the installation folder and manifest; no removal was started.");
        auto command = glide::setup::UninstallCommandLine(powershell, script, GetCurrentProcessId());
        // System32 is the working/temp directory: the child must not keep the
        // installation directory open while deleting it. No inherited PATH,
        // module paths, profiles, CLR profilers, or handles enter the child.
        auto environment = glide::setup::Environment(windows, system, programFiles, system);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(powershell.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, environment.data(), system.c_str(), &startup, &process))
            return Fail(L"Windows PowerShell could not start the uninstaller. Keep the installation folder and retry.");
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        // The script validates this PID/path and waits for this small launcher
        // to exit before deleting it; no self-copy, reboot task or delayed delete.
        return 0;
    } catch (...) { return Fail(L"The uninstaller could not prepare its protected launch. No removal was started."); }
}
