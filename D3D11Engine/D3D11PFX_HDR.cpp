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

#include <cmath>

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
    if ( ActiveLPMSetupTarget == nullptr || index >= 24 )
        return;

    for ( uint32_t component = 0; component < 4; ++component )
        ActiveLPMSetupTarget->LPM_Ctl[index][component] = values[component];
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
const LPMConstantsBuffer& GetLPMConstants() {
    static LPMConstantsBuffer constants = {};
    static bool initialized = false;
    if ( initialized )
        return constants;

    // Gothic's adapted frame maps average luminance to 18% gray. Keep the
    // matching 16.0 HDR maximum / 4-stop exposure and use LPM's official
    // look controls for stronger separation without changing scene exposure.
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
    initialized = true;
    return constants;
}

void BindLPMConstants( D3D11PShader* shader ) {
    shader->GetBuffer( "LPM_Constants" ).Update( &GetLPMConstants() ).Bind();
}
}

const int LUM_SIZE = 512;

D3D11PFX_HDR::D3D11PFX_HDR( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {
	D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

	// Create lum-buffer
	LumBuffer1 = new RenderToTextureBuffer( engine->GetDevice().Get(), LUM_SIZE, LUM_SIZE, DXGI_FORMAT_R16_FLOAT, nullptr,
        DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, static_cast<int>(log( LUM_SIZE ) / log( 2 )) );
	LumBuffer2 = new RenderToTextureBuffer( engine->GetDevice().Get(), LUM_SIZE, LUM_SIZE, DXGI_FORMAT_R16_FLOAT, nullptr,
        DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, static_cast<int>(log( LUM_SIZE ) / log( 2 )) );
	LumBuffer3 = new RenderToTextureBuffer( engine->GetDevice().Get(), LUM_SIZE, LUM_SIZE, DXGI_FORMAT_R16_FLOAT, nullptr,
        DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, static_cast<int>(log( LUM_SIZE ) / log( 2 )) );

	ResetAdaptation();
}

D3D11PFX_HDR::~D3D11PFX_HDR() {
	delete LumBuffer1;
	delete LumBuffer2;
	delete LumBuffer3;
}

void D3D11PFX_HDR::ResetAdaptation() {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !engine || !LumBuffer1 || !LumBuffer2 || !LumBuffer3 ) {
        return;
    }

    // 18% gray produces exactly neutral exposure (1.0) in GetToneMapExposure.
    const float neutralLuminance[4] = { 0.18f, 0.18f, 0.18f, 0.18f };
    RenderToTextureBuffer* buffers[3] = { LumBuffer1, LumBuffer2, LumBuffer3 };
    for ( RenderToTextureBuffer* buffer : buffers ) {
        engine->GetContext()->ClearRenderTargetView( buffer->GetRenderTargetView().Get(), neutralLuminance );
        engine->GetContext()->GenerateMips( buffer->GetShaderResView().Get() );
    }
    ActiveLumBuffer = 0;
}

/** Draws this effect to the given buffer */
XRESULT D3D11PFX_HDR::Render( ID3D11RenderTargetView* output, ID3D11ShaderResourceView* backbuffer ) {
	D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    engine->SetDefaultStates();
	Engine::GAPI->GetRendererState().BlendState.BlendEnabled = false;
	Engine::GAPI->GetRendererState().BlendState.SetDirty();
    engine->UpdateRenderStates();

	// Save old rendertargets
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
	engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

	RenderToTextureBuffer* lum = CalcLuminance();

    auto tempBufferDs4_1 = FxRenderer->GetTempBufferDS4();
	CreateBloom( lum, tempBufferDs4_1.get() );

    auto tempBuffer = FxRenderer->GetTempBuffer();
	// Copy the original image to our temp-buffer
    FxRenderer->CopyTextureToRTV( backbuffer, tempBuffer->GetRenderTargetView(), engine->GetResolution() );

    // Bind scene and luminance
    tempBuffer->BindToPixelShader( engine->GetContext().Get(), 0 );
    lum->BindToPixelShader( engine->GetContext().Get(), 1 );

    // Bind bloom
    tempBufferDs4_1->BindToPixelShader( engine->GetContext().Get(), 2 );

    // Draw the HDR-Shader
    auto hps = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_HDR );
    hps->Apply();

    HDRSettingsConstantBuffer hcb = {};
    hcb.HDR_LumWhite = Engine::GAPI->GetRendererState().RendererSettings.HDRLumWhite;
    hcb.HDR_MiddleGray = Engine::GAPI->GetRendererState().RendererSettings.HDRMiddleGray;
    hcb.HDR_Threshold = Engine::GAPI->GetRendererState().RendererSettings.BloomThreshold;
    hcb.HDR_BloomStrength = Engine::GAPI->GetRendererState().RendererSettings.BloomStrength;
    hcb.HDR_ToneMapStrength = Engine::GAPI->GetRendererState().RendererSettings.HDRToneMapStrength;
    hps->GetBuffer( "HDR_Settings" ).Update( &hcb ).Bind();
    BindLPMConstants( hps.get() );

    FxRenderer->CopyTextureToRTV( tempBuffer->GetShaderResView(), output, engine->GetResolution(), true );

	// Restore rendertargets
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
	engine->GetContext()->PSSetShaderResources( 1, 1, srv.GetAddressOf() );
	engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

	return XR_SUCCESS;
}

