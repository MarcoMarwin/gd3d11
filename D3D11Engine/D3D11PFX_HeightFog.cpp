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

/** Draws this effect to the given buffer */
XRESULT D3D11PFX_HeightFog::Render( RenderToTextureBuffer* fxbuffer ) {
	D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);

	// Save old rendertargets
	Microsoft::WRL::ComPtr<ID3D11RenderTargetView> oldRTV;
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> oldDSV;
	engine->GetContext()->OMGetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.GetAddressOf() );

	auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
	auto hfPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Heightfog );

	hfPS->Apply();
	vs->Apply();

	HeightfogConstantBuffer cb = {};
	{
		auto& proj = Engine::GAPI->GetProjectionMatrix();
		cb.HF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
	}
	XMStoreFloat4x4( &cb.InvView, XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() ) );

	cb.CameraPosition = Engine::GAPI->GetCameraPosition();

	cb.HF_GlobalDensity = Engine::GAPI->GetRendererState().RendererSettings.FogGlobalDensity;
	cb.HF_HeightFalloff = Engine::GAPI->GetRendererState().RendererSettings.FogHeightFalloff;
	float height = Engine::GAPI->GetRendererState().RendererSettings.FogHeight;
	cb.HF_RainGlobalDensity = Engine::GAPI->GetRendererState().RendererSettings.RainFogDensity;
	cb.HF_RainHeightFalloff = cb.HF_HeightFalloff;
	cb.HF_RainFogHeight = height;
	XMVECTOR color = XMLoadFloat3( Engine::GAPI->GetRendererState().RendererSettings.FogColorMod.toXMFLOAT3() );

	float fnear = 15000.0f;
	float ffar = 60000.0f;
    float secScale = std::min<float>( Engine::GAPI->GetRendererState().RendererSettings.SectionDrawRadius, Engine::GAPI->GetRendererState().RendererSettings.FogRange );

	cb.HF_WeightZNear = std::max( 0.0f, WORLD_SECTION_SIZE * ((secScale - 0.5f) * 0.7f) - (ffar - fnear) ); // Keep distance from original fog but scale the near-fog up to section draw distance
	cb.HF_WeightZFar = WORLD_SECTION_SIZE * ((secScale - 0.5f) * 0.8f);

	float atmoMax = 83200.0f; // TODO: Calculate!
	float atmoMin = 27799.9922f;

	cb.HF_WeightZFar = std::min( cb.HF_WeightZFar, atmoMax );
	cb.HF_WeightZNear = std::min( cb.HF_WeightZNear, atmoMin );
	cb.HF_RainWeightZNear = cb.HF_WeightZNear;
	cb.HF_RainWeightZFar = cb.HF_WeightZFar;
	cb.HF_FogOverride = std::clamp( Engine::GAPI->GetFogOverride(), 0.0f, 1.0f );

#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    float fogDensityFactor = 2;
#else
    float fogDensityFactor = pow( 15000.0f / Engine::GAPI->GetFarZ(), 4.0f );
#endif

	if ( Engine::GAPI->GetFogOverride() > 0.0f ) {
		// Make sure the camera is inside the fog when in fog zone
		height = Toolbox::lerp( height, Engine::GAPI->GetCameraPosition().y + 10000, Engine::GAPI->GetFogOverride() ); // TODO: Get this from the actual fog-distance in the fogzone!

		// Override fog color when in fog zone
		color = Engine::GAPI->GetFogColor();

#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
		// Make it z-Fog
		cb.HF_HeightFalloff = Toolbox::lerp( cb.HF_HeightFalloff, 0.000001f, Engine::GAPI->GetFogOverride() );
#endif

		// Turn up density
		cb.HF_GlobalDensity = Toolbox::lerp( cb.HF_GlobalDensity, cb.HF_GlobalDensity * fogDensityFactor, Engine::GAPI->GetFogOverride() );

#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
		// Use other fog-values for fog-zones
		cb.HF_WeightZNear = Toolbox::lerp( cb.HF_WeightZNear, WORLD_SECTION_SIZE * 0.09f, Engine::GAPI->GetFogOverride() );
		cb.HF_WeightZFar = Toolbox::lerp( cb.HF_WeightZFar, WORLD_SECTION_SIZE * 0.8, Engine::GAPI->GetFogOverride() );
#endif
	}

	cb.HF_FogHeight = height;

	cb.HF_ProjAB = float2( Engine::GAPI->GetProjectionMatrix()._33, Engine::GAPI->GetProjectionMatrix()._34 );


	GSky* sky = Engine::GAPI->GetSky();
	XMFLOAT3 FogColorMod;
	XMStoreFloat3( &FogColorMod, color );
	cb.HF_FogColorMod = FogColorMod;
	cb.HF_RainFogColor = Engine::GAPI->GetRendererState().RendererSettings.RainFogColor;


	hfPS->GetBuffer( "PFXBuffer" ).Update( &cb ).Bind();

	hfPS->GetBuffer( "Atmosphere" ).Update( &sky->GetAtmosphereCB() ).Bind();

	engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), nullptr );

	// Bind depthbuffer
	engine->GetDepthBuffer()->BindToPixelShader( engine->GetContext().Get(), 1 );

    engine->SetDefaultStates();
    Engine::GAPI->GetRendererState().RasterizerState.CullMode = GothicRasterizerStateInfo::CM_CULL_NONE;
    Engine::GAPI->GetRendererState().RasterizerState.SetDirty();
	Engine::GAPI->GetRendererState().BlendState.SetDefault();
	//Engine::GAPI->GetRendererState().BlendState.SetAdditiveBlending();
	Engine::GAPI->GetRendererState().BlendState.BlendEnabled = true;
	Engine::GAPI->GetRendererState().BlendState.SetDirty();

	// Copy
	FxRenderer->DrawFullScreenQuad();

	// Restore rendertargets
    ID3D11ShaderResourceView* nullResources[2] = {};
	engine->GetContext()->PSSetShaderResources( 1, 2, nullResources );

	engine->GetContext()->OMSetRenderTargets( 1, oldRTV.GetAddressOf(), oldDSV.Get() );

	return XR_SUCCESS;
}
