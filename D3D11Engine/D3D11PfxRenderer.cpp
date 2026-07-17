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

#include <algorithm>
#include <array>
#include <cmath>

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
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !engine || !engine->GetDevice() ) {
        LogError() << "Post-processing renderer created without a valid D3D11 device.";
        return;
    }

    m_texturePool = std::make_unique<TexturePool>( engine->GetDevice().Get() );
    m_depthStencilPool = std::make_unique<DepthStencilPool>( engine->GetDevice().Get() );

    FX_Blur = std::make_unique<D3D11PFX_Blur>( this );
    FX_HeightFog = std::make_unique<D3D11PFX_HeightFog>( this );
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


/** Blurs the given texture */
XRESULT D3D11PfxRenderer::BlurTexture( RenderToTextureBuffer* texture, bool leaveResultInD4_2, float scale, const XMFLOAT4& colorMod, PShaderID finalCopyShader ) {
    if ( !FX_Blur || !texture || !texture->IsValid() || !std::isfinite( scale )
        || scale <= 0.0f ) {
        return XR_INVALID_ARG;
    }
    return FX_Blur->RenderBlur(
        texture, leaveResultInD4_2, 0.0f, scale, colorMod, finalCopyShader );
}

/** Renders the heightfog */
XRESULT D3D11PfxRenderer::RenderHeightfog() {
    return FX_HeightFog ? FX_HeightFog->Render( nullptr ) : XR_FAILED;
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
    return FX_DepthOfField && backbuffer
        ? FX_DepthOfField->Render( backbuffer )
        : XR_INVALID_ARG;
}