/** Builds a multi-resolution bloom pyramid and leaves the result in bloomTempBuffer. */
void D3D11PFX_HDR::CreateBloom( RenderToTextureBuffer* lum, RenderToTextureBuffer* bloomTempBuffer ) {
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    const INT2 fullRes = Engine::GraphicsEngine->GetResolution();
    const DXGI_FORMAT bloomFormat = engine->GetBackBufferFormat();
    const auto bloomBindFlags = static_cast<DXGI_USAGE>(D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);

    auto makeResolution = []( const INT2& source, int divisor ) {
        return INT2( std::max( 1, source.x / divisor ), std::max( 1, source.y / divisor ) );
    };

    auto acquireBloomBuffer = [&]( const INT2& resolution ) {
        return FxRenderer->GetTexturePool()->Acquire( TexturePool::Description{
            resolution.x,
            resolution.y,
            bloomFormat,
            bloomBindFlags
        } );
    };

    const INT2 ds4Res = makeResolution( fullRes, 4 );
    const INT2 ds8Res = makeResolution( fullRes, 8 );
    const INT2 ds16Res = makeResolution( fullRes, 16 );

    auto level0Temp = acquireBloomBuffer( ds4Res );
    auto level1 = acquireBloomBuffer( ds8Res );
    auto level1Temp = acquireBloomBuffer( ds8Res );
    auto level2 = acquireBloomBuffer( ds16Res );
    auto level2Temp = acquireBloomBuffer( ds16Res );
    auto level1Combined = acquireBloomBuffer( ds8Res );
    auto finalCombined = acquireBloomBuffer( ds4Res );

    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();

    auto tonemapPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Tonemap );
    tonemapPS->Apply();

    HDRSettingsConstantBuffer hcb = {};
    hcb.HDR_LumWhite = Engine::GAPI->GetRendererState().RendererSettings.HDRLumWhite;
    hcb.HDR_MiddleGray = Engine::GAPI->GetRendererState().RendererSettings.HDRMiddleGray;
    hcb.HDR_Threshold = Engine::GAPI->GetRendererState().RendererSettings.BloomThreshold;
    hcb.HDR_BloomStrength = Engine::GAPI->GetRendererState().RendererSettings.BloomStrength;
    hcb.HDR_ToneMapStrength = Engine::GAPI->GetRendererState().RendererSettings.HDRToneMapStrength;
    tonemapPS->GetBuffer( "HDR_Settings" ).Update( &hcb ).Bind();
    BindLPMConstants( tonemapPS.get() );

    lum->BindToPixelShader( context.Get(), 1 );
    FxRenderer->CopyTextureToRTV( engine->GetHDRBackBuffer().GetShaderResView(), bloomTempBuffer->GetRenderTargetView(), ds4Res, true );

    auto simplePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Simple );
    auto gaussPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GaussBlur );
    auto combinePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_BloomCombine );

    auto blurInto = [&]( RenderToTextureBuffer* input, RenderToTextureBuffer* output, RenderToTextureBuffer* scratch, const INT2& resolution, float blurSize ) {
        gaussPS->Apply();

        BlurConstantBuffer bcb = {};
        bcb.B_BlurSize = blurSize;
        bcb.B_PixelSize = float2( 1.0f / std::max<UINT>( output->GetSizeX(), 1 ), 0.0f );
        bcb.B_Threshold = 0.0f;
        bcb.B_ColorMod = float4( 1.0f, 1.0f, 1.0f, 1.0f );
        gaussPS->GetBuffer( "B_BlurSettings" ).Update( &bcb ).Bind();
        FxRenderer->CopyTextureToRTV( input->GetShaderResView(), scratch->GetRenderTargetView(), resolution, true );

        bcb.B_PixelSize = float2( 0.0f, 1.0f / std::max<UINT>( output->GetSizeY(), 1 ) );
        gaussPS->GetBuffer( "B_BlurSettings" ).Update( &bcb ).Bind();
        FxRenderer->CopyTextureToRTV( scratch->GetShaderResView(), output->GetRenderTargetView(), resolution, true );
    };

    blurInto( bloomTempBuffer, bloomTempBuffer, level0Temp.get(), ds4Res, 0.85f );

    simplePS->Apply();
    FxRenderer->CopyTextureToRTV( bloomTempBuffer->GetShaderResView(), level1->GetRenderTargetView(), ds8Res, true );
    blurInto( level1.get(), level1.get(), level1Temp.get(), ds8Res, 1.15f );

    simplePS->Apply();
    FxRenderer->CopyTextureToRTV( level1->GetShaderResView(), level2->GetRenderTargetView(), ds16Res, true );
    blurInto( level2.get(), level2.get(), level2Temp.get(), ds16Res, 1.45f );

    auto combineInto = [&]( RenderToTextureBuffer* baseBloom, RenderToTextureBuffer* wideBloom, RenderToTextureBuffer* output, const INT2& resolution, float baseWeight, float wideWeight ) {
        combinePS->Apply();
        BloomCombineConstantBuffer cb = {};
        cb.BC_BaseWeight = baseWeight;
        cb.BC_WideWeight = wideWeight;
        cb.BC_Pad = float2( 0.0f, 0.0f );
        combinePS->GetBuffer( "BloomCombineSettings" ).Update( &cb ).Bind();

        ID3D11ShaderResourceView* wideSrv = wideBloom->GetShaderResView().Get();
        context->PSSetShaderResources( 1, 1, &wideSrv );
        FxRenderer->CopyTextureToRTV( baseBloom->GetShaderResView(), output->GetRenderTargetView(), resolution, true );
    };

    combineInto( level1.get(), level2.get(), level1Combined.get(), ds8Res, 0.76f, 0.24f );
    combineInto( bloomTempBuffer, level1Combined.get(), finalCombined.get(), ds4Res, 0.64f, 0.36f );

    simplePS->Apply();
    FxRenderer->CopyTextureToRTV( finalCombined->GetShaderResView(), bloomTempBuffer->GetRenderTargetView(), ds4Res, true );

    ID3D11ShaderResourceView* nullSrvs[3] = {};
    context->PSSetShaderResources( 0, 3, nullSrvs );
}

