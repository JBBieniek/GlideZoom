// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license. See LICENSE in the project root for license information.

#include "test_assertions.h"
#include "../src/gpu_shaders.h"

#include <d3dcompiler.h>
#include <wrl.h>

#include <string>

using namespace glide_test;

namespace GlideZoomTests
{
    class GpuShaderTests
    {
    public:
        void AllProductionEntryPointsCompileWithoutAGraphicsDevice()
        {
            struct Entry
            {
                const char* name;
                const char* profile;
            };
            for (const auto entry : { Entry{ "VertexMain", "vs_5_0" }, Entry{ "DesktopMain", "ps_5_0" }, Entry{ "CursorMain", "ps_5_0" }, Entry{ "OverviewMain", "ps_5_0" }, Entry{ "SolidMain", "ps_5_0" }, Entry{ "TextMain", "ps_5_0" } })
            {
                Logger::WriteMessage(entry.name);
                Microsoft::WRL::ComPtr<ID3DBlob> bytecode, errors;
                const HRESULT result = D3DCompile(glide::gpu::MagnifierShader, sizeof(glide::gpu::MagnifierShader) - 1, "GlideZoom production magnifier", nullptr, nullptr, entry.name, entry.profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
                if (errors && errors->GetBufferSize())
                {
                    const std::string diagnostics(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
                    Logger::WriteMessage(diagnostics.c_str());
                }
                Assert::IsTrue(SUCCEEDED(result), L"Production HLSL compilation failed; see the entry-point name and compiler diagnostics above.");
                Assert::IsTrue(bytecode && bytecode->GetBufferSize() > 0, L"The shader compiler must produce real bytecode.");
            }
        }
    };
}

int main()
{
    try
    {
        GlideZoomTests::GpuShaderTests suite;
        suite.AllProductionEntryPointsCompileWithoutAGraphicsDevice();
        std::cout << "PASS: GpuShaderTests (1 methods)\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}