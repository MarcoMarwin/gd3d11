#pragma once
#include "pch.h"
#include <array>

class D3D11PFXOutputStateGuard {
public:
    explicit D3D11PFXOutputStateGuard( ID3D11DeviceContext* context )
        : Context( context ) {
        if ( !Context ) return;

        Context->OMGetRenderTargets(
            static_cast<UINT>(RenderTargets.size()), RenderTargets.data(),
            DepthStencil.GetAddressOf() );
        ViewportCount = static_cast<UINT>(Viewports.size());
        Context->RSGetViewports( &ViewportCount, Viewports.data() );
        Captured = true;
    }

    ~D3D11PFXOutputStateGuard() {
        if ( Captured ) {
            Context->OMSetRenderTargets(
                static_cast<UINT>(RenderTargets.size()), RenderTargets.data(),
                DepthStencil.Get() );
            Context->RSSetViewports(
                ViewportCount, ViewportCount ? Viewports.data() : nullptr );
        }

        for ( auto* renderTarget : RenderTargets ) {
            if ( renderTarget ) renderTarget->Release();
        }
    }

    D3D11PFXOutputStateGuard( const D3D11PFXOutputStateGuard& ) = delete;
    D3D11PFXOutputStateGuard& operator=( const D3D11PFXOutputStateGuard& ) = delete;

    bool IsValid() const { return Captured; }

private:
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context;
    std::array<ID3D11RenderTargetView*,
        D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> RenderTargets{};
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> DepthStencil;
    std::array<D3D11_VIEWPORT,
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> Viewports{};
    UINT ViewportCount = 0;
    bool Captured = false;
};

struct RenderToTextureBuffer;
class D3D11PfxRenderer;
class D3D11PFX_Effect {
public:
    D3D11PFX_Effect( D3D11PfxRenderer* rnd ) :
        FxRenderer( rnd ) {
    }
    virtual ~D3D11PFX_Effect() = default;

    /** Draws this effect to the given buffer */
    virtual XRESULT Render( RenderToTextureBuffer* fxbuffer ) = 0;
protected:
    /** FX-Object */
    D3D11PfxRenderer* FxRenderer;
};

