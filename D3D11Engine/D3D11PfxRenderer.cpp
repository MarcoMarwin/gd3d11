#include "pch.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11ShaderManager.h"
#include "D3D11PShader.h"
#include "D3D11VShader.h"
#include "D3D11PFX_Blur.h"
#include "D3D11PFX_HeightFog.h"
#include "D3D11PFX_DistanceBlur.h"
#include "D3D11PFX_HDR.h"
#include "D3D11PFX_SMAA.h"
#include "D3D11PFX_GodRays.h"
#include "D3D11PFX_DepthOfField.h"
#include "D3D11PFX_SimpleSharpen.h"
#include "D3D11PFX_CAS.h"
#include "D3D11PFX_FSR3.h"
#include "D3D11PFX_XeGTAO.h"
#include "D3D11PFX_Effect.h"
#include "D3D11Effect.h"
#include "D3D11ShadowMap.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "GSky.h"
#include <cmath>
#include <array>
#include <algorithm>

namespace {
    bool IsFiniteMatrix( const XMFLOAT4X4& matrix ) {
        for ( size_t row = 0; row < 4; ++row ) {
            for ( size_t column = 0; column < 4; ++column ) {
                if ( !std::isfinite( matrix.m[row][column] ) ) return false;
            }
        }
        return true;
    }

    bool HasUsableProjection( const XMFLOAT4X4& projection ) {
        return IsFiniteMatrix( projection )
            && std::abs( projection._11 ) > 1.0e-6f
            && std::abs( projection._22 ) > 1.0e-6f;
    }

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

D3D11PfxRenderer::D3D11PfxRenderer() {

    auto engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    m_texturePool = std::make_unique<TexturePool>( engine->GetDevice().Get() );
    m_depthStencilPool = std::make_unique<DepthStencilPool>( engine->GetDevice().Get() );

    FX_Blur = std::make_unique<D3D11PFX_Blur>( this );
    FX_HeightFog = std::make_unique<D3D11PFX_HeightFog>( this );
    //FX_DistanceBlur = std::make_unique<D3D11PFX_DistanceBlur>( this );
    FX_HDR = std::make_unique<D3D11PFX_HDR>( this );
    FX_GodRays = std::make_unique<D3D11PFX_GodRays>( this );
    FX_DepthOfField = std::make_unique<D3D11PFX_DepthOfField>( this );

    if ( !FeatureLevel10Compatibility ) {
        FX_SMAA = std::make_unique<D3D11PFX_SMAA>( this );
        PFX_XeGTAO = std::make_unique<D3D11PFX_XeGTAO>( this );
        PFX_FSR3 = std::make_unique<D3D11PFX_FSR3>( this );
    }

    PFX_CAS = std::make_unique<D3D11PFX_CAS>( this );
    PFX_SimpleSharpen = std::make_unique<D3D11PFX_SimpleSharpen>( this );
}

D3D11PfxRenderer::~D3D11PfxRenderer() = default;

/** Renders the distance blur effect */
XRESULT D3D11PfxRenderer::RenderDistanceBlur(ID3D11ShaderResourceView* diffuse ) {
    FX_DistanceBlur->Render( diffuse );
    return XR_SUCCESS;
}

/** Blurs the given texture */
XRESULT D3D11PfxRenderer::BlurTexture( RenderToTextureBuffer* texture, bool leaveResultInD4_2, float scale, const XMFLOAT4& colorMod, PShaderID finalCopyShader ) {
    FX_Blur->RenderBlur( texture, leaveResultInD4_2, 0.0f, scale, colorMod, finalCopyShader );
    return XR_SUCCESS;
}

/** Renders the heightfog */
XRESULT D3D11PfxRenderer::RenderHeightfog() {
    return FX_HeightFog->Render( nullptr );
}

/** Renders the godrays-Effect */
XRESULT D3D11PfxRenderer::RenderGodRays(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* lowClouds ) {
    return FX_GodRays && backbuffer && depth
        ? FX_GodRays->Render( backbuffer, depth, lowClouds )
        : XR_INVALID_ARG;
}

/** Renders the depth-of-field effect */
XRESULT D3D11PfxRenderer::RenderDepthOfField( ID3D11ShaderResourceView* backbuffer ) {
    return FX_DepthOfField->Render( backbuffer );
}

XRESULT D3D11PfxRenderer::RenderWetGroundSSR(
    ID3D11RenderTargetView* outputRTV,
    ID3D11ShaderResourceView* sceneSRV,
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11ShaderResourceView* waterMaskSRV ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    auto* rainShadow = engine->Effects ? engine->Effects->GetRainShadowmap() : nullptr;
    auto* shadowMaps = engine->GetShadowMaps();
    if ( !outputRTV || !sceneSRV || !depthSRV || !normalsSRV || !waterMaskSRV || !rainShadow || !shadowMaps ) {
        return XR_FAILED;
    }

    auto ps = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_WetGroundSSR );
    auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    ps->Apply();
    vs->Apply();

    WetGroundSSRConstantBuffer cb = {};
    auto& projection = Engine::GAPI->GetProjectionMatrix();
    cb.WG_ProjParams = float4( 1.0f / projection._11, 1.0f / projection._22, projection._43, projection._33 );
    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    XMStoreFloat4x4( &cb.WG_InvView, XMMatrixInverse( nullptr, view ) );
    XMStoreFloat4x4( &cb.WG_ViewProj, XMLoadFloat4x4( &projection ) * view );

