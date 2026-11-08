#pragma once

#include "zoom_core.h"
#include "zoom_session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// An explicit screenshot command reconstructs the saved, applied zoom from
// pixels captured while this app's magnification is verifiably at identity.
// It does not make assumptions about other screenshot applications or about
// whether a Windows capture API samples before or after fullscreen zoom.
namespace glide::capture
{

    using PixelRect = session::Rect;
    constexpr std::size_t kMaxMonitors = 32;
    constexpr std::uint32_t kMaxDimension = 32768;
    constexpr std::uint64_t kMaxPixels = 32ull * 1024 * 1024;
    constexpr std::uint64_t kMaxPixelBytes = 256ull * 1024 * 1024;
    constexpr std::int32_t kCoordinateLimit = 262144;

    enum class Scope
    {
        Monitor,
        Desktop
    };
    enum class ClipboardResult
    {
        Copied,
        Busy,
        EmptyFailed,
        SetFailed,
        CloseFailed,
        Invalid
    };
    enum class Result
    {
        Copied,
        NotReady,
        PrepareFailed,
        IdentityFailed,
        EnvironmentChanged,
        CaptureFailed,
        RestoreFailed,
        RenderFailed,
        ClipboardBusy,
        ClipboardEmptyFailed,
        ClipboardSetFailed,
        ClipboardCloseFailed,
        ClipboardInvalid
    };

    // The UI thread can receive sent/reentrant messages inside a native API call.
    // Freeze this lease when the saved view is first read, BEFORE preparation.
    // Advance only before the one cancellation an intentional transaction step is
    // allowed to perform. Never adopt a new epoch after returning from native code:
    // an emergency reset may have changed it and must cancel restoration of the
    // old view even if lifecycle::State has returned to Ready at identity.
    class Lease
    {
    public:
        bool Acquire(std::uint64_t epoch) noexcept
        {
            expected_ = epoch;
            armed_ = epoch != 0;
            return armed_;
        }
        bool Matches(std::uint64_t actual) const noexcept { return armed_ && actual == expected_; }
        // A native getter can dispatch a sent message before returning. Validate
        // the same lease and Ready state on both sides; a successful native read
        // alone must not authorize publication after a nested reset/pause/stop.
        template<class Runtime, class Reader>
        bool Observe(const Runtime& runtime, Reader&& reader) const
            noexcept(noexcept(runtime.Epoch()) && noexcept(runtime.Ready()) &&
                     noexcept(static_cast<bool>(reader())))
        {
            if (!Matches(runtime.Epoch()) || !runtime.Ready())
                return false;
            const bool observed = static_cast<bool>(reader());
            const bool stillReady = Matches(runtime.Epoch()) && runtime.Ready();
            return observed && stillReady;
        }
        bool AdvanceOne(std::uint64_t actualBefore) noexcept
        {
            if (!Matches(actualBefore) || expected_ == std::numeric_limits<std::uint64_t>::max())
            {
                Disarm();
                return false;
            }
            ++expected_;
            return true;
        }
        void Disarm() noexcept
        {
            expected_ = 0;
            armed_ = false;
        }
        bool Armed() const noexcept { return armed_; }

    private:
        std::uint64_t expected_ = 0;
        bool armed_ = false;
    };

    struct Topology
    {
        std::array<PixelRect, kMaxMonitors> monitors{};
        std::size_t count = 0;
    };

    inline constexpr bool ValidRect(PixelRect r) noexcept
    {
        return r.left >= -kCoordinateLimit && r.top >= -kCoordinateLimit &&
               r.right <= kCoordinateLimit && r.bottom <= kCoordinateLimit &&
               r.right > r.left && r.bottom > r.top;
    }

    inline constexpr bool Intersects(PixelRect a, PixelRect b) noexcept
    {
        return a.left < b.right && b.left < a.right &&
               a.top < b.bottom && b.top < a.bottom;
    }

    inline constexpr PixelRect Intersection(PixelRect a, PixelRect b) noexcept
    {
        return { std::max(a.left, b.left), std::max(a.top, b.top), std::min(a.right, b.right), std::min(a.bottom, b.bottom) };
    }

    inline constexpr bool RectLess(PixelRect a, PixelRect b) noexcept
    {
        if (a.left != b.left)
            return a.left < b.left;
        if (a.top != b.top)
            return a.top < b.top;
        if (a.right != b.right)
            return a.right < b.right;
        return a.bottom < b.bottom;
    }