/** Calcualtes the luminance */
RenderToTextureBuffer* D3D11PFX_HDR::CalcLuminance() {
	D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

	RenderToTextureBuffer* lumRTV = nullptr;
	RenderToTextureBuffer* lastLum = nullptr;
	RenderToTextureBuffer* currentLum = nullptr;

	// Figure out which buffers to use where
	switch ( ActiveLumBuffer ) {
	case 0:
		lumRTV = LumBuffer1;
		lastLum = LumBuffer2;
		currentLum = LumBuffer3;
		break;

	case 1:
		lumRTV = LumBuffer3;
		lastLum = LumBuffer1;
		currentLum = LumBuffer2;
		break;

	case 2:
		lumRTV = LumBuffer2;
		lastLum = LumBuffer3;
		currentLum = LumBuffer1;
		break;
	default:
		return nullptr;
	}

	auto lps = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_LumConvert );
	lps->Apply();

	// Convert the backbuffer to our luminance buffer
    FxRenderer->CopyTextureToRTV( engine->GetHDRBackBuffer().GetShaderResView(), currentLum->GetRenderTargetView(), INT2( LUM_SIZE, LUM_SIZE ), true );

	// Create the average luminance
	engine->GetContext()->GenerateMips( currentLum->GetShaderResView().Get() );

	auto aps = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_LumAdapt );
	aps->Apply();

	LumAdaptConstantBuffer lcb;
	lcb.LC_DeltaTime = Engine::GAPI->GetDeltaTime();
	aps->GetBuffer( "LumConvertCB" ).Update( &lcb ).Bind();

	// Bind luminances
	lastLum->BindToPixelShader( engine->GetContext().Get(), 1 );
	currentLum->BindToPixelShader( engine->GetContext().Get(), 2 );

	// Convert the backbuffer to our luminance buffer
    FxRenderer->CopyTextureToRTV( nullptr, lumRTV->GetRenderTargetView(), INT2( LUM_SIZE, LUM_SIZE ), true );

	// Create the average luminance
	engine->GetContext()->GenerateMips( lumRTV->GetShaderResView().Get() );

	// Increment
	ActiveLumBuffer++;
	ActiveLumBuffer = (ActiveLumBuffer == 3 ? 0 : ActiveLumBuffer);

	return lumRTV;
}
