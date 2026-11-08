// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#pragma once

namespace glide::gpu
{
    inline constexpr char MagnifierShader[] = R"hlsl(
cbuffer DrawData : register(b0) {
    float4 source;
    float4 rectangle;
    float4 viewport;
    float4 color;
};
Texture2D desktop : register(t0);
Texture2D cursorImage : register(t1);
SamplerState linearClamp : register(s0);
SamplerState pointClamp : register(s1);
struct Vertex { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
Vertex VertexMain(uint index : SV_VertexID) {
    const float2 corners[6] = {
        float2(0, 0), float2(1, 0), float2(0, 1),
        float2(0, 1), float2(1, 0), float2(1, 1)
    };
    Vertex result;
    result.uv = corners[index];
    float2 projected = lerp(rectangle.xy, rectangle.zw, result.uv);
    result.position = float4(projected.x * 2 - 1, 1 - projected.y * 2, 0, 1);
    return result;
}
float3 ToLinear(float3 value) {
    return lerp(value / 12.92, pow((value + 0.055) / 1.055, 2.4), step(0.04045, value));
}
float3 ToSrgb(float3 value) {
    value = saturate(value);
    return lerp(value * 12.92, 1.055 * pow(value, 1.0 / 2.4) - 0.055, step(0.0031308, value));
}
float4 DesktopMain(Vertex input) : SV_TARGET {
    return float4(desktop.Sample(linearClamp, lerp(source.xy, source.zw, input.uv)).rgb, 1);
}
float4 OverviewMain(Vertex input) : SV_TARGET {
    return float4(desktop.Sample(linearClamp, input.uv).rgb * color.a, color.a);
}
float4 SolidMain(Vertex input) : SV_TARGET {
    return color;
}
float4 TextMain(Vertex input) : SV_TARGET {
    float coverage = cursorImage.Sample(linearClamp, input.uv).r;
    return color * coverage;
}
float4 CursorMain(Vertex input) : SV_TARGET {
    float4 pixel = cursorImage.Sample(pointClamp, input.uv);
    if (viewport.z > 0.5) {
        // Monochrome AND/XOR cursor: preserve transparency and inversion.
        if (pixel.a < 0.5) return float4(pixel.rgb, 1);
        if (pixel.r < 0.5) return float4(0, 0, 0, 0);
        float2 uv = input.position.xy / viewport.xy;
        float3 below = desktop.Sample(linearClamp, lerp(source.xy, source.zw, uv)).rgb;
        return float4(ToLinear(1 - ToSrgb(below)), 1);
    }
    if (pixel.a <= 0) return float4(0, 0, 0, 0);
    // GDI supplied premultiplied sRGB. Convert its straight color to scRGB
    // before blending onto the floating-point desktop capture.
    return float4(ToLinear(saturate(pixel.rgb / pixel.a)) * pixel.a, pixel.a);
}
)hlsl";
}