    // Enumeration order is immaterial. Exact clone rectangles are folded; partly
    // overlapping rectangles are rejected rather than repeatedly rendering an
    // ambiguous/expensive desktop layout. Failure leaves the argument unchanged.
    inline bool Canonicalize(Topology& topology) noexcept
    {
        if (topology.count == 0 || topology.count > kMaxMonitors)
            return false;
        Topology next = topology;
        for (std::size_t i = 0; i < next.count; ++i)
            if (!ValidRect(next.monitors[i]))
                return false;
        std::sort(next.monitors.begin(), next.monitors.begin() + next.count, RectLess);
        std::size_t count = 0;
        for (std::size_t i = 0; i < next.count; ++i)
        {
            if (count && next.monitors[i] == next.monitors[count - 1])
                continue;
            next.monitors[count++] = next.monitors[i];
        }
        next.count = count;
        for (std::size_t i = 0; i < count; ++i)
            for (std::size_t j = i + 1; j < count; ++j)
                if (Intersects(next.monitors[i], next.monitors[j]))
                    return false;
        for (std::size_t i = count; i < kMaxMonitors; ++i)
            next.monitors[i] = {};
        topology = next;
        return true;
    }

    inline bool SameTopology(Topology a, Topology b) noexcept
    {
        if (!Canonicalize(a) || !Canonicalize(b) || a.count != b.count)
            return false;
        for (std::size_t i = 0; i < a.count; ++i)
            if (!(a.monitors[i] == b.monitors[i]))
                return false;
        return true;
    }

    inline PixelRect DesktopBounds(const Topology& topology) noexcept
    {
        if (topology.count == 0 || topology.count > kMaxMonitors)
            return {};
        PixelRect bounds = topology.monitors[0];
        for (std::size_t i = 1; i < topology.count; ++i)
        {
            const auto r = topology.monitors[i];
            bounds = { std::min(bounds.left, r.left), std::min(bounds.top, r.top), std::max(bounds.right, r.right), std::max(bounds.bottom, r.bottom) };
        }
        return bounds;
    }

    inline constexpr Rect AsRect(PixelRect r) noexcept
    {
        return { static_cast<double>(r.left), static_cast<double>(r.top), static_cast<double>(r.right), static_cast<double>(r.bottom) };
    }

    inline bool ToPixelRect(Rect r, PixelRect& output) noexcept
    {
        for (double v : { r.left, r.top, r.right, r.bottom })
            if (!std::isfinite(v) || v < -kCoordinateLimit || v > kCoordinateLimit ||
                std::floor(v) != v)
                return false;
        const PixelRect next{ static_cast<std::int32_t>(r.left), static_cast<std::int32_t>(r.top), static_cast<std::int32_t>(r.right), static_cast<std::int32_t>(r.bottom) };
        if (!ValidRect(next))
            return false;
        output = next;
        return true;
    }

    struct Plan
    {
        Scope scope = Scope::Monitor;
        PixelRect output{}, source{};
        Transform transform{};
        Topology topology{};
        std::uint32_t width = 0, height = 0, sourceWidth = 0, sourceHeight = 0;
        std::uint64_t outputPixels = 0, sourcePixels = 0;
    };

