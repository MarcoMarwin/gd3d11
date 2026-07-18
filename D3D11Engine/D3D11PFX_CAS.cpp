#include "pch.h"
#include "D3D11PFX_CAS.h"

#include "D3D11PfxRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShaderManager.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "Engine.h"
#include "GothicAPI.h"

#include <algorithm>
#include <cmath>

#define FFX_CPU
#define FFX_HLSL
#include "Shaders/FidelityFX/ffx_core.h"
#include "Shaders/FidelityFX/cas/ffx_cas.h"

namespace {
    class ScopedTrackedRendererState {
    public:
        ScopedTrackedRendererState( D3D11GraphicsEngine* graphicsEngine, GothicAPI* gapi )
            : GraphicsEngine( graphicsEngine ),
            GAPI( gapi ),
            BlendState( gapi->GetRendererState().BlendState ),
            DepthState( gapi->GetRendererState().DepthState ),
            RasterizerState( gapi->GetRendererState().RasterizerState ) {
        }

        ~ScopedTrackedRendererState() {
            Restore();
        }

        bool Restore() {
            if ( Restored ) return true;
            Restored = true;
            if ( !GraphicsEngine || !GAPI ) return false;

            auto& state = GAPI->GetRendererState();
            state.BlendState = BlendState;
            state.DepthState = DepthState;
            state.RasterizerState = RasterizerState;
            state.BlendState.SetDirty();
            state.DepthState.SetDirty();
            state.RasterizerState.SetDirty();
            return GraphicsEngine->UpdateRenderStates() == XR_SUCCESS;
        }

    private:
        D3D11GraphicsEngine* GraphicsEngine;
        GothicAPI* GAPI;
        GothicBlendStateInfo BlendState;
        GothicDepthBufferStateInfo DepthState;
        GothicRasterizerStateInfo RasterizerState;
        bool Restored = false;
    };

    bool ViewsAlias(
        ID3D11ShaderResourceView* source,
        ID3D11RenderTargetView* destination ) {
        if ( !source || !destination ) return false;

        Microsoft::WRL::ComPtr<ID3D11Resource> sourceResource;
        Microsoft::WRL::ComPtr<ID3D11Resource> destinationResource;
        source->GetResource( sourceResource.GetAddressOf() );
        destination->GetResource( destinationResource.GetAddressOf() );
        return sourceResource
            && sourceResource.Get() == destinationResource.Get();
    }
}

D3D11PFX_CAS::D3D11PFX_CAS( D3D11PfxRenderer* renderer )
    : Renderer( renderer ), Sharpness( 0.1f ) {
}

void D3D11PFX_CAS::SetSharpness( float sharpness ) {
    if ( std::isfinite( sharpness ) ) {
        Sharpness = std::clamp( sharpness, 0.0f, 1.0f );
    }
}

XRESULT D3D11PFX_CAS::Apply(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& inputTexture,
    INT2 inputSize,
    const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& outputTexture,
    INT2 outputSize,
    RenderToTextureBuffer& intermediateBuffer ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    const auto intermediateRTV = intermediateBuffer.GetRenderTargetView();
    const auto intermediateSRV = intermediateBuffer.GetShaderResView();
    if ( !Renderer || !engine || !gapi || !context || !inputTexture
        || !outputTexture || !intermediateBuffer.IsValid()
        || !intermediateRTV || !intermediateSRV || !std::isfinite( Sharpness )
        || inputSize.x <= 0 || inputSize.y <= 0
        || outputSize.x <= 0 || outputSize.y <= 0
        || intermediateBuffer.GetSizeX() < static_cast<UINT>(outputSize.x)
        || intermediateBuffer.GetSizeY() < static_cast<UINT>(outputSize.y)
        || ViewsAlias( inputTexture.Get(), intermediateRTV.Get() )
        || ViewsAlias( intermediateSRV.Get(), outputTexture.Get() ) ) {
        return XR_INVALID_ARG;
    }

    const auto casPS =
        engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_CAS );
    if ( !casPS || casPS->Apply() != XR_SUCCESS ) return XR_FAILED;

    CASConstantBuffer constants{};
    ffxCasSetup(
        constants.const0,
        constants.const1,
        static_cast<FfxFloat32>(Sharpness),
        static_cast<FfxFloat32>(inputSize.x),
        static_cast<FfxFloat32>(inputSize.y),
        static_cast<FfxFloat32>(outputSize.x),
        static_cast<FfxFloat32>(outputSize.y) );

    auto constantBuffer = casPS->GetBuffer( "CASConstants" );
    constantBuffer.Update( &constants ).Bind();
    if ( !constantBuffer.Succeeded() ) return XR_FAILED;

    ScopedTrackedRendererState trackedState( engine, gapi );
    engine->SetDefaultStates();

    XRESULT result = Renderer->CopyTextureToRTV(
        inputTexture, intermediateRTV, outputSize, true );
    if ( result == XR_SUCCESS ) {
        result = Renderer->CopyTextureToRTV(
            intermediateSRV, outputTexture, outputSize );
    }

    if ( !trackedState.Restore() ) result = XR_FAILED;
    return result;
}
