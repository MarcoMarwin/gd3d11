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
#include "D3D11Effect.h"
#include "D3D11ShadowMap.h"
#include "D3D11ConstantBuffer.h"
#include "ConstantBufferStructs.h"
#include "GothicAPI.h"
#include "GSky.h"

namespace {
    // Water Reflections has its own strength/enable controls in PS_Water.
    // Wet-ground and puddle reflections use this independent normalized base.
    constexpr float WET_GROUND_SSR_DEFAULT_STRENGTH = 0.84f;
}

D3D11PfxRenderer::D3D11PfxRenderer() {

    auto engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
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

D3D11PfxRenderer::~D3D11PfxRenderer()
{
    FX_Blur.reset();
    FX_HeightFog.reset();
    FX_DistanceBlur.reset();
    FX_HDR.reset();
    FX_GodRays.reset();
    FX_DepthOfField.reset();
    FX_SMAA.reset();
    PFX_CAS.reset();
    PFX_SimpleSharpen.reset();
    PFX_FSR3.reset();
    PFX_XeGTAO.reset();

    NightFogRainFade = 0.0f;
LastNightFogRainFadeTime = 0.0f;
NightFogRainFadeInitialized = false;
m_texturePool.reset();
m_depthStencilPool.reset();
}

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
XRESULT D3D11PfxRenderer::RenderGodRays( ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* depth, ID3D11ShaderResourceView* lowClouds ) {
    return FX_GodRays->Render( backbuffer, depth, lowClouds );
}

/** Renders the depth-of-field effect */
XRESULT D3D11PfxRenderer::RenderDepthOfField( ID3D11ShaderResourceView* backbuffer, ID3D11ShaderResourceView* waterMaskSRV, ID3D11ShaderResourceView* specularSRV ) {
    return FX_DepthOfField->Render( backbuffer, waterMaskSRV, specularSRV );
}

XRESULT D3D11PfxRenderer::RenderWetGroundSSR( ID3D11RenderTargetView* outputRTV, ID3D11ShaderResourceView* sceneSRV, ID3D11ShaderResourceView* depthSRV, ID3D11ShaderResourceView* normalsSRV, ID3D11ShaderResourceView* waterMaskSRV, ID3D11ShaderResourceView* materialSRV, ID3D11ShaderResourceView* lowCloudLayerSRV ) {
    auto* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    auto* rainShadow = engine->Effects ? engine->Effects->GetRainShadowmap() : nullptr;
    auto* shadowMaps = engine->GetShadowMaps();
    if ( !outputRTV || !sceneSRV || !depthSRV || !normalsSRV || !waterMaskSRV || !materialSRV || !rainShadow || !shadowMaps ) { return XR_FAILED; }
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
    XMStoreFloat4x4( &cb.WG_RainViewProj, XMLoadFloat4x4( &rainCamera.ProjectionReplacement ) * XMLoadFloat4x4( &rainCamera.ViewReplacement ) );
    cb.WG_CameraPosition = Engine::GAPI->GetCameraPosition();
    cb.WG_Wetness = Engine::GAPI->GetSceneWetness();
    const INT2 resolution = engine->GetResolution();
    cb.WG_InvResolution = float2( 1.0f / std::max( resolution.x, 1 ), 1.0f / std::max( resolution.y, 1 ) );
    auto& rendererSettings = Engine::GAPI->GetRendererState().RendererSettings;
    // Do not reuse Water Reflections' EnableSSR/SSRStrength here. Those
    // controls are intentionally limited to water surfaces. The separate
    // Rain effects option controls wet-ground reflections and procedural
    // puddles together; rain impacts remain available independently.
    cb.WG_Strength = WET_GROUND_SSR_DEFAULT_STRENGTH;
    cb.WG_Time = Engine::GAPI->GetTimeSeconds();
    cb.WG_RainFXWeight = Engine::GAPI->GetRainFXWeight();
    cb.WG_RainFogColor = rendererSettings.RainFogColor;
    cb.WG_RainFogDensity = rendererSettings.RainFogDensity;
    cb.WG_FogRange = rendererSettings.FogRange;
    const bool rainEffectsEnabled = rendererSettings.GetEffectiveRainEffects();
    cb.WG_WetMaterialReflectionsStrength = rainEffectsEnabled ? 1.0f : 0.0f;
    cb.WG_ProceduralPuddlesStrength = rainEffectsEnabled ? 1.0f : 0.0f;
    cb.WG_PuddleReflectionsStrength = rainEffectsEnabled ? 1.0f : 0.0f;
    cb.WG_WetGroundRainImpactsStrength = 1.0f;
    cb.WG_PuddleAccumulation = rainEffectsEnabled ? Engine::GAPI->GetPuddleAccumulation() : 0.0f;
    cb.WG_ReflectionsEnabled = rainEffectsEnabled ? 1.0f : 0.0f;
    ps->GetBuffer( "WetGroundSSRConstantBuffer" ).Update( &cb ).Bind();

    if ( GSky* sky = Engine::GAPI->GetSky() )
    {
        ps->GetBuffer( "Atmosphere" ).Update( &sky->GetAtmosphereCB() ).Bind();
    }

    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> previousRTV;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilView> previousDSV;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> previousSampler0;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> previousSampler1;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> previousSampler2;
    context->OMGetRenderTargets( 1, previousRTV.GetAddressOf(), previousDSV.GetAddressOf() );
    context->PSGetSamplers( 0, 1, previousSampler0.GetAddressOf() );
    context->PSGetSamplers( 1, 1, previousSampler1.GetAddressOf() );
    context->PSGetSamplers( 2, 1, previousSampler2.GetAddressOf() );
    context->OMSetRenderTargets( 1, &outputRTV, nullptr );
    ID3D11ShaderResourceView* resources[4] = { sceneSRV, depthSRV, normalsSRV, rainShadow->GetShaderResView().Get() };
    context->PSSetShaderResources( 0, 4, resources );
    engine->GetDistortionTexture()->BindToPixelShader( 4 );
    context->PSSetShaderResources( 5, 1, &waterMaskSRV );
    context->PSSetShaderResources( 6, 1, &materialSRV );
    context->PSSetShaderResources( 7, 1, &lowCloudLayerSRV );

    ID3D11ShaderResourceView* reflectionCubeSRV = engine->ReflectionCube.Get();
    context->PSSetShaderResources( 8, 1, &reflectionCubeSRV );
    ID3D11SamplerState* samplers[3] = { engine->GetClampSamplerState(), shadowMaps->GetShadowmapSampler(), engine->GetCubeSamplerState() };
    context->PSSetSamplers( 0, 3, samplers );
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

    ID3D11ShaderResourceView* nullResources[9] = {};
    context->PSSetShaderResources( 0, 9, nullResources );
    ID3D11SamplerState* restoredSamplers[3] = { previousSampler0.Get(), previousSampler1.Get(), previousSampler2.Get() };
    context->PSSetSamplers( 0, 3, restoredSamplers );
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
    return FX_GodRays->RenderToTexture( backbuffer, depthCopy, lowClouds, outGodRaysSRV );
}
XRESULT D3D11PfxRenderer::RenderVolumetricGodRaysToTexture(
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* lowClouds,
    ID3D11ShaderResourceView** outGodRaysSRV ) {
    return FX_GodRays->RenderVolumetricToTexture( depthCopy, lowClouds, outGodRaysSRV );
}
XRESULT D3D11PfxRenderer::RenderCombinedGodRaysToTexture(
    ID3D11ShaderResourceView* backbuffer,
    ID3D11ShaderResourceView* depthCopy,
    ID3D11ShaderResourceView* lowClouds,
    ID3D11ShaderResourceView** outGodRaysSRV ) {
    return FX_GodRays->RenderCombinedToTexture( backbuffer, depthCopy, lowClouds, outGodRaysSRV );
}

XRESULT D3D11PfxRenderer::RenderPostFXComposition(
    ID3D11RenderTargetView* outputRTV,
    ID3D11ShaderResourceView* backbufferSRV,
    ID3D11ShaderResourceView* godraysSRV,
    ID3D11ShaderResourceView* depthSRV,
    bool compositionHeightFog ) {

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    auto res = engine->GetResolution();

    // Set up shaders
    engine->GetShaderManager().GetVShader( VShaderID::VS_PFX )->Apply();
    auto compositionPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_Composition );
    compositionPS->Apply();

    // Update constants used by the height-fog composition permutation.
    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    GSky* sky = Engine::GAPI->GetSky();
    CompositionControlConstantBuffer control = {};
    control.CC_HeightFogEnabled = compositionHeightFog ? 1.0f : 0.0f;
    if ( compositionHeightFog )
        compositionPS->GetBuffer( "CompositionControl" ).Update( &control ).Bind();
    if ( compositionHeightFog ) {
        HeightfogConstantBuffer cb = {};
        {
            auto& proj = Engine::GAPI->GetProjectionMatrix();
            cb.HF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
        }
        XMStoreFloat4x4( &cb.InvView, XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() ) );
        cb.CameraPosition = Engine::GAPI->GetCameraPosition();
        cb.HF_GlobalDensity = settings.FogGlobalDensity;
        cb.HF_HeightFalloff = settings.FogHeightFalloff;
        float height = settings.FogHeight;
        cb.HF_RainGlobalDensity = settings.RainFogDensity;
        cb.HF_RainHeightFalloff = cb.HF_HeightFalloff;
        cb.HF_RainFogHeight = height;
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
        cb.HF_RainWeightZNear = cb.HF_WeightZNear;
        cb.HF_RainWeightZFar = cb.HF_WeightZFar;
        cb.HF_FogOverride = std::clamp( Engine::GAPI->GetFogOverride(), 0.0f, 1.0f );

#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
        float fogDensityFactor = 2;
#else
        float fogDensityFactor = pow( 15000.0f / Engine::GAPI->GetFarZ(), 4.0f );
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
float rain = sky ? std::clamp( sky->GetAtmosphereCB().AC_RainFXWeight, 0.0f, 1.0f ) : 0.0f;
float nightWeight = sky ? std::clamp( -sky->GetAtmosphereCB().AC_LightPos.y * 4.0f, 0.0f, 1.0f ) : 0.0f;
float rainNightFogFade = std::clamp( (rain - 0.18f) / (0.88f - 0.18f), 0.0f, 1.0f );
rainNightFogFade = rainNightFogFade * rainNightFogFade * (3.0f - 2.0f * rainNightFogFade);
float targetNightFogRainFade = nightWeight * rainNightFogFade;
float currentTime = Engine::GAPI->GetTimeSeconds();
if ( !NightFogRainFadeInitialized ) {
    NightFogRainFadeInitialized = true;
    LastNightFogRainFadeTime = currentTime;
    NightFogRainFade = 0.0f;
}
float nightFogFadeDeltaTime = std::clamp( currentTime - LastNightFogRainFadeTime, 0.0f, 0.1f );
LastNightFogRainFadeTime = currentTime;
float nightFogFadeSpeed = targetNightFogRainFade > NightFogRainFade ? 0.35f : 0.55f;
float nightFogFadeStep = std::clamp( nightFogFadeDeltaTime * nightFogFadeSpeed, 0.0f, 1.0f );
NightFogRainFade = std::clamp( NightFogRainFade + (targetNightFogRainFade - NightFogRainFade) * nightFogFadeStep, 0.0f, 1.0f );
cb.HF_Pad3 = float2( NightFogRainFade, 0.0f );
XMFLOAT3 FogColorMod;
XMStoreFloat3( &FogColorMod, color );
cb.HF_FogColorMod = FogColorMod;
cb.HF_RainFogColor = settings.RainFogColor;
compositionPS->GetBuffer( "PFXBuffer" ).Update( &cb ).Bind();
    }
    if ( compositionHeightFog && sky ) {
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

    // Bind SRVs: t0=backbuffer, t1=GodRays, t2=Depth
    ID3D11ShaderResourceView* srvs[3] = {
        backbufferSRV, godraysSRV, depthSRV
    };
    context->PSSetShaderResources( 0, 3, srvs );

    // No blending - direct overwrite
    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc =
        GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    DrawFullScreenQuad();

    // Unbind SRVs
    ID3D11ShaderResourceView* nullSRVs[3] = {};
    context->PSSetShaderResources( 0, 3, nullSRVs );

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
    ID3D11RenderTargetView* skyCloudLayerRTV,
    ID3D11ShaderResourceView* sceneSRV,
    ID3D11ShaderResourceView* depthSRV ) {
    if ( !cloudLayerRTV || !cloudDepthRTV || !skyCloudLayerRTV || !sceneSRV || !depthSRV ) {
        return XR_SUCCESS;
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    GSky* sky = Engine::GAPI->GetSky();
    if ( !sky ) {
        return XR_SUCCESS;
    }

    auto lowCloudPS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_LowClouds );
    auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    if ( !lowCloudPS || !vs ) {
        return XR_FAILED;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    HeightfogConstantBuffer cb = {};
    {
        auto& proj = Engine::GAPI->GetProjectionMatrix();
        cb.HF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
    }
    XMStoreFloat4x4( &cb.InvView, XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() ) );
    cb.CameraPosition = Engine::GAPI->GetCameraPosition();
    cb.HF_GlobalDensity = settings.FogGlobalDensity;
    const float baseFogDensity = cb.HF_GlobalDensity;
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
    cb.HF_FogOverride = std::clamp( Engine::GAPI->GetFogOverride(), 0.0f, 1.0f );

#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    float fogDensityFactor = 2;
#else
    float fogDensityFactor = pow( 15000.0f / Engine::GAPI->GetFarZ(), 4.0f );
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

    float rain = sky->GetAtmosphereCB().AC_RainFXWeight;
    float rainFogColorWeight = std::min( 1.0f, rain * 2.0f );
    float daylightRainFog = std::max( 0.0f, std::min( 1.0f, (sky->GetAtmosphereCB().AC_LightPos.y + 0.05f) * 4.0f ) );
    daylightRainFog = daylightRainFog * daylightRainFog * (3.0f - 2.0f * daylightRainFog);
    rainFogColorWeight *= daylightRainFog;

    XMFLOAT3 fogColorMod;
    XMStoreFloat3( &fogColorMod, XMVectorLerpV( color, XMLoadFloat3( &settings.RainFogColor ), XMVectorSet( rainFogColorWeight, rainFogColorWeight, rainFogColorWeight, 0 ) ) );
    cb.HF_FogColorMod = fogColorMod;
    const float rainFogDensity = Toolbox::lerp( baseFogDensity, settings.RainFogDensity, std::clamp( rain, 0.0f, 1.0f ) );
    cb.HF_GlobalDensity = std::max( cb.HF_GlobalDensity, rainFogDensity );

    vs->Apply();

    const INT2 res = engine->GetResolution();

    Microsoft::WRL::ComPtr<ID3D11Resource> cloudLayerResource;
    cloudLayerRTV->GetResource( cloudLayerResource.GetAddressOf() );
    Microsoft::WRL::ComPtr<ID3D11Texture2D> cloudLayerTexture;
    if ( !cloudLayerResource || FAILED( cloudLayerResource.As( &cloudLayerTexture ) ) || !cloudLayerTexture ) {
        return XR_FAILED;
    }
    D3D11_TEXTURE2D_DESC cloudLayerDesc = {};
    cloudLayerTexture->GetDesc( &cloudLayerDesc );

    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>( cloudLayerDesc.Width );
    vp.Height = static_cast<float>( cloudLayerDesc.Height );
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports( 1, &vp );

    const float clearValue[4] = {};
    context->ClearRenderTargetView( cloudLayerRTV, clearValue );
    context->ClearRenderTargetView( cloudDepthRTV, clearValue );
    context->ClearRenderTargetView( skyCloudLayerRTV, clearValue );

    ID3D11RenderTargetView* lowCloudRTVs[3] = { cloudLayerRTV, cloudDepthRTV, skyCloudLayerRTV };
    context->OMSetRenderTargets( 3, lowCloudRTVs, nullptr );

    lowCloudPS->Apply();
    lowCloudPS->GetBuffer( "PFXBuffer" ).Update( &cb ).Bind();
    lowCloudPS->GetBuffer( "Atmosphere" ).Update( &sky->GetAtmosphereCB() ).Bind();

    ID3D11ShaderResourceView* cloudSRVs[2] = { sceneSRV, depthSRV };
    context->PSSetShaderResources( 0, 2, cloudSRVs );

    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    DrawFullScreenQuad();

    ID3D11ShaderResourceView* nullSRVs[2] = {};
    context->PSSetShaderResources( 0, 2, nullSRVs );

    vp.Width = static_cast<float>( res.x );
    vp.Height = static_cast<float>( res.y );
    context->RSSetViewports( 1, &vp );
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::DEFAULT_DEPTH_COMP_STATE;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    return XR_SUCCESS;
}

