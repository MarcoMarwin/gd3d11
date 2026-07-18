#pragma once
#include "pch.h"
#include "RenderToTextureBuffer.h"

class D3D11PfxRenderer;

class D3D11PFX_CAS {
public:
    explicit D3D11PFX_CAS( D3D11PfxRenderer* renderer );
    ~D3D11PFX_CAS() = default;

    /** Applies CAS sharpening, using an intermediate target for in-place output. */
    XRESULT Apply(
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input,
        INT2 inputSize,
        const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& target,
        INT2 outputSize,
        RenderToTextureBuffer& intermediateBuffer );

    /** Sets sharpening intensity (0.0 = none, 1.0 = maximum). */
    void SetSharpness( float sharpness );

private:
    D3D11PfxRenderer* Renderer;
    float Sharpness;
};
