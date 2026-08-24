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
#include "D3D11ShadowMap.h"

extern bool FeatureLevel10Compatibility;

namespace {
	float GetRainSkyVisibility() {
		auto* sky = Engine::GAPI->GetSky();
		if( !sky )
			return 1.0f;

		const float rain = std::clamp( sky->GetAtmosphereCB().AC_RainFXWeight, 0.0f, 1.0f );
		const float transition = std::clamp( (rain - 0.05f) / 0.60f, 0.0f, 1.0f );
		const float smoothOcclusion = transition * transition * (3.0f - 2.0f * transition);
		return 1.0f - smoothOcclusion;
	}

	float GetLowSunGodRayBoost() {
		auto* sky = Engine::GAPI->GetSky();
		if( !sky )
			return 1.0f;

		const float sunHeight = std::clamp( sky->GetAtmosphereCB().AC_LightPos.y, 0.0f, 1.0f );
		const float lowSun = 1.0f - std::clamp( (sunHeight - 0.05f) / 0.30f, 0.0f, 1.0f );
		return 1.0f + lowSun * 0.35f;
	}
}

D3D11PFX_GodRays::D3D11PFX_GodRays( D3D11PfxRenderer* rnd ) : D3D11PFX_Effect( rnd ) {}
void D3D11PFX_GodRays::ResetTemporalHistory() {
    m_VolumetricHistory[0].reset();
    m_VolumetricHistory[1].reset();
    m_VolumetricDepthHistory[0].reset();
    m_VolumetricDepthHistory[1].reset();
    m_VolumetricHistoryIndex = 0;
    m_VolumetricHistoryValid = false;
    m_PreviousViewProjection = {};
    m_PreviousCameraPosition = {};
    m_LastVolumetricRenderTime = -1.0f;
}

/** Draws this effect to the given buffer */
XRESULT D3D11PFX_GodRays::Render(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depth,
    ID3D11ShaderResourceView* lowClouds ) {
    if ( Engine::GAPI->GetSky()->GetAtmosphereSettings().LightDirection.y <= 0 ) {
        return XR_SUCCESS; // Don't render the godrays in the night-time
    }

	D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

	engine->SetDefaultStates();

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    if ( !FeatureLevel10Compatibility ) {
        auto res = RenderCS( backbuffer, depth, lowClouds );
        engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
        return res;
    }

	XMVECTOR xmSunPosition = XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );

	float outerRadius = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_OuterRadius;
	xmSunPosition *= outerRadius;
	xmSunPosition += Engine::GAPI->GetCameraPositionXM(); // Maybe use cameraposition from sky?

	XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
	XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );

	XMMATRIX viewProj = XMMatrixTranspose( XMMatrixMultiply( proj, view ) );
	view = XMMatrixTranspose( view );

	XMFLOAT3 sunViewPosition; XMStoreFloat3( &sunViewPosition, XMVector3TransformCoord( xmSunPosition, view ) ); // This is for checking if the light is behind the camera
	XMFLOAT3 sunPosition; XMStoreFloat3( &sunPosition, XMVector3TransformCoord( xmSunPosition, viewProj ) );

	if ( sunViewPosition.z < 0.0f )
		return XR_SUCCESS; // Don't render the godrays when the sun is behind the camera

	GodRayZoomConstantBuffer gcb = {};
	gcb.GR_Weight = 1.0f;
	gcb.GR_Decay = Engine::GAPI->GetRendererState().RendererSettings.GodRayDecay;
	gcb.GR_Weight = Engine::GAPI->GetRendererState().RendererSettings.GodRayWeight * Engine::GAPI->GetRendererState().RendererSettings.GodRayStrength * GetRainSkyVisibility() * GetLowSunGodRayBoost();
	gcb.GR_Density = Engine::GAPI->GetRendererState().RendererSettings.GodRayDensity;

	gcb.GR_Center.x = sunPosition.x / 2.0f + 0.5f;
	gcb.GR_Center.y = sunPosition.y / -2.0f + 0.5f;

	gcb.GR_ColorMod = Engine::GAPI->GetRendererState().RendererSettings.GodRayColorMod;

	if ( abs( gcb.GR_Center.x - 0.5f ) > 0.5f )
		gcb.GR_Weight *= std::max( 0.0f, 1.0f - (abs( gcb.GR_Center.x - 0.5f ) - 0.5f) / 0.5f );

	if ( abs( gcb.GR_Center.y - 0.5f ) > 0.5f )
		gcb.GR_Weight *= std::max( 0.0f, 1.0f - (abs( gcb.GR_Center.y - 0.5f ) - 0.5f) / 0.5f );
	if ( gcb.GR_Weight <= 0.0f )
		return XR_SUCCESS;

	auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
	auto maskPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GodRayMask );
	auto zoomPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GodRayZoom );

	maskPS->Apply();
	vs->Apply();

    auto tempBuffer = FxRenderer->GetTempBufferDS4();
    auto tempBuffer2 = FxRenderer->GetTempBufferDS4();

	// Draw downscaled mask
	engine->GetContext()->OMSetRenderTargets( 1, tempBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    ID3D11ShaderResourceView* srvs[3] {
        backbuffer,
        depth,
        lowClouds,
    };
    engine->GetContext()->PSSetShaderResources( 0, 3, srvs );

    engine->SetViewport({ 0,0, INT2(tempBuffer->GetSizeX(), tempBuffer->GetSizeY()) });

    FxRenderer->DrawFullScreenQuad();

    // Zoom
    zoomPS->Apply();

    zoomPS->GetBuffer( "GodRayZoomConstantBuffer" ).Update( &gcb ).Bind();

    auto clampSampler = engine->GetClampSamplerState();
    engine->GetContext()->PSSetSamplers( 0, 1, &clampSampler );

    FxRenderer->CopyTextureToRTV( tempBuffer->GetShaderResView(), tempBuffer2->GetRenderTargetView(), INT2( 0, 0 ), true );

    // Upscale and blend
    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    FxRenderer->CopyTextureToRTV( tempBuffer2->GetShaderResView(), oldRTV, engine->GetResolution() );

    engine->SetViewport({ 0,0, engine->GetResolution() });

    ID3D11ShaderResourceView* nullSRVs[3] {
        nullptr,
        nullptr,
        nullptr,
    };
    engine->GetContext()->PSSetShaderResources( 0, 3, nullSRVs );

	engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

	return XR_SUCCESS;
}

