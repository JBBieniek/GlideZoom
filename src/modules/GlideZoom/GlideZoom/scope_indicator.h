// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#pragma once

#include "monitor_geometry.h"

#include <algorithm>
#include <array>
#include <cmath>

// The UI owner supplies fade alpha. Changing these options never changes a
// camera, input mapping, cursor lease or native window's capture exclusion.
struct GpuScopeIndicator
{
    double alpha = 0;
    int style = 0, sizePercent = 20, opacity = 70, border = 2;
    bool details = false;
    int xPercent = 100, yPercent = 100;
    int overviewOpacity = 55;
};

namespace glide::scope
{
    inline GpuScopeIndicator Sanitize(GpuScopeIndicator options) noexcept
    {
        options.alpha = std::isfinite(options.alpha) ? std::clamp(options.alpha, 0.0, 1.0) : 0;
        options.style = options.style >= 0 && options.style <= 2 ? options.style : 0;
        options.sizePercent = std::clamp(options.sizePercent, 10, 35);
        options.opacity = std::clamp(options.opacity, 10, 90);
        options.overviewOpacity = std::clamp(options.overviewOpacity, 0, 100);
        options.border = std::clamp(options.border, 1, 8);
        options.xPercent = std::clamp(options.xPercent, 0, 100);
        options.yPercent = std::clamp(options.yPercent, 0, 100);
        return options;
    }

    inline constexpr bool Same(const GpuScopeIndicator& a, const GpuScopeIndicator& b) noexcept
    {
        return a.alpha == b.alpha && a.style == b.style && a.sizePercent == b.sizePercent &&
               a.opacity == b.opacity && a.border == b.border && a.details == b.details &&
               a.xPercent == b.xPercent && a.yPercent == b.yPercent && a.overviewOpacity == b.overviewOpacity;
    }

    struct Layout
    {
        Rect overview{}, crop{}, screenCrop{}, details{}; // Host-local physical output pixels.
        double alpha = 0, overviewAlpha = 0, border = 0;
        bool showOverview = false, showScreenOutline = false, showDetails = false;
    };

    inline bool MakeLayout(const monitor::Frame& frame, GpuScopeIndicator requested, Layout& output) noexcept
    {
        if (!monitor::ValidFrame(frame))
            return false;
        const auto options = Sanitize(requested);
        const auto destination = monitor::AsRect(frame.destination);
        const double width = destination.right - destination.left;
        const double height = destination.bottom - destination.top;
        const double margin = std::min({ 12.0, width / 8, height / 8 });
        Layout next;
        next.alpha = options.alpha * options.opacity / 100;
        next.overviewAlpha = options.alpha * options.overviewOpacity / 100;
        next.border = options.border;
        next.showOverview = options.style != 1;
        next.showScreenOutline = options.style != 0;
        next.showDetails = options.details;
        // The full-screen outline always uses original desktop coordinates,
        // translated once to host-local pixels, independently of the preview.
        next.screenCrop = { frame.source.left - destination.left, frame.source.top - destination.top, frame.source.right - destination.left, frame.source.bottom - destination.top };
        if (next.showOverview)
        {
            const double factor = options.sizePercent / 100.0;
            const double w = width * factor, h = height * factor;
            const double left = margin + std::max(0.0, width - 2 * margin - w) * options.xPercent / 100;
            const double top = margin + std::max(0.0, height - 2 * margin - h) * options.yPercent / 100;
            next.overview = { left, top, left + w, top + h };
            // Normalize the actual integer input source crop within the whole
            // captured monitor. This outline is never magnified a second time.
            next.crop = { left + (frame.source.left - destination.left) * factor,
                          top + (frame.source.top - destination.top) * factor,
                          left + (frame.source.right - destination.left) * factor,
                          top + (frame.source.bottom - destination.top) * factor };
        }
        else
        {
            // Original desktop coordinates, translated once to host-local
            // pixels. Deliberately do not apply SourceToScreen or camera zoom.
            next.crop = next.screenCrop;
        }
        const Rect anchor = next.showOverview ? next.overview : next.crop;
        const double detailsWidth = std::min(384.0, width - 2 * margin);
        const double detailsHeight = std::min(48.0, height - 2 * margin);
        const double detailsLeft = std::clamp(anchor.left, margin, width - margin - detailsWidth);
        const double detailsTop = std::clamp(anchor.bottom + 4 + detailsHeight <= height - margin ? anchor.bottom + 4 : anchor.top - 4 - detailsHeight,
                                             margin,
                                             height - margin - detailsHeight);
        next.details = { detailsLeft, detailsTop, detailsLeft + detailsWidth, detailsTop + detailsHeight };
        output = next;
        return true;
    }

    // A four-quad inward stroke leaves the center untouched, including when a
    // high zoom produces a source rectangle narrower than the chosen border.
    inline constexpr std::array<Rect, 4> Stroke(Rect rectangle, double border) noexcept
    {
        border = std::max(0.0, std::min({ border, (rectangle.right - rectangle.left) / 2, (rectangle.bottom - rectangle.top) / 2 }));
        return { Rect{ rectangle.left, rectangle.top, rectangle.right, rectangle.top + border },
                 Rect{ rectangle.left, rectangle.bottom - border, rectangle.right, rectangle.bottom },
                 Rect{ rectangle.left, rectangle.top + border, rectangle.left + border, rectangle.bottom - border },
                 Rect{ rectangle.right - border, rectangle.top + border, rectangle.right, rectangle.bottom - border } };
    }
}