    // Bilinear reconstruction uses source pixel centers. Capture only the pixels
    // its taps can touch; retain the actual uniform transform, not the rounded
    // rectangle used for Windows input mapping. Edge samples clamp to the selected
    // monitor/desktop boundary. A one-pixel edge allowance accounts for native
    // fullscreen integer-offset rounding, not arbitrary out-of-range panning.
    inline bool MakePlan(Scope scope, Rect selected, Transform applied, const Topology& displays, Plan& output) noexcept
    {
        if (scope != Scope::Monitor && scope != Scope::Desktop)
            return false;
        if (!std::isfinite(applied.zoom) || applied.zoom < 1 || applied.zoom > 4096 ||
            !detail::Finite(applied.offset) || std::abs(applied.offset.x) > kCoordinateLimit ||
            std::abs(applied.offset.y) > kCoordinateLimit)
            return false;
        Topology topology = displays;
        if (!Canonicalize(topology))
            return false;
        PixelRect bounds = DesktopBounds(topology);
        if (scope == Scope::Monitor)
        {
            if (!ToPixelRect(selected, bounds))
                return false;
            bool found = false;
            for (std::size_t i = 0; i < topology.count; ++i)
                found = found || bounds == topology.monitors[i];
            if (!found)
                return false;
        }
        const auto width = static_cast<std::uint32_t>(bounds.right - bounds.left);
        const auto height = static_cast<std::uint32_t>(bounds.bottom - bounds.top);
        const std::uint64_t pixels = static_cast<std::uint64_t>(width) * height;
        if (width > kMaxDimension || height > kMaxDimension || pixels > kMaxPixels)
            return false;
        if (applied.zoom == 1)
            applied.offset = {}; // Native identity ignores offsets.
        const double leftEdge = applied.offset.x + bounds.left / applied.zoom;
        const double topEdge = applied.offset.y + bounds.top / applied.zoom;
        const double rightEdge = applied.offset.x + bounds.right / applied.zoom;
        const double bottomEdge = applied.offset.y + bounds.bottom / applied.zoom;
        if (leftEdge < bounds.left - 1.0 || topEdge < bounds.top - 1.0 ||
            rightEdge > bounds.right + 1.0 || bottomEdge > bounds.bottom + 1.0 ||
            rightEdge <= bounds.left || bottomEdge <= bounds.top ||
            leftEdge >= bounds.right || topEdge >= bounds.bottom)
            return false;
        const auto tap = [&](double screen, bool horizontal) noexcept {
            const double offset = horizontal ? applied.offset.x : applied.offset.y;
            const double low = horizontal ? bounds.left : bounds.top;
            const double high = (horizontal ? bounds.right : bounds.bottom) - 1.0;
            return std::floor(std::clamp(offset + screen / applied.zoom - 0.5, low, high));
        };
        const PixelRect source{
            static_cast<std::int32_t>(tap(bounds.left + 0.5, true)),
            static_cast<std::int32_t>(tap(bounds.top + 0.5, false)),
            static_cast<std::int32_t>(std::min<double>(bounds.right, tap(bounds.right - 0.5, true) + 2)),
            static_cast<std::int32_t>(std::min<double>(bounds.bottom, tap(bounds.bottom - 0.5, false) + 2))
        };
        if (!ValidRect(source))
            return false;
        const auto sourceWidth = static_cast<std::uint32_t>(source.right - source.left);
        const auto sourceHeight = static_cast<std::uint32_t>(source.bottom - source.top);
        const std::uint64_t sourcePixels = static_cast<std::uint64_t>(sourceWidth) * sourceHeight;
        if (sourceWidth > kMaxDimension || sourceHeight > kMaxDimension ||
            sourcePixels > kMaxPixels || (pixels + sourcePixels) * 4 > kMaxPixelBytes)
            return false;
        output = { scope, bounds, source, applied, topology, width, height, sourceWidth, sourceHeight, pixels, sourcePixels };
        return true;
    }

    inline bool ValidPlan(const Plan& plan) noexcept
    {
        Plan expected;
        if (!MakePlan(plan.scope, AsRect(plan.output), plan.transform, plan.topology, expected))
            return false;
        if (plan.topology.count != expected.topology.count)
            return false;
        for (std::size_t i = 0; i < plan.topology.count; ++i)
            if (!(plan.topology.monitors[i] == expected.topology.monitors[i]))
                return false;
        return plan.output == expected.output && plan.source == expected.source &&
               plan.transform.zoom == expected.transform.zoom &&
               plan.transform.offset.x == expected.transform.offset.x &&
               plan.transform.offset.y == expected.transform.offset.y &&
               plan.width == expected.width && plan.height == expected.height &&
               plan.sourceWidth == expected.sourceWidth && plan.sourceHeight == expected.sourceHeight &&
               plan.outputPixels == expected.outputPixels && plan.sourcePixels == expected.sourcePixels;
    }

    // Poll is called on the application's owning thread. It may update watchdog
    // heartbeats and cancel, but must not pump messages, throw, or mutate the saved
    // plan. No synthetic input, cursor movement, or pointer-speed setting is used.
    struct Heartbeat
    {
        bool (*poll)(void*) noexcept = nullptr;
        void* context = nullptr;
        bool Continue() const noexcept { return !poll || poll(context); }
    };

    struct AxisSample
    {
        std::uint32_t first = 0, second = 0, weight = 0;
    };

