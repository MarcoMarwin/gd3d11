#include "pch.h"
#include "D3D11PFX_Blur.h"
#include "Engine.h"
#include "D3D11GraphicsEngine.h"
#include "D3D11PfxRenderer.h"
#include "RenderToTextureBuffer.h"
#include "D3D11ShaderManager.h"
#include "D3D11VShader.h"
#include "D3D11PShader.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include <cmath>

D3D11PFX_Blur::D3D11PFX_Blur( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {}

D3D11PFX_Blur::~D3D11PFX_Blur() {}

/** Draws this effect to the given buffer */
XRESULT D3D11PFX_Blur::RenderBlur( RenderToTextureBuffer* fxbuffer, bool leaveResultInD4_2, float threshold, float scale, const XMFLOAT4& colorMod, PShaderID finalCopyShader ) {
    if ( !fxbuffer || !FxRenderer || !fxbuffer->IsValid()
        || fxbuffer->GetSizeX() == 0 || fxbuffer->GetSizeY() == 0
        || leaveResultInD4_2 || !std::isfinite( threshold )
        || !std::isfinite( scale ) || scale <= 0.0f
        || !std::isfinite( colorMod.x ) || !std::isfinite( colorMod.y )
        || !std::isfinite( colorMod.z ) || !std::isfinite( colorMod.w ) ) {
        return XR_INVALID_ARG;
    }

    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    const auto context = engine ? engine->GetContext() : nullptr;
    if ( !engine || !context ) return XR_FAILED;

    auto tempBuffer = FxRenderer->GetTempBufferDS4();
    auto tempBuffer2 = FxRenderer->GetTempBufferDS4();
    if ( !tempBuffer || !tempBuffer2 || !tempBuffer->IsValid() || !tempBuffer2->IsValid()
        || tempBuffer->GetSizeX() == 0 || tempBuffer->GetSizeY() == 0
        || tempBuffer->GetSizeX() != tempBuffer2->GetSizeX()
        || tempBuffer->GetSizeY() != tempBuffer2->GetSizeY() ) {
        return XR_FAILED;
    }

    const auto fullscreenVS = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    const auto gaussPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GaussBlur );
    const auto simplePS = engine->GetShaderManager().GetPShader( finalCopyShader );
    if ( !fullscreenVS || !gaussPS || !simplePS
        || fullscreenVS->Apply() != XR_SUCCESS || gaussPS->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    D3D11PFXOutputStateGuard outputState( context.Get() );
    if ( !outputState.IsValid() ) return XR_FAILED;

    const INT2 downsampledResolution(
        static_cast<int>(tempBuffer->GetSizeX()),
        static_cast<int>(tempBuffer->GetSizeY()) );
    BlurConstantBuffer bcb{};
    bcb.B_BlurSize = scale;
    bcb.B_PixelSize = float2( 1.0f / tempBuffer->GetSizeX(), 0.0f );
    bcb.B_Threshold = threshold;
    bcb.B_ColorMod = colorMod;

    auto blurSettings = gaussPS->GetBuffer( "B_BlurSettings" );
    blurSettings.Update( &bcb ).Bind();
    if ( !blurSettings.Succeeded()
        || FxRenderer->CopyTextureToRTV(
            fxbuffer->GetShaderResView(), tempBuffer->GetRenderTargetView(),
            downsampledResolution, true ) != XR_SUCCESS ) {
        return XR_FAILED;
    }

    bcb.B_PixelSize = float2( 0.0f, 1.0f / tempBuffer->GetSizeY() );
    bcb.B_Threshold = 0.0f;
    blurSettings.Update( &bcb ).Bind();
    if ( !blurSettings.Succeeded()
        || FxRenderer->CopyTextureToRTV(
            tempBuffer->GetShaderResView(), tempBuffer2->GetRenderTargetView(),
            downsampledResolution, true ) != XR_SUCCESS
        || simplePS->Apply() != XR_SUCCESS ) {
        return XR_FAILED;
    }

    return FxRenderer->CopyTextureToRTV(
        tempBuffer2->GetShaderResView(), fxbuffer->GetRenderTargetView(),
        INT2( 0, 0 ), true );
}
/** Draws this effect to the given buffer */
XRESULT D3D11PFX_Blur::Render( RenderToTextureBuffer* fxbuffer ) {
	return RenderBlur( fxbuffer );
}