XRESULT D3D11PfxRenderer::RenderWetGroundSSR(
    ID3D11RenderTargetView* outputRTV,
    ID3D11ShaderResourceView* sceneSRV,
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11ShaderResourceView* waterMaskSRV ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    auto* rainShadow = engine && engine->Effects
        ? engine->Effects->GetRainShadowmap() : nullptr;
    auto* shadowMaps = engine ? engine->GetShadowMaps() : nullptr;
    auto* distortion = engine ? engine->GetDistortionTexture() : nullptr;
    if ( !engine || !gapi || !context || !outputRTV || !sceneSRV || !depthSRV
        || !normalsSRV || !waterMaskSRV || !rainShadow || !rainShadow->IsValid()
        || !shadowMaps || !distortion || !distortion->IsValid()
        || !engine->GetClampSamplerState() || !shadowMaps->GetShadowmapSampler() ) {
        return XR_INVALID_ARG;
    }

    const XMFLOAT4X4& projection = gapi->GetProjectionMatrix();
    const INT2 resolution = engine->GetResolution();
    if ( !HasUsableProjection( projection )
        || resolution.x <= 0 || resolution.y <= 0 ) {
        return XR_FAILED;
    }

    auto ps = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_WetGroundSSR );
    auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    if ( !ps || !vs || ps->Apply() != XR_SUCCESS || vs->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    WetGroundSSRConstantBuffer cb{};
    cb.WG_ProjParams = float4(
        1.0f / projection._11, 1.0f / projection._22,
        projection._43, projection._33 );
    const XMMATRIX view = gapi->GetViewMatrixXM();
    XMStoreFloat4x4( &cb.WG_InvView, XMMatrixInverse( nullptr, view ) );
    XMStoreFloat4x4(
        &cb.WG_ViewProj, XMLoadFloat4x4( &projection ) * view );

    const auto& rainCamera = engine->Effects->GetRainShadowmapCameraRepl();
    XMStoreFloat4x4( &cb.WG_RainViewProj,
        XMLoadFloat4x4( &rainCamera.ProjectionReplacement )
        * XMLoadFloat4x4( &rainCamera.ViewReplacement ) );

    cb.WG_CameraPosition = gapi->GetCameraPosition();
    cb.WG_Wetness = gapi->GetSceneWetness();
    cb.WG_InvResolution = float2(
        1.0f / resolution.x, 1.0f / resolution.y );
    cb.WG_Strength =
        gapi->GetRendererState().RendererSettings.SSRStrength * 0.84f;
    cb.WG_Time = gapi->GetTimeSeconds();
    cb.WG_RainFXWeight = gapi->GetRainFXWeight();

    auto constantBuffer = ps->GetBuffer( "WetGroundSSRConstantBuffer" );
    if ( !constantBuffer.GetRawBuffer()
        || !constantBuffer.GetRawBuffer()->UpdateBuffer( &cb ) ) {
        return XR_FAILED;
    }
    constantBuffer.Bind();

    D3D11PFXOutputStateGuard outputState( context.Get() );
    context->OMSetRenderTargets( 1, &outputRTV, nullptr );
    ID3D11ShaderResourceView* resources[6] = {
        sceneSRV,
        depthSRV,
        normalsSRV,
        rainShadow->GetShaderResView().Get(),
        distortion->GetShaderResourceView().Get(),
        waterMaskSRV
    };
    context->PSSetShaderResources( 0, 6, resources );

    ID3D11SamplerState* samplers[2] = {
        engine->GetClampSamplerState(),
        shadowMaps->GetShadowmapSampler()
    };
    context->PSSetSamplers( 0, 2, samplers );

    auto& rendererState = gapi->GetRendererState();
    const GothicBlendStateInfo previousBlendState = rendererState.BlendState;
    const GothicDepthBufferStateInfo previousDepthState = rendererState.DepthState;
    const GothicRasterizerStateInfo previousRasterizerState = rendererState.RasterizerState;

    engine->SetDefaultStates();
    rendererState.BlendState.SetDefault();
    rendererState.BlendState.SetDirty();
    rendererState.DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    rendererState.DepthState.DepthWriteEnabled = false;
    rendererState.DepthState.SetDirty();
    rendererState.RasterizerState.CullMode =
        GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.SetDirty();

    XRESULT result = engine->SetViewport( ViewportInfo( 0, 0, resolution ) );
    if ( result == XR_SUCCESS ) result = DrawFullScreenQuad();

    ID3D11ShaderResourceView* nullResources[6]{};
    context->PSSetShaderResources( 0, 6, nullResources );

    rendererState.BlendState = previousBlendState;
    rendererState.DepthState = previousDepthState;
    rendererState.RasterizerState = previousRasterizerState;
    rendererState.BlendState.SetDirty();
    rendererState.DepthState.SetDirty();
    rendererState.RasterizerState.SetDirty();
    if ( engine->UpdateRenderStates() != XR_SUCCESS ) result = XR_FAILED;
    return result;
}
/** Renders the HDR-Effect */
XRESULT D3D11PfxRenderer::RenderHDR( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* backbuffer ) {
    return FX_HDR && output && backbuffer
        ? FX_HDR->Render( output, backbuffer )
        : XR_INVALID_ARG;
}

void D3D11PfxRenderer::ResetHDRAdaptation() {
    if ( FX_HDR ) {
        FX_HDR->ResetAdaptation();
    }
}

/** Renders the SMAA-Effect */
XRESULT D3D11PfxRenderer::RenderSMAA( ID3D11ShaderResourceView* backbuffer ) {
    return FX_SMAA && backbuffer
        ? FX_SMAA->RenderPostFX( backbuffer )
        : XR_INVALID_ARG;
}

XRESULT D3D11PfxRenderer::RenderCAS( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& input, INT2 inputSize, const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& output, INT2 outputSize, RenderToTextureBuffer& intermediateBuffer ) {
    if ( !PFX_CAS || !Engine::GAPI || !input || !output
        || inputSize.x <= 0 || inputSize.y <= 0
        || outputSize.x <= 0 || outputSize.y <= 0
        || !intermediateBuffer.IsValid() ) {
        return XR_INVALID_ARG;
    }

    PFX_CAS->SetSharpness(
        Engine::GAPI->GetRendererState().RendererSettings.SharpenFactor );
    return PFX_CAS->Apply(
        input, inputSize, output, outputSize, intermediateBuffer );
}

XRESULT D3D11PfxRenderer::RenderSimpleSharpen( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& source, INT2 sourceSize, RenderToTextureBuffer* dest, INT2 destSize ) {
    if ( !PFX_SimpleSharpen || !source || !dest || !dest->IsValid()
        || sourceSize.x <= 0 || sourceSize.y <= 0
        || destSize.x <= 0 || destSize.y <= 0 ) {
        return XR_INVALID_ARG;
    }
    return PFX_SimpleSharpen->Apply( source, sourceSize, dest, destSize );
}

