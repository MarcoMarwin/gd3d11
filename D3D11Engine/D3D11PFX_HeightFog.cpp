#include "pch.h"
#include "D3D11PFX_HeightFog.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "GSky.h"

#include <algorithm>
#include <array>
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

    class ScopedPixelResourceClear {
    public:
        explicit ScopedPixelResourceClear( ID3D11DeviceContext* context )
            : Context( context ) {
        }

        ~ScopedPixelResourceClear() {
            if ( !Context ) return;
            std::array<ID3D11ShaderResourceView*, 2> nullResources{};
            Context->PSSetShaderResources(
                0, static_cast<UINT>(nullResources.size()), nullResources.data() );
        }

    private:
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context;
    };

    float FiniteOr( float value, float fallback ) {
        return std::isfinite( value ) ? value : fallback;
    }

    bool IsFiniteColor( const XMFLOAT3& color ) {
        return std::isfinite( color.x ) && std::isfinite( color.y )
            && std::isfinite( color.z );
    }

    bool IsFiniteColor( const float3& color ) {
        return std::isfinite( color.x ) && std::isfinite( color.y )
            && std::isfinite( color.z );
    }

    bool BuildHeightFogConstants(
        GothicAPI* gapi,
        GSky* sky,
        HeightfogConstantBuffer& constants ) {
        if ( !gapi || !sky ) return false;

        const XMFLOAT4X4 projection = gapi->GetProjectionMatrix();
        if ( !std::isfinite( projection._11 ) || !std::isfinite( projection._22 )
            || !std::isfinite( projection._33 ) || !std::isfinite( projection._34 )
            || !std::isfinite( projection._43 )
            || std::abs( projection._11 ) <= 1.0e-6f
            || std::abs( projection._22 ) <= 1.0e-6f ) {
            return false;
        }

        XMVECTOR determinant{};
        const XMMATRIX inverseView = XMMatrixInverse(
            &determinant, gapi->GetViewMatrixXM() );
        const float determinantValue = XMVectorGetX( determinant );
        if ( !std::isfinite( determinantValue )
            || std::abs( determinantValue ) <= 1.0e-12f ) {
            return false;
        }

        const auto& settings = gapi->GetRendererState().RendererSettings;
        constants = {};
        constants.HF_ProjParams = float4(
            1.0f / projection._11, 1.0f / projection._22,
            projection._43, projection._33 );
        XMStoreFloat4x4( &constants.InvView, inverseView );
        constants.CameraPosition = gapi->GetCameraPosition();
        constants.HF_GlobalDensity = std::max(
            0.0f, FiniteOr( settings.FogGlobalDensity, 0.0f ) );
        constants.HF_HeightFalloff = std::max(
            0.0f, FiniteOr( settings.FogHeightFalloff, 0.0f ) );

        float fogHeight = FiniteOr( settings.FogHeight, 0.0f );
        XMFLOAT3 baseFogColor = *settings.FogColorMod.toXMFLOAT3();
        if ( !IsFiniteColor( baseFogColor ) ) baseFogColor = XMFLOAT3( 1.0f, 1.0f, 1.0f );
        XMVECTOR fogColor = XMLoadFloat3( &baseFogColor );

        const float sectionScale = std::min(
            FiniteOr( static_cast<float>(settings.SectionDrawRadius), 0.5f ),
            FiniteOr( settings.FogRange, 0.5f ) );
        constants.HF_WeightZNear = std::max(
            0.0f, WORLD_SECTION_SIZE * ((sectionScale - 0.5f) * 0.7f) - 45000.0f );
        constants.HF_WeightZFar =
            WORLD_SECTION_SIZE * ((sectionScale - 0.5f) * 0.8f);
        constants.HF_WeightZFar = std::min( constants.HF_WeightZFar, 83200.0f );
        constants.HF_WeightZNear = std::min( constants.HF_WeightZNear, 27799.9922f );

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
            const float cameraY = FiniteOr( gapi->GetCameraPosition().y, fogHeight );
            fogHeight = Toolbox::lerp(
                fogHeight, cameraY + 10000.0f, fogOverride );
            XMFLOAT3 overrideColor{};
            XMStoreFloat3( &overrideColor, gapi->GetFogColor() );
            if ( IsFiniteColor( overrideColor ) ) fogColor = XMLoadFloat3( &overrideColor );
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            constants.HF_HeightFalloff = Toolbox::lerp(
                constants.HF_HeightFalloff, 0.000001f, fogOverride );
#endif
            constants.HF_GlobalDensity = Toolbox::lerp(
                constants.HF_GlobalDensity,
                constants.HF_GlobalDensity * fogDensityFactor, fogOverride );
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
            constants.HF_WeightZNear = Toolbox::lerp(
                constants.HF_WeightZNear, WORLD_SECTION_SIZE * 0.09f, fogOverride );
            constants.HF_WeightZFar = Toolbox::lerp(
                constants.HF_WeightZFar, WORLD_SECTION_SIZE * 0.8f, fogOverride );
#endif
        }

        constants.HF_FogHeight = fogHeight;
        constants.HF_ProjAB = float2( projection._33, projection._34 );

        const float rawRain = sky->GetAtmosphereCB().AC_RainFXWeight;
        const float rain = std::isfinite( rawRain )
            ? std::clamp( rawRain, 0.0f, 1.0f ) : 0.0f;
        float rainFogColorWeight = std::min( 1.0f, rain * 2.0f );
        const float rawLightY = sky->GetAtmosphereCB().AC_LightPos.y;
        float daylightRainFog = std::isfinite( rawLightY )
            ? std::clamp( (rawLightY + 0.05f) * 4.0f, 0.0f, 1.0f )
            : 0.0f;
        daylightRainFog =
            daylightRainFog * daylightRainFog * (3.0f - 2.0f * daylightRainFog);
        rainFogColorWeight *= daylightRainFog;

        XMFLOAT3 rainFogColor = settings.RainFogColor;
        if ( !IsFiniteColor( rainFogColor ) ) rainFogColor = baseFogColor;
        XMFLOAT3 rainFogColorMod;
        XMStoreFloat3( &rainFogColorMod, XMVectorLerpV(
            fogColor, XMLoadFloat3( &rainFogColor ),
            XMVectorReplicate( rainFogColorWeight ) ) );
        constants.HF_FogColorMod = float3( rainFogColorMod );
        constants.HF_GlobalDensity = std::max( 0.0f, Toolbox::lerp(
            constants.HF_GlobalDensity,
            std::max( 0.0f, FiniteOr( settings.RainFogDensity, 0.0f ) ),
            rain * fogDensityFactorRain ) );

        return std::isfinite( constants.HF_FogHeight )
            && std::isfinite( constants.HF_GlobalDensity )
            && IsFiniteColor( constants.HF_FogColorMod );
    }
}

