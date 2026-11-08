#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace glide::gpu
{

    constexpr std::uint32_t MaximumCursorDimension = 1024;

    struct CursorPixelSpan
    {
        const std::uint32_t* data = nullptr;
        std::size_t size = 0;
    };

    inline bool CursorPixelCount(std::uint32_t width, std::uint32_t height, std::size_t& output) noexcept
    {
        if (!width || !height || width > MaximumCursorDimension || height > MaximumCursorDimension)
            return false;
        output = static_cast<std::size_t>(width) * height;
        return true;
    }

    // Color cursors are drawn by GDI over black and white backgrounds. Reconstruct
    // a premultiplied sRGB texel from those two results; GDI's alpha byte is ignored.
    // A color-dependent XOR operation cannot be represented as alpha blending and
    // is rejected rather than producing misleading cursor pixels. Ordinary
    // monochrome AND/XOR cursors use the separate encoding below.
    inline bool AlphaCursorPixel(std::uint32_t black, std::uint32_t white, std::uint32_t& output) noexcept
    {
        unsigned alpha = 0, minimumAlpha = 255;
        for (unsigned shift : { 0u, 8u, 16u })
        {
            const unsigned low = (black >> shift) & 255u;
            const unsigned high = (white >> shift) & 255u;
            if (high < low)
                return false;
            const unsigned channelAlpha = 255u - high + low;
            if (low > channelAlpha)
                return false;
            alpha = std::max(alpha, channelAlpha);
            minimumAlpha = std::min(minimumAlpha, channelAlpha);
        }
        // Independent channel rounding is allowed one 8-bit quantization unit.
        if (alpha - minimumAlpha > 1)
            return false;
        output = (black & 0x00ffffffu) | (alpha << 24);
        return true;
    }

    // BGRA texture encoding used by CursorMain: RGB contains the XOR bit and
    // alpha contains the AND bit. The shader preserves all four operations:
    // opaque black, opaque white, transparency, and background inversion.
    inline bool MonochromeCursorPixel(std::uint32_t andPixel, std::uint32_t xorPixel, std::uint32_t& output) noexcept
    {
        const auto andRgb = andPixel & 0x00ffffffu;
        const auto xorRgb = xorPixel & 0x00ffffffu;
        if ((andRgb != 0 && andRgb != 0x00ffffffu) ||
            (xorRgb != 0 && xorRgb != 0x00ffffffu))
            return false;
        output = xorRgb | (andRgb ? 0xff000000u : 0u);
        return true;
    }

    // With monochrome=true, first/second are AND/XOR bitmap rows. Otherwise they
    // are color cursor pixels drawn over black/white. Exact lengths and bounds
    // are checked before access. Input may alias output. Every failure, including
    // allocation failure, preserves the caller's previous complete output.
    inline bool BuildCursorPixels(bool monochrome, std::uint32_t width, std::uint32_t height, CursorPixelSpan first, CursorPixelSpan second, std::vector<std::uint32_t>& output) noexcept
    {
        std::size_t count = 0;
        if (!CursorPixelCount(width, height, count) || !first.data || !second.data ||
            first.size != count || second.size != count)
            return false;
        try
        {
            std::vector<std::uint32_t> next(count);
            for (std::size_t index = 0; index != count; ++index)
            {
                if (monochrome ? !MonochromeCursorPixel(first.data[index], second.data[index], next[index]) : !AlphaCursorPixel(first.data[index], second.data[index], next[index]))
                    return false;
            }
            output.swap(next);
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

} // namespace glide::gpu