    inline AxisSample Sample(double coordinate, std::int32_t low, std::int32_t high, std::int32_t cropLow) noexcept
    {
        const double value = std::clamp(coordinate, static_cast<double>(low), high - 1.0);
        const auto first = static_cast<std::int32_t>(std::floor(value));
        const auto second = std::min(first + 1, high - 1);
        return { static_cast<std::uint32_t>(first - cropLow),
                 static_cast<std::uint32_t>(second - cropLow),
                 static_cast<std::uint32_t>(std::lround((value - first) * 65536.0)) };
    }

    inline std::uint32_t Blend(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, std::uint32_t x, std::uint32_t y) noexcept
    {
        std::uint32_t result = 0xff000000u;
        for (unsigned shift : { 0u, 8u, 16u })
        {
            constexpr std::uint64_t weightUnit = 65536;
            const std::uint64_t top = static_cast<std::uint64_t>((a >> shift) & 255u) * (weightUnit - x) +
                                      static_cast<std::uint64_t>((b >> shift) & 255u) * x;
            const std::uint64_t bottom = static_cast<std::uint64_t>((c >> shift) & 255u) * (weightUnit - x) +
                                         static_cast<std::uint64_t>((d >> shift) & 255u) * x;
            const auto channel = static_cast<std::uint32_t>((top * (weightUnit - y) +
                                                             bottom * y + (1ull << 31)) >>
                                                            32);
            result |= channel << shift;
        }
        return result;
    }

    inline bool MemoryOverlaps(const void* a, std::uint64_t aBytes, const void* b, std::uint64_t bBytes) noexcept
    {
        const auto x = reinterpret_cast<std::uintptr_t>(a), y = reinterpret_cast<std::uintptr_t>(b);
        constexpr auto maximum = std::numeric_limits<std::uintptr_t>::max();
        if (aBytes > maximum - x || bBytes > maximum - y)
            return true;
        return x < y + bBytes && y < x + aBytes;
    }

    // Buffers are tightly packed, top-down BGRA. Alpha is always made opaque.
    // Source desktop gaps must already be black; destination gaps are explicitly
    // black. Working storage is caller-allocated so allocation finishes before
    // temporarily restoring zoom. False leaves an unpublished partial output.
    inline bool Render(const Plan& plan, const std::uint32_t* source, std::size_t sourceCount, std::uint32_t* output, std::size_t outputCount, AxisSample* horizontal, std::size_t horizontalCount, Heartbeat heartbeat = {}) noexcept
    {
        if (!ValidPlan(plan) || !source || !output || !horizontal ||
            sourceCount < plan.sourcePixels || outputCount < plan.outputPixels ||
            horizontalCount < plan.width ||
            MemoryOverlaps(source, plan.sourcePixels * 4, output, plan.outputPixels * 4) ||
            MemoryOverlaps(source, plan.sourcePixels * 4, horizontal, plan.width * sizeof(AxisSample)) ||
            MemoryOverlaps(output, plan.outputPixels * 4, horizontal, plan.width * sizeof(AxisSample)) ||
            !heartbeat.Continue())
            return false;
        for (std::uint32_t x = 0; x < plan.width; ++x)
        {
            horizontal[x] = Sample(plan.transform.offset.x + (static_cast<double>(plan.output.left) + x + 0.5) / plan.transform.zoom - 0.5, plan.output.left, plan.output.right, plan.source.left);
            if (horizontal[x].first >= plan.sourceWidth || horizontal[x].second >= plan.sourceWidth)
                return false;
        }
        for (std::uint32_t y = 0; y < plan.height; ++y)
        {
            if ((y & 31u) == 0 && !heartbeat.Continue())
                return false;
            const auto vertical = Sample(plan.transform.offset.y + (static_cast<double>(plan.output.top) + y + 0.5) / plan.transform.zoom - 0.5, plan.output.top, plan.output.bottom, plan.source.top);
            if (vertical.first >= plan.sourceHeight || vertical.second >= plan.sourceHeight)
                return false;
            auto* row = output + static_cast<std::size_t>(y) * plan.width;
            std::fill(row, row + plan.width, 0xff000000u);
            const auto* top = source + static_cast<std::size_t>(vertical.first) * plan.sourceWidth;
            const auto* bottom = source + static_cast<std::size_t>(vertical.second) * plan.sourceWidth;
            const auto screenY = plan.output.top + static_cast<std::int32_t>(y);
            for (std::size_t i = 0; i < plan.topology.count; ++i)
            {
                const auto display = Intersection(plan.topology.monitors[i], plan.output);
                if (display.right <= display.left || screenY < display.top || screenY >= display.bottom)
                    continue;
                const auto start = static_cast<std::uint32_t>(display.left - plan.output.left);
                const auto end = static_cast<std::uint32_t>(display.right - plan.output.left);
                for (auto x = start; x < end; ++x)
                {
                    const auto h = horizontal[x];
                    row[x] = Blend(top[h.first], top[h.second], bottom[h.first], bottom[h.second], h.weight, vertical.weight);
                }
            }
        }
        return heartbeat.Continue();
    }

