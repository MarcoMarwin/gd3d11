#include "pch.h"
#include "D3D11PFX_HDR.h"
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

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

#define FFX_CPU
#include "include/FidelityFX/gpu/ffx_core.h"

// FidelityFX LPM still uses the legacy unprefixed CPU helper names.
#define opAAddOneF3 ffxOpAAddOneF3
#define opACpyF3 ffxOpACpyF3
#define opAMulF3 ffxOpAMulF3
#define opAMulOneF3 ffxOpAMulOneF3
#define opARcpF3 ffxOpARcpF3
#define packHalf2x16 ffxPackHalf2x16

static LPMConstantsBuffer* ActiveLPMSetupTarget = nullptr;

static void LpmSetupOut( uint32_t index, uint32_t* values ) {
    if ( !ActiveLPMSetupTarget || !values || index >= 24 ) return;
    for ( uint32_t component = 0; component < 4; ++component ) {
        ActiveLPMSetupTarget->LPM_Ctl[index][component] = values[component];
    }
}

#include "include/FidelityFX/gpu/lpm/ffx_lpm.h"
#undef packHalf2x16
#undef opARcpF3
#undef opAMulOneF3
#undef opAMulF3
#undef opACpyF3
#undef opAAddOneF3
#undef FFX_CPU

namespace {
    constexpr int LUM_SIZE = 512;
    constexpr int LUM_MIP_LEVELS = 10;

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

    class ScopedPixelResourceClear {
    public:
        explicit ScopedPixelResourceClear( ID3D11DeviceContext* context )
            : Context( context ) {
        }

        ~ScopedPixelResourceClear() {
            if ( !Context ) return;
            std::array<ID3D11ShaderResourceView*, 3> nullResources{};
            Context->PSSetShaderResources(
                0, static_cast<UINT>(nullResources.size()), nullResources.data() );
        }

    private:
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> Context;
    };

    const LPMConstantsBuffer& GetLPMConstants() {
        static LPMConstantsBuffer constants{};
        static std::once_flag initializeOnce;
        std::call_once( initializeOnce, []() {
            FfxFloat32x3 saturation = { 0.08f, 0.08f, 0.08f };
            FfxFloat32x3 crosstalk = { 1.0f, 1.0f, 1.0f };

            ActiveLPMSetupTarget = &constants;
            FfxCalculateLpmConsts(
                FFX_TRUE,
                LPM_CONFIG_709_709,
                LPM_COLORS_709_709,
                0.0f,
                16.0f,
                4.0f,
                0.18f,
                1.15f,
                saturation,
                crosstalk );
            ActiveLPMSetupTarget = nullptr;
        } );
        return constants;
    }

    bool BindLPMConstants( D3D11PShader* shader ) {
        if ( !shader ) return false;
        auto constants = shader->GetBuffer( "LPM_Constants" );
        constants.Update( &GetLPMConstants() ).Bind();
        return constants.Succeeded();
    }

    float SanitizeSetting( float value, float fallback, float minimum, float maximum ) {
        return std::isfinite( value )
            ? std::clamp( value, minimum, maximum )
            : fallback;
    }

    HDRSettingsConstantBuffer BuildHDRSettings( const GothicRendererSettings& settings ) {
        HDRSettingsConstantBuffer result{};
        result.HDR_LumWhite = SanitizeSetting( settings.HDRLumWhite, 11.2f, 0.01f, 100.0f );
        result.HDR_MiddleGray = SanitizeSetting( settings.HDRMiddleGray, 0.8f, 0.001f, 10.0f );
        result.HDR_Threshold = SanitizeSetting( settings.BloomThreshold, 0.9f, 0.0f, 100.0f );
        result.HDR_BloomStrength = SanitizeSetting( settings.BloomStrength, 1.0f, 0.0f, 10.0f );
        result.HDR_ToneMapStrength = SanitizeSetting( settings.HDRToneMapStrength, 1.0f, 0.0f, 2.0f );
        return result;
    }

    bool IsUsableTexture( RenderToTextureBuffer* texture ) {
        return texture && texture->IsValid() && texture->GetSizeX() > 0
            && texture->GetSizeY() > 0 && texture->GetShaderResView();
    }
}

