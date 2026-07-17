#include "pch.h"
#include "D3D11PFX_SimpleSharpen.h"
#include "D3D11PfxRenderer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShaderManager.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "Engine.h"
#include "GothicAPI.h"

#include <algorithm>
#include <cmath>

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
        return sourceResource && sourceResource.Get() == destinationResource.Get();
    }
}

XRESULT D3D11PFX_SimpleSharpen::Apply(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& source,
    INT2 sourceSize,
    RenderToTextureBuffer* dest,
    INT2 destSize ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !Renderer || !engine || !gapi || !context || !source || !dest
        || !dest->IsValid() || !dest->GetRenderTargetView()
        || sourceSize.x <= 0 || sourceSize.y <= 0
        || destSize.x <= 0 || destSize.y <= 0
        || dest->GetSizeX() < static_cast<UINT>(destSize.x)
        || dest->GetSizeY() < static_cast<UINT>(destSize.y) ) {
        return XR_INVALID_ARG;
    }

    const auto destination = dest->GetRenderTargetView();
    if ( ViewsAlias( source.Get(), destination.Get() )
        || !engine->GetLinearSamplerState() ) {
        return XR_INVALID_ARG;
    }

    const auto sharpenPS =
        engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Sharpen );
    if ( !sharpenPS || sharpenPS->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    const float configuredStrength =
        gapi->GetRendererState().RendererSettings.SharpenFactor;
    if ( !std::isfinite( configuredStrength ) ) return XR_INVALID_ARG;

    PfxSharpenConstantBuffer constants{};
    constants.G_TextureSize = sourceSize;
    constants.G_SharpenStrength = std::clamp( configuredStrength, 0.0f, 1.0f );
    auto constantBuffer = sharpenPS->GetBuffer( "PfxSharpenConstantBuffer" );
    constantBuffer.Update( &constants ).Bind();
    if ( !constantBuffer.Succeeded() ) return XR_FAILED;

    ScopedTrackedRendererState trackedState( engine, gapi );
    engine->SetDefaultStates();

    Microsoft::WRL::ComPtr<ID3D11SamplerState> previousSampler;
    context->PSGetSamplers( 0, 1, previousSampler.GetAddressOf() );
    ID3D11SamplerState* linearSampler = engine->GetLinearSamplerState();
    context->PSSetSamplers( 0, 1, &linearSampler );

    XRESULT result = Renderer->CopyTextureToRTV(
        source, destination, destSize, true );

    ID3D11SamplerState* samplerToRestore = previousSampler.Get();
    context->PSSetSamplers( 0, 1, &samplerToRestore );
    if ( !trackedState.Restore() ) result = XR_FAILED;
    return result;
}