    auto& rainCamera = engine->Effects->GetRainShadowmapCameraRepl();
    XMStoreFloat4x4( &cb.WG_RainViewProj,
        XMLoadFloat4x4( &rainCamera.ProjectionReplacement ) *
        XMLoadFloat4x4( &rainCamera.ViewReplacement ) );

    cb.WG_CameraPosition = Engine::GAPI->GetCameraPosition();
    cb.WG_Wetness = Engine::GAPI->GetSceneWetness();
    const INT2 resolution = engine->GetResolution();
    cb.WG_InvResolution = float2( 1.0f / std::max( resolution.x, 1 ), 1.0f / std::max( resolution.y, 1 ) );
    cb.WG_Strength = Engine::GAPI->GetRendererState().RendererSettings.SSRStrength * 0.84f;
    cb.WG_Time = Engine::GAPI->GetTimeSeconds();
    cb.WG_RainFXWeight = Engine::GAPI->GetRainFXWeight();
    ps->GetBuffer( "WetGroundSSRConstantBuffer" ).Update( &cb ).Bind();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> previousRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> previousDSV;
    context->OMGetRenderTargets( 1, previousRTV.GetAddressOf(), previousDSV.GetAddressOf() );

    context->OMSetRenderTargets( 1, &outputRTV, nullptr );
    ID3D11ShaderResourceView* resources[4] = {
        sceneSRV,
        depthSRV,
        normalsSRV,
        rainShadow->GetShaderResView().Get()
    };
    context->PSSetShaderResources( 0, 4, resources );
    engine->GetDistortionTexture()->BindToPixelShader( 4 );
    context->PSSetShaderResources( 5, 1, &waterMaskSRV );

    ID3D11SamplerState* samplers[2] = {
        engine->GetClampSamplerState(),
        shadowMaps->GetShadowmapSampler()
    };
    context->PSSetSamplers( 0, 2, samplers );

    engine->SetDefaultStates();
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
    engine->SetViewport( ViewportInfo( 0, 0, resolution ) );

    DrawFullScreenQuad();

    ID3D11ShaderResourceView* nullResources[7] = {};
    context->PSSetShaderResources( 0, 7, nullResources );
    context->OMSetRenderTargets( 1, previousRTV.GetAddressOf(), previousDSV.Get() );
    return XR_SUCCESS;
}

/** Renders the HDR-Effect */
XRESULT D3D11PfxRenderer::RenderHDR( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* backbuffer ) {
    return FX_HDR->Render( output, backbuffer );
}

void D3D11PfxRenderer::ResetHDRAdaptation() {
    if ( FX_HDR ) {
        FX_HDR->ResetAdaptation();
    }
}

/** Renders the SMAA-Effect */
XRESULT D3D11PfxRenderer::RenderSMAA(ID3D11ShaderResourceView* backbuffer) {
    FX_SMAA->RenderPostFX( backbuffer );
    return XR_SUCCESS;
}

XRESULT D3D11PfxRenderer::RenderCAS( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input, INT2 inputSize, const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& output, INT2 outputSize, RenderToTextureBuffer& intermediateBuffer ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    PFX_CAS->SetSharpness( Engine::GAPI->GetRendererState().RendererSettings.SharpenFactor );
    PFX_CAS->Apply(
        input ? input : engine->GetHDRBackBuffer().GetShaderResView(),
        input ? inputSize : engine->GetResolution(),
        output ? output : engine->GetHDRBackBuffer().GetRenderTargetView(),
        output ? outputSize : engine->GetResolution(),
        intermediateBuffer );
    return XR_SUCCESS;
}

XRESULT D3D11PfxRenderer::RenderSimpleSharpen( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& source, INT2 sourceSize, RenderToTextureBuffer* dest, INT2 destSize ) {
    PFX_SimpleSharpen->Apply( source, sourceSize, dest, destSize );
    return XR_SUCCESS;
}

/** Draws a fullscreenquad */
XRESULT D3D11PfxRenderer::DrawFullScreenQuad() {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    engine->UpdateRenderStates();

    engine->GetContext()->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );

    //Draw the mesh
    engine->GetContext()->Draw( 3, 0 );

    return XR_SUCCESS;
}

/** Unbinds texturesamplers from the pixel-shader */
XRESULT D3D11PfxRenderer::UnbindPSResources( int num ) {
    ID3D11ShaderResourceView** srv = new ID3D11ShaderResourceView*[num];
    ZeroMemory( srv, sizeof( ID3D11ShaderResourceView* ) * num );
    reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine)->GetContext()->PSSetShaderResources( 0, num, srv );
    delete[] srv;

    return XR_SUCCESS;
}