/** Draws a fullscreenquad */
XRESULT D3D11PfxRenderer::DrawFullScreenQuad() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !engine || !context ) return XR_FAILED;
    if ( engine->UpdateRenderStates() != XR_SUCCESS ) return XR_FAILED;

    context->IASetPrimitiveTopology( D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST );
    context->Draw( 3, 0 );
    return XR_SUCCESS;
}

/** Unbinds texturesamplers from the pixel-shader */
XRESULT D3D11PfxRenderer::UnbindPSResources( int num ) {
    if ( num < 0 || num > D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT ) {
        return XR_INVALID_ARG;
    }
    if ( num == 0 ) return XR_SUCCESS;

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !context ) return XR_FAILED;

    std::array<ID3D11ShaderResourceView*,
        D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> nullResources{};
    context->PSSetShaderResources(
        0, static_cast<UINT>(num), nullResources.data() );
    return XR_SUCCESS;
}

/** Copies the given texture to the given RTV */
XRESULT D3D11PfxRenderer::CopyTextureToRTV( const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& texture, const Microsoft::WRL::ComPtr<ID3D11RenderTargetView>& rtv, INT2 targetResolution, bool useCustomPS, INT2 offset, ID3D11RenderTargetView* extraRTV ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !engine || !rtv || (!texture && !useCustomPS) ) {
        return XR_INVALID_ARG;
    }

    const auto context = engine->GetContext();
    const auto device = engine->GetDevice();
    if ( !context || !device ) {
        return XR_FAILED;
    }

    const bool changeViewport = targetResolution.x != 0 || targetResolution.y != 0;
    if ( changeViewport && (targetResolution.x <= 0 || targetResolution.y <= 0) ) {
        return XR_INVALID_ARG;
    }
    if ( extraRTV == rtv.Get() ) return XR_INVALID_ARG;

    if ( extraRTV && !m_extraMaskBlendState ) {
        D3D11_BLEND_DESC blendDesc{};
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

        const HRESULT hr = device->CreateBlendState( &blendDesc, m_extraMaskBlendState.ReleaseAndGetAddressOf() );
        if ( FAILED( hr ) ) {
            LogError() << "Failed to create post-process mask blend state: 0x"
                << std::hex << static_cast<unsigned long>(hr);
            return XR_FAILED;
        }
    }

    ID3D11SamplerState* linearSampler = nullptr;
    if ( !useCustomPS ) {
        const auto simplePS =
            engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Simple );
        linearSampler = engine->GetLinearSamplerState();
        if ( !simplePS || !linearSampler
            || simplePS->Apply() != XR_SUCCESS ) {
            return XR_FAILED;
        }
    }

    const auto fullscreenVS = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    if ( !fullscreenVS || fullscreenVS->Apply() != XR_SUCCESS
        || engine->UpdateRenderStates() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    std::array<D3D11_VIEWPORT,
        D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> oldViewports{};
    UINT oldViewportCount = static_cast<UINT>(oldViewports.size());
    if ( changeViewport ) {
        context->RSGetViewports( &oldViewportCount, oldViewports.data() );

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = static_cast<float>(offset.x);
        viewport.TopLeftY = static_cast<float>(offset.y);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        viewport.Width = static_cast<float>(targetResolution.x);
        viewport.Height = static_cast<float>(targetResolution.y);
        context->RSSetViewports( 1, &viewport );
    }

    struct RenderTargetSnapshot {
        std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets{};
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depthStencil;

        ~RenderTargetSnapshot() {
            for ( auto* target : targets ) {
                if ( target ) target->Release();
            }
        }
    } oldTargets;
    context->OMGetRenderTargets(
        static_cast<UINT>(oldTargets.targets.size()), oldTargets.targets.data(),
        oldTargets.depthStencil.GetAddressOf() );

    Microsoft::WRL::ComPtr<ID3D11BlendState> oldBlendState;
    FLOAT oldBlendFactor[4]{};
    UINT oldSampleMask = 0xffffffff;
    if ( extraRTV ) {
        context->OMGetBlendState( oldBlendState.GetAddressOf(), oldBlendFactor, &oldSampleMask );
        const FLOAT blendFactor[4]{};
        context->OMSetBlendState( m_extraMaskBlendState.Get(), blendFactor, 0xffffffff );
    }

    Microsoft::WRL::ComPtr<ID3D11SamplerState> oldSampler;
    if ( !useCustomPS ) {
        context->PSGetSamplers( 0, 1, oldSampler.GetAddressOf() );
        context->PSSetSamplers( 0, 1, &linearSampler );
    }

    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources( 0, 1, &nullSRV );

    ID3D11RenderTargetView* targets[2] = { rtv.Get(), extraRTV };
    context->OMSetRenderTargets( extraRTV ? 2u : 1u, targets, nullptr );

    ID3D11ShaderResourceView* source = texture.Get();
    context->PSSetShaderResources( 0, 1, &source );
    const XRESULT drawResult = DrawFullScreenQuad();

    context->PSSetShaderResources( 0, 1, &nullSRV );
    context->OMSetRenderTargets(
        static_cast<UINT>(oldTargets.targets.size()), oldTargets.targets.data(),
        oldTargets.depthStencil.Get() );

    if ( extraRTV ) {
        context->OMSetBlendState( oldBlendState.Get(), oldBlendFactor, oldSampleMask );
    }
    if ( !useCustomPS ) {
        ID3D11SamplerState* samplerToRestore = oldSampler.Get();
        context->PSSetSamplers( 0, 1, &samplerToRestore );
    }
    if ( changeViewport ) {
        context->RSSetViewports(
            oldViewportCount, oldViewportCount ? oldViewports.data() : nullptr );
    }

    return drawResult;
}
/** Called on resize */
XRESULT D3D11PfxRenderer::OnResize( const INT2& newResolution ) {


    if ( newResolution.x <= 0 || newResolution.y <= 0 ) return XR_INVALID_ARG;

    if ( PFX_FSR3 ) PFX_FSR3->Destroy();
    m_extraMaskBlendState.Reset();
    ScreenSpaceLightingHistory[0].reset();
    ScreenSpaceLightingHistory[1].reset();
    ScreenSpaceLightingDepthHistory[0].reset();
    ScreenSpaceLightingDepthHistory[1].reset();
    ScreenSpaceLightingHistoryValid = false;
    ScreenSpaceLightingHistoryIndex = 0;
    if ( !m_texturePool || !m_depthStencilPool ) return XR_FAILED;
    m_texturePool->Clear();
    m_depthStencilPool->Clear();

    if ( FX_SMAA ) FX_SMAA->OnResize( newResolution );
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
    if ( outLightingSRV ) *outLightingSRV = nullptr;

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    const auto device = engine ? engine->GetDevice() : nullptr;
    if ( !engine || !gapi || !context || !device || !m_texturePool ) {
        ScreenSpaceLightingHistoryValid = false;
        return XR_FAILED;
    }

    auto& settings = gapi->GetRendererState().RendererSettings;
    const bool contactActive = settings.EnableContactShadows;
    const bool giActive =
        settings.EnableScreenSpaceGI && settings.ScreenSpaceGIStrength > 0.0f;
    if ( !contactActive && !giActive ) {
        ScreenSpaceLightingHistoryValid = false;
        return XR_SUCCESS;
    }
    if ( !sceneSRV || !depthSRV || !normalsSRV
        || !waterMaskSRV || !materialSRV ) {
        ScreenSpaceLightingHistoryValid = false;
        return XR_INVALID_ARG;
    }

    const INT2 resolution = engine->GetResolution();
    const XMFLOAT4X4& projection = gapi->GetProjectionMatrix();
    if ( resolution.x <= 0 || resolution.y <= 0
        || !HasUsableProjection( projection )
        || !engine->GetClampSamplerState() ) {
        ScreenSpaceLightingHistoryValid = false;
        return XR_FAILED;
    }

    for ( size_t i = 0; i < 2; ++i ) {
        const bool needsHistory = !ScreenSpaceLightingHistory[i]
            || !ScreenSpaceLightingHistory[i]->IsValid()
            || !ScreenSpaceLightingDepthHistory[i]
            || !ScreenSpaceLightingDepthHistory[i]->IsValid()
            || ScreenSpaceLightingHistory[i]->GetSizeX()
                != static_cast<UINT>(resolution.x)
            || ScreenSpaceLightingHistory[i]->GetSizeY()
                != static_cast<UINT>(resolution.y);
        if ( !needsHistory ) continue;

        auto newHistory = std::make_unique<RenderToTextureBuffer>(
            device.Get(), resolution.x, resolution.y,
            DXGI_FORMAT_R16G16B16A16_FLOAT );
        auto newDepthHistory = std::make_unique<RenderToTextureBuffer>(
            device.Get(), resolution.x, resolution.y, DXGI_FORMAT_R32_FLOAT );
        if ( !newHistory->IsValid() || !newDepthHistory->IsValid() ) {
            ScreenSpaceLightingHistoryValid = false;
            return XR_FAILED;
        }

        ScreenSpaceLightingHistory[i] = std::move( newHistory );
        ScreenSpaceLightingDepthHistory[i] = std::move( newDepthHistory );
        ScreenSpaceLightingHistoryValid = false;
    }

    auto rawLighting = m_texturePool->Acquire( TexturePool::Description{
        resolution.x, resolution.y, DXGI_FORMAT_R16G16B16A16_FLOAT } );
    if ( !rawLighting || !rawLighting->IsValid() ) {
        ScreenSpaceLightingHistoryValid = false;
        return XR_FAILED;
    }

    auto fullscreenVS =
        engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    auto tracePS = engine->GetShaderManager().GetPShader(
        PShaderID::PS_PFX_ScreenSpaceLightingTrace );
    auto temporalPS = engine->GetShaderManager().GetPShader(
        PShaderID::PS_PFX_ScreenSpaceLightingTemporal );
    if ( !fullscreenVS || !tracePS || !temporalPS
        || fullscreenVS->Apply() != XR_SUCCESS
        || tracePS->Apply() != XR_SUCCESS ) {
        ScreenSpaceLightingHistoryValid = false;
        return XR_FAILED;
    }

    ScreenSpaceLightingConstantBuffer cb{};
    GSky* sky = gapi->GetSky();
    const float mainLightVisibility =
        sky ? sky->GetMainLightVisibility() : 1.0f;
    cb.SSL_ProjParams = float4(
        1.0f / projection._11, 1.0f / projection._22,
        projection._43, projection._33 );
    cb.SSL_Projection = projection;
    const XMMATRIX view = gapi->GetViewMatrixXM();
    XMStoreFloat4x4( &cb.SSL_View, view );
    XMStoreFloat4x4( &cb.SSL_InvView, XMMatrixInverse( nullptr, view ) );
    cb.SSL_InvResolution =
        float2( 1.0f / resolution.x, 1.0f / resolution.y );
    cb.SSL_ContactStrength =
        (contactActive ? settings.GetContactShadowFixedStrength() : 0.0f)
        * mainLightVisibility;
    cb.SSL_GIStrength = settings.ScreenSpaceGIStrength;
    cb.SSL_EnableContact = contactActive ? 1.0f : 0.0f;
    cb.SSL_EnableGI = giActive ? 1.0f : 0.0f;
    cb.SSL_HistoryValid = ScreenSpaceLightingHistoryValid ? 1.0f : 0.0f;
    cb.SSL_FSR3Active =
        settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3
        && settings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        ? 1.0f : 0.0f;
    cb.SSL_FrameIndex =
        static_cast<float>(ScreenSpaceLightingFrameIndex & 1023u);

    if ( sky ) {
        const XMFLOAT3 mainLightDirection = sky->GetMainLightDirection();
        const XMVECTOR transformedDirection =
            XMVector3TransformNormal( XMLoadFloat3( &mainLightDirection ), view );
        if ( XMVectorGetX( XMVector3LengthSq( transformedDirection ) ) > 1.0e-8f ) {
            XMStoreFloat3( &cb.SSL_LightDirectionVS,
                XMVector3Normalize( transformedDirection ) );
        } else {
            cb.SSL_LightDirectionVS = XMFLOAT3( 0.0f, 1.0f, 0.0f );
        }
    } else {
        cb.SSL_LightDirectionVS = XMFLOAT3( 0.0f, 1.0f, 0.0f );
    }

    auto traceBuffer = tracePS->GetBuffer( "ScreenSpaceLightingConstantBuffer" );
    if ( !traceBuffer.GetRawBuffer()
        || !traceBuffer.GetRawBuffer()->UpdateBuffer( &cb ) ) {
        ScreenSpaceLightingHistoryValid = false;
        return XR_FAILED;
    }
    traceBuffer.Bind();

    D3D11PFXOutputStateGuard outputState( context.Get() );
    auto& rendererState = gapi->GetRendererState();
    const GothicBlendStateInfo previousBlendState = rendererState.BlendState;
    const GothicDepthBufferStateInfo previousDepthState = rendererState.DepthState;
    const GothicRasterizerStateInfo previousRasterizerState =
        rendererState.RasterizerState;

    engine->SetDefaultStates();
    rendererState.BlendState.SetDefault();
    rendererState.BlendState.SetDirty();
    rendererState.DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    rendererState.DepthState.DepthWriteEnabled = false;
    rendererState.DepthState.SetDirty();

    XRESULT result = engine->SetViewport(
        ViewportInfo( 0, 0, resolution ) );
    if ( result == XR_SUCCESS ) {
        ID3D11RenderTargetView* rawRTV =
            rawLighting->GetRenderTargetView().Get();
        context->OMSetRenderTargets( 1, &rawRTV, nullptr );
        ID3D11ShaderResourceView* traceResources[5] = {
            sceneSRV, depthSRV, normalsSRV, waterMaskSRV, materialSRV
        };
        context->PSSetShaderResources( 0, 5, traceResources );
        ID3D11SamplerState* sampler = engine->GetClampSamplerState();
        context->PSSetSamplers( 0, 1, &sampler );
        result = DrawFullScreenQuad();
    }

    const uint32_t readIndex = ScreenSpaceLightingHistoryIndex & 1u;
    const uint32_t writeIndex = 1u - readIndex;
    if ( result == XR_SUCCESS ) {
        result = temporalPS->Apply();
    }
    if ( result == XR_SUCCESS ) {
        cb.SSL_HistoryValid =
            ScreenSpaceLightingHistoryValid ? 1.0f : 0.0f;
        auto temporalBuffer =
            temporalPS->GetBuffer( "ScreenSpaceLightingConstantBuffer" );
        if ( !temporalBuffer.GetRawBuffer()
            || !temporalBuffer.GetRawBuffer()->UpdateBuffer( &cb ) ) {
            result = XR_FAILED;
        } else {
            temporalBuffer.Bind();
        }
    }
    if ( result == XR_SUCCESS ) {
        ID3D11RenderTargetView* temporalTargets[2] = {
            ScreenSpaceLightingHistory[writeIndex]->GetRenderTargetView().Get(),
            ScreenSpaceLightingDepthHistory[writeIndex]->GetRenderTargetView().Get()
        };
        context->OMSetRenderTargets( 2, temporalTargets, nullptr );
        ID3D11ShaderResourceView* temporalResources[7] = {
            rawLighting->GetShaderResView().Get(),
            ScreenSpaceLightingHistoryValid
                ? ScreenSpaceLightingHistory[readIndex]->GetShaderResView().Get()
                : rawLighting->GetShaderResView().Get(),
            depthSRV,
            normalsSRV,
            velocitySRV,
            ScreenSpaceLightingHistoryValid
                ? ScreenSpaceLightingDepthHistory[readIndex]->GetShaderResView().Get()
                : depthSRV,
            materialSRV
        };
        context->PSSetShaderResources( 0, 7, temporalResources );
        result = DrawFullScreenQuad();
    }

    ID3D11ShaderResourceView* nullResources[7]{};
    context->PSSetShaderResources( 0, 7, nullResources );

    rendererState.BlendState = previousBlendState;
    rendererState.DepthState = previousDepthState;
    rendererState.RasterizerState = previousRasterizerState;
    rendererState.BlendState.SetDirty();
    rendererState.DepthState.SetDirty();
    rendererState.RasterizerState.SetDirty();
    if ( engine->UpdateRenderStates() != XR_SUCCESS ) result = XR_FAILED;

    if ( result != XR_SUCCESS ) {
        ScreenSpaceLightingHistoryValid = false;
        return result;
    }

    ScreenSpaceLightingHistoryIndex = writeIndex;
    ScreenSpaceLightingHistoryValid = true;
    ++ScreenSpaceLightingFrameIndex;
    if ( outLightingSRV ) {
        *outLightingSRV =
            ScreenSpaceLightingHistory[writeIndex]->GetShaderResView().Get();
    }
    return XR_SUCCESS;
}
XRESULT D3D11PfxRenderer::RenderPostFXComposition(
    ID3D11RenderTargetView* outputRTV,
    ID3D11ShaderResourceView* backbufferSRV,
    ID3D11ShaderResourceView* godraysSRV,
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* screenSpaceLightingSRV,
    bool compositionHeightFog ) {

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !engine || !gapi || !context || !outputRTV || !backbufferSRV ) {
        return XR_INVALID_ARG;
    }

    const INT2 res = engine->GetResolution();
    if ( res.x <= 0 || res.y <= 0 ) {
        return XR_FAILED;
    }

    auto& settings = gapi->GetRendererState().RendererSettings;
    GSky* sky = gapi->GetSky();
    const bool contactShadowsActive = settings.EnableContactShadows;
    const bool screenSpaceGIActive = settings.EnableScreenSpaceGI
        && std::isfinite( settings.ScreenSpaceGIStrength )
        && settings.ScreenSpaceGIStrength > 0.0f;
    const bool geometryLightingActive =
        contactShadowsActive || screenSpaceGIActive;
    const bool heightFogPermutation = settings.DrawFog;
    const bool needsAtmosphere = compositionHeightFog;
    const bool needsControl = heightFogPermutation || contactShadowsActive;
    if ( !engine->GetLinearSamplerState()
        || (compositionHeightFog
            && (!depthSRV || !sky
                || !HasUsableProjection( gapi->GetProjectionMatrix() )))
        || (geometryLightingActive && !screenSpaceLightingSRV)
        || ViewsAlias( backbufferSRV, outputRTV ) ) {
        return XR_INVALID_ARG;
    }

    auto fullscreenVS =
        engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    auto compositionPS = engine->GetShaderManager().GetPShader(
        PShaderID::PS_PFX_Composition );
    if ( !fullscreenVS || !compositionPS
        || fullscreenVS->Apply() != XR_SUCCESS
        || compositionPS->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    CompositionControlConstantBuffer control = {};
    control.CC_HeightFogEnabled = compositionHeightFog ? 1.0f : 0.0f;
    const bool fsr3ContactReduction =
        settings.AntiAliasingMode == GothicRendererSettings::AA_FSR3
        && settings.Upscaler == GothicRendererSettings::UPSCALER_FSR_3;
    control.CC_ContactShadowScale = fsr3ContactReduction ? 0.70f : 1.0f;
    if ( needsControl ) {
        auto controlBuffer = compositionPS->GetBuffer( "CompositionControl" );
        controlBuffer.Update( &control ).Bind();
        if ( !controlBuffer.Succeeded() ) return XR_FAILED;
    }
    if ( compositionHeightFog ) {
        HeightfogConstantBuffer fogConstants{};
        if ( !D3D11PFX_HeightFog::BuildConstants(
                gapi, sky, fogConstants ) ) {
            return XR_FAILED;
        }

        auto fogBuffer = compositionPS->GetBuffer( "PFXBuffer" );
        fogBuffer.Update( &fogConstants ).Bind();
        if ( !fogBuffer.Succeeded() ) return XR_FAILED;
    }
    if ( needsAtmosphere ) {
        auto atmosphereBuffer = compositionPS->GetBuffer( "Atmosphere" );
        atmosphereBuffer.Update( &sky->GetAtmosphereCB() ).Bind();
        if ( !atmosphereBuffer.Succeeded() ) return XR_FAILED;
    }

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;

    auto& rendererState = gapi->GetRendererState();
    const GothicBlendStateInfo previousBlendState = rendererState.BlendState;
    const GothicDepthBufferStateInfo previousDepthState = rendererState.DepthState;
    const GothicRasterizerStateInfo previousRasterizerState =
        rendererState.RasterizerState;

    engine->SetDefaultStates();
    rendererState.BlendState.SetDefault();
    rendererState.BlendState.SetDirty();
    rendererState.DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    rendererState.DepthState.DepthWriteEnabled = false;
    rendererState.DepthState.SetDirty();
    rendererState.RasterizerState.CullMode =
        GothicRasterizerStateInfo::CM_CULL_NONE;
    rendererState.RasterizerState.SetDirty();

    Microsoft::WRL::ComPtr<ID3D11SamplerState> previousSampler;
    context->PSGetSamplers( 0, 1, previousSampler.GetAddressOf() );
    ID3D11SamplerState* linearSampler = engine->GetLinearSamplerState();
    context->PSSetSamplers( 0, 1, &linearSampler );

    XRESULT result = engine->SetViewport( ViewportInfo( 0, 0, res ) );
    if ( result == XR_SUCCESS ) {
        context->OMSetRenderTargets( 1, &outputRTV, nullptr );
        ID3D11ShaderResourceView* resources[6] = {
            backbufferSRV, godraysSRV, depthSRV,
            nullptr, nullptr, screenSpaceLightingSRV
        };
        context->PSSetShaderResources( 0, 6, resources );
        result = DrawFullScreenQuad();
    }

    ID3D11ShaderResourceView* nullResources[6]{};
    context->PSSetShaderResources( 0, 6, nullResources );
    ID3D11SamplerState* samplerToRestore = previousSampler.Get();
    context->PSSetSamplers( 0, 1, &samplerToRestore );

    rendererState.BlendState = previousBlendState;
    rendererState.DepthState = previousDepthState;
    rendererState.RasterizerState = previousRasterizerState;
    rendererState.BlendState.SetDirty();
    rendererState.DepthState.SetDirty();
    rendererState.RasterizerState.SetDirty();
    if ( engine->UpdateRenderStates() != XR_SUCCESS ) result = XR_FAILED;
    return result;
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
XRESULT D3D11PfxRenderer::RenderXeGTAO(
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* normalsSRV,
    ID3D11RenderTargetView* outputRTV ) {
    if ( !PFX_XeGTAO || !depthSRV || !normalsSRV || !outputRTV ) {
        return XR_INVALID_ARG;
    }
    return PFX_XeGTAO->Render( depthSRV, normalsSRV, outputRTV );
}

TextureHandle D3D11PfxRenderer::GetTempBuffer() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !engine || !m_texturePool ) return {};

    const INT2 resolution = engine->GetResolution();
    const DXGI_FORMAT format = engine->GetBackBufferFormat();
    if ( resolution.x <= 0 || resolution.y <= 0
        || format == DXGI_FORMAT_UNKNOWN ) {
        return {};
    }
    return m_texturePool->Acquire(
        TexturePool::Description{ resolution.x, resolution.y, format } );
}

