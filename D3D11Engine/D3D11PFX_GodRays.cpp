#include "pch.h"
#include "D3D11PFX_GodRays.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11CShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "GSky.h"
#include "TexturePool.h"

#include <algorithm>
#include <cmath>

extern bool FeatureLevel10Compatibility;

namespace {
    enum class GodRaySetupResult {
        Visible,
        Hidden,
        Invalid,
    };

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

            auto& rendererState = GAPI->GetRendererState();
            rendererState.BlendState = BlendState;
            rendererState.DepthState = DepthState;
            rendererState.RasterizerState = RasterizerState;
            rendererState.BlendState.SetDirty();
            rendererState.DepthState.SetDirty();
            rendererState.RasterizerState.SetDirty();
            return GraphicsEngine->UpdateRenderStates() == XR_SUCCESS;
        }

        ScopedTrackedRendererState( const ScopedTrackedRendererState& ) = delete;
        ScopedTrackedRendererState& operator=( const ScopedTrackedRendererState& ) = delete;

    private:
        D3D11GraphicsEngine* GraphicsEngine;
        GothicAPI* GAPI;
        GothicBlendStateInfo BlendState;
        GothicDepthBufferStateInfo DepthState;
        GothicRasterizerStateInfo RasterizerState;
        bool Restored = false;
    };

    float GetRainSkyVisibility( GSky* sky ) {
        if ( !sky ) return 0.0f;
        const float rawRain = sky->GetAtmosphereCB().AC_RainFXWeight;
        if ( !std::isfinite( rawRain ) ) return 0.0f;

        const float rain = std::clamp( rawRain, 0.0f, 1.0f );
        const float transition = std::clamp( (rain - 0.05f) / 0.60f, 0.0f, 1.0f );
        const float smoothOcclusion = transition * transition * (3.0f - 2.0f * transition);
        return 1.0f - smoothOcclusion;
    }

    GodRaySetupResult BuildGodRayConstants(
        GothicAPI* gapi,
        GSky* sky,
        GodRayZoomConstantBuffer& constants ) {
        if ( !gapi || !sky ) return GodRaySetupResult::Invalid;

        const float lightDirectionY = sky->GetAtmosphereSettings().LightDirection.y;
        if ( !std::isfinite( lightDirectionY ) ) return GodRaySetupResult::Invalid;
        if ( lightDirectionY <= 0.0f ) return GodRaySetupResult::Hidden;

        const auto& atmosphere = sky->GetAtmosphereCB();
        const float outerRadius = atmosphere.AC_OuterRadius;
        if ( !std::isfinite( outerRadius ) || outerRadius <= 0.0f ) {
            return GodRaySetupResult::Invalid;
        }

        XMVECTOR sunPositionWorld = XMLoadFloat3( atmosphere.AC_LightPos.toXMFLOAT3() );
        sunPositionWorld *= outerRadius;
        sunPositionWorld += gapi->GetCameraPositionXM();

        XMMATRIX view = gapi->GetViewMatrixXM();
        const XMMATRIX projection = XMLoadFloat4x4( &gapi->GetProjectionMatrix() );
        const XMMATRIX viewProjection = XMMatrixTranspose( XMMatrixMultiply( projection, view ) );
        view = XMMatrixTranspose( view );

        XMFLOAT3 sunViewPosition{};
        XMFLOAT3 sunPosition{};
        XMStoreFloat3( &sunViewPosition, XMVector3TransformCoord( sunPositionWorld, view ) );
        XMStoreFloat3( &sunPosition, XMVector3TransformCoord( sunPositionWorld, viewProjection ) );
        if ( !std::isfinite( sunViewPosition.z )
            || !std::isfinite( sunPosition.x ) || !std::isfinite( sunPosition.y ) ) {
            return GodRaySetupResult::Invalid;
        }
        if ( sunViewPosition.z < 0.0f ) return GodRaySetupResult::Hidden;

        const auto& settings = gapi->GetRendererState().RendererSettings;
        constants = {};
        constants.GR_Decay = settings.GodRayDecay;
        constants.GR_Weight = settings.GodRayWeight * settings.GodRayStrength
            * atmosphere.AC_SunVisibility * GetRainSkyVisibility( sky );
        constants.GR_Density = settings.GodRayDensity;
        constants.GR_Center.x = sunPosition.x / 2.0f + 0.5f;
        constants.GR_Center.y = sunPosition.y / -2.0f + 0.5f;
        constants.GR_ColorMod = settings.GodRayColorMod;

        if ( !std::isfinite( constants.GR_Decay )
            || !std::isfinite( constants.GR_Weight )
            || !std::isfinite( constants.GR_Density )
            || !std::isfinite( constants.GR_Center.x )
            || !std::isfinite( constants.GR_Center.y )
            || !std::isfinite( constants.GR_ColorMod.x )
            || !std::isfinite( constants.GR_ColorMod.y )
            || !std::isfinite( constants.GR_ColorMod.z ) ) {
            return GodRaySetupResult::Invalid;
        }

        const float centerDistanceX = std::abs( constants.GR_Center.x - 0.5f );
        const float centerDistanceY = std::abs( constants.GR_Center.y - 0.5f );
        if ( centerDistanceX > 0.5f ) {
            constants.GR_Weight *= std::max(
                0.0f, 1.0f - (centerDistanceX - 0.5f) / 0.5f );
        }
        if ( centerDistanceY > 0.5f ) {
            constants.GR_Weight *= std::max(
                0.0f, 1.0f - (centerDistanceY - 0.5f) / 0.5f );
        }

        return std::isfinite( constants.GR_Weight )
            ? GodRaySetupResult::Visible
            : GodRaySetupResult::Invalid;
    }

    void ClearComputeBindings( ID3D11DeviceContext* context ) {
        if ( !context ) return;
        ID3D11UnorderedAccessView* nullUAV = nullptr;
        ID3D11ShaderResourceView* nullResources[3]{};
        context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
        context->CSSetShaderResources( 0, 3, nullResources );
        context->CSSetShader( nullptr, nullptr, 0 );
    }
}

