#pragma once
#include "D3D11PFX_Effect.h"

class D3D11PFX_MotionBlur : public D3D11PFX_Effect {
public:
    explicit D3D11PFX_MotionBlur( D3D11PfxRenderer* rnd );
    ~D3D11PFX_MotionBlur() override;

    XRESULT Render( ID3D11RenderTargetView* outputRTV,
                    ID3D11ShaderResourceView* sceneSRV,
                    ID3D11ShaderResourceView* velocitySRV,
                    ID3D11ShaderResourceView* depthSRV );
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override;
};