/** Compute shader path for FL11+ */
XRESULT D3D11PFX_GodRays::RenderCS(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* lowClouds ) {

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();

    engine->SetDefaultStates();

    XMVECTOR xmSunPosition = XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );

    float outerRadius = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_OuterRadius;
    xmSunPosition *= outerRadius;
    xmSunPosition += Engine::GAPI->GetCameraPositionXM();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );

    XMMATRIX viewProj = XMMatrixTranspose( XMMatrixMultiply( proj, view ) );
    view = XMMatrixTranspose( view );

    XMFLOAT3 sunViewPosition; XMStoreFloat3( &sunViewPosition, XMVector3TransformCoord( xmSunPosition, view ) );
    XMFLOAT3 sunPosition; XMStoreFloat3( &sunPosition, XMVector3TransformCoord( xmSunPosition, viewProj ) );

    if ( sunViewPosition.z < 0.0f )
        return XR_SUCCESS;

    GodRayZoomConstantBuffer gcb = {};
    gcb.GR_Weight = 1.0f;
    gcb.GR_Decay = Engine::GAPI->GetRendererState().RendererSettings.GodRayDecay;
    gcb.GR_Weight = Engine::GAPI->GetRendererState().RendererSettings.GodRayWeight * Engine::GAPI->GetRendererState().RendererSettings.GodRayStrength * GetRainSkyVisibility() * GetLowSunGodRayBoost();
    gcb.GR_Density = Engine::GAPI->GetRendererState().RendererSettings.GodRayDensity;

    gcb.GR_Center.x = sunPosition.x / 2.0f + 0.5f;
    gcb.GR_Center.y = sunPosition.y / -2.0f + 0.5f;

    gcb.GR_ColorMod = Engine::GAPI->GetRendererState().RendererSettings.GodRayColorMod;

    if ( abs( gcb.GR_Center.x - 0.5f ) > 0.5f )
        gcb.GR_Weight *= std::max( 0.0f, 1.0f - (abs( gcb.GR_Center.x - 0.5f ) - 0.5f) / 0.5f );

    if ( abs( gcb.GR_Center.y - 0.5f ) > 0.5f )
        gcb.GR_Weight *= std::max( 0.0f, 1.0f - (abs( gcb.GR_Center.y - 0.5f ) - 0.5f) / 0.5f );

    if ( gcb.GR_Weight <= 0.0f )
        return XR_SUCCESS;

    // Save old render targets
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
    context->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

    ID3D11RenderTargetView* nullRtv = nullptr;
    engine->GetContext()->OMSetRenderTargets( 1, &nullRtv, nullptr );

    auto res = engine->GetResolution();
    INT2 ds4Size = { res.x / 4, res.y / 4 };

    // Acquire DS4 UAV-capable textures from the pool
    auto maskBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
    auto zoomBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );

    auto clampSampler = engine->GetClampSamplerState();

    // --- Pass 1: Compute Shader Mask ---
    auto maskCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayMask );
    maskCS->Apply();

    context->CSSetSamplers( 0, 1, &clampSampler );

    ID3D11ShaderResourceView* maskSRVs[3] = { backbuffer, depthCopy, lowClouds };
    context->CSSetShaderResources( 0, 3, maskSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, maskBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (ds4Size.x + 7) / 8, (ds4Size.y + 7) / 8, 1 );

    // Unbind UAV and SRVs
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    context->CSSetShaderResources( 0, 3, nullSRVs );

    // --- Pass 2: Compute Shader Zoom ---
    auto zoomCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayZoom );
    zoomCS->Apply();

    zoomCS->GetBuffer( "GodRayZoomConstantBuffer" ).Update( &gcb ).Bind();

    context->CSSetSamplers( 0, 1, &clampSampler );

    ID3D11ShaderResourceView* zoomSRV = maskBuffer->GetShaderResView().Get();
    context->CSSetShaderResources( 0, 1, &zoomSRV );
    context->CSSetUnorderedAccessViews( 0, 1, zoomBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );

    context->Dispatch( (ds4Size.x + 7) / 8, (ds4Size.y + 7) / 8, 1 );

    // Unbind
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    ID3D11ShaderResourceView* nullSRV1 = nullptr;
    context->CSSetShaderResources( 0, 1, &nullSRV1 );
    context->CSSetShader( nullptr, nullptr, 0 );

    // --- Pass 3: Upscale and additive blend via pixel shader (reuse existing path) ---
    Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();

    FxRenderer->CopyTextureToRTV( zoomBuffer->GetShaderResView(), oldRTV, res );

    engine->SetViewport( { 0, 0, res } );

    engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );
    auto defaultSampler = engine->GetDefaultSamplerState();
    engine->GetContext()->PSSetSamplers( 0, 1, &defaultSampler );

    return XR_SUCCESS;
}