    inline Result ClipboardOutcome(ClipboardResult result) noexcept
    {
        switch (result)
        {
        case ClipboardResult::Copied:
            return Result::Copied;
        case ClipboardResult::Busy:
            return Result::ClipboardBusy;
        case ClipboardResult::EmptyFailed:
            return Result::ClipboardEmptyFailed;
        case ClipboardResult::SetFailed:
            return Result::ClipboardSetFailed;
        case ClipboardResult::CloseFailed:
            return Result::ClipboardCloseFailed;
        default:
            return Result::ClipboardInvalid;
        }
    }

    // Windows clipboard replacement has no atomic exchange API. Complete pixel
    // allocation/rendering and restore the zoom before calling this function.
    // Open failure changes nothing. After Empty succeeds, Set failure can leave
    // the previous clipboard gone; callers must report that boundary honestly.
    // Backend::Set transfers the prebuilt memory handle only on actual success.
    template<class Backend>
    ClipboardResult PublishPrepared(Backend& backend) noexcept
    {
        static_assert(noexcept(backend.Open()) && noexcept(backend.Empty()) &&
                          noexcept(backend.Set()) && noexcept(backend.Close()),
                      "Clipboard backend methods must not throw");
        if (!backend.Open())
            return ClipboardResult::Busy;
        ClipboardResult result = ClipboardResult::EmptyFailed;
        if (backend.Empty())
            result = backend.Set() ? ClipboardResult::Copied : ClipboardResult::SetFailed;
        // A transient Close failure gets exactly one retry. If release still cannot
        // be verified, the owner must stop the application so Windows releases any
        // remaining clipboard lock; it must not continue as an ordinary copy error.
        return (backend.Close() || backend.Close()) ? result : ClipboardResult::CloseFailed;
    }

    // All Host methods must be nonthrowing. Ready/Prepare do not change zoom or
    // clipboard. Quiesce cancels gestures and prevents newly queued input from
    // changing the frozen applied view. EnterIdentity uses the existing conditional
    // Session restoration (including its journal); a failure retains any recovery
    // obligation. RestoreView MUST independently recheck identity, desktop and
    // topology and must never overwrite an observed foreign magnifier state.
    // Finish runs on every post-Quiesce exit and invalidates queued input once more.
    // API compare/readback is not an atomic cross-process ownership primitive.
    template<class Host>
    Result Run(Host& host) noexcept
    {
        static_assert(noexcept(host.Ready()) && noexcept(host.Prepare()) &&
                          noexcept(host.Quiesce()) && noexcept(host.EnterIdentity()) &&
                          noexcept(host.IdentityMatches()) && noexcept(host.Capture()) &&
                          noexcept(host.RestoreView()) && noexcept(host.Render()) &&
                          noexcept(host.RestoredMatches()) && noexcept(host.Publish()) &&
                          noexcept(host.Finish()),
                      "Capture Host methods must not throw");
        if (!host.Ready())
            return Result::NotReady;
        if (!host.Prepare())
            return Result::PrepareFailed;
        host.Quiesce();
        struct Finish
        {
            Host& host;
            ~Finish() noexcept { host.Finish(); }
        } finish{ host };
        if (!host.EnterIdentity())
            return Result::IdentityFailed;
        const bool before = host.IdentityMatches();
        const bool captured = before && host.Capture();
        const bool after = host.IdentityMatches();
        // Restoration is attempted once on every successfully entered baseline,
        // including capture and environment failures. The Host decides whether a
        // restore is still authorized; policy never writes a stale saved transform.
        if (!host.RestoreView())
            return Result::RestoreFailed;
        if (!before || !after)
            return Result::EnvironmentChanged;
        if (!captured)
            return Result::CaptureFailed;
        if (!host.Render())
            return Result::RenderFailed;
        if (!host.RestoredMatches())
            return Result::EnvironmentChanged;
        return ClipboardOutcome(host.Publish());
    }

} // namespace glide::capture