/** Copies the given texture to the given RTV */
XRESULT D3D11PfxRenderer::CopyTextureToRTV( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& texture, const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv, INT2 targetResolution, bool useCustomPS, INT2 offset, ID3D11RenderTargetView* extraRTV ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

    D3D11_VIEWPORT oldVP;
    if ( targetResolution.x != 0 && targetResolution.y != 0 ) {
        UINT n = 1;
        engine->GetContext()->RSGetViewports( &n, &oldVP );

        D3D11_VIEWPORT vp;
        vp.TopLeftX = static_cast<float>(offset.x);
        vp.TopLeftY = static_cast<float>(offset.y);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        vp.Width = static_cast<float>(targetResolution.x);
        vp.Height = static_cast<float>(targetResolution.y);

        engine->GetContext()->RSSetViewports( 1, &vp );
    }

    // Save old rendertargets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    // Bind shaders
    if ( !useCustomPS ) {
        auto simplePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Simple );
        simplePS->Apply();
    }

    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();

    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    engine->GetContext()->PSSetShaderResources( 0, 1, srv.GetAddressOf() );

    Microsoft::WRL::ComPtr<ID3D11BlendState> oldBlendState;
    FLOAT oldBlendFactor[4] = {};
    UINT oldSampleMask = 0xffffffff;
    if ( extraRTV ) {
        engine->GetContext()->OMGetBlendState( oldBlendState.GetAddressOf(), oldBlendFactor, &oldSampleMask );

        static Microsoft::WRL::ComPtr<ID3D11BlendState> s_extraMaskBlendState;
        if ( !s_extraMaskBlendState ) {
            D3D11_BLEND_DESC blendDesc = {};
            blendDesc.IndependentBlendEnable = TRUE;
            blendDesc.RenderTarget[0].BlendEnable = FALSE;
            blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            blendDesc.RenderTarget[1].BlendEnable = TRUE;
            blendDesc.RenderTarget[1].SrcBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[1].DestBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[1].BlendOp = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[1].SrcBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[1].DestBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[1].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED;
            engine->GetDevice()->CreateBlendState( &blendDesc, s_extraMaskBlendState.GetAddressOf() );
        }
        if ( s_extraMaskBlendState ) {
            const FLOAT blendFactor[4] = {};
            engine->GetContext()->OMSetBlendState( s_extraMaskBlendState.Get(), blendFactor, 0xffffffff );
        }
    }

    ID3D11RenderTargetView* rtvs[2] = { rtv.Get(), extraRTV };
    engine->GetContext()->OMSetRenderTargets( extraRTV ? 2 : 1, rtvs, nullptr );

    if ( texture.Get() )
        engine->GetContext()->PSSetShaderResources( 0, 1, texture.GetAddressOf() );

    DrawFullScreenQuad();

    if ( extraRTV ) {
        engine->GetContext()->OMSetBlendState( oldBlendState.Get(), oldBlendFactor, oldSampleMask );
    }

    engine->GetContext()->PSSetShaderResources( 0, 1, srv.GetAddressOf() );
    engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

    if ( targetResolution.x != 0 && targetResolution.y != 0 ) {
        engine->GetContext()->RSSetViewports( 1, &oldVP );
    }

    return XR_SUCCESS;
}

/** Called on resize */
XRESULT D3D11PfxRenderer::OnResize( const INT2& newResolution ) {


    if ( PFX_FSR3 ) PFX_FSR3->Destroy();
    ScreenSpaceLightingHistory[0].reset();
    ScreenSpaceLightingHistory[1].reset();
    ScreenSpaceLightingDepthHistory[0].reset();
    ScreenSpaceLightingDepthHistory[1].reset();
    ScreenSpaceLightingHistoryValid = false;
    ScreenSpaceLightingHistoryIndex = 0;
    m_texturePool->Clear(); // textures will be created on demand
    if ( !FeatureLevel10Compatibility ) {
        FX_SMAA->OnResize( newResolution );
    }

    return XR_SUCCESS;
}

XRESULT D3D11PfxRenderer::RenderGodRaysToTexture(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* lowClouds,
    ID3D11ShaderResourceView** outGodRaysSRV ) {
    if ( outGodRaysSRV ) *outGodRaysSRV = nullptr;
    if ( !FX_GodRays || !backbuffer || !depthCopy || !outGodRaysSRV ) {
        return XR_INVALID_ARG;
    }
    return FX_GodRays->RenderToTexture(
        backbuffer, depthCopy, lowClouds, outGodRaysSRV );
}