/** Public entry point: renders godrays to a pool texture, skipping the final additive blit */
XRESULT D3D11PFX_GodRays::RenderToTexture(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* lowClouds,
    ID3D11ShaderResourceView** outGodRaysSRV ) {

    *outGodRaysSRV = nullptr;

    if ( Engine::GAPI->GetSky()->GetAtmosphereSettings().LightDirection.y <= 0 )
        return XR_SUCCESS;

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    engine->SetDefaultStates();

    if ( !FeatureLevel10Compatibility ) {
        return RenderToTextureCS( backbuffer, depthCopy, lowClouds, outGodRaysSRV );
    }

    // FL10 pixel shader path: mask -> zoom -> write to pool texture (no additive blit)
    XMVECTOR xmSunPosition = XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );
    float outerRadius = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_OuterRadius;
    xmSunPosition *= outerRadius;
    xmSunPosition += Engine::GAPI->GetCameraPositionXM();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );
    XMMATRIX viewProj = XMMatrixTranspose( XMMatrixMultiply( proj, view ) );
    view = XMMatrixTranspose( view );

    XMFLOAT3 sunViewPosition; XMStoreFloat3( &sunViewPosition, XMVector3TransformCoord( xmSunPosition, view ) );
    XMFLOAT3 sunPosition; XMStoreFloat3( &sunPosition, XMVector3TransformCoord( xmSunPosition, viewProj ) );

    if ( sunViewPosition.z < 0.0f )
        return XR_SUCCESS;

    GodRayZoomConstantBuffer gcb = {};
    gcb.GR_Weight = 1.0f;
    gcb.GR_Decay = Engine::GAPI->GetRendererState().RendererSettings.GodRayDecay;
    gcb.GR_Weight = Engine::GAPI->GetRendererState().RendererSettings.GodRayWeight * Engine::GAPI->GetRendererState().RendererSettings.GodRayStrength * GetRainSkyVisibility() * GetLowSunGodRayBoost();
    gcb.GR_Density = Engine::GAPI->GetRendererState().RendererSettings.GodRayDensity;
    gcb.GR_Center.x = sunPosition.x / 2.0f + 0.5f;
    gcb.GR_Center.y = sunPosition.y / -2.0f + 0.5f;
    gcb.GR_ColorMod = Engine::GAPI->GetRendererState().RendererSettings.GodRayColorMod;

    if ( abs( gcb.GR_Center.x - 0.5f ) > 0.5f )
        gcb.GR_Weight *= std::max( 0.0f, 1.0f - (abs( gcb.GR_Center.x - 0.5f ) - 0.5f) / 0.5f );
    if ( abs( gcb.GR_Center.y - 0.5f ) > 0.5f )
        gcb.GR_Weight *= std::max( 0.0f, 1.0f - (abs( gcb.GR_Center.y - 0.5f ) - 0.5f) / 0.5f );

    if ( gcb.GR_Weight <= 0.0f )
        return XR_SUCCESS;

    auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    auto maskPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GodRayMask );
    auto zoomPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_GodRayZoom );

    maskPS->Apply();
    vs->Apply();

    auto tempBuffer = FxRenderer->GetTempBufferDS4();
    auto tempBuffer2 = FxRenderer->GetTempBufferDS4();

    engine->GetContext()->OMSetRenderTargets( 1, tempBuffer->GetRenderTargetView().GetAddressOf(), nullptr );

    ID3D11ShaderResourceView* srvs[3] { backbuffer, depthCopy, lowClouds };
    engine->GetContext()->PSSetShaderResources( 0, 3, srvs );
    engine->SetViewport({ 0, 0, INT2(tempBuffer->GetSizeX(), tempBuffer->GetSizeY()) });
    FxRenderer->DrawFullScreenQuad();

    zoomPS->Apply();
    zoomPS->GetBuffer( "GodRayZoomConstantBuffer" ).Update( &gcb ).Bind();

    auto clampSampler = engine->GetClampSamplerState();
    engine->GetContext()->PSSetSamplers( 0, 1, &clampSampler );

    FxRenderer->CopyTextureToRTV( tempBuffer->GetShaderResView(), tempBuffer2->GetRenderTargetView(), INT2( 0, 0 ), true );

    // Keep the result texture alive until next frame by storing the handle as a member
    m_GodRaysResult = std::move( tempBuffer2 );
    *outGodRaysSRV = m_GodRaysResult->GetShaderResView().Get();

    ID3D11ShaderResourceView* nullSRVs[3] { nullptr, nullptr, nullptr };
    engine->GetContext()->PSSetShaderResources( 0, 3, nullSRVs );

    auto defaultSampler2 = engine->GetDefaultSamplerState();
    engine->GetContext()->PSSetSamplers( 0, 1, &defaultSampler2 );

    return XR_SUCCESS;
}

