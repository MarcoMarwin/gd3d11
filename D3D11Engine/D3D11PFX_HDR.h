#pragma once
#include "D3D11PFX_Effect.h"
#include <memory>

struct RenderToTextureBuffer;
class D3D11PFX_HDR : public D3D11PFX_Effect {
public:
    explicit D3D11PFX_HDR( D3D11PfxRenderer* rnd );
    ~D3D11PFX_HDR() override;

    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override { return XR_FAILED; }
    XRESULT Render( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* backbuffer );
    void ResetAdaptation();

protected:
    RenderToTextureBuffer* CalcLuminance();
    XRESULT CreateBloom( RenderToTextureBuffer* lum, RenderToTextureBuffer* bloomTempBuffer );

    std::unique_ptr<RenderToTextureBuffer> LumBuffer1;
    std::unique_ptr<RenderToTextureBuffer> LumBuffer2;
    std::unique_ptr<RenderToTextureBuffer> LumBuffer3;
    int ActiveLumBuffer = 0;
};