D3D11PFX_GodRays::D3D11PFX_GodRays( D3D11PfxRenderer* rnd )
    : D3D11PFX_Effect( rnd ) {
}

XRESULT D3D11PFX_GodRays::Render(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* lowClouds ) {
    if ( !backbuffer || !depth || !FxRenderer ) return XR_INVALID_ARG;

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !engine || !gapi || !context ) return XR_FAILED;

    const INT2 resolution = engine->GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) return XR_FAILED;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputRTV;
    context->OMGetRenderTargets( 1, outputRTV.GetAddressOf(), nullptr );
    if ( !outputRTV ) return XR_FAILED;

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;

    ScopedTrackedRendererState rendererState( engine, gapi );
    engine->SetDefaultStates();

    ID3D11ShaderResourceView* godRaysSRV = nullptr;
    XRESULT result = RenderToTexture( backbuffer, depth, lowClouds, &godRaysSRV );
    if ( result == XR_SUCCESS && godRaysSRV ) {
        auto& blendState = gapi->GetRendererState().BlendState;
        blendState.SetAdditiveBlending();
        blendState.SetDirty();
        result = FxRenderer->CopyTextureToRTV(
            m_GodRaysResult->GetShaderResView(), outputRTV, resolution );
    }

    if ( !rendererState.Restore() ) result = XR_FAILED;
    return result;
}