/** CS path: mask+zoom to pool texture, no final blit */
XRESULT D3D11PFX_GodRays::RenderToTextureCS(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* lowClouds,
    ID3D11ShaderResourceView** outGodRaysSRV ) {

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();

    XMVECTOR xmSunPosition = XMLoadFloat3( Engine::GAPI->GetSky()->GetAtmosphereCB().AC_LightPos.toXMFLOAT3() );
    float outerRadius = Engine::GAPI->GetSky()->GetAtmosphereCB().AC_OuterRadius;
    xmSunPosition *= outerRadius;
    xmSunPosition += Engine::GAPI->GetCameraPositionXM();

    XMMATRIX view = Engine::GAPI->GetViewMatrixXM();
    XMMATRIX proj = XMLoadFloat4x4( &Engine::GAPI->GetProjectionMatrix() );
    XMMATRIX viewProj = XMMatrixTranspose( XMMatrixMultiply( proj, view ) );
    view = XMMatrixTranspose( view );

    XMFLOAT3 sunViewPosition; XMStoreFloat3( &sunViewPosition, XMVector3TransformCoord( xmSunPosition, view ) );
    XMFLOAT3 sunPosition; XMStoreFloat3( &sunPosition, XMVector3TransformCoord( xmSunPosition, viewProj ) );

    if ( sunViewPosition.z < 0.0f )
        return XR_SUCCESS;

    GodRayZoomConstantBuffer gcb = {};
    gcb.GR_Weight = 1.0f;
    gcb.GR_Decay = Engine::GAPI->GetRendererState().RendererSettings.GodRayDecay;
    gcb.GR_Weight = Engine::GAPI->GetRendererState().RendererSettings.GodRayWeight * Engine::GAPI->GetRendererState().RendererSettings.GodRayStrength * GetRainSkyVisibility() * GetLowSunGodRayBoost();
    gcb.GR_Density = Engine::GAPI->GetRendererState().RendererSettings.GodRayDensity;
    gcb.GR_Center.x = sunPosition.x / 2.0f + 0.5f;
    gcb.GR_Center.y = sunPosition.y / -2.0f + 0.5f;
    gcb.GR_ColorMod = Engine::GAPI->GetRendererState().RendererSettings.GodRayColorMod;

    if ( abs( gcb.GR_Center.x - 0.5f ) > 0.5f )
        gcb.GR_Weight *= std::max( 0.0f, 1.0f - (abs( gcb.GR_Center.x - 0.5f ) - 0.5f) / 0.5f );
    if ( abs( gcb.GR_Center.y - 0.5f ) > 0.5f )
        gcb.GR_Weight *= std::max( 0.0f, 1.0f - (abs( gcb.GR_Center.y - 0.5f ) - 0.5f) / 0.5f );

    if ( gcb.GR_Weight <= 0.0f )
        return XR_SUCCESS;

    ID3D11RenderTargetView* nullRtv = nullptr;
    context->OMSetRenderTargets( 1, &nullRtv, nullptr );

    auto res = engine->GetResolution();
    INT2 ds4Size = { res.x / 4, res.y / 4 };

    auto maskBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
    auto zoomBuffer = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );

    auto clampSampler = engine->GetClampSamplerState();

    // --- Pass 1: CS Mask ---
    auto maskCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayMask );
    maskCS->Apply();
    context->CSSetSamplers( 0, 1, &clampSampler );

    ID3D11ShaderResourceView* maskSRVs[3] = { backbuffer, depthCopy, lowClouds };
    context->CSSetShaderResources( 0, 3, maskSRVs );
    context->CSSetUnorderedAccessViews( 0, 1, maskBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );
    context->Dispatch( (ds4Size.x + 7) / 8, (ds4Size.y + 7) / 8, 1 );

    ID3D11UnorderedAccessView* nullUAV = nullptr;
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    context->CSSetShaderResources( 0, 3, nullSRVs );

    // --- Pass 2: CS Zoom ---
    auto zoomCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayZoom );
    zoomCS->Apply();
    zoomCS->GetBuffer( "GodRayZoomConstantBuffer" ).Update( &gcb ).Bind();
    context->CSSetSamplers( 0, 1, &clampSampler );

    ID3D11ShaderResourceView* zoomSRV = maskBuffer->GetShaderResView().Get();
    context->CSSetShaderResources( 0, 1, &zoomSRV );
    context->CSSetUnorderedAccessViews( 0, 1, zoomBuffer->GetUnorderedAccessView().GetAddressOf(), nullptr );
    context->Dispatch( (ds4Size.x + 7) / 8, (ds4Size.y + 7) / 8, 1 );

    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    ID3D11ShaderResourceView* nullSRV1 = nullptr;
    context->CSSetShaderResources( 0, 1, &nullSRV1 );
    context->CSSetShader( nullptr, nullptr, 0 );

    // Keep the result texture alive until next frame by storing the handle as a member
    m_GodRaysResult = std::move( zoomBuffer );
    *outGodRaysSRV = m_GodRaysResult->GetShaderResView().Get();
    return XR_SUCCESS;
}

