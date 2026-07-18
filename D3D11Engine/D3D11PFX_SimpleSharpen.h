#pragma once
#include "pch.h"
#include "RenderToTextureBuffer.h"

class D3D11PfxRenderer;

class D3D11PFX_SimpleSharpen {
public:
    explicit D3D11PFX_SimpleSharpen( D3D11PfxRenderer* renderer )
        : Renderer( renderer ) {
    }

    ~D3D11PFX_SimpleSharpen() = default;

    /** Applies unsharp-mask sharpening from a source texture into a different
        destination render target. */
    XRESULT Apply(
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& source,
        INT2 sourceSize,
        RenderToTextureBuffer* dest,
        INT2 destSize );

private:
    D3D11PfxRenderer* Renderer;
};
