#pragma once
#include <windows.h>
#include "zoom_session.h"
#include "desktop_context.h"

// Use the public API via the system DLL. This also supports MinGW SDKs whose
// magnification.h omits the full-screen declarations and import library.
class MagnificationApi
{
public:
    using InitializeFn = BOOL(WINAPI*)();
    using SetFullscreenFn = BOOL(WINAPI*)(float, int, int);
    using GetFullscreenFn = BOOL(WINAPI*)(float*, int*, int*);
    using SetInputFn = BOOL(WINAPI*)(BOOL, const RECT*, const RECT*);
    using GetInputFn = BOOL(WINAPI*)(BOOL*, RECT*, RECT*);
    // Same ABI as MAGTRANSFORM; keep the fullscreen adapter independent of
    // SDK magnification headers which vary between Windows toolchains.
    struct WindowTransform
    {
        float v[3][3]{};
    };
    using SetWindowTransformFn = BOOL(WINAPI*)(HWND, WindowTransform*);
    using GetWindowTransformFn = BOOL(WINAPI*)(HWND, WindowTransform*);
    using SetWindowSourceFn = BOOL(WINAPI*)(HWND, RECT);
    using GetWindowSourceFn = BOOL(WINAPI*)(HWND, RECT*);
    using SetWindowFilterListFn = BOOL(WINAPI*)(HWND, DWORD, int, HWND*);
    using ShowSystemCursorFn = BOOL(WINAPI*)(BOOL);

    InitializeFn Initialize = nullptr, Uninitialize = nullptr;
    SetFullscreenFn SetFullscreenTransform = nullptr;
    GetFullscreenFn GetFullscreenTransform = nullptr;
    SetInputFn SetInputTransform = nullptr;
    GetInputFn GetInputTransform = nullptr;
    SetWindowTransformFn SetWindowTransform = nullptr;
    GetWindowTransformFn GetWindowTransform = nullptr;
    SetWindowSourceFn SetWindowSource = nullptr;
    GetWindowSourceFn GetWindowSource = nullptr;
    SetWindowFilterListFn SetWindowFilterList = nullptr;
    ShowSystemCursorFn ShowSystemCursor = nullptr;

    MagnificationApi() = default;
    MagnificationApi(const MagnificationApi&) = delete;
    MagnificationApi& operator=(const MagnificationApi&) = delete;
    ~MagnificationApi()
    {
        if (module_)
            FreeLibrary(module_);
    }

    bool WindowedAvailable() const noexcept
    {
        return SetWindowTransform && GetWindowTransform && SetWindowSource &&
               GetWindowSource && SetWindowFilterList && ShowSystemCursor;
    }

    bool Read(glide::session::Snapshot& state) const noexcept
    {
        if (!GetFullscreenTransform || !GetInputTransform || !glide::OnInputDesktop())
            return false;
        float zoom = 1;
        int x = 0, y = 0;
        BOOL enabled = FALSE;
        RECT source{}, destination{};
        if (!GetFullscreenTransform(&zoom, &x, &y) ||
            !GetInputTransform(&enabled, &source, &destination))
            return false;
        state = { { zoom, x, y }, { enabled != FALSE, { source.left, source.top, source.right, source.bottom }, { destination.left, destination.top, destination.right, destination.bottom } } };
        return glide::OnInputDesktop();
    }

    bool SetVisual(glide::session::Visual visual) const noexcept
    {
        return SetFullscreenTransform && glide::session::Valid(visual) && glide::OnInputDesktop() &&
               SetFullscreenTransform(visual.zoom, visual.x, visual.y) != FALSE;
    }

    bool SetInput(glide::session::Input input) const noexcept
    {
        if (!SetInputTransform || !glide::session::Valid(input) || !glide::OnInputDesktop())
            return false;
        const RECT source{ input.source.left, input.source.top, input.source.right, input.source.bottom };
        const RECT destination{ input.destination.left, input.destination.top, input.destination.right, input.destination.bottom };
        return SetInputTransform(input.enabled ? TRUE : FALSE,
                                 input.enabled ? &source : nullptr,
                                 input.enabled ? &destination : nullptr) != FALSE;
    }

    bool Load()
    {
        module_ = LoadLibraryExW(L"Magnification.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_)
            return false;
        Initialize = reinterpret_cast<InitializeFn>(GetProcAddress(module_, "MagInitialize"));
        Uninitialize = reinterpret_cast<InitializeFn>(GetProcAddress(module_, "MagUninitialize"));
        SetFullscreenTransform = reinterpret_cast<SetFullscreenFn>(GetProcAddress(module_, "MagSetFullscreenTransform"));
        GetFullscreenTransform = reinterpret_cast<GetFullscreenFn>(GetProcAddress(module_, "MagGetFullscreenTransform"));
        SetInputTransform = reinterpret_cast<SetInputFn>(GetProcAddress(module_, "MagSetInputTransform"));
        GetInputTransform = reinterpret_cast<GetInputFn>(GetProcAddress(module_, "MagGetInputTransform"));
        SetWindowTransform = reinterpret_cast<SetWindowTransformFn>(GetProcAddress(module_, "MagSetWindowTransform"));
        GetWindowTransform = reinterpret_cast<GetWindowTransformFn>(GetProcAddress(module_, "MagGetWindowTransform"));
        SetWindowSource = reinterpret_cast<SetWindowSourceFn>(GetProcAddress(module_, "MagSetWindowSource"));
        GetWindowSource = reinterpret_cast<GetWindowSourceFn>(GetProcAddress(module_, "MagGetWindowSource"));
        SetWindowFilterList = reinterpret_cast<SetWindowFilterListFn>(GetProcAddress(module_, "MagSetWindowFilterList"));
        ShowSystemCursor = reinterpret_cast<ShowSystemCursorFn>(GetProcAddress(module_, "MagShowSystemCursor"));
        if (!Initialize || !Uninitialize || !SetFullscreenTransform || !GetFullscreenTransform ||
            !SetInputTransform || !GetInputTransform)
        {
            SetLastError(ERROR_PROC_NOT_FOUND);
            return false;
        }
        return true;
    }

private:
    HMODULE module_ = nullptr;
};