XRESULT D3D11PFX_GodRays::RenderVolumetricToTexture(
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* lowClouds,
    ID3D11ShaderResourceView** outGodRaysSRV ) {
    if ( !outGodRaysSRV )
        return XR_FAILED;
    *outGodRaysSRV = nullptr;
    if ( FeatureLevel10Compatibility || !depthCopy )
        return XR_FAILED;
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    GSky* sky = Engine::GAPI->GetSky();
    D3D11ShadowMap* shadowMaps = engine ? engine->GetShadowMaps() : nullptr;
    D3D11CascadedShadowMapBuffer* csm = shadowMaps ? shadowMaps->GetCascadedShadowMap() : nullptr;
    if ( !engine || !sky || !shadowMaps || !csm || !csm->GetShaderResourceView() )
        return XR_FAILED;
    const auto& atmosphere = sky->GetAtmosphereCB();
    if ( sky->GetAtmosphereSettings().LightDirection.y <= 0.0f || atmosphere.AC_SunVisibility <= 0.0001f )
        return XR_SUCCESS;
    // The low-sun volumetric look remains unchanged through 30 degrees of elevation.
    // Above that, fade only the volumetric contribution while keeping radial godrays,
    // low clouds and the FL10 path untouched. At 53 degrees the expensive volumetric
    // and temporal passes are fully unnecessary and the combined path falls back to radial.
    constexpr float volumetricSunFadeStart = 0.50f;
    constexpr float volumetricSunFadeEnd = 0.80f;
    const float sunHeight = std::clamp( atmosphere.AC_LightPos.y, 0.0f, 1.0f );
    if ( sunHeight >= volumetricSunFadeEnd ) {
        ResetTemporalHistory();
        return XR_SUCCESS;
    }
    float volumetricSunFade = 1.0f;
    if ( sunHeight > volumetricSunFadeStart ) {
        const float fadeT = std::clamp(
            (sunHeight - volumetricSunFadeStart) / (volumetricSunFadeEnd - volumetricSunFadeStart),
            0.0f, 1.0f );
        const float smoothFade = fadeT * fadeT * (3.0f - 2.0f * fadeT);
        volumetricSunFade = 1.0f - smoothFade;
    }
    const auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    auto& context = engine->GetContext();
    const INT2 resolution = engine->GetResolution();
    const INT2 ds4Size = { std::max( resolution.x / 4, 1 ), std::max( resolution.y / 4, 1 ) };
    GodRayVolumetricConstantBuffer cb = {};
    const auto& projection = Engine::GAPI->GetProjectionMatrix();
    cb.GRV_ProjParams = float4( 1.0f / projection._11, 1.0f / projection._22, projection._43, projection._33 );
    XMStoreFloat4x4( &cb.GRV_InvView, XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() ) );
    cb.GRV_CameraPosition = Engine::GAPI->GetCameraPosition();
    const float worldDrawDistance = std::max(
        static_cast<float>( settings.SectionDrawRadius ) * WORLD_SECTION_SIZE,
        6000.0f );
    const float cascadeFarDistance = shadowMaps->GetCascadeFarDistance();
    if ( cascadeFarDistance <= 1.0f )
        return XR_FAILED;
    cb.GRV_MaxDistance = std::min( worldDrawDistance, cascadeFarDistance );
    const DS_ScreenQuadConstantBuffer sunCB = shadowMaps->FillSunCSMConstantBuffer();
    for ( int i = 0; i < MAX_CSM_CASCADES; ++i )
        cb.GRV_ShadowViewProj[i] = sunCB.SQ_ShadowViewProj[i];
    cb.GRV_LightColor = sunCB.SQ_LightColor;
    cb.GRV_LightColor.x *= settings.GodRayColorMod.x;
    cb.GRV_LightColor.y *= settings.GodRayColorMod.y;
    cb.GRV_LightColor.z *= settings.GodRayColorMod.z;
    cb.GRV_LightDirectionWS = sky->GetMainLightDirection();
    cb.GRV_ShadowmapSize = std::max( sunCB.SQ_ShadowmapSize, 1.0f );
    cb.GRV_FogHeight = settings.FogHeight;
    cb.GRV_HeightFalloff = settings.FogHeightFalloff;
    cb.GRV_GlobalDensity = std::max( settings.FogGlobalDensity, 0.00012f );
    cb.GRV_WeightZNear = 0.0f;
    cb.GRV_WeightZFar = std::max( cb.GRV_MaxDistance * 0.55f, 1.0f );
    cb.GRV_RainFogHeight = cb.GRV_FogHeight;
    cb.GRV_RainHeightFalloff = cb.GRV_HeightFalloff;
    cb.GRV_RainGlobalDensity = settings.RainFogDensity;
    cb.GRV_RainWeightZNear = cb.GRV_WeightZNear;
    cb.GRV_RainWeightZFar = cb.GRV_WeightZFar;
    cb.GRV_FogOverride = std::clamp( Engine::GAPI->GetFogOverride(), 0.0f, 1.0f );
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    if ( cb.GRV_FogOverride > 0.0f ) {
        cb.GRV_FogHeight = Toolbox::lerp( cb.GRV_FogHeight, cb.GRV_CameraPosition.y + 10000.0f, cb.GRV_FogOverride );
        cb.GRV_HeightFalloff = Toolbox::lerp( cb.GRV_HeightFalloff, 0.000001f, cb.GRV_FogOverride );
        cb.GRV_GlobalDensity = Toolbox::lerp( cb.GRV_GlobalDensity, cb.GRV_GlobalDensity * 2.0f, cb.GRV_FogOverride );
        cb.GRV_WeightZNear = Toolbox::lerp( cb.GRV_WeightZNear, WORLD_SECTION_SIZE * 0.09f, cb.GRV_FogOverride );
        cb.GRV_WeightZFar = Toolbox::lerp( cb.GRV_WeightZFar, WORLD_SECTION_SIZE * 0.8f, cb.GRV_FogOverride );
    }
