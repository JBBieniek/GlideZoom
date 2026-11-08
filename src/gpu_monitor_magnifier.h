#pragma once

#include <cstdint>
#include <memory>
#include <windows.h>

#include "magnification_api.h"
#include "monitor_geometry.h"
#include "monitor_visual.h"
#include "scope_indicator.h"

// A monitor capture and GPU presentation surface. This class owns no global
// input or cursor settings: the existing Session and cursor lease own those.
// All methods run on the application's UI thread. WGC's internal capture
// worker is consumed by bounded polling, with no callbacks into this object.
class GpuMonitorMagnifier
{
public:
    using VisualRefresh = glide::monitor::VisualRefresh;
    using ScopeIndicator = GpuScopeIndicator;
    using CursorPositionResolver = bool (*)(void* context, glide::Vec2 physical, glide::Transform applied, glide::Rect bounds, glide::Vec2& precise) noexcept;
    GpuMonitorMagnifier() noexcept;
    ~GpuMonitorMagnifier() noexcept;
    GpuMonitorMagnifier(const GpuMonitorMagnifier&) = delete;
    GpuMonitorMagnifier& operator=(const GpuMonitorMagnifier&) = delete;

    bool Initialize(HINSTANCE instance, MagnificationApi& api) noexcept;
    bool Prepare(const glide::monitor::Frame& frame) noexcept;
    bool Show() noexcept;
    // Private visual refresh only; never changes the active input map or cursor
    // lease. Root releases its cursor lease before parking this view.
    VisualRefresh RefreshPassive(const glide::monitor::Frame& frame) noexcept;
    void SetCursorEnabled(bool enabled) noexcept;
    // Last successful submission metadata; this alone is not a fresh native
    // z-order check. CanRetainVisual verifies eligibility before retaining it.
    bool TryGetPresentedFrame(glide::monitor::Frame& frame) const noexcept;
    bool CanRetainVisual(const glide::monitor::Frame& frame) noexcept;
    void SetScopeIndicator(const ScopeIndicator& options) noexcept;
    // Screenshot recovery temporarily hides only the HWND. Ordinary reset,
    // pause and failures also close capture, so 1x does not capture in background.
    void Hide(bool preserveCapture = false) noexcept;
    void Destroy() noexcept;
    bool Active() const noexcept;
    bool Matches(const glide::monitor::Frame& frame) const noexcept;
    bool Pending() const noexcept;
    // UI-thread diagnostic count; survives resource rebuilds and saturates.
    std::uint64_t RestagedFrameCount() const noexcept;
    bool CanRetain(const glide::session::Snapshot& snapshot) noexcept;
    HWND PaintWindow() const noexcept { return nullptr; }
    bool ReplacementVisibleAt(glide::Vec2 source) const noexcept;
    // Fresh native observation for both sides of a global cursor-hide call.
    // A false result must release an owned cursor, including during GPU waits.
    bool ReplacementVisibleNow() const noexcept;
    void SetSystemCursorHiddenByUs(bool hidden) noexcept;
    // UI-thread callback only. It may refine the drawn cursor by at most half
    // a source pixel and never changes native input or the viewport camera.
    void SetCursorPositionResolver(CursorPositionResolver resolver, void* context) noexcept;
    const wchar_t* FailureReason() const noexcept { return reason_; }
    HRESULT LastResult() const noexcept { return result_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    HRESULT result_ = S_OK;
    const wchar_t* reason_ = L"";
    CursorPositionResolver cursorResolver_ = nullptr;
    void* cursorResolverContext_ = nullptr;
    bool cursorEnabled_ = true;
    bool passiveRefresh_ = false;
    ScopeIndicator indicator_{};
    std::uint64_t indicatorRevision_ = 0;
    bool Error(HRESULT result, const wchar_t* reason) noexcept;
};
