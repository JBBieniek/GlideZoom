// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#include "CppUnitTest.h"
#include "../GlideZoom/scope_indicator.h"

#include <cmath>
#include <limits>

using Microsoft::VisualStudio::CppUnitTestFramework::Assert;

namespace GlideZoomTests
{
    namespace
    {
        glide::monitor::Frame MakeIndicatorFrame(glide::Rect bounds, double zoom)
        {
            glide::monitor::Frame frame;
            const glide::Vec2 center{ (bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2 };
            Assert::IsTrue(glide::monitor::MakeFrame(bounds, glide::AnchorZoom({}, zoom, center, bounds), frame));
            return frame;
        }
        void Near(double actual, double expected)
        {
            Assert::IsTrue(std::isfinite(actual) && std::abs(actual - expected) < 0.000001);
        }
        bool SameRect(glide::Rect a, glide::Rect b)
        {
            return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
        }
    }

    TEST_CLASS (ScopeIndicatorTests)
    {
    public:
        TEST_METHOD (OverviewUsesRawMonitorCoordinatesExactlyOnce)
        {
            for (const auto bounds : { glide::Rect{ -1920, -1080, 0, 0 }, glide::Rect{ 1920, 0, 4480, 1440 } })
            {
                const auto frame = MakeIndicatorFrame(bounds, 3.25);
                const auto original = frame;
                GpuScopeIndicator options;
                options.alpha = 1;
                options.sizePercent = 25;
                glide::scope::Layout layout;
                Assert::IsTrue(glide::scope::MakeLayout(frame, options, layout));
                Assert::IsTrue(layout.showOverview && glide::monitor::SameFrame(frame, original));
                Near(layout.overview.right - layout.overview.left, (bounds.right - bounds.left) * 0.25);
                Near(layout.overview.bottom - layout.overview.top, (bounds.bottom - bounds.top) * 0.25);
                Near(layout.crop.left, layout.overview.left + (frame.source.left - bounds.left) * 0.25);
                Near(layout.crop.top, layout.overview.top + (frame.source.top - bounds.top) * 0.25);
                Near(layout.crop.right, layout.overview.left + (frame.source.right - bounds.left) * 0.25);
                Near(layout.crop.bottom, layout.overview.top + (frame.source.bottom - bounds.top) * 0.25);
            }
        }

        TEST_METHOD (OutlineMarksOriginalSourceInsteadOfMagnifyingIt)
        {
            const glide::Rect bounds{ -2560, 200, 0, 1640 };
            const auto frame = MakeIndicatorFrame(bounds, 4);
            GpuScopeIndicator options;
            options.style = 1;
            options.alpha = 1;
            glide::scope::Layout layout;
            Assert::IsTrue(glide::scope::MakeLayout(frame, options, layout));
            Assert::IsFalse(layout.showOverview);
            Assert::IsTrue(layout.showScreenOutline && SameRect(layout.screenCrop, layout.crop));
            Near(layout.crop.left, frame.source.left - bounds.left);
            Near(layout.crop.top, frame.source.top - bounds.top);
            Near(layout.crop.right - layout.crop.left, static_cast<double>(frame.source.right) - frame.source.left);
            Near(layout.crop.bottom - layout.crop.top, static_cast<double>(frame.source.bottom) - frame.source.top);
            const auto wronglyReprojected = glide::SourceToScreen({ static_cast<double>(frame.source.left), static_cast<double>(frame.source.top) }, frame.transform);
            Assert::IsTrue(std::abs(layout.crop.left - (wronglyReprojected.x - bounds.left)) > 100);
        }

        TEST_METHOD (CombinedStyleKeepsBothCoordinateSystemsAndSharedFade)
        {
            for (const auto bounds : { glide::Rect{ -2560, -1440, 0, 0 }, glide::Rect{ 1920, 200, 4480, 1640 } })
            {
                const auto frame = MakeIndicatorFrame(bounds, 4);
                GpuScopeIndicator options;
                options.alpha = 0.5;
                options.opacity = 80;
                options.overviewOpacity = 80;
                options.style = 2;
                options.sizePercent = 25;
                glide::scope::Layout combined;
                Assert::IsTrue(glide::scope::MakeLayout(frame, options, combined));
                Assert::IsTrue(combined.showOverview && combined.showScreenOutline);
                Near(combined.alpha, 0.4);
                Near(combined.overviewAlpha, 0.4);
                options.style = 0;
                glide::scope::Layout overview;
                Assert::IsTrue(glide::scope::MakeLayout(frame, options, overview));
                options.style = 1;
                glide::scope::Layout outline;
                Assert::IsTrue(glide::scope::MakeLayout(frame, options, outline));
                Assert::IsTrue(SameRect(combined.overview, overview.overview) && SameRect(combined.crop, overview.crop));
                Assert::IsTrue(SameRect(combined.screenCrop, outline.crop));
                Assert::IsFalse(overview.showScreenOutline);
                Near(combined.crop.right - combined.crop.left, (combined.screenCrop.right - combined.screenCrop.left) * 0.25);
                Near(combined.alpha, outline.alpha);
                Near(combined.alpha, overview.alpha);
            }
        }

        TEST_METHOD (OverviewOpacityIsIndependentAndSharesOnlyFadeAndGeometry)
        {
            const auto frame = MakeIndicatorFrame({ -2560, 200, 0, 1640 }, 4);
            GpuScopeIndicator options;
            options.style = 2;
            options.alpha = 0.5;
            options.opacity = 80;
            glide::scope::Layout previous;
            for (const int opacity : { 0, 10, 25, 55, 100 })
            {
                options.overviewOpacity = opacity;
                glide::scope::Layout layout;
                Assert::IsTrue(glide::scope::MakeLayout(frame, options, layout));
                Near(layout.alpha, 0.4);
                Near(layout.overviewAlpha, opacity / 200.0);
                Assert::IsTrue(layout.showOverview && layout.showScreenOutline);
                if (previous.overview.right)
                    Assert::IsTrue(SameRect(previous.overview, layout.overview) && SameRect(previous.crop, layout.crop) &&
                                   SameRect(previous.screenCrop, layout.screenCrop));
                previous = layout;
            }
            const auto original = options;
            options.overviewOpacity = 0;
            Assert::IsFalse(glide::scope::Same(options, original));
            options.overviewOpacity = -1;
            Assert::IsTrue(glide::scope::Sanitize(options).overviewOpacity == 0);
            options.overviewOpacity = 101;
            Assert::IsTrue(glide::scope::Sanitize(options).overviewOpacity == 100);
        }

        TEST_METHOD (PositionUsesAvailableSpaceAndDetailsStayInside)
        {
            const auto frame = MakeIndicatorFrame({ 0, 0, 1920, 1080 }, 2);
            GpuScopeIndicator options;
            options.details = true;
            options.sizePercent = 35;
            glide::scope::Layout first;
            options.xPercent = options.yPercent = 0;
            Assert::IsTrue(glide::scope::MakeLayout(frame, options, first));
            Near(first.overview.left, 12);
            Near(first.overview.top, 12);
            options.xPercent = options.yPercent = 100;
            glide::scope::Layout last;
            Assert::IsTrue(glide::scope::MakeLayout(frame, options, last));
            Near(last.overview.right, 1908);
            Near(last.overview.bottom, 1068);
            options.xPercent = options.yPercent = 50;
            glide::scope::Layout middle;
            Assert::IsTrue(glide::scope::MakeLayout(frame, options, middle));
            Near(middle.overview.left, (first.overview.left + last.overview.left) / 2);
            Near(middle.overview.top, (first.overview.top + last.overview.top) / 2);
            for (const auto layout : { first, middle, last })
                Assert::IsTrue(layout.showDetails && layout.details.left >= 12 && layout.details.top >= 12 &&
                               layout.details.right <= 1908 && layout.details.bottom <= 1068);
        }

        TEST_METHOD (FadeAndSanitizationCannotChangeCameraGeometry)
        {
            const auto frame = MakeIndicatorFrame({ 0, 0, 1920, 1080 }, 2.125);
            GpuScopeIndicator options;
            options.opacity = 80;
            glide::scope::Layout previous;
            for (const double alpha : { 0.0, 0.25, 0.5, 0.75, 1.0, 0.5, 0.0 })
            {
                options.alpha = alpha;
                glide::scope::Layout layout;
                Assert::IsTrue(glide::scope::MakeLayout(frame, options, layout));
                Near(layout.alpha, alpha * 0.8);
                if (previous.overview.right)
                    Assert::IsTrue(SameRect(layout.overview, previous.overview) && SameRect(layout.crop, previous.crop));
                previous = layout;
            }
            options.alpha = std::numeric_limits<double>::quiet_NaN();
            options.style = 99;
            options.sizePercent = -4;
            options.opacity = 900;
            options.border = 100;
            options.xPercent = -1;
            options.yPercent = 101;
            const auto safe = glide::scope::Sanitize(options);
            Assert::IsTrue(safe.alpha == 0 && safe.style == 0 && safe.sizePercent == 10 && safe.opacity == 90 &&
                           safe.border == 8 && safe.xPercent == 0 && safe.yPercent == 100);
            Assert::IsTrue(glide::scope::Same(safe, glide::scope::Sanitize(safe)));
        }

        TEST_METHOD (InwardStrokeNeverFillsOutsideItsCrop)
        {
            for (const auto rectangle : { glide::Rect{ 100, 50, 600, 400 }, glide::Rect{ 0, 0, 0.25, 0.5 } })
            {
                const auto edges = glide::scope::Stroke(rectangle, 8);
                double area = 0;
                for (const auto edge : edges)
                {
                    Assert::IsTrue(edge.left >= rectangle.left && edge.top >= rectangle.top && edge.right <= rectangle.right &&
                                   edge.bottom <= rectangle.bottom && edge.right >= edge.left && edge.bottom >= edge.top);
                    area += (edge.right - edge.left) * (edge.bottom - edge.top);
                }
                const double width = rectangle.right - rectangle.left, height = rectangle.bottom - rectangle.top;
                const double border = std::min({ 8.0, width / 2, height / 2 });
                Near(area, width * height - (width - 2 * border) * (height - 2 * border));
            }
        }

        TEST_METHOD (InvalidGeometryLeavesOutputUntouched)
        {
            auto frame = MakeIndicatorFrame({ 0, 0, 1920, 1080 }, 2);
            frame.source.right = frame.source.left;
            glide::scope::Layout output;
            output.alpha = 0.123;
            Assert::IsFalse(glide::scope::MakeLayout(frame, {}, output));
            Near(output.alpha, 0.123);
        }
    };
}