D3D11PFX_HDR::D3D11PFX_HDR( D3D11PfxRenderer* rnd )
    : D3D11PFX_Effect( rnd ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const auto device = engine ? engine->GetDevice() : nullptr;
    if ( !device ) {
        LogError() << "Cannot create HDR luminance buffers without a D3D11 device.";
        return;
    }

    auto createLuminanceBuffer = [&]() -> std::unique_ptr<RenderToTextureBuffer> {
        HRESULT result = E_FAIL;
        auto buffer = std::make_unique<RenderToTextureBuffer>(
            device.Get(), LUM_SIZE, LUM_SIZE, DXGI_FORMAT_R16_FLOAT, &result,
            DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, LUM_MIP_LEVELS );
        if ( FAILED( result ) || !buffer->IsValid() ) return {};
        return buffer;
    };

    LumBuffer1 = createLuminanceBuffer();
    LumBuffer2 = createLuminanceBuffer();
    LumBuffer3 = createLuminanceBuffer();
    if ( !LumBuffer1 || !LumBuffer2 || !LumBuffer3 ) {
        LogError() << "Failed to create the HDR luminance adaptation buffers.";
        LumBuffer1.reset();
        LumBuffer2.reset();
        LumBuffer3.reset();
        return;
    }

    ResetAdaptation();
}

D3D11PFX_HDR::~D3D11PFX_HDR() = default;

void D3D11PFX_HDR::ResetAdaptation() {
    ActiveLumBuffer = 0;
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !context || !IsUsableTexture( LumBuffer1.get() )
        || !IsUsableTexture( LumBuffer2.get() )
        || !IsUsableTexture( LumBuffer3.get() ) ) {
        return;
    }

    const float neutralLuminance[4] = { 0.18f, 0.18f, 0.18f, 0.18f };
    RenderToTextureBuffer* buffers[3] = {
        LumBuffer1.get(), LumBuffer2.get(), LumBuffer3.get()
    };
    for ( auto* buffer : buffers ) {
        context->ClearRenderTargetView(
            buffer->GetRenderTargetView().Get(), neutralLuminance );
        context->GenerateMips( buffer->GetShaderResView().Get() );
    }
}

XRESULT D3D11PFX_HDR::Render(
    ID3D11RenderTargetView* output,
    ID3D11ShaderResourceView* backbuffer ) {
    if ( !output || !backbuffer || !FxRenderer ) return XR_INVALID_ARG;

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !engine || !gapi || !context
        || !IsUsableTexture( LumBuffer1.get() )
        || !IsUsableTexture( LumBuffer2.get() )
        || !IsUsableTexture( LumBuffer3.get() ) ) {
        return XR_FAILED;
    }

    const INT2 resolution = engine->GetResolution();
    if ( resolution.x <= 0 || resolution.y <= 0 ) return XR_FAILED;

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;
    ScopedTrackedRendererState rendererState( engine, gapi );
    ScopedPixelResourceClear clearResources( context.Get() );

    const XRESULT resultBeforeRestore = [&]() -> XRESULT {
        engine->SetDefaultStates();
        gapi->GetRendererState().BlendState.BlendEnabled = false;
        gapi->GetRendererState().BlendState.SetDirty();
        if ( engine->UpdateRenderStates() != XR_SUCCESS ) return XR_FAILED;

        RenderToTextureBuffer* luminance = CalcLuminance();
        if ( !IsUsableTexture( luminance ) ) return XR_FAILED;

        auto bloomBuffer = FxRenderer->GetTempBufferDS4();
        if ( !IsUsableTexture( bloomBuffer.get() )
            || CreateBloom( luminance, bloomBuffer.get() ) != XR_SUCCESS ) {
            return XR_FAILED;
        }

        auto sceneBuffer = FxRenderer->GetTempBuffer();
        if ( !IsUsableTexture( sceneBuffer.get() ) ) return XR_FAILED;

        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceView = backbuffer;
        if ( FxRenderer->CopyTextureToRTV(
                sourceView, sceneBuffer->GetRenderTargetView(), resolution ) != XR_SUCCESS ) {
            return XR_FAILED;
        }

        const auto hdrPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_HDR );
        if ( !hdrPS || hdrPS->Apply() != XR_SUCCESS ) return XR_FAILED;

        const HDRSettingsConstantBuffer settings = BuildHDRSettings(
            gapi->GetRendererState().RendererSettings );
        auto settingsBuffer = hdrPS->GetBuffer( "HDR_Settings" );
        settingsBuffer.Update( &settings ).Bind();
        if ( !settingsBuffer.Succeeded() || !BindLPMConstants( hdrPS.get() ) ) {
            return XR_FAILED;
        }

        sceneBuffer->BindToPixelShader( context.Get(), 0 );
        luminance->BindToPixelShader( context.Get(), 1 );
        bloomBuffer->BindToPixelShader( context.Get(), 2 );

        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputView = output;
        return FxRenderer->CopyTextureToRTV(
            sceneBuffer->GetShaderResView(), outputView, resolution, true );
    }();

    XRESULT result = resultBeforeRestore;
    if ( !rendererState.Restore() ) result = XR_FAILED;
    return result;
}

