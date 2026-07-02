#pragma once
#include "pch.h"

#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

class D3D11PfxRenderer;

class D3D11PFX_FSR3 {
public:
    explicit D3D11PFX_FSR3( D3D11PfxRenderer* renderer );
    ~D3D11PFX_FSR3();

    bool Init( const INT2& maxInputSize, const INT2& maxOutputSize );
    void Destroy();

    XRESULT Apply(
        ID3D11ShaderResourceView* color,
        ID3D11ShaderResourceView* depth,
        ID3D11ShaderResourceView* motionVectors,
        ID3D11ShaderResourceView* reactiveMask,
        ID3D11ShaderResourceView* transparencyAndCompositionMask,
        ID3D11RenderTargetView* output,
        const INT2& inputSize,
        const INT2& outputSize,
        float deltaTimeMs,
        const float2& jitterOffset,
        const float2& motionVectorScale,
        bool resetAccumulation,
        float cameraFovAngleVertical,
        float cameraNear = 0.1f,
        float cameraFar = 1000.0f,
        bool enableSharpening = true,
        float sharpness = 0.2f );

private:
    D3D11PfxRenderer* Renderer;

    FfxInterface Backend;
    FfxFsr3UpscalerContext* Context;
    void* ScratchMemory;

    INT2 MaxInputSize;
    INT2 MaxOutputSize;
    bool Initialized;
};