XRESULT D3D11PfxRenderer::RenderScreenSpaceLighting(
    ID3D11ShaderResourceView* sceneSRV,
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11ShaderResourceView* waterMaskSRV,
    ID3D11ShaderResourceView* materialSRV,
    ID3D11ShaderResourceView* velocitySRV,
    ID3D11ShaderResourceView** outLightingSRV ) {

    if ( outLightingSRV ) {
        *outLightingSRV = nullptr;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    const bool contactActive = settings.EnableContactShadows;
    const bool giActive = settings.EnableScreenSpaceGI && settings.ScreenSpaceGIStrength > 0.0f;
    if ( !sceneSRV || !depthSRV || !normalsSRV || !waterMaskSRV || !materialSRV || (!contactActive && !giActive) ) {
        ScreenSpaceLightingHistoryValid = false;
        return XR_SUCCESS;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    const INT2 res = engine->GetResolution();
    if ( res.x <= 0 || res.y <= 0 ) {
        ScreenSpaceLightingHistoryValid = false;
        return XR_FAILED;
    }

    for ( int i = 0; i < 2; ++i ) {
        if ( !ScreenSpaceLightingHistory[i]
            || ScreenSpaceLightingHistory[i]->GetSizeX() != static_cast<UINT>(res.x)
            || ScreenSpaceLightingHistory[i]->GetSizeY() != static_cast<UINT>(res.y) ) {
            ScreenSpaceLightingHistory[i] = std::make_unique<RenderToTextureBuffer>(
                engine->GetDevice().Get(), res.x, res.y, DXGI_FORMAT_R16G16B16A16_FLOAT );
            ScreenSpaceLightingDepthHistory[i] = std::make_unique<RenderToTextureBuffer>(
                engine->GetDevice().Get(), res.x, res.y, DXGI_FORMAT_R32_FLOAT );
            ScreenSpaceLightingHistoryValid = false;
        }
    }

    auto raw = m_texturePool->Acquire( TexturePool::Description{ res.x, res.y, DXGI_FORMAT_R16G16B16A16_FLOAT } );

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> previousRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> previousDSV;
    context->OMGetRenderTargets( 1, previousRTV.GetAddressOf(), previousDSV.GetAddressOf() );

    ScreenSpaceLightingConstantBuffer cb = {};
    GSky* sky = Engine::GAPI->GetSky();
    const float mainLightVisibility = sky ? sky->GetMainLightVisibility() : 1.0f;
    const XMFLOAT4X4& projection = Engine::GAPI->GetProjectionMatrix();
    cb.SSL_ProjParams = float4( 1.0f / projection._11, 1.0f / projection._22, projection._43, projection._33 );
    cb.SSL_Projection = projection;
    const XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    XMStoreFloat4x4( &cb.SSL_View, view );
    XMStoreFloat4x4( &cb.SSL_InvView, XMMatrixInverse( nullptr, view ) );
    cb.SSL_InvResolution = float2( 1.0f / std::max( 1, res.x ), 1.0f / std::max( 1, res.y ) );
    cb.SSL_ContactStrength = (contactActive ? settings.GetContactShadowFixedStrength() : 0.0f) * mainLightVisibility;
    cb.SSL_GIStrength = settings.ScreenSpaceGIStrength;
    cb.SSL_EnableContact = contactActive ? 1.0f : 0.0f;
    cb.SSL_EnableGI = giActive ? 1.0f : 0.0f;
    cb.SSL_HistoryValid = ScreenSpaceLightingHistoryValid ? 1.0f : 0.0f;
    cb.SSL_FSR3Active = (settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3
        && settings.AntiAliasingMode == GothicRendererSettings::AA_FSR3) ? 1.0f : 0.0f;
    cb.SSL_FrameIndex = static_cast<float>(ScreenSpaceLightingFrameIndex++ & 1023u);
    if ( sky ) {
        const XMFLOAT3 mainLightDirection = sky->GetMainLightDirection();
        const XMVECTOR directionToLightVS = XMVector3Normalize( XMVector3TransformNormal(
            XMLoadFloat3( &mainLightDirection ), view ) );
        XMStoreFloat3( &cb.SSL_LightDirectionVS, directionToLightVS );
    } else {
        cb.SSL_LightDirectionVS = XMFLOAT3( 0.0f, 1.0f, 0.0f );
    }

    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();
    auto tracePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_ScreenSpaceLightingTrace );
    tracePS->Apply();
    tracePS->GetBuffer( "ScreenSpaceLightingConstantBuffer" ).Update( &cb ).Bind();

    ID3D11RenderTargetView* rawRTV = raw->GetRenderTargetView().Get();
    context->OMSetRenderTargets( 1, &rawRTV, nullptr );
    ID3D11ShaderResourceView* traceSRVs[5] = { sceneSRV, depthSRV, normalsSRV, waterMaskSRV, materialSRV };
    context->PSSetShaderResources( 0, 5, traceSRVs );
    ID3D11SamplerState* sampler = engine->GetClampSamplerState();
    context->PSSetSamplers( 0, 1, &sampler );
    engine->SetDefaultStates();
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();
    engine->SetViewport( ViewportInfo( 0, 0, res ) );
    DrawFullScreenQuad();

    const uint32_t readIndex = ScreenSpaceLightingHistoryIndex;
    const uint32_t writeIndex = 1u - readIndex;
    auto temporalPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_ScreenSpaceLightingTemporal );
    temporalPS->Apply();
    cb.SSL_HistoryValid = ScreenSpaceLightingHistoryValid ? 1.0f : 0.0f;
    cb.SSL_FSR3Active = (settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3
        && settings.AntiAliasingMode == GothicRendererSettings::AA_FSR3) ? 1.0f : 0.0f;
    temporalPS->GetBuffer( "ScreenSpaceLightingConstantBuffer" ).Update( &cb ).Bind();

    ID3D11RenderTargetView* temporalRTVs[2] = {
        ScreenSpaceLightingHistory[writeIndex]->GetRenderTargetView().Get(),
        ScreenSpaceLightingDepthHistory[writeIndex]->GetRenderTargetView().Get()
    };
    context->OMSetRenderTargets( 2, temporalRTVs, nullptr );
    ID3D11ShaderResourceView* temporalSRVs[7] = {
        raw->GetShaderResView().Get(),
        ScreenSpaceLightingHistoryValid ? ScreenSpaceLightingHistory[readIndex]->GetShaderResView().Get() : raw->GetShaderResView().Get(),
        depthSRV,
        normalsSRV,
        velocitySRV,
        ScreenSpaceLightingHistoryValid ? ScreenSpaceLightingDepthHistory[readIndex]->GetShaderResView().Get() : depthSRV,
        materialSRV
    };
    context->PSSetShaderResources( 0, 7, temporalSRVs );
    DrawFullScreenQuad();

    ID3D11ShaderResourceView* nullSRVs[7] = {};
    context->PSSetShaderResources( 0, 7, nullSRVs );
    context->OMSetRenderTargets( 1, previousRTV.GetAddressOf(), previousDSV.Get() );

    ScreenSpaceLightingHistoryIndex = writeIndex;
    ScreenSpaceLightingHistoryValid = true;
    if ( outLightingSRV ) {
        *outLightingSRV = ScreenSpaceLightingHistory[writeIndex]->GetShaderResView().Get();
    }
    return XR_SUCCESS;
}
XRESULT D3D11PfxRenderer::RenderPostFXComposition(
    ID3D11RenderTargetView* outputRTV,
    ID3D11ShaderResourceView* backbufferSRV,
    ID3D11ShaderResourceView* godraysSRV,
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11ShaderResourceView* waterMaskSRV,
    ID3D11ShaderResourceView* screenSpaceLightingSRV,
    bool compositionHeightFog ) {

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    auto res = engine->GetResolution();

    // Set up shaders
    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();
    auto compositionPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Composition );
    compositionPS->Apply();

    // Update constants shared by height fog and the view-space ray tracing effects.
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    GSky* sky = Engine::GAPI->GetSky();
    const bool contactShadowsActive = settings.EnableContactShadows;
    const bool screenSpaceGIActive = settings.EnableScreenSpaceGI && settings.ScreenSpaceGIStrength > 0.0f;
    const bool needsAtmosphere = compositionHeightFog || contactShadowsActive || screenSpaceGIActive;
    CompositionControlConstantBuffer control = {};
    control.CC_HeightFogEnabled = compositionHeightFog ? 1.0f : 0.0f;
    control.CC_InvResolution = float2( 1.0f / std::max( 1, res.x ), 1.0f / std::max( 1, res.y ) );
    const XMFLOAT4X4& projection = Engine::GAPI->GetProjectionMatrix();
    control.CC_ProjParams = float4( 1.0f / projection._11, 1.0f / projection._22, projection._43, projection._33 );
    control.CC_Projection = projection;
    if ( sky ) {
        const XMFLOAT3 mainLightDirection = sky->GetMainLightDirection();
        const XMVECTOR directionToLightVS = XMVector3Normalize( XMVector3TransformNormal(
            XMLoadFloat3( &mainLightDirection ), Engine::GAPI->GetViewMatrixXM() ) );
        XMStoreFloat3( &control.CC_LightDirectionVS, directionToLightVS );
    } else {
        control.CC_LightDirectionVS = XMFLOAT3( 0.0f, 1.0f, 0.0f );
    }
    if ( needsAtmosphere )
        compositionPS->GetBuffer( "CompositionControl" ).Update( &control ).Bind();
    if ( compositionHeightFog ) {
        HeightfogConstantBuffer cb;
        {
            auto& proj = Engine::GAPI->GetProjectionMatrix();
            cb.HF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
        }
        XMStoreFloat4x4( &cb.InvView, XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() ) );
        cb.CameraPosition = Engine::GAPI->GetCameraPosition();
        cb.HF_GlobalDensity = settings.FogGlobalDensity;
        cb.HF_HeightFalloff = settings.FogHeightFalloff;

        float height = settings.FogHeight;
        XMVECTOR color = XMLoadFloat3( settings.FogColorMod.toXMFLOAT3() );

        float fnear = 15000.0f;
        float ffar = 60000.0f;
        float secScale = std::min<float>( settings.SectionDrawRadius, settings.FogRange );

        cb.HF_WeightZNear = std::max( 0.0f, WORLD_SECTION_SIZE * ((secScale - 0.5f) * 0.7f) - (ffar - fnear) );
        cb.HF_WeightZFar = WORLD_SECTION_SIZE * ((secScale - 0.5f) * 0.8f);

        float atmoMax = 83200.0f;
        float atmoMin = 27799.9922f;
        cb.HF_WeightZFar = std::min( cb.HF_WeightZFar, atmoMax );
        cb.HF_WeightZNear = std::min( cb.HF_WeightZNear, atmoMin );

