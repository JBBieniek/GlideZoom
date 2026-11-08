#pragma once

#include "zoom_capture.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <memory>

namespace glide::capture
{

    // Uses physical rectangles from EnumDisplayMonitors. Canonicalizes order and
    // exact duplicate displays, and fails cleanly on unsupported or oversized lists.
    bool ReadTopology(Topology& output) noexcept;

    // Owns only private capture buffers/GDI objects and a not-yet-published DIBV5.
    // Prepare performs every allocation before the application's zoom transaction.
    // Capture assumes the Host has independently verified an unzoomed baseline.
    class NativeImage final
    {
    public:
        NativeImage() noexcept = default;
        ~NativeImage() noexcept;
        NativeImage(const NativeImage&) = delete;
        NativeImage& operator=(const NativeImage&) = delete;

        bool Prepare(const Plan& plan) noexcept;
        bool Capture(Heartbeat heartbeat = {}) noexcept;
        bool Render(Heartbeat heartbeat = {}) noexcept;
        ClipboardResult Publish(HWND owner) noexcept;
        DWORD LastError() const noexcept { return error_; }

    private:
        void Reset() noexcept;
        bool Failure(DWORD fallback) noexcept;
        Plan plan_{};
        HDC memory_ = nullptr;
        HBITMAP bitmap_ = nullptr;
        HGDIOBJ previous_ = nullptr;
        std::uint32_t* source_ = nullptr;
        HGLOBAL clipboard_ = nullptr;
        void* clipboardData_ = nullptr;
        std::uint32_t* output_ = nullptr;
        std::unique_ptr<AxisSample[]> horizontal_;
        DWORD error_ = ERROR_SUCCESS;
        bool prepared_ = false, captured_ = false, rendered_ = false;
    };

} // namespace glide::capture
