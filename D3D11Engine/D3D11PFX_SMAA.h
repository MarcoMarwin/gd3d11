#pragma once
#include "pch.h"
#include "D3D11PFX_Effect.h"
#include "SMAA/D3D11SMAA.h"

struct RenderToTextureBuffer;

class D3D11PFX_SMAA : public D3D11PFX_Effect {
public:
    explicit D3D11PFX_SMAA( D3D11PfxRenderer* renderer );
    ~D3D11PFX_SMAA() override = default;

    bool Init();
    void OnResize( const INT2& size );
    XRESULT RenderPostFX(
        const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& renderTargetSRV );

    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override {
        return XR_INVALID_ARG;
    }

    void ReleaseResources();

private:
    std::unique_ptr<D3D11SMAA> m_Native;
};