#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        float fogDensityFactor = 2;
        float fogDensityFactorRain = (1.0f - Engine::GAPI->GetFogOverride());
#else
        float fogDensityFactor = pow( 15000.0f / Engine::GAPI->GetFarZ(), 4.0f );
        float fogDensityFactorRain = 1.0f;
#endif

        if ( Engine::GAPI->GetFogOverride() > 0.0f ) {
            height = Toolbox::lerp( height, Engine::GAPI->GetCameraPosition().y + 10000, Engine::GAPI->GetFogOverride() );
            color = Engine::GAPI->GetFogColor();
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            cb.HF_HeightFalloff = Toolbox::lerp( cb.HF_HeightFalloff, 0.000001f, Engine::GAPI->GetFogOverride() );
#endif
            cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, cb.HF_GlobalDensity * fogDensityFactor, Engine::GAPI->GetFogOverride() );
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            cb.HF_WeightZNear = Toolbox::lerp( cb.HF_WeightZNear, WORLD_SECTION_SIZE * 0.09f, Engine::GAPI->GetFogOverride() );
            cb.HF_WeightZFar = Toolbox::lerp( cb.HF_WeightZFar, WORLD_SECTION_SIZE * 0.8, Engine::GAPI->GetFogOverride() );
#endif
        }

        cb.HF_FogHeight = height;
        cb.HF_ProjAB = float2( Engine::GAPI->GetProjectionMatrix()._33, Engine::GAPI->GetProjectionMatrix()._34 );

        float rain = sky ? sky->GetAtmosphereCB().AC_RainFXWeight : Engine::GAPI->GetRainFXWeight();
        float rainFogColorWeight = std::min( 1.0f, rain * 2.0f );
        if ( sky ) {
            float daylightRainFog = std::max( 0.0f, std::min( 1.0f, (sky->GetAtmosphereCB().AC_LightPos.y + 0.05f) * 4.0f ) );
            daylightRainFog = daylightRainFog * daylightRainFog * (3.0f - 2.0f * daylightRainFog);
            rainFogColorWeight *= daylightRainFog;
        }
        XMFLOAT3 FogColorMod;
        XMStoreFloat3( &FogColorMod, XMVectorLerpV( color, XMLoadFloat3( &settings.RainFogColor ), XMVectorSet( rainFogColorWeight, rainFogColorWeight, rainFogColorWeight, 0 ) ) );
        cb.HF_FogColorMod = FogColorMod;
        cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, settings.RainFogDensity, rain * fogDensityFactorRain );

        compositionPS->GetBuffer( "PFXBuffer" ).Update( &cb ).Bind();
    }
    if ( needsAtmosphere && sky ) {
        compositionPS->GetBuffer( "Atmosphere" ).Update( &sky->GetAtmosphereCB() ).Bind();
    }

    // Set viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(res.x);
    vp.Height = static_cast<float>(res.y);
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports( 1, &vp );

    // Bind output RTV (no depth)
    context->OMSetRenderTargets( 1, &outputRTV, nullptr );

    // Bind SRVs: t0=backbuffer, t1=GodRays, t2=Depth, t3=Normals, t4=WaterMask, t5=ScreenSpaceLighting
    ID3D11ShaderResourceView* srvs[6] = {
        backbufferSRV, godraysSRV, depthSRV, normalsSRV, waterMaskSRV, screenSpaceLightingSRV
    };
    context->PSSetShaderResources( 0, 6, srvs );

    // No blending - direct overwrite
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    DrawFullScreenQuad();

    // Unbind SRVs
    ID3D11ShaderResourceView* nullSRVs[6] = {};
    context->PSSetShaderResources( 0, 6, nullSRVs );

    // Restore default states
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::DEFAULT_DEPTH_COMP_STATE;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    return XR_SUCCESS;
}

