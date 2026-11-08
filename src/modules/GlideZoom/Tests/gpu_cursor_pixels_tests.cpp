#include <stdexcept>
#include "../GlideZoom/gpu_cursor_pixels.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <vector>

namespace {
std::size_t checks = 0;
void Check(bool passed, const char* description) {
    ++checks;
    if (!passed) { std::cerr << "FAIL: " << description << '\n'; throw std::runtime_error("GlideZoom assertion failed; see captured output"); }
}
} // namespace

int Run_gpu_cursor_pixels() {
    using namespace glide::gpu;
    const std::array<std::uint32_t, 4> andPixels{0, 0, 0x00ffffffu, 0x00ffffffu};
    const std::array<std::uint32_t, 4> xorPixels{0, 0x00ffffffu, 0, 0x00ffffffu};
    std::vector<std::uint32_t> output;
    Check(BuildCursorPixels(true, 2, 2, {andPixels.data(), 4}, {xorPixels.data(), 4}, output),
          "four monochrome operations encode");
    Check(output == std::vector<std::uint32_t>{0, 0x00ffffffu, 0xff000000u, 0xffffffffu},
          "black white transparent inverted encodings");
    for (unsigned background = 0; background != 256; ++background) {
        for (unsigned index = 0; index != 4; ++index) {
            const unsigned andBit = andPixels[index] ? 255 : 0;
            const unsigned xorBit = xorPixels[index] ? 255 : 0;
            const unsigned expected = (background & andBit) ^ xorBit;
            const auto encoded = output[index];
            const unsigned actual = (encoded & 0xff000000u)
                ? ((encoded & 255u) ? 255 - background : background)
                : (encoded & 255u);
            Check(actual == expected, "monochrome encoding agrees with AND/XOR truth table");
        }
    }

    const std::array<std::uint32_t, 8> colors{
        0, 0x00ffffffu, 0x000000ffu, 0x0000ff00u, 0x00ff0000u,
        0x00123456u, 0x00abcdefu, 0x00808080u};
    std::size_t alphaCases = 0;
    for (unsigned alpha = 0; alpha != 256; ++alpha) {
        for (auto color : colors) {
            std::uint32_t black = 0, white = 0;
            for (unsigned shift : {0u, 8u, 16u}) {
                const unsigned straight = (color >> shift) & 255u;
                const unsigned premultiplied = (straight * alpha + 127u) / 255u;
                black |= premultiplied << shift;
                white |= (premultiplied + 255u - alpha) << shift;
            }
            const auto expected = black | (alpha << 24);
            // Source alpha bytes are deliberately unrelated garbage: only the
            // two observed RGB backgrounds determine coverage.
            black |= 0x3c000000u;
            white |= 0xa7000000u;
            std::uint32_t pixel = 0xdeadbeefu;
            Check(AlphaCursorPixel(black, white, pixel), "valid alpha coverage reconstructs");
            Check(pixel == expected, "alpha and premultiplied color round trip");
            Check(BuildCursorPixels(false, 1, 1, {&black, 1}, {&white, 1}, output),
                  "color conversion integrates pixel reconstruction");
            Check(output.size() == 1 && output[0] == expected, "complete color output");
            ++alphaCases;
        }
    }

    std::uint32_t pixel = 0x76543210u;
    Check(!AlphaCursorPixel(0x00ffffffu, 0, pixel) && pixel == 0x76543210u,
          "color XOR rejected without output mutation");
    Check(!AlphaCursorPixel(0, 0x0000ff00u, pixel) && pixel == 0x76543210u,
          "inconsistent channel opacity rejected");
    Check(!MonochromeCursorPixel(0x000000ffu, 0, pixel) && pixel == 0x76543210u,
          "nonmonochrome mask rejected");
    Check(!MonochromeCursorPixel(0, 0x007f7f7fu, pixel) && pixel == 0x76543210u,
          "nonmonochrome XOR rejected");

    const std::vector<std::uint32_t> unchanged{0x12345678u, 0x9abcdef0u};
    output = unchanged;
    const std::uint32_t black = 0, white = 0x00ffffffu;
    for (const auto dimensions : std::array<std::array<std::uint32_t, 2>, 6>{{
        {0, 1}, {1, 0}, {0, 0}, {1025, 1}, {1, 1025},
        {std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max()}}}) {
        std::size_t size = 77;
        Check(!CursorPixelCount(dimensions[0], dimensions[1], size) && size == 77,
              "invalid dimensions leave count unchanged");
        Check(!BuildCursorPixels(false, dimensions[0], dimensions[1], {&black, 1}, {&white, 1}, output) &&
              output == unchanged, "invalid dimensions leave pixels unchanged");
    }
    for (const auto bad : std::array<CursorPixelSpan, 4>{{{nullptr, 1}, {&black, 0}, {&black, 2}, {nullptr, 0}}}) {
        Check(!BuildCursorPixels(false, 1, 1, bad, {&white, 1}, output) && output == unchanged,
              "invalid first span is atomic");
        Check(!BuildCursorPixels(false, 1, 1, {&black, 1}, bad, output) && output == unchanged,
              "invalid second span is atomic");
    }
    const std::array<std::uint32_t, 2> first{0, 0x00ffffffu}, second{0, 0};
    Check(!BuildCursorPixels(false, 2, 1, {first.data(), 2}, {second.data(), 2}, output) && output == unchanged,
          "late invalid pixel does not publish partial image");
    output = {0x00808080u};
    const std::uint32_t overWhite = 0x00ffffffu;
    Check(BuildCursorPixels(false, 1, 1, {output.data(), 1}, {&overWhite, 1}, output),
          "input aliasing old output is supported");
    Check(output == std::vector<std::uint32_t>{0x80808080u}, "aliased conversion is exact");
    std::size_t largest = 0;
    Check(CursorPixelCount(1024, 1024, largest) && largest == 1024u * 1024u,
          "largest permitted cursor count is bounded");
    std::cout << "PASS: " << checks << " GPU cursor pixel assertions; " << alphaCases
              << " alpha reconstruction cases; 1024 monochrome truth-table cases; bounded/atomic output\n";
    return 0;
}