#endif
    cb.GRV_RainWeight = std::clamp( atmosphere.AC_RainFXWeight, 0.0f, 1.0f );
    cb.GRV_SunVisibility = std::clamp( atmosphere.AC_SunVisibility, 0.0f, 1.0f ) * GetRainSkyVisibility();
    cb.GRV_Strength = std::max( settings.GodRayStrength, 0.0f ) * volumetricSunFade;
    cb.GRV_FrameIndex = static_cast<uint32_t>(std::max( Engine::GAPI->GetTimeSeconds(), 0.0f ) * 60.0f);
    cb.GRV_NumCascades = static_cast<uint32_t>(std::clamp<size_t>( settings.NumShadowCascades, 1, MAX_CSM_CASCADES ));
    cb.GRV_PreviousViewProjection = m_PreviousViewProjection;
    cb.GRV_InvOutputSize = float2( 1.0f / std::max( ds4Size.x, 1 ), 1.0f / std::max( ds4Size.y, 1 ) );
    cb.GRV_HistoryValid = 0.0f;
    cb.GRV_HistoryWeight = 0.86f;
    auto output = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
    if ( !output )
        return XR_FAILED;
    auto cs = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayVolumetric );
    if ( !cs )
        return XR_FAILED;
    cs->Apply();
    cs->GetBuffer( "GodRayVolumetricConstantBuffer" ).Update( &cb ).Bind();
    context->OMSetRenderTargets( 0, nullptr, nullptr );
    ID3D11ShaderResourceView* resources[3] = { depthCopy, csm->GetShaderResourceView(), lowClouds };
    context->CSSetShaderResources( 0, 3, resources );
    ID3D11SamplerState* samplers[2] = { shadowMaps->GetShadowmapSampler(), engine->GetClampSamplerState() };
    context->CSSetSamplers( 0, 2, samplers );
    context->CSSetUnorderedAccessViews( 0, 1, output->GetUnorderedAccessView().GetAddressOf(), nullptr );
    context->Dispatch( (ds4Size.x + 7) / 8, (ds4Size.y + 7) / 8, 1 );
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11ShaderResourceView* nullSRVs[3] = { nullptr, nullptr, nullptr };
    ID3D11SamplerState* nullSamplers[2] = { nullptr, nullptr };
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 3, nullSRVs );
    context->CSSetSamplers( 0, 2, nullSamplers );
    context->CSSetShader( nullptr, nullptr, 0 );
    const float currentTime = std::max( Engine::GAPI->GetTimeSeconds(), 0.0f );
    const XMFLOAT3 currentCameraPosition = Engine::GAPI->GetCameraPosition();
    const float cameraDeltaX = currentCameraPosition.x - m_PreviousCameraPosition.x;
    const float cameraDeltaY = currentCameraPosition.y - m_PreviousCameraPosition.y;
    const float cameraDeltaZ = currentCameraPosition.z - m_PreviousCameraPosition.z;
    const float cameraDeltaSquared = cameraDeltaX * cameraDeltaX + cameraDeltaY * cameraDeltaY + cameraDeltaZ * cameraDeltaZ;
    if ( m_LastVolumetricRenderTime < 0.0f || currentTime - m_LastVolumetricRenderTime > 0.10f || cameraDeltaSquared > 250000.0f )
        m_VolumetricHistoryValid = false;
    const bool historySizeMismatch = !m_VolumetricHistory[0] || !m_VolumetricHistory[1]
        || !m_VolumetricDepthHistory[0] || !m_VolumetricDepthHistory[1]
        || m_VolumetricHistory[0]->GetSizeX() != static_cast<UINT>( ds4Size.x )
        || m_VolumetricHistory[0]->GetSizeY() != static_cast<UINT>( ds4Size.y )
        || m_VolumetricHistory[1]->GetSizeX() != static_cast<UINT>( ds4Size.x )
        || m_VolumetricHistory[1]->GetSizeY() != static_cast<UINT>( ds4Size.y )
        || m_VolumetricDepthHistory[0]->GetSizeX() != static_cast<UINT>( ds4Size.x )
        || m_VolumetricDepthHistory[0]->GetSizeY() != static_cast<UINT>( ds4Size.y )
        || m_VolumetricDepthHistory[1]->GetSizeX() != static_cast<UINT>( ds4Size.x )
        || m_VolumetricDepthHistory[1]->GetSizeY() != static_cast<UINT>( ds4Size.y );
    if ( historySizeMismatch ) {
        ResetTemporalHistory();
        m_VolumetricHistory[0] = FxRenderer->GetTexturePool()->Acquire(
            TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
                D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
        m_VolumetricHistory[1] = FxRenderer->GetTexturePool()->Acquire(
            TexturePool::Description{ ds4Size.x, ds4Size.y, engine->GetBackBufferFormat(),
                D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
        m_VolumetricDepthHistory[0] = FxRenderer->GetTexturePool()->Acquire(
            TexturePool::Description{ ds4Size.x, ds4Size.y, DXGI_FORMAT_R32_FLOAT,
                D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
        m_VolumetricDepthHistory[1] = FxRenderer->GetTexturePool()->Acquire(
            TexturePool::Description{ ds4Size.x, ds4Size.y, DXGI_FORMAT_R32_FLOAT,
                D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
    }
    if ( !m_VolumetricHistory[0] || !m_VolumetricHistory[1]
        || !m_VolumetricDepthHistory[0] || !m_VolumetricDepthHistory[1] ) {
        ResetTemporalHistory();
        m_VolumetricGodRaysResult = std::move( output );
        *outGodRaysSRV = m_VolumetricGodRaysResult->GetShaderResView().Get();
        return XR_SUCCESS;
    }
    const uint32_t readIndex = m_VolumetricHistoryIndex;
    const uint32_t writeIndex = 1u - readIndex;
    const XMMATRIX currentView = Engine::GAPI->GetViewMatrixXM();
    const XMMATRIX currentProjection = XMLoadFloat4x4( &projection );
    XMFLOAT4X4 currentViewProjection = {};
    XMStoreFloat4x4( &currentViewProjection, XMMatrixMultiply( currentProjection, currentView ) );
    cb.GRV_PreviousViewProjection = m_PreviousViewProjection;
    cb.GRV_HistoryValid = m_VolumetricHistoryValid ? 1.0f : 0.0f;
    auto temporalCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayTemporal );
    if ( !temporalCS ) {
        ResetTemporalHistory();
        m_VolumetricGodRaysResult = std::move( output );
        *outGodRaysSRV = m_VolumetricGodRaysResult->GetShaderResView().Get();
        return XR_SUCCESS;
    }
    temporalCS->Apply();
    temporalCS->GetBuffer( "GodRayVolumetricConstantBuffer" ).Update( &cb ).Bind();
    ID3D11ShaderResourceView* temporalResources[4] = {
        output->GetShaderResView().Get(),
        m_VolumetricHistoryValid ? m_VolumetricHistory[readIndex]->GetShaderResView().Get() : output->GetShaderResView().Get(),
        depthCopy,
        m_VolumetricHistoryValid ? m_VolumetricDepthHistory[readIndex]->GetShaderResView().Get() : depthCopy
    };
    context->CSSetShaderResources( 0, 4, temporalResources );
    ID3D11SamplerState* temporalSampler = engine->GetClampSamplerState();
    context->CSSetSamplers( 0, 1, &temporalSampler );
    ID3D11UnorderedAccessView* temporalOutputs[2] = {
        m_VolumetricHistory[writeIndex]->GetUnorderedAccessView().Get(),
        m_VolumetricDepthHistory[writeIndex]->GetUnorderedAccessView().Get()
    };
    context->CSSetUnorderedAccessViews( 0, 2, temporalOutputs, nullptr );
    context->Dispatch( (ds4Size.x + 7) / 8, (ds4Size.y + 7) / 8, 1 );
    ID3D11UnorderedAccessView* nullTemporalUAVs[2] = { nullptr, nullptr };
    ID3D11ShaderResourceView* nullTemporalSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
    ID3D11SamplerState* nullTemporalSampler = nullptr;
    context->CSSetUnorderedAccessViews( 0, 2, nullTemporalUAVs, nullptr );
    context->CSSetShaderResources( 0, 4, nullTemporalSRVs );
    context->CSSetSamplers( 0, 1, &nullTemporalSampler );
    context->CSSetShader( nullptr, nullptr, 0 );
    m_VolumetricHistoryIndex = writeIndex;
    m_VolumetricHistoryValid = true;
    m_PreviousViewProjection = currentViewProjection;
    m_PreviousCameraPosition = currentCameraPosition;
    m_LastVolumetricRenderTime = currentTime;
    *outGodRaysSRV = m_VolumetricHistory[writeIndex]->GetShaderResView().Get();
    return XR_SUCCESS;
}
XRESULT D3D11PFX_GodRays::RenderCombinedToTexture(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* lowClouds,
    ID3D11ShaderResourceView** outGodRaysSRV ) {
    if ( !outGodRaysSRV )
        return XR_FAILED;
    *outGodRaysSRV = nullptr;
    if ( FeatureLevel10Compatibility )
        return RenderToTexture( backbuffer, depthCopy, lowClouds, outGodRaysSRV );
    ID3D11ShaderResourceView* volumetricSRV = nullptr;
    const XRESULT volumetricResult = RenderVolumetricToTexture( depthCopy, lowClouds, &volumetricSRV );
    ID3D11ShaderResourceView* radialSRV = nullptr;
    const XRESULT radialResult = RenderToTexture( backbuffer, depthCopy, lowClouds, &radialSRV );
    if ( radialSRV )
        m_RadialGodRaysResult = std::move( m_GodRaysResult );
    if ( !volumetricSRV && !radialSRV )
        return volumetricResult == XR_SUCCESS || radialResult == XR_SUCCESS ? XR_SUCCESS : XR_FAILED;
    if ( !volumetricSRV ) {
        *outGodRaysSRV = m_RadialGodRaysResult->GetShaderResView().Get();
        return XR_SUCCESS;
    }
    if ( !radialSRV ) {
        *outGodRaysSRV = volumetricSRV;
        return XR_SUCCESS;
    }
    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    if ( !engine )
        return XR_FAILED;
    const INT2 resolution = engine->GetResolution();
    auto combined = FxRenderer->GetTexturePool()->Acquire(
        TexturePool::Description{ resolution.x, resolution.y, engine->GetBackBufferFormat(),
            D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE } );
    auto combineCS = engine->GetShaderManager().GetCShader( CShaderID::CS_PFX_GodRayCombine );
    if ( !combined || !combineCS || !m_VolumetricDepthHistory[m_VolumetricHistoryIndex] ) {
        *outGodRaysSRV = volumetricSRV;
        return XR_SUCCESS;
    }
    auto& context = engine->GetContext();
    combineCS->Apply();
    ID3D11ShaderResourceView* inputs[4] = {
        volumetricSRV,
        m_RadialGodRaysResult->GetShaderResView().Get(),
        m_VolumetricDepthHistory[m_VolumetricHistoryIndex]->GetShaderResView().Get(),
        depthCopy
    };
    context->CSSetShaderResources( 0, 4, inputs );
    ID3D11SamplerState* combineSampler = engine->GetClampSamplerState();
    context->CSSetSamplers( 0, 1, &combineSampler );
    context->CSSetUnorderedAccessViews( 0, 1, combined->GetUnorderedAccessView().GetAddressOf(), nullptr );
    context->Dispatch( (resolution.x + 7) / 8, (resolution.y + 7) / 8, 1 );
    ID3D11UnorderedAccessView* nullUAV = nullptr;
    ID3D11ShaderResourceView* nullSRVs[4] = { nullptr, nullptr, nullptr, nullptr };
    ID3D11SamplerState* nullCombineSampler = nullptr;
    context->CSSetUnorderedAccessViews( 0, 1, &nullUAV, nullptr );
    context->CSSetShaderResources( 0, 4, nullSRVs );
    context->CSSetSamplers( 0, 1, &nullCombineSampler );
    context->CSSetShader( nullptr, nullptr, 0 );
    m_CombinedGodRaysResult = std::move( combined );
    *outGodRaysSRV = m_CombinedGodRaysResult->GetShaderResView().Get();
    return XR_SUCCESS;
}