bool D3D11PFX_HeightFog::BuildConstants(
    GothicAPI* gapi,
    GSky* sky,
    HeightfogConstantBuffer& constants ) {
    return BuildHeightFogConstants( gapi, sky, constants );
}

XRESULT D3D11PFX_HeightFog::Render( RenderToTextureBuffer* fxbuffer ) {
    (void)fxbuffer;
    if ( !FxRenderer ) return XR_FAILED;

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    GSky* sky = gapi ? gapi->GetSky() : nullptr;
    const auto context = engine ? engine->GetContext() : nullptr;
    auto* depthBuffer = engine ? engine->GetDepthBuffer() : nullptr;
    if ( !engine || !gapi || !sky || !context
        || !depthBuffer || !depthBuffer->IsValid() ) {
        return XR_FAILED;
    }

    const INT2 resolution = engine->GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) return XR_FAILED;

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputRTV;
    context->OMGetRenderTargets( 1, outputRTV.GetAddressOf(), nullptr );
    if ( !outputRTV ) return XR_FAILED;

    const auto fullscreenVS = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    const auto heightFogPS = engine->GetShaderManager().GetPShader(
        PShaderID::PS_PFX_Heightfog );
    if ( !fullscreenVS || !heightFogPS
        || !fullscreenVS->GetShader() || !heightFogPS->GetShader() ) {
        return XR_FAILED;
    }

    HeightfogConstantBuffer constants{};
    if ( !BuildConstants( gapi, sky, constants ) ) return XR_FAILED;
    auto fogBuffer = heightFogPS->GetBuffer( "PFXBuffer" );
    auto atmosphereBuffer = heightFogPS->GetBuffer( "Atmosphere" );
    fogBuffer.Update( &constants );
    atmosphereBuffer.Update( &sky->GetAtmosphereCB() );
    if ( !fogBuffer.Succeeded() || !atmosphereBuffer.Succeeded() ) {
        return XR_FAILED;
    }

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;
    ScopedTrackedRendererState rendererState( engine, gapi );
    ScopedPixelResourceClear clearResources( context.Get() );

    XRESULT result = [&]() -> XRESULT {
        engine->SetDefaultStates();
        auto& state = gapi->GetRendererState();
        state.RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
        state.RasterizerState.SetDirty();
        state.BlendState.SetDefault();
        state.BlendState.BlendEnabled = true;
        state.BlendState.SetDirty();

        if ( fullscreenVS->Apply() != XR_SUCCESS
            || heightFogPS->Apply() != XR_SUCCESS ) {
            return XR_FAILED;
        }
        fogBuffer.Bind();
        atmosphereBuffer.Bind();
        if ( !fogBuffer.Succeeded() || !atmosphereBuffer.Succeeded()
            || engine->SetViewport(
                ViewportInfo( 0, 0, resolution ) ) != XR_SUCCESS ) {
            return XR_FAILED;
        }

        ID3D11RenderTargetView* output = outputRTV.Get();
        context->OMSetRenderTargets( 1, &output, nullptr );
        depthBuffer->BindToPixelShader( context.Get(), 1 );
        return FxRenderer->DrawFullScreenQuad();
    }();

    if ( !rendererState.Restore() ) result = XR_FAILED;
    return result;
}