XRESULT D3D11PFX_HDR::CreateBloom(
    RenderToTextureBuffer* luminance,
    RenderToTextureBuffer* bloomTempBuffer ) {
    if ( !FxRenderer || !IsUsableTexture( luminance )
        || !IsUsableTexture( bloomTempBuffer ) ) {
        return XR_INVALID_ARG;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    TexturePool* texturePool = FxRenderer->GetTexturePool();
    if ( !engine || !gapi || !context || !texturePool
        || !engine->GetHDRBackBuffer().IsValid() ) {
        return XR_FAILED;
    }

    const INT2 fullResolution = engine->GetResolution();
    const DXGI_FORMAT bloomFormat = engine->GetBackBufferFormat();
    if ( fullResolution.x <= 0 || fullResolution.y <= 0
        || bloomFormat == DXGI_FORMAT_UNKNOWN ) {
        return XR_FAILED;
    }

    auto makeResolution = []( const INT2& source, int divisor ) {
        return INT2(
            std::max( 1, source.x / divisor ),
            std::max( 1, source.y / divisor ) );
    };
    const INT2 ds4Resolution = makeResolution( fullResolution, 4 );
    const INT2 ds8Resolution = makeResolution( fullResolution, 8 );
    const INT2 ds16Resolution = makeResolution( fullResolution, 16 );
    if ( bloomTempBuffer->GetSizeX() != static_cast<UINT>(ds4Resolution.x)
        || bloomTempBuffer->GetSizeY() != static_cast<UINT>(ds4Resolution.y) ) {
        return XR_INVALID_ARG;
    }

    constexpr uint32_t bloomBindFlags =
        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    auto acquireBloomBuffer = [&]( const INT2& resolution ) {
        return texturePool->Acquire( TexturePool::Description{
            resolution.x, resolution.y, bloomFormat, bloomBindFlags } );
    };

    auto level0Temp = acquireBloomBuffer( ds4Resolution );
    auto level1 = acquireBloomBuffer( ds8Resolution );
    auto level1Temp = acquireBloomBuffer( ds8Resolution );
    auto level2 = acquireBloomBuffer( ds16Resolution );
    auto level2Temp = acquireBloomBuffer( ds16Resolution );
    auto level1Combined = acquireBloomBuffer( ds8Resolution );
    auto finalCombined = acquireBloomBuffer( ds4Resolution );
    if ( !IsUsableTexture( level0Temp.get() ) || !IsUsableTexture( level1.get() )
        || !IsUsableTexture( level1Temp.get() ) || !IsUsableTexture( level2.get() )
        || !IsUsableTexture( level2Temp.get() )
        || !IsUsableTexture( level1Combined.get() )
        || !IsUsableTexture( finalCombined.get() ) ) {
        return XR_FAILED;
    }

    const auto fullscreenVS = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    const auto tonemapPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Tonemap );
    const auto simplePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Simple );
    const auto gaussPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GaussBlur );
    const auto combinePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_BloomCombine );
    if ( !fullscreenVS || !tonemapPS || !simplePS || !gaussPS || !combinePS ) {
        return XR_FAILED;
    }

    const HDRSettingsConstantBuffer hdrSettings = BuildHDRSettings(
        gapi->GetRendererState().RendererSettings );
    auto hdrSettingsBuffer = tonemapPS->GetBuffer( "HDR_Settings" );
    hdrSettingsBuffer.Update( &hdrSettings ).Bind();
    if ( !hdrSettingsBuffer.Succeeded() || !BindLPMConstants( tonemapPS.get() ) ) {
        return XR_FAILED;
    }

    ScopedPixelResourceClear clearResources( context.Get() );
    XRESULT result = fullscreenVS->Apply();
    if ( result == XR_SUCCESS ) result = tonemapPS->Apply();
    if ( result != XR_SUCCESS ) return result;

    luminance->BindToPixelShader( context.Get(), 1 );
    result = FxRenderer->CopyTextureToRTV(
        engine->GetHDRBackBuffer().GetShaderResView(),
        bloomTempBuffer->GetRenderTargetView(), ds4Resolution, true );
    if ( result != XR_SUCCESS ) return result;

    auto blurSettings = gaussPS->GetBuffer( "B_BlurSettings" );
    auto blurInto = [&]( RenderToTextureBuffer* input,
                         RenderToTextureBuffer* output,
                         RenderToTextureBuffer* scratch,
                         const INT2& resolution,
                         float blurSize ) -> XRESULT {
        if ( !IsUsableTexture( input ) || !IsUsableTexture( output )
            || !IsUsableTexture( scratch ) || !std::isfinite( blurSize )
            || blurSize <= 0.0f || gaussPS->Apply() != XR_SUCCESS ) {
            return XR_FAILED;
        }

        BlurConstantBuffer blur{};
        blur.B_BlurSize = blurSize;
        blur.B_PixelSize = float2(
            1.0f / static_cast<float>(resolution.x), 0.0f );
        blur.B_Threshold = 0.0f;
        blur.B_ColorMod = float4( 1.0f, 1.0f, 1.0f, 1.0f );
        blurSettings.Update( &blur ).Bind();
        if ( !blurSettings.Succeeded() ) return XR_FAILED;

        XRESULT passResult = FxRenderer->CopyTextureToRTV(
            input->GetShaderResView(), scratch->GetRenderTargetView(),
            resolution, true );
        if ( passResult != XR_SUCCESS ) return passResult;

        blur.B_PixelSize = float2(
            0.0f, 1.0f / static_cast<float>(resolution.y) );
        blurSettings.Update( &blur ).Bind();
        if ( !blurSettings.Succeeded() ) return XR_FAILED;
        return FxRenderer->CopyTextureToRTV(
            scratch->GetShaderResView(), output->GetRenderTargetView(),
            resolution, true );
    };

    result = blurInto(
        bloomTempBuffer, bloomTempBuffer, level0Temp.get(), ds4Resolution, 0.85f );
    if ( result != XR_SUCCESS ) return result;

    if ( simplePS->Apply() != XR_SUCCESS ) return XR_FAILED;
    result = FxRenderer->CopyTextureToRTV(
        bloomTempBuffer->GetShaderResView(), level1->GetRenderTargetView(),
        ds8Resolution, true );
    if ( result != XR_SUCCESS ) return result;
    result = blurInto( level1.get(), level1.get(), level1Temp.get(), ds8Resolution, 1.15f );
    if ( result != XR_SUCCESS ) return result;

    if ( simplePS->Apply() != XR_SUCCESS ) return XR_FAILED;
    result = FxRenderer->CopyTextureToRTV(
        level1->GetShaderResView(), level2->GetRenderTargetView(),
        ds16Resolution, true );
    if ( result != XR_SUCCESS ) return result;
    result = blurInto( level2.get(), level2.get(), level2Temp.get(), ds16Resolution, 1.45f );
    if ( result != XR_SUCCESS ) return result;

    auto combineSettings = combinePS->GetBuffer( "BloomCombineSettings" );
    auto combineInto = [&]( RenderToTextureBuffer* baseBloom,
                            RenderToTextureBuffer* wideBloom,
                            RenderToTextureBuffer* output,
                            const INT2& resolution,
                            float baseWeight,
                            float wideWeight ) -> XRESULT {
        if ( !IsUsableTexture( baseBloom ) || !IsUsableTexture( wideBloom )
            || !IsUsableTexture( output ) || combinePS->Apply() != XR_SUCCESS ) {
            return XR_FAILED;
        }

        BloomCombineConstantBuffer settings{};
        settings.BC_BaseWeight = baseWeight;
        settings.BC_WideWeight = wideWeight;
        combineSettings.Update( &settings ).Bind();
        if ( !combineSettings.Succeeded() ) return XR_FAILED;

        ID3D11ShaderResourceView* wideSRV = wideBloom->GetShaderResView().Get();
        context->PSSetShaderResources( 1, 1, &wideSRV );
        return FxRenderer->CopyTextureToRTV(
            baseBloom->GetShaderResView(), output->GetRenderTargetView(),
            resolution, true );
    };

    result = combineInto(
        level1.get(), level2.get(), level1Combined.get(),
        ds8Resolution, 0.76f, 0.24f );
    if ( result != XR_SUCCESS ) return result;
    result = combineInto(
        bloomTempBuffer, level1Combined.get(), finalCombined.get(),
        ds4Resolution, 0.64f, 0.36f );
    if ( result != XR_SUCCESS ) return result;

    if ( simplePS->Apply() != XR_SUCCESS ) return XR_FAILED;
    return FxRenderer->CopyTextureToRTV(
        finalCombined->GetShaderResView(), bloomTempBuffer->GetRenderTargetView(),
        ds4Resolution, true );
}

