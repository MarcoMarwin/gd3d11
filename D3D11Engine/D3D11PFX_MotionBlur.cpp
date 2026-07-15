#include "pch.h"
#include "D3D11PFX_MotionBlur.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "GothicAPI.h"

namespace {
    struct MotionBlurConstants {
        float2 MB_InvResolution;
        float MB_Strength;
        float MB_MaxPixels;
        float MB_DepthTolerance;
        float MB_MinVelocityPixels;
        float2 MB_Padding;
    };
}

D3D11PFX_MotionBlur::D3D11PFX_MotionBlur( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {}
D3D11PFX_MotionBlur::~D3D11PFX_MotionBlur() = default;

XRESULT D3D11PFX_MotionBlur::Render( ID3D11RenderTargetView* outputRTV,
                                     ID3D11ShaderResourceView* sceneSRV,
                                     ID3D11ShaderResourceView* velocitySRV,
                                     ID3D11ShaderResourceView* depthSRV ) {
    if ( !outputRTV || !sceneSRV || !velocitySRV ) {
        return XR_SUCCESS;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>( Engine::GraphicsEngine );
    auto& context = engine->GetContext();
    const INT2 resolution = engine->GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) {
        return XR_SUCCESS;
    }

    auto sceneCopy = FxRenderer->GetTempBuffer();
    sceneSRV->AddRef();
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneView;
    sceneView.Attach( sceneSRV );
    FxRenderer->CopyTextureToRTV( sceneView, sceneCopy->GetRenderTargetView(), resolution );

    auto ps = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_MotionBlur );
    auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    if ( !ps || !vs ) {
        return XR_FAILED;
    }

    vs->Apply();
    ps->Apply();

    MotionBlurConstants cb = {};
    cb.MB_InvResolution = float2( 1.0f / static_cast<float>( resolution.x ), 1.0f / static_cast<float>( resolution.y ) );
    cb.MB_Strength = 0.65f;
    cb.MB_MaxPixels = 10.0f;
    cb.MB_DepthTolerance = 0.0025f;
    cb.MB_MinVelocityPixels = 0.65f;
    ps->GetBuffer( "MotionBlurConstants" ).Update( &cb ).Bind();

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>( resolution.x );
    vp.Height = static_cast<float>( resolution.y );
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports( 1, &vp );
    context->OMSetRenderTargets( 1, &outputRTV, nullptr );

    ID3D11ShaderResourceView* srvs[3] = { sceneCopy->GetShaderResView().Get(), velocitySRV, depthSRV };
    context->PSSetShaderResources( 0, 3, srvs );

    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    FxRenderer->DrawFullScreenQuad();

    ID3D11ShaderResourceView* nullSRVs[3] = {};
    context->PSSetShaderResources( 0, 3, nullSRVs );

    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::DEFAULT_DEPTH_COMP_STATE;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    return XR_SUCCESS;
}

XRESULT D3D11PFX_MotionBlur::Render( RenderToTextureBuffer* ) {
    return XR_SUCCESS;
}