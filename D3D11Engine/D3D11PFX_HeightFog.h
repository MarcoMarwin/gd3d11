#pragma once
#include "D3D11PFX_Effect.h"

class GothicAPI;
class GSky;
struct HeightfogConstantBuffer;

class D3D11PFX_HeightFog :
    public D3D11PFX_Effect {
public:
    D3D11PFX_HeightFog( D3D11PfxRenderer* rnd )
        : D3D11PFX_Effect( rnd ) {
    }
    ~D3D11PFX_HeightFog() override = default;

    static bool BuildConstants(
        GothicAPI* gapi,
        GSky* sky,
        HeightfogConstantBuffer& constants );

    /** Draws this effect to the given buffer */
    XRESULT Render( RenderToTextureBuffer* fxbuffer ) override;
};