RenderToTextureBuffer* D3D11PFX_HDR::CalcLuminance() {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto* gapi = Engine::GAPI;
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !engine || !gapi || !context || !FxRenderer
        || !engine->GetHDRBackBuffer().IsValid() ) {
        return nullptr;
    }

    RenderToTextureBuffer* outputLuminance = nullptr;
    RenderToTextureBuffer* previousLuminance = nullptr;
    RenderToTextureBuffer* currentLuminance = nullptr;
    switch ( ActiveLumBuffer ) {
    case 0:
        outputLuminance = LumBuffer1.get();
        previousLuminance = LumBuffer2.get();
        currentLuminance = LumBuffer3.get();
        break;
    case 1:
        outputLuminance = LumBuffer3.get();
        previousLuminance = LumBuffer1.get();
        currentLuminance = LumBuffer2.get();
        break;
    case 2:
        outputLuminance = LumBuffer2.get();
        previousLuminance = LumBuffer3.get();
        currentLuminance = LumBuffer1.get();
        break;
    default:
        return nullptr;
    }

    if ( !IsUsableTexture( outputLuminance )
        || !IsUsableTexture( previousLuminance )
        || !IsUsableTexture( currentLuminance ) ) {
        return nullptr;
    }

    const auto convertPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_LumConvert );
    const auto adaptPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_LumAdapt );
    if ( !convertPS || !adaptPS ) return nullptr;

    ScopedPixelResourceClear clearResources( context.Get() );
    if ( convertPS->Apply() != XR_SUCCESS
        || FxRenderer->CopyTextureToRTV(
            engine->GetHDRBackBuffer().GetShaderResView(),
            currentLuminance->GetRenderTargetView(),
            INT2( LUM_SIZE, LUM_SIZE ), true ) != XR_SUCCESS ) {
        return nullptr;
    }
    context->GenerateMips( currentLuminance->GetShaderResView().Get() );

    if ( adaptPS->Apply() != XR_SUCCESS ) return nullptr;
    const float rawDeltaTime = gapi->GetDeltaTime();
    LumAdaptConstantBuffer adaptation{};
    adaptation.LC_DeltaTime = std::isfinite( rawDeltaTime )
        ? std::clamp( rawDeltaTime, 0.0f, 0.25f ) : 0.0f;
    auto adaptationBuffer = adaptPS->GetBuffer( "LumConvertCB" );
    adaptationBuffer.Update( &adaptation ).Bind();
    if ( !adaptationBuffer.Succeeded() ) return nullptr;

    previousLuminance->BindToPixelShader( context.Get(), 1 );
    currentLuminance->BindToPixelShader( context.Get(), 2 );
    const Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> noSlotZeroSource;
    if ( FxRenderer->CopyTextureToRTV(
            noSlotZeroSource, outputLuminance->GetRenderTargetView(),
            INT2( LUM_SIZE, LUM_SIZE ), true ) != XR_SUCCESS ) {
        return nullptr;
    }
    context->GenerateMips( outputLuminance->GetShaderResView().Get() );

    ActiveLumBuffer = (ActiveLumBuffer + 1) % 3;
    return outputLuminance;
}