TextureHandle D3D11PfxRenderer::GetBackbufferTempBuffer() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !engine || !m_texturePool ) return {};

    const INT2 resolution = engine->GetBackbufferResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) return {};
    return m_texturePool->Acquire( TexturePool::Description{
        resolution.x, resolution.y, DXGI_FORMAT_ENGINE_SWAPCHAIN } );
}

TextureHandle D3D11PfxRenderer::GetTempBufferDS4() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !engine || !m_texturePool ) return {};

    const INT2 resolution = engine->GetResolution();
    const DXGI_FORMAT format = engine->GetBackBufferFormat();
    if ( resolution.x <= 0 || resolution.y <= 0
        || format == DXGI_FORMAT_UNKNOWN ) {
        return {};
    }
    return m_texturePool->Acquire( TexturePool::Description{
        std::max( 1, resolution.x / 4 ),
        std::max( 1, resolution.y / 4 ), format } );
}
void D3D11PfxRenderer::FreeResources() {
    if ( !Engine::GAPI ) return;

    const auto& settings =
        Engine::GAPI->GetRendererState().RendererSettings;
    if ( FX_SMAA
        && settings.AntiAliasingMode != GothicRendererSettings::AA_SMAA ) {
        FX_SMAA->ReleaseResources();
    }
}