XRESULT D3D11PFX_GodRays::RenderToTexture(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* lowClouds,
    ID3D11ShaderResourceView** outGodRaysSRV ) {
    if ( outGodRaysSRV ) *outGodRaysSRV = nullptr;
    m_GodRaysResult.reset();
    if ( !backbuffer || !depthCopy || !outGodRaysSRV || !FxRenderer ) {
        return XR_INVALID_ARG;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    GSky* sky = gapi ? gapi->GetSky() : nullptr;
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !engine || !gapi || !sky || !context ) return XR_FAILED;

    GodRayZoomConstantBuffer constants{};
    const GodRaySetupResult setup = BuildGodRayConstants( gapi, sky, constants );
    if ( setup == GodRaySetupResult::Hidden ) return XR_SUCCESS;
    if ( setup != GodRaySetupResult::Visible ) return XR_FAILED;

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;

    if ( !FeatureLevel10Compatibility ) {
        return RenderToTextureCS(
            backbuffer, depthCopy, lowClouds, constants, outGodRaysSRV );
    }

    const auto fullscreenVS = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    const auto maskPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GodRayMask );
    const auto zoomPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GodRayZoom );
    auto tempBuffer = FxRenderer->GetTempBufferDS4();
    auto zoomBuffer = FxRenderer->GetTempBufferDS4();
    ID3D11SamplerState* clampSampler = engine->GetClampSamplerState();
    ID3D11SamplerState* defaultSampler = engine->GetDefaultSamplerState();
    if ( !fullscreenVS || !maskPS || !zoomPS || !tempBuffer || !zoomBuffer
        || !tempBuffer->IsValid() || !zoomBuffer->IsValid()
        || !clampSampler || !defaultSampler
        || tempBuffer->GetSizeX() == 0 || tempBuffer->GetSizeY() == 0
        || tempBuffer->GetSizeX() != zoomBuffer->GetSizeX()
        || tempBuffer->GetSizeY() != zoomBuffer->GetSizeY() ) {
        return XR_FAILED;
    }

    auto zoomConstants = zoomPS->GetBuffer( "GodRayZoomConstantBuffer" );
    zoomConstants.Update( &constants ).Bind();
    if ( !zoomConstants.Succeeded() ) return XR_FAILED;

    ScopedTrackedRendererState rendererState( engine, gapi );
    engine->SetDefaultStates();

    XRESULT result = fullscreenVS->Apply();
    if ( result == XR_SUCCESS ) result = maskPS->Apply();
    if ( result == XR_SUCCESS ) {
        ID3D11RenderTargetView* maskRTV = tempBuffer->GetRenderTargetView().Get();
        context->OMSetRenderTargets( 1, &maskRTV, nullptr );
        ID3D11ShaderResourceView* resources[3] = { backbuffer, depthCopy, lowClouds };
        context->PSSetShaderResources( 0, 3, resources );
        result = engine->SetViewport( ViewportInfo(
            0, 0, INT2(
                static_cast<int>(tempBuffer->GetSizeX()),
                static_cast<int>(tempBuffer->GetSizeY()) ) ) );
    }
    if ( result == XR_SUCCESS ) result = FxRenderer->DrawFullScreenQuad();

    ID3D11ShaderResourceView* nullResources[3]{};
    context->PSSetShaderResources( 0, 3, nullResources );

    if ( result == XR_SUCCESS ) result = zoomPS->Apply();
    if ( result == XR_SUCCESS ) {
        context->PSSetSamplers( 0, 1, &clampSampler );
        result = FxRenderer->CopyTextureToRTV(
            tempBuffer->GetShaderResView(), zoomBuffer->GetRenderTargetView(),
            INT2(
                static_cast<int>(zoomBuffer->GetSizeX()),
                static_cast<int>(zoomBuffer->GetSizeY()) ), true );
    }
    context->PSSetSamplers( 0, 1, &defaultSampler );

    if ( !rendererState.Restore() ) result = XR_FAILED;
    if ( result != XR_SUCCESS ) return result;

    m_GodRaysResult = std::move( zoomBuffer );
    *outGodRaysSRV = m_GodRaysResult->GetShaderResView().Get();
    return *outGodRaysSRV ? XR_SUCCESS : XR_FAILED;
}