XRESULT D3D11PfxRenderer::RenderLowCloudLayer(
    ID3D11RenderTargetView* cloudLayerRTV,
    ID3D11RenderTargetView* cloudDepthRTV,
    ID3D11ShaderResourceView* sceneSRV,
    ID3D11ShaderResourceView* depthSRV ) {
    if ( !cloudLayerRTV || !cloudDepthRTV || !sceneSRV || !depthSRV ) {
        return XR_INVALID_ARG;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    GSky* sky = gapi ? gapi->GetSky() : nullptr;
    if ( !sky ) return XR_SUCCESS;
    if ( !engine || !gapi || !context
        || !engine->GetClampSamplerState()
        || !engine->GetDefaultSamplerState() ) {
        return XR_FAILED;
    }

    const INT2 resolution = engine->GetResolution();
    const XMFLOAT4X4& projection = gapi->GetProjectionMatrix();
    if ( resolution.x <= 0 || resolution.y <= 0
        || !HasUsableProjection( projection ) ) {
        return XR_FAILED;
    }

    auto lowCloudPS = engine->GetShaderManager().GetPShader(
        PShaderID::PS_PFX_LowClouds );
    auto fullscreenVS =
        engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    if ( !lowCloudPS || !fullscreenVS
        || fullscreenVS->Apply() != XR_SUCCESS
        || lowCloudPS->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    auto& settings = gapi->GetRendererState().RendererSettings;
    HeightfogConstantBuffer cb{};
    cb.HF_ProjParams = float4(
        1.0f / projection._11, 1.0f / projection._22,
        projection._43, projection._33 );
    XMStoreFloat4x4(
        &cb.InvView, XMMatrixInverse( nullptr, gapi->GetViewMatrixXM() ) );
    cb.CameraPosition = gapi->GetCameraPosition();
    cb.HF_GlobalDensity = settings.FogGlobalDensity;
    cb.HF_HeightFalloff = settings.FogHeightFalloff;

    float height = settings.FogHeight;
    XMVECTOR color = XMLoadFloat3( settings.FogColorMod.toXMFLOAT3() );
    const float sectionScale =
        std::min<float>( settings.SectionDrawRadius, settings.FogRange );
    cb.HF_WeightZNear = std::max(
        0.0f, WORLD_SECTION_SIZE * ((sectionScale - 0.5f) * 0.7f) - 45000.0f );
    cb.HF_WeightZFar =
        WORLD_SECTION_SIZE * ((sectionScale - 0.5f) * 0.8f);
    cb.HF_WeightZFar = std::min( cb.HF_WeightZFar, 83200.0f );
    cb.HF_WeightZNear = std::min( cb.HF_WeightZNear, 27799.9922f );

    const float rawFogOverride = gapi->GetFogOverride();
    const float fogOverride = std::isfinite( rawFogOverride )
        ? std::clamp( rawFogOverride, 0.0f, 1.0f ) : 0.0f;
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    const float fogDensityFactor = 2.0f;
    const float fogDensityFactorRain = 1.0f - fogOverride;
#else
    const float rawFarZ = gapi->GetFarZ();
    const float safeFarZ = std::isfinite( rawFarZ )
        ? std::max( std::abs( rawFarZ ), 1.0f ) : 1.0f;
    const float fogDensityFactor = std::pow( 15000.0f / safeFarZ, 4.0f );
    const float fogDensityFactorRain = 1.0f;
#endif

    if ( fogOverride > 0.0f ) {
        height = Toolbox::lerp(
            height, gapi->GetCameraPosition().y + 10000.0f, fogOverride );
        color = gapi->GetFogColor();
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        cb.HF_HeightFalloff = Toolbox::lerp(
            cb.HF_HeightFalloff, 0.000001f, fogOverride );
#endif
        cb.HF_GlobalDensity = Toolbox::lerp(
            cb.HF_GlobalDensity,
            cb.HF_GlobalDensity * fogDensityFactor, fogOverride );
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        cb.HF_WeightZNear = Toolbox::lerp(
            cb.HF_WeightZNear, WORLD_SECTION_SIZE * 0.09f, fogOverride );
        cb.HF_WeightZFar = Toolbox::lerp(
            cb.HF_WeightZFar, WORLD_SECTION_SIZE * 0.8f, fogOverride );
#endif
    }

    cb.HF_FogHeight = height;
    cb.HF_ProjAB = float2( projection._33, projection._34 );

    const float rawRain = sky->GetAtmosphereCB().AC_RainFXWeight;
    const float rain = std::isfinite( rawRain )
        ? std::clamp( rawRain, 0.0f, 1.0f ) : 0.0f;
    float rainFogColorWeight = std::min( 1.0f, rain * 2.0f );
    float daylightRainFog = std::clamp(
        (sky->GetAtmosphereCB().AC_LightPos.y + 0.05f) * 4.0f, 0.0f, 1.0f );
    daylightRainFog =
        daylightRainFog * daylightRainFog * (3.0f - 2.0f * daylightRainFog);
    rainFogColorWeight *= daylightRainFog;

    XMFLOAT3 fogColorMod{};
    XMStoreFloat3( &fogColorMod, XMVectorLerpV(
        color, XMLoadFloat3( &settings.RainFogColor ),
        XMVectorReplicate( rainFogColorWeight ) ) );
    cb.HF_FogColorMod = fogColorMod;
    cb.HF_GlobalDensity = Toolbox::lerp(
        cb.HF_GlobalDensity, settings.RainFogDensity,
        rain * fogDensityFactorRain );

    auto pfxBuffer = lowCloudPS->GetBuffer( "PFXBuffer" );
    auto atmosphereBuffer = lowCloudPS->GetBuffer( "Atmosphere" );
    if ( !pfxBuffer.GetRawBuffer() || !atmosphereBuffer.GetRawBuffer()
        || !pfxBuffer.GetRawBuffer()->UpdateBuffer( &cb )
        || !atmosphereBuffer.GetRawBuffer()->UpdateBuffer(
            &sky->GetAtmosphereCB() ) ) {
        return XR_FAILED;
    }
    pfxBuffer.Bind();
    atmosphereBuffer.Bind();

    const INT2 cloudResolution(
        std::max( 1, resolution.x / 2 + resolution.x % 2 ),
        std::max( 1, resolution.y / 2 + resolution.y % 2 ) );

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;

    auto& rendererState = gapi->GetRendererState();
    const GothicBlendStateInfo previousBlendState = rendererState.BlendState;
    const GothicDepthBufferStateInfo previousDepthState = rendererState.DepthState;
    const GothicRasterizerStateInfo previousRasterizerState =
        rendererState.RasterizerState;

    rendererState.BlendState.SetDefault();
    rendererState.BlendState.SetDirty();
    rendererState.DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    rendererState.DepthState.DepthWriteEnabled = false;
    rendererState.DepthState.SetDirty();
    rendererState.RasterizerState.CullMode =
        GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.SetDirty();

    ID3D11SamplerState* clampSampler = engine->GetClampSamplerState();
    context->PSSetSamplers( 0, 1, &clampSampler );

    XRESULT result = engine->SetViewport(
        ViewportInfo( 0, 0, cloudResolution ) );
    if ( result == XR_SUCCESS ) {
        ID3D11RenderTargetView* lowCloudRTVs[2] = {
            cloudLayerRTV,
            cloudDepthRTV
        };
        context->OMSetRenderTargets( 2, lowCloudRTVs, nullptr );
        ID3D11ShaderResourceView* cloudResources[2] = { sceneSRV, depthSRV };
        context->PSSetShaderResources( 0, 2, cloudResources );
        result = DrawFullScreenQuad();
    }

    ID3D11ShaderResourceView* nullResources[2]{};
    context->PSSetShaderResources( 0, 2, nullResources );
    ID3D11SamplerState* defaultSampler = engine->GetDefaultSamplerState();
    context->PSSetSamplers( 0, 1, &defaultSampler );

    rendererState.BlendState = previousBlendState;
    rendererState.DepthState = previousDepthState;
    rendererState.RasterizerState = previousRasterizerState;
    rendererState.BlendState.SetDirty();
    rendererState.DepthState.SetDirty();
    rendererState.RasterizerState.SetDirty();
    if ( engine->UpdateRenderStates() != XR_SUCCESS ) result = XR_FAILED;
    return result;
}

XRESULT D3D11PfxRenderer::CompositeLowClouds(
    ID3D11RenderTargetView* outputRTV,
    ID3D11ShaderResourceView* sceneSRV,
    ID3D11ShaderResourceView* lowCloudLayerSRV,
    ID3D11ShaderResourceView* lowCloudDepthSRV,
    ID3D11ShaderResourceView* depthSRV ) {
    if ( !outputRTV || !sceneSRV || !lowCloudLayerSRV
        || !lowCloudDepthSRV || !depthSRV ) {
        return XR_INVALID_ARG;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    GSky* sky = gapi ? gapi->GetSky() : nullptr;
    if ( !sky ) return XR_SUCCESS;
    if ( !engine || !gapi || !context
        || !engine->GetClampSamplerState()
        || !engine->GetDefaultSamplerState() ) {
        return XR_FAILED;
    }

    const INT2 resolution = engine->GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) return XR_FAILED;

    auto compositePS = engine->GetShaderManager().GetPShader(
        PShaderID::PS_PFX_LowCloudComposite );
    auto fullscreenVS =
        engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    if ( !compositePS || !fullscreenVS
        || fullscreenVS->Apply() != XR_SUCCESS
        || compositePS->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    auto atmosphereBuffer = compositePS->GetBuffer( "Atmosphere" );
    if ( !atmosphereBuffer.GetRawBuffer()
        || !atmosphereBuffer.GetRawBuffer()->UpdateBuffer(
            &sky->GetAtmosphereCB() ) ) {
        return XR_FAILED;
    }
    atmosphereBuffer.Bind();

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;

    auto& rendererState = gapi->GetRendererState();
    const GothicBlendStateInfo previousBlendState = rendererState.BlendState;
    const GothicDepthBufferStateInfo previousDepthState = rendererState.DepthState;
    const GothicRasterizerStateInfo previousRasterizerState =
        rendererState.RasterizerState;

    rendererState.BlendState.SetDefault();
    rendererState.BlendState.SetDirty();
    rendererState.DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    rendererState.DepthState.DepthWriteEnabled = false;
    rendererState.DepthState.SetDirty();
    rendererState.RasterizerState.CullMode =
        GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.SetDirty();

    ID3D11SamplerState* clampSampler = engine->GetClampSamplerState();
    context->PSSetSamplers( 0, 1, &clampSampler );

    XRESULT result = engine->SetViewport( ViewportInfo( 0, 0, resolution ) );
    if ( result == XR_SUCCESS ) {
        context->OMSetRenderTargets( 1, &outputRTV, nullptr );
        ID3D11ShaderResourceView* compositeResources[4] = {
            sceneSRV,
            lowCloudLayerSRV,
            lowCloudDepthSRV,
            depthSRV
        };
        context->PSSetShaderResources( 0, 4, compositeResources );
        result = DrawFullScreenQuad();
    }

    ID3D11ShaderResourceView* nullResources[4]{};
    context->PSSetShaderResources( 0, 4, nullResources );
    ID3D11SamplerState* defaultSampler = engine->GetDefaultSamplerState();
    context->PSSetSamplers( 0, 1, &defaultSampler );

    rendererState.BlendState = previousBlendState;
    rendererState.DepthState = previousDepthState;
    rendererState.RasterizerState = previousRasterizerState;
    rendererState.BlendState.SetDirty();
    rendererState.DepthState.SetDirty();
    rendererState.RasterizerState.SetDirty();
    if ( engine->UpdateRenderStates() != XR_SUCCESS ) result = XR_FAILED;
    return result;
}
XRESULT D3D11PfxRenderer::RenderXeGTAO( ID3D11ShaderResourceView* depthSRV,
                                        ID3D11ShaderResourceView* normalsSRV,
                                        ID3D11RenderTargetView* outputRTV ) {
    if ( !PFX_XeGTAO ) return XR_FAILED;
    return PFX_XeGTAO->Render( depthSRV, normalsSRV, outputRTV );
}

TextureHandle D3D11PfxRenderer::GetTempBuffer()
{
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    DXGI_FORMAT bbufferFormat = engine->GetBackBufferFormat(); // actually intermediate backbuffer format -> HDRBackbuffer
    auto res = engine->GetResolution();

    return m_texturePool->Acquire( TexturePool::Description{res.x, res.y, bbufferFormat });
}

TextureHandle D3D11PfxRenderer::GetBackbufferTempBuffer()
{
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto res = engine->GetBackbufferResolution();

    return m_texturePool->Acquire( TexturePool::Description{ res.x, res.y, DXGI_FORMAT_ENGINE_SWAPCHAIN  } );
}

TextureHandle D3D11PfxRenderer::GetTempBufferDS4()
{
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    DXGI_FORMAT bbufferFormat = engine->GetBackBufferFormat(); // actually intermediate backbuffer format -> HDRBackbuffer
    auto res = engine->GetResolution();

    return m_texturePool->Acquire( TexturePool::Description{ res.x / 4, res.y / 4, bbufferFormat } );
}

void D3D11PfxRenderer::FreeResources()
{
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    if ( this->FX_SMAA 
        && settings.AntiAliasingMode != GothicRendererSettings::AA_SMAA ) {
        this->FX_SMAA->ReleaseResources();
    }
}
