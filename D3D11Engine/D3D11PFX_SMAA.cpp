#include "pch.h"
#include "D3D11PFX_SMAA.h"
#include "Logger.h"
#include "Engine.h"
#include "RenderToTextureBuffer.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "GothicAPI.h"

#include <filesystem>

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

D3D11PFX_SMAA::D3D11PFX_SMAA( D3D11PfxRenderer* renderer )
    : D3D11PFX_Effect( renderer ) {
    if ( !Init() ) {
        LogError() << "SMAA initialization failed; the effect will remain disabled.";
    }
}

bool D3D11PFX_SMAA::Init() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !engine || !Engine::GAPI || !engine->GetDevice()
        || !engine->GetContext() ) {
        return false;
    }
    if ( m_Native ) return m_Native->Init();

    const std::wstring startDirectoryText =
        Toolbox::ToWideChar( Engine::GAPI->GetStartDirectory() );
    if ( startDirectoryText.empty() ) return false;

    const std::filesystem::path startDirectory( startDirectoryText );
    auto native = std::make_unique<D3D11SMAA>(
        engine->GetDevice().Get(),
        engine->GetContext().Get(),
        (startDirectory / L"system" / L"GD3D11" / L"Shaders"
            / L"SMAA_Wrapper.hlsl").wstring(),
        (startDirectory / L"system" / L"GD3D11" / L"Textures"
            / L"SMAA_AreaTexDX10.dds").wstring(),
        (startDirectory / L"system" / L"GD3D11" / L"Textures"
            / L"SMAA_SearchTex.dds").wstring() );
    const bool initialized = native->Init();
    m_Native = std::move( native );
    return initialized;
}

XRESULT D3D11PFX_SMAA::RenderPostFX(
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& renderTargetSRV ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    const INT2 resolution = engine ? engine->GetResolution() : INT2();
    if ( !m_Native || !FxRenderer || !engine || !gapi || !context
        || !renderTargetSRV || !FxRenderer->GetTexturePool()
        || !engine->GetLinearSamplerState()
        || resolution.x <= 0 || resolution.y <= 0 ) {
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputRTV;
    context->OMGetRenderTargets( 1, outputRTV.GetAddressOf(), nullptr );
    auto intermediate = FxRenderer->GetTempBuffer();
    if ( !outputRTV || !intermediate
        || !intermediate->GetRenderTargetView()
        || !intermediate->GetShaderResView()
        || intermediate->GetSizeX() < static_cast<UINT>(resolution.x)
        || intermediate->GetSizeY() < static_cast<UINT>(resolution.y)
        || ViewsAlias(
            renderTargetSRV.Get(), intermediate->GetRenderTargetView().Get() )
        || ViewsAlias(
            intermediate->GetShaderResView().Get(), outputRTV.Get() ) ) {
        return XR_FAILED;
    }

    m_Native->OnResize( resolution.x, resolution.y );
    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;

    ScopedTrackedRendererState trackedState( engine, gapi );
    engine->SetDefaultStates();
    if ( engine->UpdateRenderStates() != XR_SUCCESS ) {
        trackedState.Restore();
        return XR_FAILED;
    }

    Microsoft::WRL::ComPtr<ID3D11SamplerState> previousSampler;
    context->PSGetSamplers( 0, 1, previousSampler.GetAddressOf() );
    ID3D11SamplerState* linearSampler = engine->GetLinearSamplerState();
    context->PSSetSamplers( 0, 1, &linearSampler );

    XRESULT result = m_Native->Render(
        renderTargetSRV.Get(),
        intermediate->GetRenderTargetView().Get(),
        FxRenderer->GetTexturePool() )
        ? FxRenderer->CopyTextureToRTV(
            intermediate->GetShaderResView(), outputRTV, resolution )
        : XR_FAILED;

    ID3D11SamplerState* samplerToRestore = previousSampler.Get();
    context->PSSetSamplers( 0, 1, &samplerToRestore );
    if ( !trackedState.Restore() ) result = XR_FAILED;
    return result;
}

void D3D11PFX_SMAA::OnResize( const INT2& size ) {
    if ( m_Native ) m_Native->OnResize( size.x, size.y );
}

void D3D11PFX_SMAA::ReleaseResources() {
    if ( m_Native ) m_Native->ReleaseResources();
}