XRESULT D3D11PFX_GodRays::RenderToTextureCS(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* lowClouds,
    const GodRayZoomConstantBuffer& constants,
    ID3D11ShaderResourceView** outGodRaysSRV ) {
    if ( !backbuffer || !depthCopy || !outGodRaysSRV || !FxRenderer ) {
        return XR_INVALID_ARG;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    TexturePool* texturePool = FxRenderer->GetTexturePool();
    if ( !engine || !context || !texturePool ) return XR_FAILED;

    const INT2 resolution = engine->GetResolution();
    const DXGI_FORMAT format = engine->GetBackBufferFormat();
    if ( resolution.x <= 0 || resolution.y <= 0 || format == DXGI_FORMAT_UNKNOWN ) {
        return XR_FAILED;
    }

    const INT2 downsampledResolution(
        std::max( 1, resolution.x / 4 ),
        std::max( 1, resolution.y / 4 ) );
    const TexturePool::Description description{
        downsampledResolution.x, downsampledResolution.y, format,
        D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE
    };
    auto maskBuffer = texturePool->Acquire( description );
    auto zoomBuffer = texturePool->Acquire( description );

    const auto maskCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayMask );
    const auto zoomCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayZoom );
    ID3D11SamplerState* clampSampler = engine->GetClampSamplerState();
    ID3D11SamplerState* defaultSampler = engine->GetDefaultSamplerState();
    if ( !maskBuffer || !zoomBuffer || !maskBuffer->IsValid() || !zoomBuffer->IsValid()
        || !maskBuffer->GetShaderResView() || !maskBuffer->GetUnorderedAccessView()
        || !zoomBuffer->GetShaderResView() || !zoomBuffer->GetUnorderedAccessView()
        || !maskCS || !zoomCS || !maskCS->GetShader() || !zoomCS->GetShader()
        || !clampSampler || !defaultSampler ) {
        return XR_FAILED;
    }

    auto zoomConstants = zoomCS->GetBuffer( "GodRayZoomConstantBuffer" );
    zoomConstants.Update( &constants ).Bind();
    if ( !zoomConstants.Succeeded() ) return XR_FAILED;

    context->OMSetRenderTargets( 0, nullptr, nullptr );
    XRESULT result = maskCS->Apply();
    if ( result == XR_SUCCESS ) {
        context->CSSetSamplers( 0, 1, &clampSampler );
        ID3D11ShaderResourceView* resources[3] = { backbuffer, depthCopy, lowClouds };
        ID3D11UnorderedAccessView* outputUAV = maskBuffer->GetUnorderedAccessView().Get();
        context->CSSetShaderResources( 0, 3, resources );
        context->CSSetUnorderedAccessViews( 0, 1, &outputUAV, nullptr );
        context->Dispatch(
            static_cast<UINT>((downsampledResolution.x + 7) / 8),
            static_cast<UINT>((downsampledResolution.y + 7) / 8), 1 );
    }

    ClearComputeBindings( context.Get() );
    if ( result == XR_SUCCESS ) result = zoomCS->Apply();
    if ( result == XR_SUCCESS ) {
        context->CSSetSamplers( 0, 1, &clampSampler );
        ID3D11ShaderResourceView* sourceSRV = maskBuffer->GetShaderResView().Get();
        ID3D11UnorderedAccessView* outputUAV = zoomBuffer->GetUnorderedAccessView().Get();
        context->CSSetShaderResources( 0, 1, &sourceSRV );
        context->CSSetUnorderedAccessViews( 0, 1, &outputUAV, nullptr );
        context->Dispatch(
            static_cast<UINT>((downsampledResolution.x + 7) / 8),
            static_cast<UINT>((downsampledResolution.y + 7) / 8), 1 );
    }

    ClearComputeBindings( context.Get() );
    context->CSSetSamplers( 0, 1, &defaultSampler );
    if ( result != XR_SUCCESS ) return result;

    m_GodRaysResult = std::move( zoomBuffer );
    *outGodRaysSRV = m_GodRaysResult->GetShaderResView().Get();
    return *outGodRaysSRV ? XR_SUCCESS : XR_FAILED;
}