XRESULT D3D11PfxRenderer::CompositeLowClouds(
    ID3D11RenderTargetView* outputRTV,
    ID3D11ShaderResourceView* sceneSRV,
    ID3D11ShaderResourceView* lowCloudLayerSRV,
    ID3D11ShaderResourceView* lowCloudDepthSRV,
    ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* skyCloudLayerSRV ) {
    if ( !outputRTV || !sceneSRV || !lowCloudLayerSRV || !lowCloudDepthSRV || !depthSRV || !skyCloudLayerSRV ) {
        return XR_SUCCESS;
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto& context = engine->GetContext();
    GSky* sky = Engine::GAPI->GetSky();
    if ( !sky ) {
        return XR_SUCCESS;
    }

    auto lowCloudCompositePS = engine->GetShaderManager().GetPShader( PShaderID::PS_PFX_LowCloudComposite );
    auto vs = engine->GetShaderManager().GetVShader( VShaderID::VS_PFX );
    if ( !lowCloudCompositePS || !vs ) {
        return XR_FAILED;
    }

    auto& settings = Engine::GAPI->GetRendererState().RendererSettings;
    HeightfogConstantBuffer cb = {};
    {
        auto& proj = Engine::GAPI->GetProjectionMatrix();
        cb.HF_ProjParams = float4( 1.0f / proj._11, 1.0f / proj._22, proj._43, proj._33 );
    }
    XMStoreFloat4x4( &cb.InvView, XMMatrixInverse( nullptr, Engine::GAPI->GetViewMatrixXM() ) );
    cb.CameraPosition = Engine::GAPI->GetCameraPosition();
    cb.HF_GlobalDensity = settings.FogGlobalDensity;
    const float baseFogDensity = cb.HF_GlobalDensity;
    cb.HF_HeightFalloff = settings.FogHeightFalloff;
    float height = settings.FogHeight;
    XMVECTOR color = XMLoadFloat3( settings.FogColorMod.toXMFLOAT3() );
    float fnear = 15000.0f;
    float ffar = 60000.0f;
    float secScale = std::min<float>( settings.SectionDrawRadius, settings.FogRange );
    cb.HF_WeightZNear = std::max( 0.0f, WORLD_SECTION_SIZE * ((secScale - 0.5f) * 0.7f) - (ffar - fnear) );
    cb.HF_WeightZFar = WORLD_SECTION_SIZE * ((secScale - 0.5f) * 0.8f);
    cb.HF_WeightZFar = std::min( cb.HF_WeightZFar, 83200.0f );
    cb.HF_WeightZNear = std::min( cb.HF_WeightZNear, 27799.9922f );
    cb.HF_FogOverride = std::clamp( Engine::GAPI->GetFogOverride(), 0.0f, 1.0f );
#if !defined(BUILD_GOTHIC_1_08k) && !defined(BUILD_1_12F)
    float fogDensityFactor = 2;
#else
    float fogDensityFactor = pow( 15000.0f / Engine::GAPI->GetFarZ(), 4.0f );
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
    float rain = sky->GetAtmosphereCB().AC_RainFXWeight;
    float rainFogColorWeight = std::min( 1.0f, rain * 2.0f );
    float daylightRainFog = std::max( 0.0f, std::min( 1.0f, (sky->GetAtmosphereCB().AC_LightPos.y + 0.05f) * 4.0f ) );
    daylightRainFog = daylightRainFog * daylightRainFog * (3.0f - 2.0f * daylightRainFog);
    rainFogColorWeight *= daylightRainFog;
    XMFLOAT3 fogColorMod;
    XMStoreFloat3( &fogColorMod, XMVectorLerpV( color, XMLoadFloat3( &settings.RainFogColor ), XMVectorSet( rainFogColorWeight, rainFogColorWeight, rainFogColorWeight, 0 ) ) );
    cb.HF_FogColorMod = fogColorMod;
    const float rainFogDensity = Toolbox::lerp( baseFogDensity, settings.RainFogDensity, std::clamp( rain, 0.0f, 1.0f ) );
    cb.HF_GlobalDensity = std::max( cb.HF_GlobalDensity, rainFogDensity );
    auto res = engine->GetResolution();
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>( res.x );
    vp.Height = static_cast<float>( res.y );
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports( 1, &vp );
    context->OMSetRenderTargets( 1, &outputRTV, nullptr );
    vs->Apply();
    lowCloudCompositePS->Apply();
    lowCloudCompositePS->GetBuffer( "PFXBuffer" ).Update( &cb ).Bind();
    lowCloudCompositePS->GetBuffer( "Atmosphere" ).Update( &sky->GetAtmosphereCB() ).Bind();
    ID3D11ShaderResourceView* compositeSRVs[5] = {
        sceneSRV,
        lowCloudLayerSRV,
        lowCloudDepthSRV,
        depthSRV,
        skyCloudLayerSRV
    };
    context->PSSetShaderResources( 0, 5, compositeSRVs );

    Engine::GAPI->GetRendererState().BlendState.SetDefault();
    Engine::GAPI->GetRendererState().BlendState.SetDirty();
    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::CF_COMPARISON_ALWAYS;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = false;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    DrawFullScreenQuad();

    ID3D11ShaderResourceView* nullCompositeSRVs[5] = {};
    context->PSSetShaderResources( 0, 5, nullCompositeSRVs );

    Engine::GAPI->GetRendererState().DepthState.DepthBufferCompareFunc = GothicDepthBufferStateInfo::DEFAULT_DEPTH_COMP_STATE;
    Engine::GAPI->GetRendererState().DepthState.DepthWriteEnabled = true;
    Engine::GAPI->GetRendererState().DepthState.SetDirty();

    return XR_SUCCESS;
}

XRESULT D3D11PfxRenderer::RenderLowClouds( ID3D11RenderTargetView* outputRTV,
                                           ID3D11ShaderResourceView* sceneSRV,
                                           ID3D11ShaderResourceView* depthSRV ) {
    if ( !outputRTV || !sceneSRV || !depthSRV ) {
        return XR_SUCCESS;
    }

    D3D11GraphicsEngine* engine = reinterpret_cast<D3D11GraphicsEngine*>(Engine::GraphicsEngine);
    auto res = engine->GetResolution();
    const INT2 cloudRes( std::max( 1, (res.x + 3) / 4 ), std::max( 1, (res.y + 3) / 4 ) );
    auto lowCloudLayer = m_texturePool->Acquire( TexturePool::Description{ cloudRes.x, cloudRes.y, DXGI_FORMAT_R16G16B16A16_FLOAT } );
    auto lowCloudDepth = m_texturePool->Acquire( TexturePool::Description{ cloudRes.x, cloudRes.y, DXGI_FORMAT_R32_FLOAT } );
    auto skyCloudLayer = m_texturePool->Acquire( TexturePool::Description{ cloudRes.x, cloudRes.y, DXGI_FORMAT_R16G16B16A16_FLOAT } );
    if ( !lowCloudLayer || !lowCloudDepth || !skyCloudLayer
        || !lowCloudLayer->GetRenderTargetView().Get() || !lowCloudLayer->GetShaderResView().Get()
        || !lowCloudDepth->GetRenderTargetView().Get() || !lowCloudDepth->GetShaderResView().Get()
        || !skyCloudLayer->GetRenderTargetView().Get() || !skyCloudLayer->GetShaderResView().Get() ) {
        return XR_FAILED;
    }

    XRESULT result = RenderLowCloudLayer(
        lowCloudLayer->GetRenderTargetView().Get(),
        lowCloudDepth->GetRenderTargetView().Get(),
        skyCloudLayer->GetRenderTargetView().Get(),
        sceneSRV,
        depthSRV );
    if ( result != XR_SUCCESS ) {
        return result;
    }

    return CompositeLowClouds(
        outputRTV,
        sceneSRV,
        lowCloudLayer->GetShaderResView().Get(),
        lowCloudDepth->GetShaderResView().Get(),
        depthSRV,
        skyCloudLayer->GetShaderResView().Get() );
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
